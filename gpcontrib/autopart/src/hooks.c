/* ------------------------------------------------------------------------
 *
 * hooks.c
 *		definitions of rel_pathlist and join_pathlist hooks
 *
 * Copyright (c) 2016-2020, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */

#include "compat/pg_compat.h"
#include "compat/rowmarks_fix.h"

#include "access/table.h"

#include "hooks.h"
#include "init.h"
#include "partition_creation.h"
#include "partition_filter.h"
#include "planner_tree_modification.h"
#include "utils.h"
#include "xact_handling.h"

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "rewrite/rewriteManip.h"
#include "utils/typcache.h"
#include "utils/lsyscache.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbdisp.h"


#ifdef USE_ASSERT_CHECKING
#define USE_RELCACHE_LOGGING
#endif


planner_hook_type				autopart_planner_hook_next				= NULL;
post_parse_analyze_hook_type	autopart_post_parse_analyze_hook_next	= NULL;
shmem_startup_hook_type			autopart_shmem_startup_hook_next			= NULL;


/*
 * Intercept 'autopart.enable' GUC assignments.
 */
void
autopart_enable_assign_hook(bool newval, void *extra)
{
	elog(DEBUG2, "autopart_enable_assign_hook() [newval = %s] triggered",
		  newval ? "true" : "false");

	if (!(newval == autopart_init_state.autopart_enable &&
		  newval == autopart_init_state.auto_partition &&
		  newval == autopart_init_state.override_copy &&
		  newval == autopart_enable_partition_filter &&
		  newval == autopart_enable_bounds_cache))
	{
		elog(NOTICE,
			 "PartitionFilter node "
			 "and some other options have been %s",
			 newval ? "enabled" : "disabled");
	}


	autopart_init_state.auto_partition		= newval;
	autopart_init_state.override_copy		= newval;
	autopart_enable_partition_filter		= newval;
	autopart_enable_bounds_cache			= newval;

	/* Purge caches if autopart was disabled */
	if (!newval)
	{
		unload_config();
	}
}

static void
execute_for_plantree(PlannedStmt *planned_stmt,
					 Plan *(*proc) (List *rtable, Plan *plan))
{
	List		*subplans = NIL;
	ListCell	*lc;
	Plan		*resplan = proc(planned_stmt->rtable, planned_stmt->planTree);

	if (resplan)
		planned_stmt->planTree = resplan;

	foreach (lc, planned_stmt->subplans)
	{
		Plan	*subplan = lfirst(lc);
		resplan = proc(planned_stmt->rtable, (Plan *) lfirst(lc));
		if (resplan)
			subplans = lappend(subplans, resplan);
		else
			subplans = lappend(subplans, subplan);
	}
	planned_stmt->subplans = subplans;
}

/*
 * Planner hook. It disables inheritance for tables that have been partitioned
 * by autopart to prevent standart PostgreSQL partitioning mechanism from
 * handling those tables.
 *
 * Since >= 13 (6aba63ef3e6) query_string parameter was added.
 */
