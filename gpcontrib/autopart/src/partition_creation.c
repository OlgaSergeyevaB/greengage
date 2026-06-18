/*-------------------------------------------------------------------------
 *
 * partition_creation.c
 *		Various functions for partition creation.
 *
 * Copyright (c) 2016-2020, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "init.h"
#include "partition_creation.h"
#include "partition_filter.h"
#include "autopart.h"
#include "compat/pg_compat.h"
#include "xact_handling.h"

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "catalog/toasting.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/sequence.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_func.h"
#include "parser/parse_utilcmd.h"
#include "parser/parse_relation.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/jsonb.h"
#include "utils/snapmgr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/partcache.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_opclass.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbsrlz.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbutil.h"
#include "catalog/oid_dispatch.h"
#include "nodes/makefuncs.h"
#include "nodes/bitmapset.h"
#include "executor/execdesc.h"
#include "libpq/pqformat.h"

#include "utils/regproc.h"


static ObjectAddress create_table_using_stmt(CreateStmt *create_stmt,
											 Oid relowner, GpPolicy * policy,
											 bool do_dispatch);


/*
 * ---------------------------------------
 *  Public interface (partition creation)
 * ---------------------------------------
 */


/* Create a new table using cooked CreateStmt */
static ObjectAddress
create_table_using_stmt(CreateStmt *create_stmt, Oid relowner, GpPolicy * policy,
						bool do_dispatch)
{
	ObjectAddress	table_addr;
	Datum			toast_options;
	static char	   *validnsps[] = HEAP_RELOPT_NAMESPACES;
	int				guc_level;

	/* Create new GUC level... */
	guc_level = NewGUCNestLevel();

	/* ... and set client_min_messages = warning */
	(void) set_config_option(CppAsString(client_min_messages), "WARNING",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/* Create new partition owned by parent's posessor */
	table_addr = DefineRelationCompat(create_stmt, RELKIND_RELATION, relowner,
									  NULL, policy);

	/* Save data about a simple DDL command that was just executed */
	EventTriggerCollectSimpleCommand(table_addr,
									 InvalidObjectAddress,
									 (Node *) create_stmt);

	/*
	 * Let NewRelationCreateToastTable decide if this
	 * one needs a secondary relation too.
	 */
	CommandCounterIncrement();

	/* Parse and validate reloptions for the toast table */
	toast_options = transformRelOptions((Datum) 0, create_stmt->options,
										"toast", validnsps, true, false);

	/* Parse options for a new toast table */
	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options, true);

	/* Now create the toast table if needed */
	NewRelationCreateToastTable(table_addr.objectId, toast_options);

	/* Restore original GUC values */
	AtEOXact_GUC(true, guc_level);

	if (do_dispatch && Gp_role == GP_ROLE_DISPATCH)
	{
		CdbDispatchUtilityStatement((Node *) create_stmt,
									DF_CANCEL_ON_ERROR |
									DF_NEED_TWO_PHASE |
									DF_WITH_SNAPSHOT,
									GetAssignedOidsForDispatch(),
									NULL);
	}

	/* Return the address */
	return table_addr;
}


/*
 * ============================================================================
 *  On-demand RANGE partition creation distributed via the coordinator
 *
 *  The coordinator creates the partition relation locally (without dispatch),
 *  harvests the full set of assigned OIDs and ships them to the requesting QE,
 *  which recreates an identical relation via AddPreassignedOids().  Every node
 *  then attaches its local partition independently.  This avoids issuing a
 *  nested distributed query from within the running INSERT.
 * ============================================================================
 */

/* One on-demand partition created during the current statement. */
typedef struct CreatedPartition
{
	Oid			parent;			/* partitioned (root) relation */
	int32		lower;			/* RANGE lower bound (inclusive) */
	int32		upper;			/* RANGE upper bound (exclusive) */
	Oid			relid;			/* created partition relation */
	char	   *serialized;		/* serialized List of OidAssignment (QD only) */
	int			serialized_len;
	bool		worker_created;	/* created+attached cluster-wide by a worker;
								 * no deferred CREATE/ATTACH needed */
} CreatedPartition;

