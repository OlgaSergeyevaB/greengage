/* ------------------------------------------------------------------------
 *
 * autopart.h
 *		structures and prototypes for autopart functions
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#ifndef AUTOPART_H
#define AUTOPART_H


#include "relation_info.h"
#include "rangeset.h"

#include "postgres.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "nodes/primnodes.h"
#include "nodes/execnodes.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"


/* Get CString representation of Datum (simple wrapper) */
#ifdef USE_ASSERT_CHECKING
	#include "utils.h"
	#define DebugPrintDatum(datum, typid) ( datum_to_cstring((datum), (typid)) )
#else
	#define DebugPrintDatum(datum, typid) ( "[use --enable-cassert]" )
#endif


/*
 * Main GUC variables.
 */
#define AUTOPART_ENABLE					"autopart.enable"
#define AUTOPART_ENABLE_AUTO_PARTITION	"autopart.enable_auto_partition"
#define AUTOPART_OVERRIDE_COPY			"autopart.override_copy"


/*
 * Definitions for the "autopart_config" table.
 */
#define AUTOPART_CONFIG						"autopart_config"
#define Natts_autopart_config				4
#define Anum_autopart_config_partrel			1	/* partitioned relation (regclass) */
#define Anum_autopart_config_expr			2	/* partition expression (original) */
#define Anum_autopart_config_parttype		3	/* partitioning type (1|2) */
#define Anum_autopart_config_range_interval	4	/* interval for RANGE pt. (text) */

/* type modifier (typmod) for 'range_interval' */
#define AUTOPART_CONFIG_interval_typmod		-1

/*
 * Definitions for the "autopart_config_params" table.
 */
#define AUTOPART_CONFIG_PARAMS						"autopart_config_params"
#define Natts_autopart_config_params					5
#define Anum_autopart_config_params_partrel			1	/* primary key */
#define Anum_autopart_config_params_enable_parent	2	/* include parent into plan */
#define Anum_autopart_config_params_auto				3	/* auto partitions creation */
#define Anum_autopart_config_params_init_callback	4	/* partition action callback */
#define Anum_autopart_config_params_spawn_using_bgw	5	/* should we use spawn BGW? */

/*
 * Definitions for the "autopart_partition_list" view.
 */
#define AUTOPART_PARTITION_LIST				"autopart_partition_list"
#define Natts_autopart_partition_list		6
#define Anum_autopart_pl_parent				1	/* partitioned relation (regclass) */
#define Anum_autopart_pl_partition			2	/* child partition (regclass) */
#define Anum_autopart_pl_parttype			3	/* partitioning type (1|2) */
#define Anum_autopart_pl_partattr			4	/* partitioned column (text) */
#define Anum_autopart_pl_range_min			5	/* partition's min value */
#define Anum_autopart_pl_range_max			6	/* partition's max value */

/*
 * Definitions for the "autopart_cache_stats" view.
 */
#define AUTOPART_CACHE_STATS					"autopart_cache_stats"
#define Natts_autopart_cache_stats			4
#define Anum_autopart_cs_context				1	/* name of memory context */
#define Anum_autopart_cs_size				2	/* size of memory context */
#define Anum_autopart_cs_used				3	/* used space */
#define Anum_autopart_cs_entries				4	/* number of cache entries */


/*
 * Cache current AUTOPART_CONFIG relid (set during load_config()).
 */
extern Oid	autopart_config_relid;
extern Oid	autopart_config_params_relid;

/*
 * Just to clarify our intentions (return the corresponding relid).
 */
Oid get_autopart_config_relid(bool invalid_is_ok);
Oid get_autopart_config_params_relid(bool invalid_is_ok);
Oid get_autopart_schema(void);


/*
 * Copied from PostgreSQL (prepunion.c)
 */
void make_inh_translation_list(Relation oldrelation, Relation newrelation,
							   Index newvarno, List **translated_vars);

Bitmapset *translate_col_privs(const Bitmapset *parent_privs,
							   List *translated_vars);


typedef struct
{
	const Node			   *orig;		/* examined expression */
	List				   *args;		/* clauses/wrappers extracted from 'orig' */
	List				   *rangeset;	/* IndexRanges representing selected parts */
	double					paramsel;	/* estimated selectivity of PARAMs
										   (for RuntimeAppend costs) */
	bool					found_gap;	/* were there any gaps? */
} WrapperNode;

#define InvalidWrapperNode	{ NULL, NIL, NIL, 0.0, false }

typedef struct
{
	Node				   *prel_expr;		/* expression from PartRelationInfo */
	const PartRelationInfo *prel;			/* main partitioning structure */
	ExprContext			   *econtext;		/* for ExecEvalExpr() */
} WalkerContext;

/* Usual initialization procedure for WalkerContext */
#define InitWalkerContext(context, expr, prel_info, ecxt) \
	do { \
		(context)->prel_expr = (expr); \
		(context)->prel = (prel_info); \
		(context)->econtext = (ecxt); \
	} while (0)

/* Check that WalkerContext contains ExprContext (plan execution stage) */
#define WcxtHasExprContext(wcxt) ( (wcxt)->econtext != NULL )

/* Examine expression in order to select partitions */
WrapperNode *walk_expr_tree(Expr *expr, const WalkerContext *context);


void select_range_partitions(const Datum value,
							 const Oid collid,
							 FmgrInfo *cmp_func,
							 const RangeEntry *ranges,
							 const int nranges,
							 const int strategy,
							 WrapperNode *result);


/* Convert hash value to the partition index */
static inline uint32
hash_to_part_index(uint32 value, uint32 partitions)
{
	return value % partitions;
}


/*
 * Compare two Datums using the given comarison function.
 *
 * flinfo is a pointer to FmgrInfo, arg1 & arg2 are Datums.
 */
#define check_lt(finfo, collid, arg1, arg2) \
	( DatumGetInt32(FunctionCall2Coll((finfo), (collid), (arg1), (arg2))) < 0 )

#define check_le(finfo, collid, arg1, arg2) \
	( DatumGetInt32(FunctionCall2Coll((finfo), (collid), (arg1), (arg2))) <= 0 )

#define check_eq(finfo, collid, arg1, arg2) \
	( DatumGetInt32(FunctionCall2Coll((finfo), (collid), (arg1), (arg2))) == 0 )

#define check_ge(finfo, collid, arg1, arg2) \
	( DatumGetInt32(FunctionCall2Coll((finfo), (collid), (arg1), (arg2))) >= 0 )

#define check_gt(finfo, collid, arg1, arg2) \
	( DatumGetInt32(FunctionCall2Coll((finfo), (collid), (arg1), (arg2))) > 0 )


#endif /* AUTOPART_H */