PlannedStmt *
autopart_planner_hook(Query *parse, int cursorOptions, ParamListInfo boundParams)
{
	PlannedStmt	   *result;
	uint32			query_id = parse->queryId;

	/* Save the result in case it changes */
	bool			autopart_ready = IsAutopartReady();

	PG_TRY();
	{
		if (autopart_ready)
		{
			/* Increase planner() calls count */
			incr_planner_calls_count();

			/* Modify query tree if needed */
		}

		/* Invoke original hook if needed */
		if (autopart_planner_hook_next)
			result = autopart_planner_hook_next(parse, cursorOptions, boundParams);
		else
			result = standard_planner(parse, cursorOptions, boundParams);

		if (autopart_ready)
		{
			/* Add PartitionFilter node for INSERT queries */
			execute_for_plantree(result, add_partition_filters);

			/* Add PartitionRouter node for UPDATE queries */

			/* Decrement planner() calls count */
			decr_planner_calls_count();

			/* HACK: restore queryId set by pg_stat_statements */
			result->queryId = query_id;
		}
	}
	/* We must decrease parenthood statuses refcount on ERROR */
	PG_CATCH();
	{
		if (autopart_ready)
		{
			/* Caught an ERROR, decrease count */
			decr_planner_calls_count();
		}

		/* Rethrow ERROR further */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Finally return the Plan */
	return result;
}

/*
 * Post parse analysis hook. It makes sure the config is loaded before executing
 * any statement, including utility commands.
 */
void
autopart_post_parse_analyze_hook(ParseState *pstate, Query *query)
{
	/* Invoke original hook if needed */
	if (autopart_post_parse_analyze_hook_next)
		autopart_post_parse_analyze_hook_next(pstate, query);

	/* See cook_partitioning_expression() */
	if (!autopart_hooks_enabled)
		return;

	/* We shouldn't proceed on: ... */
	if (query->commandType == CMD_UTILITY)
	{
		/* ... BEGIN */
		if (xact_is_transaction_stmt(query->utilityStmt))
			return;

		/* ... SET autopart.enable */
		if (xact_is_set_stmt(query->utilityStmt, AUTOPART_ENABLE))
		{
			/* Accept all events in case it's "enable = OFF" */
			if (IsAutopartReady())
				finish_delayed_invalidation();

			return;
		}

		/* ... SET [TRANSACTION] */
		if (xact_is_set_stmt(query->utilityStmt, NULL))
			return;

		/* ... ALTER EXTENSION autopart */
		if (xact_is_alter_autopart_stmt(query->utilityStmt))
		{
			/* Leave no delayed events before ALTER EXTENSION */
			if (IsAutopartReady())
				finish_delayed_invalidation();

			/* Disable autopart to perform a painless update */
			(void) set_config_option(AUTOPART_ENABLE, "off",
									 PGC_SUSET, PGC_S_SESSION,
									 GUC_ACTION_SAVE, true, 0, false);

			return;
		}
	}

	/* Finish all delayed invalidation jobs */
	if (IsAutopartReady())
		finish_delayed_invalidation();

	/* Load config if autopart exists & it's still necessary */
	if (IsAutopartEnabled() &&
		!IsAutopartInitialized() &&
		/* Now evaluate the most expensive clause */
		get_autopart_schema() != InvalidOid)
	{
		load_config(); /* perform main cache initialization */
	}
	if (!IsAutopartReady())
		return;

	/* Process inlined SQL functions (we've already entered planning stage) */
	if (IsAutopartReady() && get_planner_calls_count() > 0)
	{
		/* Check that autopart is the last extension loaded */
		if (post_parse_analyze_hook != autopart_post_parse_analyze_hook)
		{
			Oid		save_userid;
			int		save_sec_context;
			bool	need_priv_escalation = !superuser(); /* we might be a SU */
			char   *spl_value; /* value of "shared_preload_libraries" GUC */

			/* Do we have to escalate privileges? */
			if (need_priv_escalation)
			{
				/* Get current user's Oid and security context */
				GetUserIdAndSecContext(&save_userid, &save_sec_context);

				/* Become superuser in order to bypass sequence ACL checks */
				SetUserIdAndSecContext(BOOTSTRAP_SUPERUSERID,
									   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
			}

			/* TODO: add a test for this case (non-privileged user etc) */

			/* Only SU can read this GUC */
			spl_value = GetConfigOptionByName("shared_preload_libraries", NULL, false);

			/* Restore user's privileges */
			if (need_priv_escalation)
				SetUserIdAndSecContext(save_userid, save_sec_context);

			ereport(ERROR,
					(errmsg("extension conflict has been detected"),
					 errdetail("shared_preload_libraries = \"%s\"", spl_value),
					 errhint("autopart should be the last extension listed in "
							 "\"shared_preload_libraries\" GUC in order to "
							 "prevent possible conflicts with other extensions")));
		}

		return;
	}
}

/*
 * Initialize dsm_config & shmem_config.
 */
void
autopart_shmem_startup_hook(void)
{
	/* Invoke original hook if needed */
	if (autopart_shmem_startup_hook_next)
		autopart_shmem_startup_hook_next();

}

/*
 * Invalidate PartRelationInfo cache entry if needed.
 */
void
autopart_relcache_hook(Datum arg, Oid relid)
{
	Oid autopart_config_relid;

	/* See cook_partitioning_expression() */
	if (!autopart_hooks_enabled)
		return;

	if (!IsAutopartReady())
		return;

	/* Invalidation event for whole cache */
	if (relid == InvalidOid)
	{
		invalidate_bounds_cache();
		invalidate_parents_cache();
		invalidate_status_cache();
		delay_autopart_shutdown();  /* see below */
	}

	/*
	 * Invalidation event for AUTOPART_CONFIG table (probably DROP EXTENSION).
	 * Digging catalogs here is expensive and probably illegal, so we take
	 * cached relid. It is possible that we don't know it atm (e.g. autopart
	 * was disabled). However, in this case caches must have been cleaned
	 * on disable, and there is no DROP-specific additional actions.
	 */
	autopart_config_relid = get_autopart_config_relid(true);
	if (relid == autopart_config_relid)
	{
		delay_autopart_shutdown();
	}

	/* Invalidation event for some user table */
	else if (relid >= FirstNormalObjectId)
	{
		/* Invalidate PartBoundInfo entry if needed */
		forget_bounds_of_rel(relid);

		/* Invalidate PartStatusInfo entry if needed */
		forget_status_of_relation(relid);

		/* Invalidate PartParentInfo entry if needed */
		forget_parent_of_partition(relid);
	}
}


/*
 * Coordinator-side handler for the "partition" NOTIFY sent by a QE that hit a
 * value with no matching RANGE partition.  Creates the partition relation
 * locally and returns the serialized set of assigned OIDs (see
 * coordinator_handle_partition_request()).  Returns NULL with *len < 0 for any
 * other channel so the dispatcher treats it as an ordinary (unhandled) notify.
 */
char *
autopart_post_parse_notify_hook(char *relname, char *extra, int *len)
{
	Oid		parent;
	int32	lower;
	int32	upper;

	/* Not our channel: signal "not handled". */
	if (relname == NULL || strcmp(relname, "partition") != 0)
	{
		*len = -1;
		return NULL;
	}

	/*
	 * Mark the notify as handled right away, so that even if the work below
	 * throws an error the dispatcher still sends an (error) response back to
	 * the waiting QE instead of leaving it blocked.
	 */
	*len = 0;

	CHECK_FOR_INTERRUPTS();

	if (extra == NULL ||
		sscanf(extra, "%u:%d:%d", &parent, &lower, &upper) != 3)
		elog(ERROR, "invalid partition request message: \"%s\"",
			 extra ? extra : "(null)");

	return coordinator_handle_partition_request(parent, lower, upper, len);
}

