/* ------------------------------------------------------------------------
 *
 * autopart.c
 *		This module sets planner hooks, handles SELECT queries and produces
 *		paths for partitioned tables
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#include "compat/pg_compat.h"
#include "compat/rowmarks_fix.h"

#include "init.h"
#include "hooks.h"
#include "autopart.h"
#include "partition_filter.h"
#include "planner_tree_modification.h"

#include "postgres.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_collation.h"
#include "catalog/indexing.h"
#include "catalog/pg_type.h"
#include "catalog/pg_extension.h"
#include "commands/extension.h"
#include "foreign/fdwapi.h"
#include "miscadmin.h"
#include "optimizer/optimizer.h"
#include "optimizer/clauses.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/cost.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"
#include "cdb/cdbdisp_async.h"
#include "cdb/cdbdisp.h"


PG_MODULE_MAGIC;


Oid		autopart_config_relid		= InvalidOid,
		autopart_config_params_relid	= InvalidOid;


/* pg module functions */
void _PG_init(void);


/* Expression tree handlers */
static void handle_const(const Const *c,
						 const Oid collid,
						 const int strategy,
						 const WalkerContext *context,
						 WrapperNode *result);

static void handle_array(ArrayType *array,
						 const Oid collid,
						 const int strategy,
						 const bool use_or,
						 const WalkerContext *context,
						 WrapperNode *result);

static void handle_boolexpr(const BoolExpr *expr,
							const WalkerContext *context,
							WrapperNode *result);

static void handle_arrexpr(const ScalarArrayOpExpr *expr,
						   const WalkerContext *context,
						   WrapperNode *result);

static void handle_opexpr(const OpExpr *expr,
						  const WalkerContext *context,
						  WrapperNode *result);

static Datum array_find_min_max(Datum *values,
								bool *isnull,
								int length,
								Oid value_type,
								Oid collid,
								bool take_min,
								bool *result_null);


/* Can we transform this node into a Const? */
static bool
IsConstValue(Node *node, const WalkerContext *context)
{
	switch (nodeTag(node))
	{
		case T_Const:
			return true;

		case T_Param:
			return WcxtHasExprContext(context);

		case T_RowExpr:
			{
				RowExpr	   *row = (RowExpr *) node;
				ListCell   *lc;

				/* Can't do anything about RECORD of wrong type */
				if (row->row_typeid != context->prel->ev_type)
					return false;

				/* Check that args are const values */
				foreach (lc, row->args)
					if (!IsConstValue((Node *) lfirst(lc), context))
						return false;
			}
			return true;

		default:
			return false;
	}
}

/* Extract a Const from node that has been checked by IsConstValue() */
static Const *
ExtractConst(Node *node, const WalkerContext *context)
{
	ExprState	   *estate;
	ExprContext	   *econtext = context->econtext;

	Datum			value;
	bool			isnull;

	Oid				typid,
					collid;
	int				typmod;

	/* Fast path for Consts */
	if (IsA(node, Const))
		return (Const *) node;

	/* Just a paranoid check */
	Assert(IsConstValue(node, context));

	switch (nodeTag(node))
	{
		case T_Param:
			{
				Param *param = (Param *) node;

				typid	= param->paramtype;
				typmod	= param->paramtypmod;
				collid	= param->paramcollid;

				/* It must be provided */
				Assert(WcxtHasExprContext(context));
			}
			break;

		case T_RowExpr:
			{
				RowExpr *row = (RowExpr *) node;

				typid	= row->row_typeid;
				typmod	= -1;
				collid	= InvalidOid;

				/* If there's no context - create it! */
				if (!WcxtHasExprContext(context))
					econtext = CreateStandaloneExprContext();
			}
			break;

		default:
			elog(ERROR, "error in function " CppAsString(ExtractConst));
	}

	/* Evaluate expression */
	estate = ExecInitExpr((Expr *) node, NULL);
	value = ExecEvalExprCompat(estate, econtext, &isnull);

	/* Free temp econtext if needed */
	if (econtext && !WcxtHasExprContext(context))
		FreeExprContext(econtext, true);

	/* Finally return Const */
	return makeConst(typid, typmod, collid, get_typlen(typid),
					 value, isnull, get_typbyval(typid));
}

/*
 * Checks if expression is a KEY OP PARAM or PARAM OP KEY,
 * where KEY is partitioning expression and PARAM is whatever.
 *
 * Returns:
 *		operator's Oid if KEY is a partitioning expr,
 *		otherwise InvalidOid.
 */