/* List of CreatedPartition*, valid for the duration of one statement. */
static List		   *created_partitions = NIL;
static MemoryContext created_partitions_cxt = NULL;

static MemoryContext
get_created_partitions_cxt(void)
{
	if (created_partitions_cxt == NULL)
		created_partitions_cxt = AllocSetContextCreate(TopMemoryContext,
													   "autopart auto-partitions",
													   ALLOCSET_DEFAULT_SIZES);
	return created_partitions_cxt;
}

static CreatedPartition *
find_created_partition(Oid parent, int32 lower, int32 upper)
{
	ListCell *lc;

	foreach (lc, created_partitions)
	{
		CreatedPartition *cp = (CreatedPartition *) lfirst(lc);

		if (cp->parent == parent && cp->lower == lower && cp->upper == upper)
			return cp;
	}
	return NULL;
}

/* Deterministic partition name shared by the coordinator and the segments. */
char *
make_range_partition_name(Oid parent_relid, int32 lower, int32 upper)
{
	return psprintf("%s_%d_%d", get_rel_name(parent_relid), lower, upper);
}

/* Build a list of ColumnDef matching a relation's (non-dropped) attributes. */
static List *
make_column_defs_from_rel(Relation parentrel)
{
	List	   *result = NIL;
	TupleDesc	tupdesc = RelationGetDescr(parentrel);
	int			i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute	attr = TupleDescAttr(tupdesc, i);
		ColumnDef		   *coldef;

		if (attr->attisdropped)
			continue;

		coldef = makeColumnDef(NameStr(attr->attname),
							   attr->atttypid,
							   attr->atttypmod,
							   attr->attcollation);
		coldef->is_not_null = attr->attnotnull;

		result = lappend(result, coldef);
	}

	return result;
}

/*
 * Create a partition relation locally, with the parent's columns and
 * distribution policy, WITHOUT dispatching anything to the segments.
 *
 * On the coordinator (GP_ROLE_DISPATCH) the assigned OIDs accumulate in the
 * private dispatch_oids list and can be harvested with
 * GetAssignedOidsForDispatch().  On a segment the caller is expected to have
 * primed the preassigned OIDs via AddPreassignedOids() beforehand.
 */
/* Build a bare CreateStmt for a partition that mirrors the parent's shape. */
static CreateStmt *
build_partition_create_stmt(Relation parentrel, const char *partition_name,
							bool if_not_exists)
{
	Oid				parent_relid = RelationGetRelid(parentrel);
	Oid				parent_nsp = get_rel_namespace(parent_relid);
	char		   *parent_nsp_name = get_namespace_name(parent_nsp);
	RangeVar	   *partition_rv;
	CreateStmt	   *create_stmt = makeNode(CreateStmt);

	partition_rv = makeRangeVar(parent_nsp_name, pstrdup(partition_name), -1);
	partition_rv->inh = false;

	create_stmt->relation		= partition_rv;
	create_stmt->tableElts		= make_column_defs_from_rel(parentrel);
	create_stmt->inhRelations	= NIL;
	create_stmt->ofTypename		= NULL;
	create_stmt->constraints	= NIL;
	create_stmt->options		= NIL;
	create_stmt->oncommit		= ONCOMMIT_NOOP;
	create_stmt->tablespacename	= NULL;
	create_stmt->if_not_exists	= if_not_exists;
	create_stmt->distributedBy	= make_distributedby_for_rel(parentrel);
	create_stmt->partitionBy	= NULL;
	create_stmt->intoPolicy		= NULL;
	create_stmt->attr_encodings	= NIL;
	create_stmt->part_idx_oids	= NIL;
	create_stmt->part_idx_names	= NIL;
	create_stmt->ownerid		= InvalidOid;
	create_stmt->partbound		= NULL;
	create_stmt->partspec		= NULL;
	create_stmt->accessMethod	= NULL;

	return create_stmt;
}

