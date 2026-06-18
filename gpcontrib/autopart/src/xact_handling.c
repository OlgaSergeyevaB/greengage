/* ------------------------------------------------------------------------
 *
 * xact_handling.c
 *		Transaction-specific locks and other functions
 *
 * Copyright (c) 2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#include "xact_handling.h"
#include "utils.h"

#include "postgres.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "utils/inval.h"


static inline void SetLocktagRelationOid(LOCKTAG *tag, Oid relid);
static inline bool do_we_hold_the_lock(Oid relid, LOCKMODE lockmode);


/*
 * Check whether our session already holds a lock >= ShareUpdateExclusive on
 * 'relid'.
 *
 * NB: unlike upstream autopart we do NOT short-circuit this on 9.6+ in favour
 * of lock groups: in GPDB the lock group only covers the coordinator, while the
 * partition-creating worker also has to take the lock on every segment (where
 * it is NOT a group member).  So if our own transaction holds a conflicting
 * lock on the parent (LOCK TABLE, or we created/altered it in this same
 * transaction), the worker would block on the segments forever; the caller must
 * create the partition inline instead.
 *
 * Locks are acquired on the coordinator first, so checking here (on the QD) is
 * sufficient for our own session.  We use LockHeldByMe (no acquisition) and
 * test every mode from ShareUpdateExclusive up, because holding a stronger mode
 * does not set the weaker mode's bit in our local lock table.
 */
bool
xact_bgw_conflicting_lock_exists(Oid relid)
{
	LOCKMODE	lockmode;
	LOCKTAG		tag;

	SetLocktagRelationOid(&tag, relid);

	for (lockmode = ShareUpdateExclusiveLock;
		 lockmode <= AccessExclusiveLock;
		 lockmode++)
	{
		if (LockHeldByMe(&tag, lockmode))
			return true;
	}

	return false;
}


/*
 * Check if 'stmt' is BEGIN/ROLLBACK/etc [TRANSACTION] statement.
 */
bool
xact_is_transaction_stmt(Node *stmt)
{
	if (!stmt)
		return false;

	if (IsA(stmt, TransactionStmt))
		return true;

	return false;
}

/*
 * Check if 'stmt' is SET ('name' | [TRANSACTION]) statement.
 */
bool
xact_is_set_stmt(Node *stmt, const char *name)
{
	/* Check that SET TRANSACTION is implemented via VariableSetStmt */
	Assert(VAR_SET_MULTI > 0);

	if (!stmt)
		return false;

	if (!IsA(stmt, VariableSetStmt))
		return false;

	if (!name)
		return true;
	else
	{
		char *set_name = ((VariableSetStmt *) stmt)->name;

		if (set_name && pg_strcasecmp(name, set_name) == 0)
			return true;
	}

	return false;
}

/*
 * Check if 'stmt' is ALTER EXTENSION autopart.
 */
bool
xact_is_alter_autopart_stmt(Node *stmt)
{
	if (!stmt)
		return false;

	if (!IsA(stmt, AlterExtensionStmt))
		return false;

	if (pg_strcasecmp(((AlterExtensionStmt *) stmt)->extname, "autopart") == 0)
		return true;

	return false;
}

/*
 * Do we hold the specified lock?
 */
#ifdef __GNUC__
__attribute__((unused))
#endif
static inline bool
do_we_hold_the_lock(Oid relid, LOCKMODE lockmode)
{
	LOCKTAG		tag;

	/* Create a tag for lock */
	SetLocktagRelationOid(&tag, relid);

	/* If lock is alredy held, release it one time (decrement) */
	switch (LockAcquire(&tag, lockmode, false, true))
	{
		case LOCKACQUIRE_ALREADY_HELD:
			LockRelease(&tag, lockmode, false);
			return true;

		case LOCKACQUIRE_OK:
			LockRelease(&tag, lockmode, false);
			return false;

		default:
			return false;
	}
}

/*
 * SetLocktagRelationOid
 *		Set up a locktag for a relation, given only relation OID
 */
static inline void
SetLocktagRelationOid(LOCKTAG *tag, Oid relid)
{
	Oid			dbid;

	if (IsSharedRelation(relid))
		dbid = InvalidOid;
	else
		dbid = MyDatabaseId;

	SET_LOCKTAG_RELATION(*tag, dbid, relid);
}
