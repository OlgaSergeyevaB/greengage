/* ------------------------------------------------------------------------
 *
 * planner_tree_modification.c
 *		Functions for query- and plan- tree modification
 *
 * Copyright (c) 2016-2020, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */

#include "compat/rowmarks_fix.h"

#include "partition_filter.h"
#include "planner_tree_modification.h"
#include "relation_info.h"
#include "rewrite/rewriteManip.h"

#include "access/table.h"
#include "access/htup_details.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "storage/lmgr.h"
#include "utils/syscache.h"


/*
 * Drop conflicting macros for the sake of TRANSFORM_CONTEXT_FIELD(...).
 * For instance, Windows.h contains a nasty "#define DELETE".
 */
#ifdef SELECT
#undef SELECT
#endif

#ifdef INSERT
#undef INSERT
#endif

#ifdef UPDATE
#undef UPDATE
#endif

#ifdef DELETE
#undef DELETE
#endif


/* for assign_rel_parenthood_status() */
#define PARENTHOOD_TAG CppAsString(PARENTHOOD)

/* Build transform_query_cxt field name */
#define TRANSFORM_CONTEXT_FIELD(command_type) \
	has_parent_##command_type##_query

/* Check that transform_query_cxt field is TRUE */
#define TRANSFORM_CONTEXT_HAS_PARENT(context, command_type) \
	( (context)->TRANSFORM_CONTEXT_FIELD(command_type) )

/* Used in switch(CmdType) statements */
#define TRANSFORM_CONTEXT_SWITCH_SET(context, command_type) \
	case CMD_##command_type: \
		(context)->TRANSFORM_CONTEXT_FIELD(command_type) = true; \
		break; \

#define TRANSFORM_CONTEXT_QUERY_IS_CTE_CTE(context, query) \
	( (context)->parent_cte && \
	  (context)->parent_cte->ctequery == (Node *) (query) )

#define TRANSFORM_CONTEXT_QUERY_IS_CTE_SL(context, query) \
	( (context)->parent_sublink && \
	  (context)->parent_sublink->subselect == (Node *) (query) && \
	  (context)->parent_sublink->subLinkType == CTE_SUBLINK )

/* Check if 'query' is CTE according to 'context' */
#define TRANSFORM_CONTEXT_QUERY_IS_CTE(context, query) \
	( TRANSFORM_CONTEXT_QUERY_IS_CTE_CTE((context), (query)) || \
	  TRANSFORM_CONTEXT_QUERY_IS_CTE_SL ((context), (query)) )

typedef struct
{
	/* Do we have a parent CmdType query? */
	bool				TRANSFORM_CONTEXT_FIELD(SELECT),
						TRANSFORM_CONTEXT_FIELD(INSERT),
						TRANSFORM_CONTEXT_FIELD(UPDATE),
						TRANSFORM_CONTEXT_FIELD(DELETE);

	/* Parameters for handle_modification_query() */
	ParamListInfo		query_params;

	/* SubLink that might contain an examined query */
	SubLink			   *parent_sublink;

	/* CommonTableExpr that might contain an examined query */
	CommonTableExpr	   *parent_cte;
} transform_query_cxt;

typedef struct
{
	Index	child_varno;
	Oid		parent_relid,
			parent_reltype,
			child_reltype;
	List   *translated_vars;
} adjust_appendrel_varnos_cxt;

static Plan *partition_filter_visitor(Plan *plan, void *context);

static void state_visit_subplans(List *plans, void (*visitor) (), void *context);
static void state_visit_members(PlanState **planstates, int nplans, void (*visitor) (), void *context);


/*
 * HACK: We have to mark each Query with a unique
 * id in order to recognize them properly.
 */
#define QUERY_ID_INITIAL 0
static uint32 latest_query_id = QUERY_ID_INITIAL;


void
assign_query_id(Query *query)
{
	uint32	prev_id = latest_query_id++;

	if (prev_id > latest_query_id)
		elog(WARNING, "assign_query_id(): queryId overflow");

	query->queryId = latest_query_id;
}


/*
 * Basic plan tree walker.
 *
 * 'visitor' is applied right before return.
 */
