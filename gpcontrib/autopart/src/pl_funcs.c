/* ------------------------------------------------------------------------
 *
 * pl_funcs.c
 *		Utility C functions for stored procedures
 *
 * Copyright (c) 2015-2020, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#include "compat/pg_compat.h"

#include "init.h"
#include "autopart.h"
#include "partition_creation.h"
#include "partition_filter.h"
#include "relation_info.h"
#include "xact_handling.h"
#include "utils.h"

#include "access/htup_details.h"
#include "access/heapam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "cdb/cdbvars.h"
#include "commands/tablespace.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/snapmgr.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"


PG_FUNCTION_INFO_V1( add_to_autopart_config );


/*
 * Try to add previously partitioned table to AUTOPART_CONFIG.
 */
Datum
add_to_autopart_config(PG_FUNCTION_ARGS)
{
	Oid					relid;
	char			   *expression;
	PartType			parttype;

	Oid				   *children;
	uint32				children_count;

	Relation			autopart_config;
	Datum				values[Natts_autopart_config];
	bool				isnull[Natts_autopart_config];
	HeapTuple			htup;

	Oid					expr_type;

	AutopartInitState	init_state;

	if (!IsAutopartReady())
		elog(ERROR, "autopart is disabled");

	if (!PG_ARGISNULL(0))
	{
		relid = PG_GETARG_OID(0);
	}
	else ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("'parent_relid' should not be NULL")));

	/* Protect data + definition from concurrent modification */
	LockRelationOid(relid, AccessExclusiveLock);

	/* Check that relation exists */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("relation \"%u\" does not exist", relid)));

	if (!PG_ARGISNULL(1))
	{
		expression = TextDatumGetCString(PG_GETARG_TEXT_P(1));
	}
	else ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("'expression' should not be NULL")));

	/* Check current user's privileges */
	if (!check_security_policy_internal(relid, GetUserId()))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("only the owner or superuser can change "
						"partitioning configuration of table \"%s\"",
						get_rel_name_or_relid(relid))));
	}

	/* Select partitioning type */
	switch (PG_NARGS())
	{
		/* HASH */
		case 2:
			{
				parttype = PT_HASH;

				values[Anum_autopart_config_range_interval - 1]	= (Datum) 0;
				isnull[Anum_autopart_config_range_interval - 1]	= true;
			}
			break;

		/* RANGE */
		case 3:
			{
				parttype = PT_RANGE;

				values[Anum_autopart_config_range_interval - 1]	= PG_GETARG_DATUM(2);
				isnull[Anum_autopart_config_range_interval - 1]	= PG_ARGISNULL(2);
			}
			break;

		default:
			elog(ERROR, "error in function " CppAsString(add_to_autopart_config));
			PG_RETURN_BOOL(false); /* keep compiler happy */
	}

	/* Parse and check expression */
	cook_partitioning_expression(relid, expression, &expr_type);

	/* Canonicalize user's expression (trim whitespaces etc) */
	expression = canonicalize_partitioning_expression(relid, expression);

	/* Check hash function for HASH partitioning */
	if (parttype == PT_HASH)
	{
		TypeCacheEntry *tce = lookup_type_cache(expr_type, TYPECACHE_HASH_PROC);

		if (!OidIsValid(tce->hash_proc))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("no hash function for partitioning expression")));
	}

	/*
	 * Initialize columns (partrel, attname, parttype, range_interval).
	 */
	values[Anum_autopart_config_partrel - 1]		= ObjectIdGetDatum(relid);
	isnull[Anum_autopart_config_partrel - 1]		= false;

	values[Anum_autopart_config_parttype - 1]	= Int32GetDatum(parttype);
	isnull[Anum_autopart_config_parttype - 1]	= false;

	values[Anum_autopart_config_expr - 1]		= CStringGetTextDatum(expression);
	isnull[Anum_autopart_config_expr - 1]		= false;

	/*
	 * Insert new row into AUTOPART_CONFIG.
	 *
	 * autopart_config must behave like a catalog: an identical row has to be
	 * present in the LOCAL heap of the coordinator AND of every segment, because
	 * the partitioning info is read with a direct heap scan locally on each
	 * node (on the coordinator at plan time to inject the PartitionFilter, on
	 * the segments at execution time to build the PartRelationInfo).
	 *
	 * Neither mechanism alone is enough:
	 *   - CatalogTupleInsert() writes only the coordinator's local heap;
	 *   - an SQL INSERT into a DISTRIBUTED REPLICATED table is dispatched to the
	 *     segments but leaves the coordinator's local heap empty.
	 * So we do both: a low-level insert for the coordinator and a dispatched
	 * SQL INSERT (via SPI) to populate all segments.
	 */
	autopart_config = heap_open_compat(get_autopart_config_relid(false),
									  RowExclusiveLock);

	htup = heap_form_tuple(RelationGetDescr(autopart_config), values, isnull);
	CatalogTupleInsert(autopart_config, htup);

	heap_close_compat(autopart_config, RowExclusiveLock);

	/* Now propagate the same row to every segment's local heap */
	if (IS_QUERY_DISPATCHER())
	{
		Oid			cfg_relid = get_autopart_config_relid(false);
		Oid			argtypes[Natts_autopart_config] = {
			REGCLASSOID, TEXTOID, INT4OID, TEXTOID
		};
		Datum		args[Natts_autopart_config];
		char		nulls[Natts_autopart_config];
		char	   *insert_sql;
		int			i;
		int			spi_rc;

		for (i = 0; i < Natts_autopart_config; i++)
		{
			args[i] = values[i];
			nulls[i] = isnull[i] ? 'n' : ' ';
		}

		insert_sql = psprintf("INSERT INTO %s "
							  "(partrel, expr, parttype, range_interval) "
							  "VALUES ($1, $2, $3, $4)",
							  quote_qualified_identifier(
								  get_namespace_name(get_rel_namespace(cfg_relid)),
								  get_rel_name(cfg_relid)));

		if (SPI_connect() != SPI_OK_CONNECT)
			elog(ERROR, "could not connect to SPI in "
				 CppAsString(add_to_autopart_config));

		spi_rc = SPI_execute_with_args(insert_sql, Natts_autopart_config,
									   argtypes, args, nulls, false, 0);
		if (spi_rc != SPI_OK_INSERT)
			elog(ERROR, "failed to insert a row into autopart_config "
				 "(SPI returned %d)", spi_rc);

		SPI_finish();
	}

	/* Make changes visible */
	CommandCounterIncrement();

	/* Update caches only if this relation has children */
	if (FCS_FOUND == find_inheritance_children_array(relid, NoLock, true,
													 &children_count,
													 &children))
	{
		pfree(children);

		PG_TRY();
		{
			/* Some flags might change during refresh attempt */
			save_autopart_init_state(&init_state);

			/* Now try to create a PartRelationInfo */
			has_autopart_relation_info(relid);
		}
		PG_CATCH();
		{
			/* We have to restore changed flags */
			restore_autopart_init_state(&init_state);

			/* Rethrow ERROR */
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	/* Check if naming sequence exists */
	if (parttype == PT_RANGE)
	{
		RangeVar   *naming_seq_rv;
		Oid			naming_seq;

		naming_seq_rv = makeRangeVar(get_namespace_name(get_rel_namespace(relid)),
									 build_sequence_name_relid_internal(relid),
									 -1);

		naming_seq = RangeVarGetRelid(naming_seq_rv, AccessShareLock, true);
		if (OidIsValid(naming_seq))
		{
			ObjectAddress	parent,
							sequence;

			ObjectAddressSet(parent, RelationRelationId, relid);
			ObjectAddressSet(sequence, RelationRelationId, naming_seq);

			/* Now this naming sequence is a "part" of partitioned relation */
			recordDependencyOn(&sequence, &parent, DEPENDENCY_NORMAL);
		}
	}

	CacheInvalidateRelcacheByRelid(relid);

	PG_RETURN_BOOL(true);
}