static Oid
IsKeyOpParam(const OpExpr *expr,
			 const WalkerContext *context,
			 Node **param_ptr) /* ret value #1 */
{
	Node   *left = linitial(expr->args),
		   *right = lsecond(expr->args);

	/* Check number of arguments */
	if (list_length(expr->args) != 2)
		return InvalidOid;

	/* KEY OP PARAM */
	if (match_expr_to_operand(context->prel_expr, left))
	{
		*param_ptr = right;

		/* return the same operator */
		return expr->opno;
	}

	/* PARAM OP KEY */
	if (match_expr_to_operand(context->prel_expr, right))
	{
		*param_ptr = left;

		/* commute to (KEY OP PARAM) */
		return get_commutator(expr->opno);
	}

	return InvalidOid;
}

/* Selectivity estimator for common 'paramsel' */
static inline double
estimate_paramsel_using_prel(const PartRelationInfo *prel, int strategy)
{
	/* If it's "=", divide by partitions number */
	if (strategy == BTEqualStrategyNumber)
		return 1.0 / (double) PrelChildrenCount(prel);

	/* Default selectivity estimate for inequalities */
	else if (prel->parttype == PT_RANGE && strategy > 0)
		return DEFAULT_INEQ_SEL;

	/* Else there's not much to do */
	else return 1.0;
}


/*
 * -------------------
 *  General functions
 * -------------------
 */

/* Set initial values for all Postmaster's forks */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
	{
		elog(ERROR, "autopart module must be initialized by Postmaster. "
					"Put the following line to configuration file: "
					"shared_preload_libraries='autopart'");
	}

	/* Request additional shared resources */
	RequestAddinShmemSpace(estimate_autopart_shmem_size());

	/* Assign autopart's initial state */
	autopart_init_state.autopart_enable		= DEFAULT_AUTOPART_ENABLE;
	autopart_init_state.auto_partition			= DEFAULT_AUTOPART_AUTO;
	autopart_init_state.override_copy			= DEFAULT_AUTOPART_OVERRIDE_COPY;
	autopart_init_state.initialization_needed	= true; /* ofc it's needed! */

	/* Set basic hooks */
	autopart_shmem_startup_hook_next			= shmem_startup_hook;
	shmem_startup_hook						= autopart_shmem_startup_hook;
	autopart_post_parse_analyze_hook_next	= post_parse_analyze_hook;
	post_parse_analyze_hook					= autopart_post_parse_analyze_hook;
	autopart_planner_hook_next				= planner_hook;
	planner_hook							= autopart_planner_hook;
	post_parse_notify_hook					= autopart_post_parse_notify_hook;

	/* Initialize static data for all subsystems */
	init_main_autopart_toggles();
	init_relation_info_static_data();
	init_partition_filter_static_data();
}

/* Get cached AUTOPART_CONFIG relation Oid */
Oid
get_autopart_config_relid(bool invalid_is_ok)
{
	if (!IsAutopartInitialized())
	{
		if (invalid_is_ok)
			return InvalidOid;
		elog(ERROR, "autopart is not initialized yet");
	}

	/* Raise ERROR if Oid is invalid */
	if (!OidIsValid(autopart_config_relid) && !invalid_is_ok)
		elog(ERROR, "unexpected error in function "
			 CppAsString(get_autopart_config_relid));

	return autopart_config_relid;
}

/* Get cached AUTOPART_CONFIG_PARAMS relation Oid */
Oid
get_autopart_config_params_relid(bool invalid_is_ok)
{
	if (!IsAutopartInitialized())
	{
		if (invalid_is_ok)
			return InvalidOid;
		elog(ERROR, "autopart is not initialized yet");
	}

	/* Raise ERROR if Oid is invalid */
	if (!OidIsValid(autopart_config_relid) && !invalid_is_ok)
		elog(ERROR, "unexpected error in function "
			 CppAsString(get_autopart_config_params_relid));

	return autopart_config_params_relid;
}

/*
 * Return autopart schema's Oid or InvalidOid if that's not possible.
 */
Oid
get_autopart_schema(void)
{
	Oid				result;
	Relation		rel;
	SysScanDesc		scandesc;
	HeapTuple		tuple;
	ScanKeyData		entry[1];
	Oid				ext_oid;

	/* It's impossible to fetch autopart's schema now */
	if (!IsTransactionState())
		return InvalidOid;

	ext_oid = get_extension_oid("autopart", true);
	if (ext_oid == InvalidOid)
		return InvalidOid; /* exit if autopart does not exist */

	ScanKeyInit(&entry[0],
				Anum_pg_extension_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ext_oid));

	rel = heap_open_compat(ExtensionRelationId, AccessShareLock);
	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_extension) GETSTRUCT(tuple))->extnamespace;
	else
		result = InvalidOid;

	systable_endscan(scandesc);

	heap_close_compat(rel, AccessShareLock);

	return result;
}


/*
 * -------------------------
 *  RANGE partition pruning
 * -------------------------
 */