Oid
create_partition_relation_local(Oid parent_relid, const char *partition_name)
{
	Oid				partition_relid = InvalidOid;
	Relation		parentrel;
	CreateStmt	   *create_stmt;
	List		   *create_stmts;
	ListCell	   *lc;
	Oid				child_relowner = get_rel_owner(parent_relid);

	parentrel = table_open(parent_relid, AccessShareLock);

	create_stmt = build_partition_create_stmt(parentrel, partition_name, false);

	/* No LIKE / constraints, so this yields a single CreateStmt. */
	create_stmts = transformCreateStmt(create_stmt, NULL);

	foreach (lc, create_stmts)
	{
		Node   *cur_stmt = (Node *) lfirst(lc);

		if (IsA(cur_stmt, CreateStmt))
		{
			partition_relid = create_table_using_stmt((CreateStmt *) cur_stmt,
													  child_relowner,
													  GpPolicyCopy(parentrel->rd_cdbpolicy),
													  false /* no dispatch */).objectId;
			CommandCounterIncrement();
		}
		else
			elog(ERROR, "unexpected statement type %d while creating partition",
				 (int) nodeTag(cur_stmt));
	}

	table_close(parentrel, NoLock);

	return partition_relid;
}

/*
 * Coordinator side, at end of statement: dispatch a CREATE (if_not_exists,
 * carrying the exact preassigned OIDs) to every segment, so segments that did
 * not route a tuple into the partition create it locally with matching OIDs,
 * and segments that already have it (where a tuple landed) skip it.
 */
static void
dispatch_create_partition_to_segments(Oid parent_relid, const char *partition_name,
									  char *serialized, int len)
{
	Relation		parentrel;
	CreateStmt	   *create_stmt;
	List		   *oids;

	if (serialized == NULL || len <= 0)
		return;

	oids = (List *) deserializeNode(serialized, len);

	parentrel = table_open(parent_relid, AccessShareLock);

	/* Dispatch the RAW CreateStmt; each QE transforms and executes it. */
	create_stmt = build_partition_create_stmt(parentrel, partition_name, true);

	elog(DEBUG1, "autopart[QD]: dispatching CREATE %s (oids_len=%d, noids=%d) to segments",
		 partition_name, len, list_length(oids));

	CdbDispatchUtilityStatement((Node *) create_stmt,
								DF_CANCEL_ON_ERROR |
								DF_NEED_TWO_PHASE |
								DF_WITH_SNAPSHOT,
								oids,
								NULL);

	table_close(parentrel, NoLock);
}

/* Attach a previously created relation as a native RANGE partition (local). */
static void
attach_partition_local(Oid parent_relid, Oid partition_relid,
					   int32 lower, int32 upper)
{
	AlterTableCmd	   *atcmd  = makeNode(AlterTableCmd);
	PartitionCmd	   *pcmd   = makeNode(PartitionCmd);
	PartitionBoundSpec *boundspec = makeNode(PartitionBoundSpec);
	Const			   *nl,
					   *nu;
	PartitionRangeDatum *pnl,
						*pnu;

	nl = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
				   Int32GetDatum(lower), false, true);
	pnl = makeNode(PartitionRangeDatum);
	pnl->kind = PARTITION_RANGE_DATUM_VALUE;
	pnl->value = (Node *) nl;

	nu = makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
				   Int32GetDatum(upper), false, true);
	pnu = makeNode(PartitionRangeDatum);
	pnu->kind = PARTITION_RANGE_DATUM_VALUE;
	pnu->value = (Node *) nu;

	boundspec->strategy = PARTITION_STRATEGY_RANGE;
	boundspec->is_default = false;
	boundspec->lowerdatums = list_make1(pnl);
	boundspec->upperdatums = list_make1(pnu);
	boundspec->location = -1;

	elog(DEBUG1, "autopart[%s]: attaching partition relid=%u to parent=%u [%d,%d)",
		 Gp_role == GP_ROLE_DISPATCH ? "QD" : "QE",
		 partition_relid, parent_relid, lower, upper);

	pcmd->name = makeRangeVar(get_namespace_name(get_rel_namespace(partition_relid)),
							  pstrdup(get_rel_name(partition_relid)), -1);
	pcmd->bound = boundspec;

	atcmd->subtype = AT_AttachPartition;
	atcmd->def = (Node *) pcmd;

	{
		AlterTableStmt *atstmt = makeNode(AlterTableStmt);

		atstmt->relation = makeRangeVar(get_namespace_name(get_rel_namespace(parent_relid)),
										pstrdup(get_rel_name(parent_relid)), -1);
		atstmt->relkind = OBJECT_TABLE;
		atstmt->missing_ok = false;
		atstmt->cmds = list_make1(atcmd);
		atstmt->is_internal = true; /* avoid re-transform */

		/*
		 * On the coordinator AlterTable() dispatches the AT_AttachPartition to
		 * all segments (where the identically-named/OID'd relation already
		 * exists), so the attach is applied cluster-wide from a single call.
		 *
		 * Use ShareUpdateExclusiveLock, not AccessExclusiveLock: this is the
		 * lock level the core actually requires for AT_AttachPartition
		 * (AlterTableGetLockLevel, tablecmds.c). ShareUpdateExclusiveLock does
		 * not conflict with the RowExclusiveLock held by the in-flight INSERT,
		 * which avoids the lock-upgrade deadlock of escalating to
		 * AccessExclusiveLock while the INSERT is still running.
		 */
		AlterTable(parent_relid, ShareUpdateExclusiveLock, atstmt);
	}
}

