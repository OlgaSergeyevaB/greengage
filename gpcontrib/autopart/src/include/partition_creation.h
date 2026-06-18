/*-------------------------------------------------------------------------
 *
 * partition_creation.h
 *		Various functions for partition creation.
 *
 * Copyright (c) 2016, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#ifndef PARTITION_CREATION_H
#define PARTITION_CREATION_H


#include "relation_info.h"

#include "postgres.h"
#include "nodes/parsenodes.h"


/* ACL privilege for partition creation */
#define ACL_SPAWN_PARTITIONS	ACL_INSERT


/* On-demand RANGE partition creation distributed via the coordinator */
char *make_range_partition_name(Oid parent_relid, int32 lower, int32 upper);
Oid create_partition_relation_local(Oid parent_relid, const char *partition_name);
char *coordinator_handle_partition_request(Oid parent_relid, int32 lower,
										   int32 upper, int *len);
void remember_segment_partition(Oid parent_relid, int32 lower, int32 upper,
								Oid relid);
Oid lookup_created_partition(Oid parent_relid, int32 lower, int32 upper);
void attach_created_partitions(void);

/* autopart_worker.c: create a partition in a separate (worker) transaction */
bool request_partition_creation(Oid parent_relid, int32 lower, int32 upper);

/* autopart_worker.c: create+attach a partition in the CURRENT transaction */
void autopart_create_partition_now(Oid parent_relid, int32 lower, int32 upper);


/* Create one RANGE partition */
Oid create_single_range_partition_internal(Oid parent_relid,
										   const Bound *start_value,
										   const Bound *end_value,
										   Oid value_type,
										   RangeVar *partition_rv,
										   char *tablespace);

/* Create one HASH partition */
Oid create_single_hash_partition_internal(Oid parent_relid,
										  uint32 part_idx,
										  uint32 part_count,
										  RangeVar *partition_rv,
										  char *tablespace);


/* RANGE constraints */


bool check_range_available(Oid parent_relid,
						   const Bound *start_value,
						   const Bound *end_value,
						   Oid value_type,
						   bool raise_error);


/* HASH constraints */


/* Add & drop autopart's check constraint */
void drop_autopart_check_constraint(Oid relid);
void add_autopart_check_constraint(Oid relid, Constraint *constraint);
void attach_buffer_partition(Oid relid, Oid buffer);


/* Partitioning callback type */
typedef enum
{
	PT_INIT_CALLBACK = 0
} part_callback_type;

/* Args for partitioning 'init_callback' */
typedef struct
{
	part_callback_type	cb_type;
	Oid					callback;
	bool				callback_is_cached;

	PartType			parttype;

	Oid					parent_relid;
	Oid					partition_relid;

	union
	{
		struct
		{
			void   *none; /* nothing (struct should have at least 1 element) */
		}	hash_params;

		struct
		{
			Bound		start_value,
						end_value;
			Oid			value_type;
		}	range_params;

	}					params;
} init_callback_params;


#define MakeInitCallbackRangeParams(params_p, cb, parent, child, start, end, type) \
	do \
	{ \
		memset((void *) (params_p), 0, sizeof(init_callback_params)); \
		(params_p)->cb_type = PT_INIT_CALLBACK; \
		(params_p)->callback = (cb); \
		(params_p)->callback_is_cached = false; \
		(params_p)->parttype = PT_RANGE; \
		(params_p)->parent_relid = (parent); \
		(params_p)->partition_relid = (child); \
		(params_p)->params.range_params.start_value = (start); \
		(params_p)->params.range_params.end_value = (end); \
		(params_p)->params.range_params.value_type = (type); \
	} while (0)

#define MakeInitCallbackHashParams(params_p, cb, parent, child) \
	do \
	{ \
		memset((void *) (params_p), 0, sizeof(init_callback_params)); \
		(params_p)->cb_type = PT_INIT_CALLBACK; \
		(params_p)->callback = (cb); \
		(params_p)->callback_is_cached = false; \
		(params_p)->parttype = PT_HASH; \
		(params_p)->parent_relid = (parent); \
		(params_p)->partition_relid = (child); \
	} while (0)


#endif /* PARTITION_CREATION_H */