/* Given 'value' and 'ranges', return selected partitions list */
void
select_range_partitions(const Datum value,
						const Oid collid,
						FmgrInfo *cmp_func,
						const RangeEntry *ranges,
						const int nranges,
						const int strategy,
						WrapperNode *result) /* returned partitions */
{
	bool	lossy = false,
			miss_left,	/* 'value' is less than left bound */
			miss_right;	/* 'value' is greater that right bound */

	int		startidx = 0,
			endidx = nranges - 1,
			cmp_min,
			cmp_max,
			i = 0;

	Bound	value_bound = MakeBound(value); /* convert value to Bound */

#ifdef USE_ASSERT_CHECKING
	int		counter = 0;
#endif

	/* Initial value (no missing partitions found) */
	result->found_gap = false;

	/* Check 'ranges' array */
	if (nranges == 0)
	{
		result->rangeset = NIL;
		return;
	}

	/* Check corner cases */
	else
	{
		Assert(ranges);
		Assert(cmp_func);

		/* Compare 'value' to absolute MIN and MAX bounds */
		cmp_min = cmp_bounds(cmp_func, collid, &value_bound, &ranges[startidx].min);
		cmp_max = cmp_bounds(cmp_func, collid, &value_bound, &ranges[endidx].max);

		if ((cmp_min <= 0 &&  strategy == BTLessStrategyNumber) ||
			(cmp_min <  0 && (strategy == BTLessEqualStrategyNumber ||
							  strategy == BTEqualStrategyNumber)))
		{
			result->rangeset = NIL;
			return;
		}

		if (cmp_max >= 0 && (strategy == BTGreaterEqualStrategyNumber ||
							 strategy == BTGreaterStrategyNumber ||
							 strategy == BTEqualStrategyNumber))
		{
			result->rangeset = NIL;
			return;
		}

		if ((cmp_min <  0 && strategy == BTGreaterStrategyNumber) ||
			(cmp_min <= 0 && strategy == BTGreaterEqualStrategyNumber))
		{
			result->rangeset = list_make1_irange(make_irange(startidx,
															 endidx,
															 IR_COMPLETE));
			return;
		}

		if (cmp_max >= 0 && (strategy == BTLessEqualStrategyNumber ||
							 strategy == BTLessStrategyNumber))
		{
			result->rangeset = list_make1_irange(make_irange(startidx,
															 endidx,
															 IR_COMPLETE));
			return;
		}
	}

	/* Binary search */
	while (true)
	{
		Assert(ranges);
		Assert(cmp_func);

		/* Calculate new pivot */
		i = startidx + (endidx - startidx) / 2;
		Assert(i >= 0 && i < nranges);

		/* Compare 'value' to current MIN and MAX bounds */
		cmp_min = cmp_bounds(cmp_func, collid, &value_bound, &ranges[i].min);
		cmp_max = cmp_bounds(cmp_func, collid, &value_bound, &ranges[i].max);

		/* How is 'value' located with respect to left & right bounds? */
		miss_left	= (cmp_min < 0 || (cmp_min == 0 && strategy == BTLessStrategyNumber));
		miss_right	= (cmp_max > 0 || (cmp_max == 0 && strategy != BTLessStrategyNumber));

		/* Searched value is inside of partition */
		if (!miss_left && !miss_right)
		{
			/* 'value' == 'min' and we want everything on the right */
			if (cmp_min == 0 && strategy == BTGreaterEqualStrategyNumber)
				lossy = false;
			/* 'value' == 'max' and we want everything on the left */
			else if (cmp_max == 0 && strategy == BTLessStrategyNumber)
				lossy = false;
			/* We're somewhere in the middle */
			else lossy = true;

			break; /* just exit loop */
		}

		/* Indices have met, looks like there's no partition */
		if (startidx >= endidx)
		{
			result->rangeset  = NIL;
			result->found_gap = true;

			/* Return if it's "key = value" */
			if (strategy == BTEqualStrategyNumber)
				return;

			/*
			 * Use current partition 'i' as a pivot that will be
			 * excluded by relation_excluded_by_constraints() if
			 * (lossy == true) & its WHERE clauses are trivial.
			 */
			if ((miss_left  && (strategy == BTLessStrategyNumber ||
								strategy == BTLessEqualStrategyNumber)) ||
				(miss_right && (strategy == BTGreaterStrategyNumber ||
								strategy == BTGreaterEqualStrategyNumber)))
				lossy = true;
			else
				lossy = false;

			break; /* just exit loop */
		}

		if (miss_left)
			endidx = i - 1;
		else if (miss_right)
			startidx = i + 1;

		/* For debug's sake */
		Assert(++counter < 100);
	}

	/* Filter partitions */
	switch(strategy)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
			if (lossy)
			{
				result->rangeset = list_make1_irange(make_irange(i, i, IR_LOSSY));
				if (i > 0)
					result->rangeset = lcons_irange(make_irange(0, i - 1, IR_COMPLETE),
													result->rangeset);
			}
			else
			{
				result->rangeset = list_make1_irange(make_irange(0, i, IR_COMPLETE));
			}
			break;

		case BTEqualStrategyNumber:
			result->rangeset = list_make1_irange(make_irange(i, i, IR_LOSSY));
			break;

		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			if (lossy)
			{
				result->rangeset = list_make1_irange(make_irange(i, i, IR_LOSSY));
				if (i < nranges - 1)
					result->rangeset = lappend_irange(result->rangeset,
													  make_irange(i + 1,
																  nranges - 1,
																  IR_COMPLETE));
			}
			else
			{
				result->rangeset = list_make1_irange(make_irange(i,
																 nranges - 1,
																 IR_COMPLETE));
			}
			break;

		default:
			elog(ERROR, "Unknown btree strategy (%u)", strategy);
			break;
	}
}