/*
 * Coordinator side: handle a QE request for the partition covering
 * [lower, upper).  Two strategies depending on what our own transaction holds:
 *
 *   - no heavy lock on the parent (the common case): a background worker creates
 *     and attaches the partition cluster-wide in its own committed transaction;
 *     no OIDs are returned and the QE re-reads the catalog by name.
 *
 *   - we already hold a lock >= ShareUpdateExclusive on the parent: create the
 *     relation locally to assign OIDs, return them to the QE (which recreates an
 *     identical relation), and defer the cluster-wide CREATE+ATTACH to
 *     partition_filter_end().
 */
char *
coordinator_handle_partition_request(Oid parent_relid, int32 lower, int32 upper,
									 int *len)
{
	CreatedPartition   *cp;
	MemoryContext		oldcxt;
	List			   *oids;
	char			   *serialized;
	int					size;
	Oid					relid;

	/*
	 * Common case: our transaction does NOT hold a heavy lock on the parent.
	 * Offload creation to a background worker that creates+attaches the
	 * partition cluster-wide in its own, independently committed transaction
	 * (Oracle-style; no lock upgrade in the inserting transaction).  No OIDs are
	 * shipped back: the QE re-reads the catalog by name.
	 */
	if (!xact_bgw_conflicting_lock_exists(parent_relid))
	{
		/*
		 * Dedup: several segments routing the first tuple of the same new range
		 * each notify us independently, so we may be asked to create the very
		 * same partition many times within one statement.  If we already spawned
		 * a worker for this range during this statement, there is nothing to do
		 * -- the partition is already present cluster-wide; just answer "done".
		 */
		if (find_created_partition(parent_relid, lower, upper) != NULL)
		{
			elog(DEBUG1, "autopart: worker already spawned for parent=%u [%d,%d) this statement, skipping",
				 parent_relid, lower, upper);
			*len = 0;
			return NULL;
		}

		if (!request_partition_creation(parent_relid, lower, upper))
			elog(ERROR,
				 "autopart: worker failed to create partition for range [%d, %d)",
				 lower, upper);

		/* Remember it so duplicate requests for this range spawn no worker. */
		oldcxt = MemoryContextSwitchTo(get_created_partitions_cxt());
		cp = palloc0(sizeof(CreatedPartition));
		cp->parent = parent_relid;
		cp->lower = lower;
		cp->upper = upper;
		cp->relid = InvalidOid;
		cp->worker_created = true;
		created_partitions = lappend(created_partitions, cp);
		MemoryContextSwitchTo(oldcxt);

		*len = 0;
		return NULL;
	}

	/*
	 * Our own (inserting) transaction already holds a lock >= SUEL on the parent
	 * -- LOCK TABLE, or we created/altered it in this very transaction.  A
	 * separate worker would block on that lock at the segments (the lock group
	 * only covers the coordinator) and might not even see the uncommitted
	 * parent; and we cannot issue a nested distributed DDL from this NOTIFY
	 * handler while the INSERT's dispatch is in flight ("multiple segworker
	 * groups").
	 *
	 * So fall back to the in-transaction mechanism: create the relation locally
	 * on the coordinator to assign OIDs, ship them to the requesting QE so it
	 * recreates an identical relation, and defer the cluster-wide CREATE
	 * (if_not_exists, with the saved OIDs) + ATTACH to partition_filter_end
	 * (attach_created_partitions()).
	 */
	LockRelationOid(parent_relid, ShareUpdateExclusiveLock);

	/* Same range already requested during this statement -> reuse OIDs. */
	cp = find_created_partition(parent_relid, lower, upper);
	if (cp != NULL)
	{
		*len = cp->serialized_len;
		return cp->serialized;
	}

	/* Drop any stale assigned-OID bookkeeping before we start. */
	(void) GetAssignedOidsForDispatch();

	/* Create the relation locally; OIDs accumulate in dispatch_oids. */
	relid = create_partition_relation_local(parent_relid,
											make_range_partition_name(parent_relid,
																	  lower, upper));

	/* Harvest the full set of assigned OIDs and serialize for transport. */
	oids = GetAssignedOidsForDispatch();

	oldcxt = MemoryContextSwitchTo(get_created_partitions_cxt());
	serialized = serializeNode((Node *) oids, &size, NULL);

	cp = palloc0(sizeof(CreatedPartition));
	cp->parent = parent_relid;
	cp->lower = lower;
	cp->upper = upper;
	cp->relid = relid;
	cp->serialized = serialized;
	cp->serialized_len = size;
	created_partitions = lappend(created_partitions, cp);

	MemoryContextSwitchTo(oldcxt);

	*len = size;
	return serialized;
}

