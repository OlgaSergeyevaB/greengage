/* ------------------------------------------------------------------------
 *
 * hooks.h
 *		prototypes of rel_pathlist and join_pathlist hooks
 *
 * Copyright (c) 2016-2020, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#ifndef AUTOPART_HOOKS_H
#define AUTOPART_HOOKS_H


#include "postgres.h"
#include "executor/executor.h"
#include "optimizer/planner.h"
#include "optimizer/paths.h"
#include "parser/analyze.h"
#include "storage/ipc.h"
#include "tcop/utility.h"


char *autopart_post_parse_notify_hook(char *relname, char *extra, int *len);

extern planner_hook_type				autopart_planner_hook_next;
extern post_parse_analyze_hook_type		autopart_post_parse_analyze_hook_next;
extern shmem_startup_hook_type			autopart_shmem_startup_hook_next;


void autopart_enable_assign_hook(bool newval, void *extra);

PlannedStmt * autopart_planner_hook(Query *parse,
								   int cursorOptions,
								   ParamListInfo boundParams);

void autopart_post_parse_analyze_hook(ParseState *pstate,
									  Query *query);

void autopart_shmem_startup_hook(void);

void autopart_relcache_hook(Datum arg, Oid relid);

#endif /* AUTOPART_HOOKS_H */