/*
 * ---------------------------------
 *  walk_expr_tree() implementation
 * ---------------------------------
 */

/* Examine expression in order to select partitions */
WrapperNode *
walk_expr_tree(Expr *expr, const WalkerContext *context)
{
	WrapperNode *result = (WrapperNode *) palloc0(sizeof(WrapperNode));

	switch (nodeTag(expr))
	{
		/* Useful for INSERT optimization */
		case T_Const:
			handle_const((Const *) expr, ((Const *) expr)->constcollid,
						 BTEqualStrategyNumber, context, result);
			return result;

		/* AND, OR, NOT expressions */
		case T_BoolExpr:
			handle_boolexpr((BoolExpr *) expr, context, result);
			return result;

		/* =, !=, <, > etc. */
		case T_OpExpr:
			handle_opexpr((OpExpr *) expr, context, result);
			return result;

		/* ANY, ALL, IN expressions */
		case T_ScalarArrayOpExpr:
			handle_arrexpr((ScalarArrayOpExpr *) expr, context, result);
			return result;

		default:
			result->orig = (const Node *) expr;
			result->args = NIL;

			result->rangeset = list_make1_irange_full(context->prel, IR_LOSSY);
			result->paramsel = 1.0;

			return result;
	}
}


/* Const handler */
static void
handle_const(const Const *c,
			 const Oid collid,
			 const int strategy,
			 const WalkerContext *context,
			 WrapperNode *result)		/* ret value #1 */
{
	const PartRelationInfo *prel = context->prel;

	/* Deal with missing strategy */
	if (strategy == 0)
		goto handle_const_return;

	/*
	 * Had to add this check for queries like:
	 *		select * from test.hash_rel where txt = NULL;
	 */
	if (c->constisnull)
	{
		result->rangeset = NIL;
		result->paramsel = 0.0;

		return; /* done, exit */
	}

	/*
	 * Had to add this check for queries like:
	 *		select * from test.hash_rel where true = false;
	 *		select * from test.hash_rel where false;
	 *		select * from test.hash_rel where $1;
	 */
	if (c->consttype == BOOLOID)
	{
		if (c->constvalue == BoolGetDatum(false))
		{
			result->rangeset = NIL;
			result->paramsel = 0.0;
		}
		else
		{
			result->rangeset = list_make1_irange_full(prel, IR_COMPLETE);
			result->paramsel = 1.0;
		}

		return; /* done, exit */
	}

	switch (prel->parttype)
	{
		case PT_HASH:
			{
				Datum	value,	/* value to be hashed */
						hash;	/* 32-bit hash */
				uint32	idx;	/* index of partition */
				bool	cast_success;

				/* Cannot do much about non-equal strategies */
				if (strategy != BTEqualStrategyNumber)
					goto handle_const_return;

				/* Peform type cast if types mismatch */
				if (prel->ev_type != c->consttype)
				{
					value = perform_type_cast(c->constvalue,
											  getBaseType(c->consttype),
											  getBaseType(prel->ev_type),
											  &cast_success);

					if (!cast_success)
						elog(ERROR, "Cannot select partition: "
									"unable to perform type cast");
				}
				/* Else use the Const's value */
				else value = c->constvalue;
				/*
				 * Calculate 32-bit hash of 'value' and corresponding index.
				 * Since 12, hashtext requires valid collation. Since we never
				 * supported this, passing db default one will do.
				 */
				hash = OidFunctionCall1Coll(prel->hash_proc,
											DEFAULT_COLLATION_OID,
											value);
				idx = hash_to_part_index(DatumGetInt32(hash),
										 PrelChildrenCount(prel));

				result->rangeset = list_make1_irange(make_irange(idx, idx, IR_LOSSY));
				result->paramsel = 1.0;

				return; /* done, exit */
			}

		case PT_RANGE:
			{
				FmgrInfo cmp_finfo;

				/* Cannot do much about non-equal strategies + diff. collations */
				if (strategy != BTEqualStrategyNumber && collid != prel->ev_collid)
				{
					goto handle_const_return;
				}

				fill_type_cmp_fmgr_info(&cmp_finfo,
										getBaseType(c->consttype),
										getBaseType(prel->ev_type));

				select_range_partitions(c->constvalue,
										collid,
										&cmp_finfo,
										PrelGetRangesArray(context->prel),
										PrelChildrenCount(context->prel),
										strategy,
										result); /* result->rangeset = ... */
				result->paramsel = 1.0;

				return; /* done, exit */
			}

		default:
			WrongPartType(prel->parttype);
	}

handle_const_return:
	result->rangeset = list_make1_irange_full(prel, IR_LOSSY);
	result->paramsel = 1.0;
}

