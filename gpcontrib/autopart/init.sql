/* ------------------------------------------------------------------------
 *
 * init.sql
 *		Config tables and the single registration entry point
 *		(add_to_autopart_config) for autopart.
 *
 * Copyright (c) 2015-2020, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */


/*
 * Main config.
 *		partrel			- regclass (relation type, stored as Oid)
 *		expr			- partitioning expression (key)
 *		parttype		- partitioning type: (1 - HASH, 2 - RANGE)
 *		range_interval	- base interval for RANGE partitioning as string
 */
CREATE TABLE @extschema@.autopart_config (
	partrel			REGCLASS NOT NULL PRIMARY KEY,
	expr			TEXT NOT NULL,
	parttype		INTEGER NOT NULL,
	range_interval	TEXT DEFAULT NULL,

	/* check for allowed part types */
	CONSTRAINT autopart_config_parttype_check CHECK (parttype IN (1, 2))
)
/*
 * Config must be identical on the coordinator and every segment (catalog-like),
 * so that partition routing on QE can read it locally without a per-segment
 * registration workaround.
 */
DISTRIBUTED REPLICATED;


/*
 * Optional parameters for partitioned tables.
 *		partrel			- regclass (relation type, stored as Oid)
 *		enable_parent	- add parent table to plan
 *		auto			- enable automatic partition creation
 *		init_callback	- text signature of cb to be executed on partition creation
 *		spawn_using_bgw	- use background worker in order to auto create partitions
 */
CREATE TABLE @extschema@.autopart_config_params (
	partrel			REGCLASS NOT NULL PRIMARY KEY,
	enable_parent	BOOLEAN NOT NULL DEFAULT FALSE,
	auto			BOOLEAN NOT NULL DEFAULT TRUE,
	init_callback	TEXT DEFAULT NULL,
	spawn_using_bgw	BOOLEAN NOT NULL DEFAULT FALSE
)
/* identical on coordinator and every segment (see autopart_config above) */
DISTRIBUTED REPLICATED;

GRANT SELECT, INSERT, UPDATE, DELETE
ON @extschema@.autopart_config, @extschema@.autopart_config_params
TO public;

/*
 * Enable dump of config tables with pg_dump.
 */
SELECT pg_catalog.pg_extension_config_dump('@extschema@.autopart_config', '');
SELECT pg_catalog.pg_extension_config_dump('@extschema@.autopart_config_params', '');


/*
 * Register a previously created native RANGE-partitioned table with autopart.
 * This is the only user-facing entry point: it writes a row describing the
 * partitioning key and base interval into autopart_config (on the coordinator
 * and every segment).  Automatic RANGE partition creation on INSERT is then
 * driven entirely by the C executor machinery.
 */
CREATE FUNCTION @extschema@.add_to_autopart_config(
	parent_relid	REGCLASS,
	expression		TEXT,
	range_interval	TEXT)
RETURNS BOOLEAN AS 'autopart', 'add_to_autopart_config'
LANGUAGE C;
