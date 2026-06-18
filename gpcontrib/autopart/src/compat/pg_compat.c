/* ------------------------------------------------------------------------
 *
 * pg_compat.c
 *		Compatibility tools for PostgreSQL API
 *
 * Copyright (c) 2016, Postgres Professional
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */

#include "compat/pg_compat.h"

#include "utils.h"

#include "access/htup_details.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
#include "foreign/fdwapi.h"
#include "optimizer/clauses.h"
#include "optimizer/pathnode.h"
#include "optimizer/prep.h"
#include "parser/parse_utilcmd.h"
#include "port.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "optimizer/planner.h"

#include <math.h>


/*
 * ----------
 *  Variants
 * ----------
 */


/*
 * create_plain_partial_paths
 *	  Build partial access paths for parallel scan of a plain relation
 */
void
create_plain_partial_paths(PlannerInfo *root, RelOptInfo *rel)
{
	int			parallel_workers;

	/* no more than max_parallel_workers_per_gather since 11 */
	parallel_workers = compute_parallel_worker_compat(rel, rel->pages, -1);

	/* If any limit was set to zero, the user doesn't want a parallel scan. */
	if (parallel_workers <= 0)
		return;

	/* Add an unordered partial path based on a parallel sequential scan. */
	add_partial_path(rel, create_seqscan_path(root, rel, NULL, parallel_workers));
}


/*
 * get_all_actual_clauses
 */
List *
get_all_actual_clauses(List *restrictinfo_list)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, restrictinfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		Assert(IsA(rinfo, RestrictInfo));

		result = lappend(result, rinfo->clause);
	}
	return result;
}


/*
 * make_restrictinfos_from_actual_clauses
 */
#include "optimizer/optimizer.h"
#include "optimizer/restrictinfo.h"

List *
make_restrictinfos_from_actual_clauses(PlannerInfo *root,
									   List *clause_list)
{
	List	   *result = NIL;
	ListCell   *l;

	foreach(l, clause_list)
	{
		Expr	   *clause = (Expr *) lfirst(l);
		bool		pseudoconstant;
		RestrictInfo *rinfo;

		/*
		 * It's pseudoconstant if it contains no Vars and no volatile
		 * functions.  We probably can't see any sublinks here, so
		 * contain_var_clause() would likely be enough, but for safety use
		 * contain_vars_of_level() instead.
		 */
		pseudoconstant =
			!contain_vars_of_level((Node *) clause, 0) &&
			!contain_volatile_functions((Node *) clause);
		if (pseudoconstant)
		{
			/* tell createplan.c to check for gating quals */
			root->hasPseudoConstantQuals = true;
		}

		rinfo = make_restrictinfo(clause,
								  true,
								  false,
								  pseudoconstant,
								  root->qual_security_level,
								  NULL,
								  NULL,
								  NULL);
		result = lappend(result, rinfo);
	}
	return result;
}


/*
 * make_result
 *	  Build a Result plan node
 */
Result *
make_result(List *tlist,
			Node *resconstantqual,
			Plan *subplan)
{
	Result	   *node = makeNode(Result);
	Plan	   *plan = &node->plan;

	plan->targetlist = tlist;
	plan->qual = NIL;
	plan->lefttree = subplan;
	plan->righttree = NULL;
	node->resconstantqual = resconstantqual;

	return node;
}


/*
 * -------------
 *  Common code
 * -------------
 */