/* Array handler */
static void
handle_array(ArrayType *array,
			 const Oid collid,
			 const int strategy,
			 const bool use_or,
			 const WalkerContext *context,
			 WrapperNode *result)		/* ret value #1 */
{
	const PartRelationInfo *prel = context->prel;

	/* Elements of the array */
	Datum	   *elem_values;
	bool	   *elem_isnull;
	int			elem_count;

	/* Element's properties */
	Oid			elem_type;
	int16		elem_len;
	bool		elem_byval;
	char		elem_align;

	/* Check if we can work with this strategy */
	if (strategy == 0)
		goto handle_array_return;

	/* Get element's properties */
	elem_type = ARR_ELEMTYPE(array);
	get_typlenbyvalalign(elem_type, &elem_len, &elem_byval, &elem_align);

	/* Extract values from the array */
	deconstruct_array(array, elem_type, elem_len, elem_byval, elem_align,
					  &elem_values, &elem_isnull, &elem_count);

	/* Handle non-null Const arrays */
	if (elem_count > 0)
	{
		List   *ranges;
		int		i;

		/* This is only for paranoia's sake */
		Assert(BTMaxStrategyNumber == 5 && BTEqualStrategyNumber == 3);

		/* Optimizations for <, <=, >=, > */
		if (strategy != BTEqualStrategyNumber)
		{
			bool	take_min;
			Datum	pivot;
			bool	pivot_null;

			/*
			 * OR:		Max for (< | <=); Min for (> | >=)
			 * AND:		Min for (< | <=); Max for (> | >=)
			 */
			take_min = strategy < BTEqualStrategyNumber ? !use_or : use_or;

			/* Extract Min (or Max) element */
			pivot = array_find_min_max(elem_values, elem_isnull,
									   elem_count, elem_type, collid,
									   take_min, &pivot_null);

			/* Write data and "shrink" the array */
			elem_values[0]	= pivot_null ? (Datum) 0 : pivot;
			elem_isnull[0]	= pivot_null;
			elem_count		= 1;

			/* If pivot is not NULL ... */
			if (!pivot_null)
			{
				/* ... append single NULL if array contains NULLs */
				if (array_contains_nulls(array))
				{
					/* Make sure that we have enough space for 2 elements */
					Assert(ArrayGetNItems(ARR_NDIM(array), ARR_DIMS(array)) >= 2);

					elem_values[1]	= (Datum) 0;
					elem_isnull[1]	= true;
					elem_count		= 2;
				}
				/* ... optimize clause ('orig') if array does not contain NULLs */
				else if (result->orig)
				{
					/* Should've been provided by the caller */
					ScalarArrayOpExpr *orig = (ScalarArrayOpExpr *) result->orig;

					/* Rebuild clause using 'pivot' */
					result->orig = (Node *)
						   make_opclause(orig->opno, BOOLOID, false,
										 (Expr *) linitial(orig->args),
										 (Expr *) makeConst(elem_type,
															-1,
															collid,
															elem_len,
															elem_values[0],
															elem_isnull[0],
															elem_byval),
										 InvalidOid,
										 collid);
				}
			}
		}

		/* Set default rangeset */
		ranges = use_or ? NIL : list_make1_irange_full(prel, IR_COMPLETE);

		/* Select partitions using values */
		for (i = 0; i < elem_count; i++)
		{
			Const			c;
			WrapperNode		wrap = InvalidWrapperNode;

			NodeSetTag(&c, T_Const);
			c.consttype		= elem_type;
			c.consttypmod	= -1;
			c.constcollid	= InvalidOid;
			c.constlen		= datumGetSize(elem_values[i],
										   elem_byval,
										   elem_len);
			c.constvalue	= elem_values[i];
			c.constisnull	= elem_isnull[i];
			c.constbyval	= elem_byval;
			c.location		= -1;

			handle_const(&c, collid, strategy, context, &wrap);

			/* Should we use OR | AND? */
			ranges = use_or ?
						irange_list_union(ranges, wrap.rangeset) :
						irange_list_intersection(ranges, wrap.rangeset);
		}

		/* Free resources */
		pfree(elem_values);
		pfree(elem_isnull);

		result->rangeset = ranges;
		result->paramsel = 1.0;

		return; /* done, exit */
	}

handle_array_return:
	result->rangeset = list_make1_irange_full(prel, IR_LOSSY);
	result->paramsel = 1.0;
}

