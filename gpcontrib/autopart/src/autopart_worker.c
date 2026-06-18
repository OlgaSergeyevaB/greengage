/* ------------------------------------------------------------------------
 *
 * autopart_worker.c
 *		Background worker that creates an on-demand RANGE partition in its own
 *		(distributed, independently committed) transaction.
 *
 *		The worker runs on the coordinator as a dispatcher (GP_ROLE_DISPATCH)
 *		and executes a single native
 *			CREATE TABLE <child> PARTITION OF <parent> FOR VALUES FROM (l) TO (u)
 *		statement, which creates the partition, attaches it and dispatches it to
 *		all segments with cluster-consistent OIDs under two-phase commit.  Because
 *		this happens in a separate transaction, the partition survives a rollback
 *		of the inserting transaction (Oracle interval-partition semantics) and the
 *		inserting transaction never has to elevate its own lock on the parent.
 *
 * ------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/gp_distribution_policy.h"
#include "cdb/cdbvars.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"

#include "compat/pg_compat.h"
#include "partition_creation.h"


/* Arguments passed to the worker through a DSM segment. */
typedef struct CreatePartitionArgs
{
	Oid			dbid;			/* database to connect to */
	Oid			userid;			/* user to run as */
	Oid			parent_relid;	/* partitioned (root) relation */
	int32		lower;			/* RANGE lower bound (inclusive) */
	int32		upper;			/* RANGE upper bound (exclusive) */
	PGPROC	   *leader_pgproc;	/* inserting backend, for lock group */
	pid_t		leader_pid;		/* its pid */
	bool		success;		/* set by the worker on success */
} CreatePartitionArgs;

static const char  *create_partition_bgw = "AutopartCreateWorker";

PGDLLEXPORT void bgw_main_create_partition(Datum main_arg);


/*
 * Worker SIGTERM handler: ask for a graceful exit.
 */
static void
handle_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	SetLatch(MyLatch);

	if (!proc_exit_inprogress)
	{
		InterruptPending = true;
		ProcDiePending = true;
	}

	errno = save_errno;
}

/*
 * Start the create-partition worker and wait for it to finish.
 */
static bool
start_create_partition_bgworker(dsm_handle segment_handle)
{
	BackgroundWorker		worker;
	BackgroundWorkerHandle *bgw_handle;
	BgwHandleStatus			bgw_status;
	pid_t					pid;

	memset(&worker, 0, sizeof(worker));

	snprintf(worker.bgw_name, BGW_MAXLEN, "%s", create_partition_bgw);
	snprintf(worker.bgw_type, BGW_MAXLEN, "%s", create_partition_bgw);
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "bgw_main_create_partition");
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "autopart");

	worker.bgw_flags		= BGWORKER_SHMEM_ACCESS |
							  BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time	= BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time	= BGW_NEVER_RESTART;
	worker.bgw_main_arg		= UInt32GetDatum(segment_handle);
	worker.bgw_notify_pid	= MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &bgw_handle))
		return false;

	bgw_status = WaitForBackgroundWorkerStartup(bgw_handle, &pid);
	if (bgw_status == BGWH_POSTMASTER_DIED)
		ereport(ERROR,
				(errmsg("postmaster died during autopart partition creation"),
				 errhint("More details may be available in the server log.")));

	bgw_status = WaitForBackgroundWorkerShutdown(bgw_handle);
	if (bgw_status == BGWH_POSTMASTER_DIED)
		ereport(ERROR,
				(errmsg("postmaster died during autopart partition creation"),
				 errhint("More details may be available in the server log.")));

	return true;
}

/*
 * Coordinator entry point: ask a background worker to create the partition
 * covering [lower, upper) for 'parent_relid' and wait until it is done.
 *
 * Returns true if the worker reported success.
 */
bool
request_partition_creation(Oid parent_relid, int32 lower, int32 upper)
{
	dsm_segment			   *segment;
	CreatePartitionArgs	   *args;
	bool					ok;

	/*
	 * Become a lock-group leader so the worker can join our group.  This makes
	 * the coordinator's deadlock detector treat us and the worker as a single
	 * entity, so a wait cycle that runs through the worker's lock requests and
	 * back into a lock we hold is detected as a deadlock instead of hanging
	 * (the inserting backend waits for the worker over IPC, which the detector
	 * cannot see on its own).
	 */
	BecomeLockGroupLeader();

	segment = dsm_create(sizeof(CreatePartitionArgs), 0);
	args = (CreatePartitionArgs *) dsm_segment_address(segment);

	args->dbid			= MyDatabaseId;
	args->userid		= GetUserId();
	args->parent_relid	= parent_relid;
	args->lower			= lower;
	args->upper			= upper;
	args->leader_pgproc	= MyProc;
	args->leader_pid	= MyProcPid;
	args->success		= false;

	if (!start_create_partition_bgworker(dsm_segment_handle(segment)))
	{
		dsm_detach(segment);
		ereport(ERROR,
				(errmsg("could not start autopart partition-creation worker"),
				 errhint("consider increasing max_worker_processes")));
	}

	ok = args->success;
	dsm_detach(segment);

	return ok;
}

/*
 * Render the parent's distribution policy as a DISTRIBUTED ... clause so the
 * standalone child table is created with a matching policy (required for
 * ATTACH PARTITION to succeed).
 */
