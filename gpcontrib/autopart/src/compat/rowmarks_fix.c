/* ------------------------------------------------------------------------
 *
 * rowmarks_fix.h
 *		Hack incorrect RowMark generation due to unset 'RTE->inh' flag
 *		NOTE: this code is only useful for vanilla
 *
 * Copyright (c) 2017, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#include "compat/rowmarks_fix.h"
#include "planner_tree_modification.h"

#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/planmain.h"
#include "utils/builtins.h"
#include "utils/rel.h"