/* Boolean expression handler */
static void
handle_boolexpr(const BoolExpr *expr,
				const WalkerContext *context,
				WrapperNode *result)	/* ret value #1 */
{
	const PartRelationInfo *prel = context->prel;
	List				   *ranges,
						   *args = NIL;
	double					paramsel = 1.0;
	ListCell			   *lc;

	/* Set default rangeset */
	ranges = (expr->boolop == AND_EXPR) ?
					list_make1_irange_full(prel, IR_COMPLETE) :
					NIL;

	/* Examine expressions */
	foreach (lc, expr->args)
	{
		WrapperNode *wrap;

		wrap = walk_expr_tree((Expr *) lfirst(lc), context);
		args = lappend(args, wrap);

		switch (expr->boolop)
		{
			case OR_EXPR:
				ranges = irange_list_union(ranges, wrap->rangeset);
				break;

			case AND_EXPR:
				ranges = irange_list_intersection(ranges, wrap->rangeset);
				paramsel *= wrap->paramsel;
				break;

			default:
				ranges = list_make1_irange_full(prel, IR_LOSSY);
				break;
		}
	}

	/* Adjust paramsel for OR */
	if (expr->boolop == OR_EXPR)
	{
		int totallen = irange_list_length(ranges);

		foreach (lc, args)
		{
			WrapperNode	   *arg = (WrapperNode *) lfirst(lc);
			int				len = irange_list_length(arg->rangeset);

			paramsel *= (1.0 - arg->paramsel * (double)len / (double)totallen);
		}

		paramsel = 1.0 - paramsel;
	}

	/* Save results */
	result->rangeset	= ranges;
	result->paramsel	= paramsel;
	result->orig		= (const Node *) expr;
	result->args		= args;
}

/* Scalar array expression handler */
static void
handle_arrexpr(const ScalarArrayOpExpr *expr,
			   const WalkerContext *context,
			   WrapperNode *result)		/* ret value #1 */
{
	Node					   *part_expr = (Node *) linitial(expr->args);
	Node					   *array = (Node *) lsecond(expr->args);
	const PartRelationInfo	   *prel = context->prel;
	TypeCacheEntry			   *tce;
	int							strategy;

	/* Small sanity check */
	Assert(list_length(expr->args) == 2);

	tce = lookup_type_cache(prel->ev_type, TYPECACHE_BTREE_OPFAMILY);
	strategy = get_op_opfamily_strategy(expr->opno, tce->btree_opf);

	/* Save expression */
	result->orig = (const Node *) expr;

	/* Check if expression tree is a partitioning expression */
	if (!match_expr_to_operand(context->prel_expr, part_expr))
		goto handle_arrexpr_all;

	/* Check if we can work with this strategy */
	if (strategy == 0)
		goto handle_arrexpr_all;

	/* Examine the array node */
	switch (nodeTag(array))
	{
		case T_Const:
			{
				Const	   *c = (Const *) array;

				/* Array is NULL */
				if (c->constisnull)
				{
					result->rangeset = NIL;
					result->paramsel = 0.0;

					return; /* done, exit */
				}

				/* Examine array */
				handle_array(DatumGetArrayTypeP(c->constvalue),
							 expr->inputcollid, strategy,
							 expr->useOr, context, result);

				return; /* done, exit */
			}

		case T_ArrayExpr:
			{
				ArrayExpr  *arr_expr = (ArrayExpr *) array;
				Oid			elem_type = arr_expr->element_typeid;
				int			array_params = 0;
				double		paramsel = 1.0;
				List	   *ranges;
				ListCell   *lc;

				if (list_length(arr_expr->elements) == 0)
					goto handle_arrexpr_all;

				/* Set default ranges for OR | AND */
				ranges = expr->useOr ? NIL : list_make1_irange_full(prel, IR_COMPLETE);

				/* Walk trough elements list */
				foreach (lc, arr_expr->elements)
				{
					Node		   *elem = lfirst(lc);
					WrapperNode		wrap = InvalidWrapperNode;

					/* Stop if ALL + quals evaluate to NIL */
					if (!expr->useOr && ranges == NIL)
						break;

					/* Is this a const value? */
					if (IsConstValue(elem, context))
					{
						Const *c = ExtractConst(elem, context);

						/* Is this an array?. */
						if (c->consttype != elem_type && !c->constisnull)
						{
							handle_array(DatumGetArrayTypeP(c->constvalue),
										 expr->inputcollid, strategy,
										 expr->useOr, context, &wrap);
						}
						/* ... or a single element? */
						else
						{
							handle_const(c, expr->inputcollid,
										 strategy, context, &wrap);
						}

						/* Should we use OR | AND? */
						ranges = expr->useOr ?
									irange_list_union(ranges, wrap.rangeset) :
									irange_list_intersection(ranges, wrap.rangeset);
					}
					else array_params++; /* we've just met non-const nodes */
				}

				/* Check for PARAM-related optimizations */
				if (array_params > 0)
				{
					double	sel = estimate_paramsel_using_prel(prel, strategy);
					int		i;

					if (expr->useOr)
					{
						/* We can't say anything if PARAMs + ANY */
						ranges = list_make1_irange_full(prel, IR_LOSSY);

						/* See handle_boolexpr() */
						for (i = 0; i < array_params; i++)
							paramsel *= (1 - sel);

						paramsel = 1 - paramsel;
					}
					else
					{
						/* Recheck condition on a narrowed set of partitions */
						ranges = irange_list_set_lossiness(ranges, IR_LOSSY);

						/* See handle_boolexpr() */
						for (i = 0; i < array_params; i++)
							paramsel *= sel;
					}
				}

				/* Save result */
				result->rangeset = ranges;
				result->paramsel = paramsel;

				return; /* done, exit */
			}

		default:
			break;
	}

handle_arrexpr_all:
	result->rangeset = list_make1_irange_full(prel, IR_LOSSY);
	result->paramsel = 1.0;
}

