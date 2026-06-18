/* ------------------------------------------------------------------------
 *
 * init.h
 *		Initialization functions
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#ifndef AUTOPART_INIT_H
#define AUTOPART_INIT_H


#include "relation_info.h"

#include "postgres.h"
#include "storage/lmgr.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/snapshot.h"


/* Help user in case of emergency */
#define INIT_ERROR_HINT "autopart will be disabled to allow you to resolve this issue"

/* Initial size of 'partitioned_rels' table */
#define PART_RELS_SIZE	10
#define CHILD_FACTOR	500


/*
 * autopart's initialization state structure.
 */
typedef struct
{
	bool 	autopart_enable;		/* GUC variable implementation */
	bool	auto_partition;			/* GUC variable for auto partition propagation */
	bool	override_copy;			/* override COPY TO/FROM */
	bool	initialization_needed;	/* do we need to perform init? */
} AutopartInitState;


/* Check that this is a temporary memory context that's going to be destroyed */
#define AssertTemporaryContext() \
	do { \
		Assert(CurrentMemoryContext != TopMemoryContext); \
		Assert(CurrentMemoryContext != TopAutopartContext); \
		Assert(CurrentMemoryContext != AutopartParentsCacheContext); \
		Assert(CurrentMemoryContext != AutopartStatusCacheContext); \
		Assert(CurrentMemoryContext != AutopartBoundsCacheContext); \
	} while (0)


#define AUTOPART_MCXT_COUNT	4
extern MemoryContext		TopAutopartContext;
extern MemoryContext		AutopartParentsCacheContext;
extern MemoryContext		AutopartStatusCacheContext;
extern MemoryContext		AutopartBoundsCacheContext;

extern HTAB				   *parents_cache;
extern HTAB				   *status_cache;
extern HTAB				   *bounds_cache;

/* autopart's initialization state */
extern AutopartInitState 	autopart_init_state;

/* autopart's hooks state */
extern bool					autopart_hooks_enabled;


#define AUTOPART_TOP_CONTEXT		"maintenance"
#define AUTOPART_PARENTS_CACHE	"partition parents cache"
#define AUTOPART_STATUS_CACHE	"partition status cache"
#define AUTOPART_BOUNDS_CACHE	"partition bounds cache"


/* Transform autopart's memory context into simple name */
static inline const char *
simplify_mcxt_name(MemoryContext mcxt)
{
	if (mcxt == TopAutopartContext)
		return AUTOPART_TOP_CONTEXT;

	else if (mcxt == AutopartParentsCacheContext)
		return AUTOPART_PARENTS_CACHE;

	else if (mcxt == AutopartStatusCacheContext)
		return AUTOPART_STATUS_CACHE;

	else if (mcxt == AutopartBoundsCacheContext)
		return AUTOPART_BOUNDS_CACHE;

	else elog(ERROR, "unknown memory context");

	return NULL;  /* keep compiler quiet */
}


/*
 * Check if autopart is initialized.
 */
#define IsAutopartInitialized()		( !autopart_init_state.initialization_needed )

/*
 * Check if autopart is enabled.
 */
#define IsAutopartEnabled()			( autopart_init_state.autopart_enable )

/*
 * Check if autopart is initialized & enabled.
 */
#define IsAutopartReady()			( IsAutopartInitialized() && IsAutopartEnabled() )

/*
 * Should we override COPY stmt handling?
 */
#define IsOverrideCopyEnabled()		( autopart_init_state.override_copy )

/*
 * Check if auto partition creation is enabled.
 */
#define IsAutoPartitionEnabled()	( autopart_init_state.auto_partition )

/*
 * Enable/disable auto partition propagation. Note that this only works if
 * partitioned relation supports this. See enable_auto() and disable_auto()
 * functions.
 */
#define SetAutoPartitionEnabled(value) \
	do { \
		Assert((value) == true || (value) == false); \
		autopart_init_state.auto_partition = (value); \
	} while (0)

/*
 * Emergency disable mechanism.
 */
#define DisableAutopart() \
	do { \
		autopart_init_state.autopart_enable		= false; \
		autopart_init_state.auto_partition			= false; \
		autopart_init_state.override_copy			= false; \
		unload_config(); \
	} while (0)


/* Default column values for AUTOPART_CONFIG_PARAMS */
#define DEFAULT_AUTOPART_ENABLE_PARENT		false
#define DEFAULT_AUTOPART_AUTO				true
#define DEFAULT_AUTOPART_INIT_CALLBACK		InvalidOid
#define DEFAULT_AUTOPART_SPAWN_USING_BGW		false

/* Other default values (for GUCs etc) */
#define DEFAULT_AUTOPART_ENABLE				true
#define DEFAULT_AUTOPART_OVERRIDE_COPY		true


/* Lowest version of Pl/PgSQL frontend compatible with internals */
#define LOWEST_COMPATIBLE_FRONT		"1.0.0"

/* Current version of native C library */
#define CURRENT_LIB_VERSION			"1.0.0"


void *autopart_cache_search_relid(HTAB *cache_table,
								 Oid relid,
								 HASHACTION action,
								 bool *found);

/*
 * Save and restore AutopartInitState.
 */
void save_autopart_init_state(AutopartInitState *temp_init_state);
void restore_autopart_init_state(const AutopartInitState *temp_init_state);

/*
 * Create main GUC variables.
 */
void init_main_autopart_toggles(void);

/*
 * Shared & local config.
 */
Size estimate_autopart_shmem_size(void);
bool load_config(void);
void unload_config(void);


/* Result of find_inheritance_children_array() */
typedef enum
{
	FCS_NO_CHILDREN = 0,	/* could not find any children (GOOD) */
	FCS_COULD_NOT_LOCK,		/* could not lock one of the children */
	FCS_FOUND				/* found some children (GOOD) */
} find_children_status;

find_children_status find_inheritance_children_array(Oid parentrelId,
													 LOCKMODE lockmode,
													 bool nowait,
													 uint32 *children_size,
													 Oid **children);

char *build_check_constraint_name_relid_internal(Oid relid);
char *build_check_constraint_name_relname_internal(const char *relname);

char *build_sequence_name_relid_internal(Oid relid);
char *build_sequence_name_relname_internal(const char *relname);

char *build_update_trigger_name_internal(Oid relid);
char *build_update_trigger_func_name_internal(Oid relid);

bool autopart_config_contains_relation(Oid relid,
									  Datum *values,
									  bool *isnull,
									  TransactionId *xmin,
									  ItemPointerData *iptr);

void autopart_config_invalidate_parsed_expression(Oid relid);

void autopart_config_refresh_parsed_expression(Oid relid,
											  Datum *values,
											  bool *isnull,
											  ItemPointer iptr);


bool read_autopart_params(Oid relid,
						 Datum *values,
						 bool *isnull);


bool validate_range_constraint(const Expr *expr,
							   const PartRelationInfo *prel,
							   Datum *lower, Datum *upper,
							   bool *lower_null, bool *upper_null);

bool validate_hash_constraint(const Expr *expr,
							  const PartRelationInfo *prel,
							  uint32 *part_idx);


#endif /* AUTOPART_INIT_H */