/*
 * Segment side: record a partition that was (re)created locally from
 * coordinator-assigned OIDs, so it can be attached at end of statement.
 */
void
remember_segment_partition(Oid parent_relid, int32 lower, int32 upper, Oid relid)
{
	MemoryContext		oldcxt;
	CreatedPartition   *cp;

	oldcxt = MemoryContextSwitchTo(get_created_partitions_cxt());

	cp = palloc0(sizeof(CreatedPartition));
	cp->parent = parent_relid;
	cp->lower = lower;
	cp->upper = upper;
	cp->relid = relid;

	created_partitions = lappend(created_partitions, cp);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * Coordinator side, at end of the modifying statement: for every partition
 * created on demand during the statement, dispatch a CREATE (if_not_exists,
 * with the saved preassigned OIDs) to all segments so each segment has a local
 * copy with matching OIDs, then attach it (AlterTable dispatches the attach).
 *
 * Runs only on the coordinator; on the segments this is a no-op besides
 * clearing the per-statement bookkeeping.
 */
void
attach_created_partitions(void)
{
	ListCell *lc;

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		foreach (lc, created_partitions)
		{
			CreatedPartition *cp = (CreatedPartition *) lfirst(lc);

			/* Worker path already created+attached this one cluster-wide. */
			if (cp->worker_created)
				continue;

			dispatch_create_partition_to_segments(cp->parent,
												  make_range_partition_name(cp->parent,
																			cp->lower,
																			cp->upper),
												  cp->serialized,
												  cp->serialized_len);
			CommandCounterIncrement();

			attach_partition_local(cp->parent, cp->relid, cp->lower, cp->upper);
			CommandCounterIncrement();
		}
	}

	created_partitions = NIL;
	if (created_partitions_cxt != NULL)
		MemoryContextReset(created_partitions_cxt);
}


/*
 * -----------------------------
 *  Check constraint generation
 * -----------------------------
 */


/*
 * ---------------------
 *  Callback invocation
 * ---------------------
 */