/* Operator expression handler */
static void
handle_opexpr(const OpExpr *expr,
			  const WalkerContext *context,
			  WrapperNode *result)		/* ret value #1 */
{
	Node					   *param;
	const PartRelationInfo	   *prel = context->prel;
	Oid							opid; /* operator's Oid */

	/* Save expression */
	result->orig = (const Node *) expr;

	/* Is it KEY OP PARAM or PARAM OP KEY? */
	if (OidIsValid(opid = IsKeyOpParam(expr, context, &param)))
	{
		TypeCacheEntry *tce;
		int				strategy;

		tce = lookup_type_cache(prel->ev_type, TYPECACHE_BTREE_OPFAMILY);
		strategy = get_op_opfamily_strategy(opid, tce->btree_opf);

		if (IsConstValue(param, context))
		{
			handle_const(ExtractConst(param, context),
						 expr->inputcollid,
						 strategy, context, result);

			return; /* done, exit */
		}
		/* TODO: estimate selectivity for param if it's Var */
		else if (IsA(param, Param) || IsA(param, Var))
		{
			result->rangeset = list_make1_irange_full(prel, IR_LOSSY);
			result->paramsel = estimate_paramsel_using_prel(prel, strategy);

			return; /* done, exit */
		}
	}

	result->rangeset = list_make1_irange_full(prel, IR_LOSSY);
	result->paramsel = 1.0;
}


/* Find Max or Min value of array */
static Datum
array_find_min_max(Datum *values,
				   bool *isnull,
				   int length,
				   Oid value_type,
				   Oid collid,
				   bool take_min,
				   bool *result_null) /* ret value #2 */
{
	TypeCacheEntry *tce = lookup_type_cache(value_type, TYPECACHE_CMP_PROC_FINFO);
	Datum		   *pivot = NULL;
	int				i;

	for (i = 0; i < length; i++)
	{
		if (isnull[i])
			continue;

		/* Update 'pivot' */
		if (pivot == NULL || (take_min ?
								check_lt(&tce->cmp_proc_finfo,
										 collid, values[i], *pivot) :
								check_gt(&tce->cmp_proc_finfo,
										 collid, values[i], *pivot)))
		{
			pivot = &values[i];
		}
	}

	/* Return results */
	*result_null = (pivot == NULL);
	return (pivot == NULL) ? (Datum) 0 : *pivot;
}


/*
 * translate_col_privs
 *	  Translate a bitmapset representing per-column privileges from the
 *	  parent rel's attribute numbering to the child's.
 *
 * The only surprise here is that we don't translate a parent whole-row
 * reference into a child whole-row reference.  That would mean requiring
 * permissions on all child columns, which is overly strict, since the
 * query is really only going to reference the inherited columns.  Instead
 * we set the per-column bits for all inherited columns.
 */