static char *
build_distributed_by_clause(Relation rel)
{
	GpPolicy	   *policy = rel->rd_cdbpolicy;
	StringInfoData	buf;
	int				i;

	initStringInfo(&buf);

	if (policy == NULL)
		return buf.data;		/* fall back to the server default */

	if (policy->ptype == POLICYTYPE_REPLICATED)
		appendStringInfoString(&buf, "DISTRIBUTED REPLICATED");
	else if (policy->ptype == POLICYTYPE_PARTITIONED)
	{
		if (policy->nattrs == 0)
			appendStringInfoString(&buf, "DISTRIBUTED RANDOMLY");
		else
		{
			TupleDesc	desc = RelationGetDescr(rel);

			appendStringInfoString(&buf, "DISTRIBUTED BY (");
			for (i = 0; i < policy->nattrs; i++)
			{
				Form_pg_attribute attr = TupleDescAttr(desc, policy->attrs[i] - 1);

				if (i > 0)
					appendStringInfoString(&buf, ", ");
				appendStringInfoString(&buf, quote_identifier(NameStr(attr->attname)));
			}
			appendStringInfoChar(&buf, ')');
		}
	}

	return buf.data;
}

/*
 * Actually create the partition (worker side).  Runs inside the worker's own
 * transaction; on the coordinator it dispatches the DDL to all segments.
 *
 * Done in two steps on purpose:
 *   1. CREATE TABLE <child> (LIKE <parent>) <distributed by> -- a standalone
 *      table; this does NOT take a strong lock on the parent.
 *   2. ALTER TABLE <parent> ATTACH PARTITION <child> ...     -- takes only
 *      ShareUpdateExclusiveLock on the parent, which is compatible with the
 *      RowExclusiveLock held by the inserting transaction.
 *
 * (CREATE TABLE ... PARTITION OF would instead take AccessExclusiveLock on the
 * parent and deadlock/​hang against the in-flight INSERT.)
 */
/*
 * Create and attach the partition in the CURRENT transaction (no separate
 * worker, no lock acquisition here -- the caller guarantees an appropriate lock
 * on the parent is already held).  Used both by the worker (after it takes
 * ShareUpdateExclusiveLock) and by the inline path on the coordinator when the
 * inserting transaction already holds a lock >= ShareUpdateExclusive on the
 * parent (in which case a separate worker would block on that lock).
 */
void
autopart_create_partition_now(Oid parent_relid, int32 lower, int32 upper)
{
	char	   *partition_name = make_range_partition_name(parent_relid, lower, upper);
	Oid			nsp = get_rel_namespace(parent_relid);
	char	   *nspname = get_namespace_name(nsp);
	char	   *qchild;
	char	   *qparent;
	char	   *distclause;
	Relation	parentrel;
	char	   *sql;
	int			rc;

	/* Someone may have created it already (another tuple/worker). */
	if (OidIsValid(get_relname_relid(partition_name, nsp)))
		return;

	qchild = quote_qualified_identifier(nspname, partition_name);
	qparent = quote_qualified_identifier(nspname, get_rel_name(parent_relid));

	/* Read the parent's distribution policy (lock already held). */
	parentrel = table_open(parent_relid, NoLock);
	distclause = build_distributed_by_clause(parentrel);
	table_close(parentrel, NoLock);

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "autopart worker: SPI_connect failed");

	/* 1. Standalone child matching the parent's shape and distribution. */
	sql = psprintf("CREATE TABLE %s (LIKE %s) %s", qchild, qparent, distclause);
	rc = SPI_execute(sql, false, 0);
	if (rc != SPI_OK_UTILITY)
		elog(ERROR, "autopart worker: CREATE failed (SPI=%d)", rc);

	/* 2. Attach it as a RANGE partition (ShareUpdateExclusive on parent). */
	sql = psprintf("ALTER TABLE %s ATTACH PARTITION %s FOR VALUES FROM (%d) TO (%d)",
				   qparent, qchild, lower, upper);
	rc = SPI_execute(sql, false, 0);
	if (rc != SPI_OK_UTILITY)
		elog(ERROR, "autopart worker: ATTACH failed (SPI=%d)", rc);

	SPI_finish();

	elog(DEBUG1, "autopart worker: created+attached partition %s [%d,%d)",
		 partition_name, lower, upper);
}

/*
 * Background worker main function.
 */
void
bgw_main_create_partition(Datum main_arg)
{
	dsm_handle				handle = DatumGetUInt32(main_arg);
	dsm_segment			   *segment;
	CreatePartitionArgs	   *args;

	pqsignal(SIGTERM, handle_sigterm);
	BackgroundWorkerUnblockSignals();

	CurrentResourceOwner = ResourceOwnerCreate(NULL, create_partition_bgw);

	segment = dsm_attach(handle);
	if (segment == NULL)
		elog(ERROR, "autopart worker: cannot attach to dsm segment");
	args = (CreatePartitionArgs *) dsm_segment_address(segment);

	/*
	 * Join the inserting backend's lock group before taking any locks, so the
	 * coordinator's deadlock detector can see a cycle that passes through us.
	 */
	if (!BecomeLockGroupMember(args->leader_pgproc, args->leader_pid))
		elog(ERROR, "autopart worker: could not join leader's lock group");

	BackgroundWorkerInitializeConnectionByOidCompat(args->dbid, args->userid);

	/* On the coordinator act as a dispatcher so the DDL reaches the segments. */
	if (IS_QUERY_DISPATCHER())
		Gp_role = GP_ROLE_DISPATCH;
	else
		Gp_role = GP_ROLE_UTILITY;

	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * Serialize concurrent creators on the parent with the same lock level the
	 * core uses for ATTACH PARTITION; ShareUpdateExclusiveLock does not conflict
	 * with the RowExclusiveLock held by the inserting transaction.
	 */
	LockRelationOid(args->parent_relid, ShareUpdateExclusiveLock);

	autopart_create_partition_now(args->parent_relid, args->lower, args->upper);

	PopActiveSnapshot();
	CommitTransactionCommand();

	args->success = true;
	dsm_detach(segment);
}