Plan *
plan_tree_visitor(Plan *plan,
				  Plan *(*visitor) (Plan *plan, void *context),
				  void *context)
{
	ListCell   *l;

	if (plan == NULL)
		return NULL;

	check_stack_depth();

	/* Plan-type-specific fixes */
	switch (nodeTag(plan))
	{
		case T_SubqueryScan:
			plan_tree_visitor(((SubqueryScan *) plan)->subplan, visitor, context);
			break;

		case T_CustomScan:
			foreach (l, ((CustomScan *) plan)->custom_plans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_ModifyTable:
			foreach (l, ((ModifyTable *) plan)->plans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_Append:
			foreach (l, ((Append *) plan)->appendplans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_MergeAppend:
			foreach (l, ((MergeAppend *) plan)->mergeplans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_BitmapAnd:
			foreach (l, ((BitmapAnd *) plan)->bitmapplans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		case T_BitmapOr:
			foreach (l, ((BitmapOr *) plan)->bitmapplans)
				plan_tree_visitor((Plan *) lfirst(l), visitor, context);
			break;

		default:
			break;
	}

	plan_tree_visitor(plan->lefttree, visitor, context);
	plan_tree_visitor(plan->righttree, visitor, context);

	/* Apply visitor to the current node */
	return visitor(plan, context);
}

void
state_tree_visitor(PlanState *state,
				   void (*visitor) (PlanState *plan, void *context),
				   void *context)
{
	Plan	   *plan;
	ListCell   *lc;

	if (state == NULL)
		return;

	plan = state->plan;

	check_stack_depth();

	/* Plan-type-specific fixes */
	switch (nodeTag(plan))
	{
		case T_SubqueryScan:
			state_tree_visitor(((SubqueryScanState *) state)->subplan, visitor, context);
			break;

		case T_CustomScan:
			foreach (lc, ((CustomScanState *) state)->custom_ps)
				state_tree_visitor((PlanState *) lfirst(lc), visitor, context);
			break;

		case T_ModifyTable:
			state_visit_members(((ModifyTableState *) state)->mt_plans,
								((ModifyTableState *) state)->mt_nplans,
								visitor, context);
			break;

		case T_Append:
			state_visit_members(((AppendState *) state)->appendplans,
								((AppendState *) state)->as_nplans,
								visitor, context);
			break;

		case T_MergeAppend:
			state_visit_members(((MergeAppendState *) state)->mergeplans,
								((MergeAppendState *) state)->ms_nplans,
								visitor, context);
			break;

		case T_BitmapAnd:
			state_visit_members(((BitmapAndState *) state)->bitmapplans,
								((BitmapAndState *) state)->nplans,
								visitor, context);
			break;

		case T_BitmapOr:
			state_visit_members(((BitmapOrState *) state)->bitmapplans,
								((BitmapOrState *) state)->nplans,
								visitor, context);
			break;

		default:
			break;
	}

	state_visit_subplans(state->initPlan, visitor, context);
	state_visit_subplans(state->subPlan, visitor, context);

	state_tree_visitor(state->lefttree, visitor, context);
	state_tree_visitor(state->righttree, visitor, context);

	/* Apply visitor to the current node */
	visitor(state, context);
}

/*
 * Walk a list of SubPlans (or initPlans, which also use SubPlan nodes).
 */
static void
state_visit_subplans(List *plans,
					 void (*visitor) (),
					 void *context)
{
	ListCell *lc;

	foreach (lc, plans)
	{
		SubPlanState *sps = lfirst_node(SubPlanState, lc);
		visitor(sps->planstate, context);
	}
}

/*
 * Walk the constituent plans of a ModifyTable, Append, MergeAppend,
 * BitmapAnd, or BitmapOr node.
 */
static void
state_visit_members(PlanState **planstates, int nplans,
					void (*visitor) (), void *context)
{
	int i;

	for (i = 0; i < nplans; i++)
		visitor(planstates[i], context);
}


/*
 * ----------------------------------------------------
 *  PartitionFilter and PartitionRouter -related stuff
 * ----------------------------------------------------
 */

/* Add PartitionFilter nodes to the plan tree */
Plan *
add_partition_filters(List *rtable, Plan *plan)
{
	if (autopart_enable_partition_filter)
		return plan_tree_visitor(plan, partition_filter_visitor, rtable);

	return NULL;
}

/*
 * Add PartitionFilters to ModifyTable node's children.
 *
 * 'context' should point to the PlannedStmt->rtable.
 */
static Plan *
partition_filter_visitor(Plan *plan, void *context)
{
	List		   *rtable = (List *) context;
	ModifyTable	   *modify_table = (ModifyTable *) plan;
	ListCell	   *lc1,
				   *lc2,
				   *lc3;

	/* Skip if not ModifyTable with 'INSERT' command */
	if (!IsA(modify_table, ModifyTable) || modify_table->operation != CMD_INSERT)
		return NULL;

	Assert(rtable && IsA(rtable, List));

	lc3 = list_head(modify_table->returningLists);
	forboth (lc1, modify_table->plans,
			 lc2, modify_table->resultRelations)
	{
		Index	rindex = lfirst_int(lc2);
		Oid		relid = getrelid(rindex, rtable);

		/* Check that table is partitioned */
		if (has_autopart_relation_info(relid))
		{
			List *returning_list = NIL;

			/* Extract returning list if possible */
			if (lc3)
			{
				returning_list = lfirst(lc3);
				lc3 = lnext_compat(modify_table->returningLists, lc3);
			}

			lfirst(lc1) = make_partition_filter((Plan *) lfirst(lc1), relid,
												modify_table->nominalRelation,
												modify_table->onConflictAction,
												modify_table->operation,
												returning_list);
		}
	}

	return NULL;
}


/*
 * -----------------------------------------------
 *  Parenthood safety checks (SELECT * FROM ONLY)
 * -----------------------------------------------
 */

#define RPS_STATUS_ASSIGNED		( (Index) 0x2 )
#define RPS_ENABLE_PARENT		( (Index) 0x1 )

/* Set parenthood status (per query level) */
void
assign_rel_parenthood_status(RangeTblEntry *rte,
							 rel_parenthood_status new_status)
{
	Assert(rte->rtekind != RTE_CTE);

	/* HACK: set relevant bits in RTE */
	rte->ctelevelsup |= RPS_STATUS_ASSIGNED;
	if (new_status == PARENTHOOD_ALLOWED)
		rte->ctelevelsup |= RPS_ENABLE_PARENT;
}


/*
 * -----------------------------------------------
 *  Count number of times we've visited planner()
 * -----------------------------------------------
 */

static int32 planner_calls = 0;

void
incr_planner_calls_count(void)
{
	Assert(planner_calls < PG_INT32_MAX);

	planner_calls++;
}

void
decr_planner_calls_count(void)
{
	Assert(planner_calls > 0);

	planner_calls--;
}

int32
get_planner_calls_count(void)
{
	return planner_calls;
}