Bitmapset *
translate_col_privs(const Bitmapset *parent_privs,
					List *translated_vars)
{
	Bitmapset  *child_privs = NULL;
	bool		whole_row;
	int			attno;
	ListCell   *lc;

	/* System attributes have the same numbers in all tables */
	for (attno = FirstLowInvalidHeapAttributeNumber + 1; attno < 0; attno++)
	{
		if (bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
								 attno - FirstLowInvalidHeapAttributeNumber);
	}

	/* Check if parent has whole-row reference */
	whole_row = bms_is_member(InvalidAttrNumber - FirstLowInvalidHeapAttributeNumber,
							  parent_privs);

	/* And now translate the regular user attributes, using the vars list */
	attno = InvalidAttrNumber;
	foreach(lc, translated_vars)
	{
		Var *var = (Var *) lfirst(lc);

		attno++;
		if (var == NULL)		/* ignore dropped columns */
			continue;
		Assert(IsA(var, Var));
		if (whole_row ||
			bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  parent_privs))
			child_privs = bms_add_member(child_privs,
						 var->varattno - FirstLowInvalidHeapAttributeNumber);
	}

	return child_privs;
}


/*
 * make_inh_translation_list
 *	  Build the list of translations from parent Vars to child Vars for
 *	  an inheritance child.
 *
 * For paranoia's sake, we match type/collation as well as attribute name.
 */
void
make_inh_translation_list(Relation oldrelation, Relation newrelation,
						  Index newvarno, List **translated_vars)
{
	List	   *vars = NIL;
	TupleDesc	old_tupdesc = RelationGetDescr(oldrelation);
	TupleDesc	new_tupdesc = RelationGetDescr(newrelation);
	int			oldnatts = old_tupdesc->natts;
	int			newnatts = new_tupdesc->natts;
	int			old_attno;

	for (old_attno = 0; old_attno < oldnatts; old_attno++)
	{
		Form_pg_attribute att;
		char	   *attname;
		Oid			atttypid;
		int32		atttypmod;
		Oid			attcollation;
		int			new_attno;

		att = TupleDescAttr(old_tupdesc, old_attno);
		if (att->attisdropped)
		{
			/* Just put NULL into this list entry */
			vars = lappend(vars, NULL);
			continue;
		}
		attname = NameStr(att->attname);
		atttypid = att->atttypid;
		atttypmod = att->atttypmod;
		attcollation = att->attcollation;

		/*
		 * When we are generating the "translation list" for the parent table
		 * of an inheritance set, no need to search for matches.
		 */
		if (oldrelation == newrelation)
		{
			vars = lappend(vars, makeVar(newvarno,
										 (AttrNumber) (old_attno + 1),
										 atttypid,
										 atttypmod,
										 attcollation,
										 0));
			continue;
		}

		/*
		 * Otherwise we have to search for the matching column by name.
		 * There's no guarantee it'll have the same column position, because
		 * of cases like ALTER TABLE ADD COLUMN and multiple inheritance.
		 * However, in simple cases it will be the same column number, so try
		 * that before we go groveling through all the columns.
		 *
		 * Note: the test for (att = ...) != NULL cannot fail, it's just a
		 * notational device to include the assignment into the if-clause.
		 */
		if (old_attno < newnatts &&
			(att = TupleDescAttr(new_tupdesc, old_attno)) != NULL &&
			!att->attisdropped && //att->attinhcount != 0 &&
			strcmp(attname, NameStr(att->attname)) == 0)
			new_attno = old_attno;
		else
		{
			for (new_attno = 0; new_attno < newnatts; new_attno++)
			{
				att = TupleDescAttr(new_tupdesc, new_attno);

				/*
				 * Make clang analyzer happy:
				 *
				 * Access to field 'attisdropped' results
				 * in a dereference of a null pointer
				 */
				if (!att)
					elog(ERROR, "error in function "
								CppAsString(make_inh_translation_list));

				if (!att->attisdropped && att->attinhcount != 0 &&
					strcmp(attname, NameStr(att->attname)) == 0)
					break;
			}
			if (new_attno >= newnatts)
				elog(ERROR, "could not find inherited attribute \"%s\" of relation \"%s\"",
					 attname, RelationGetRelationName(newrelation));
		}

		/* Found it, check type and collation match */
		if (atttypid != att->atttypid || atttypmod != att->atttypmod)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's type",
				 attname, RelationGetRelationName(newrelation));
		if (attcollation != att->attcollation)
			elog(ERROR, "attribute \"%s\" of relation \"%s\" does not match parent's collation",
				 attname, RelationGetRelationName(newrelation));

		vars = lappend(vars, makeVar(newvarno,
									 (AttrNumber) (new_attno + 1),
									 atttypid,
									 atttypmod,
									 attcollation,
									 0));
	}

	*translated_vars = vars;
}
