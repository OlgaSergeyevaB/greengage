/* ------------------------------------------------------------------------
 *
 * utils.c
 *		definitions of various support functions
 *
 * Copyright (c) 2016, Postgres Professional
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */

#include "autopart.h"
#include "utils.h"

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/sysattr.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parse_oper.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "utils/regproc.h"

static const Node *
drop_irrelevant_expr_wrappers(const Node *expr)
{
	switch (nodeTag(expr))
	{
		/* Strip relabeling */
		case T_RelabelType:
			return (const Node *) ((const RelabelType *) expr)->arg;

		/* no special actions required */
		default:
			return expr;
	}
}

/*
 * Check if user can alter/drop specified relation. This function is used to
 * make sure that current user can change autopart's config. Returns true
 * if user can manage relation, false otherwise.
 *
 * XXX currently we just check if user is a table owner. Probably it's
 * better to check user permissions in order to let other users participate.
 */
bool
check_security_policy_internal(Oid relid, Oid role)
{
	Oid owner;

	/* Superuser is allowed to do anything */
	if (superuser())
		return true;

	/* Fetch the owner */
	owner = get_rel_owner(relid);

	/*
	 * Sometimes the relation doesn't exist anymore but there is still
	 * a record in config. For instance, it happens in DDL event trigger.
	 * Still we should be able to remove this record.
	 */
	if (owner == InvalidOid)
		return true;

	/* Check if current user is the owner of the relation */
	if (owner != role)
		return false;

	return true;
}

/* Compare clause operand with expression */
bool
match_expr_to_operand(const Node *expr, const Node *operand)
{
	expr = drop_irrelevant_expr_wrappers(expr);
	operand = drop_irrelevant_expr_wrappers(operand);

	/* compare expressions and return result right away */
	return equal(expr, operand);
}


/*
 * Get relation owner.
 */
Oid
get_rel_owner(Oid relid)
{
	HeapTuple	tp;
	Oid 		owner;

	tp = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (HeapTupleIsValid(tp))
	{
		Form_pg_class reltup = (Form_pg_class) GETSTRUCT(tp);

		owner = reltup->relowner;
		ReleaseSysCache(tp);

		return owner;
	}

	return InvalidOid;
}

/*
 * Try to get relname or at least relid as cstring.
 */
char *
get_rel_name_or_relid(Oid relid)
{
	char *relname = get_rel_name(relid);

	if (!relname)
		return DatumGetCString(DirectFunctionCall1(oidout, ObjectIdGetDatum(relid)));

	return relname;
}


/*
 * Get BTORDER_PROC for two types described by Oids.
 */
void
fill_type_cmp_fmgr_info(FmgrInfo *finfo, Oid type1, Oid type2)
{
	Oid				cmp_proc_oid;
	TypeCacheEntry *tce_1,
				   *tce_2;

	/* Check type compatibility */
	if (IsBinaryCoercible(type1, type2))
		type1 = type2;

	else if (IsBinaryCoercible(type2, type1))
		type2 = type1;

	tce_1 = lookup_type_cache(type1, TYPECACHE_BTREE_OPFAMILY);
	tce_2 = lookup_type_cache(type2, TYPECACHE_BTREE_OPFAMILY);

	/* Both types should belong to the same opfamily */
	if (tce_1->btree_opf != tce_2->btree_opf)
		goto fill_type_cmp_fmgr_info_error;

	cmp_proc_oid = get_opfamily_proc(tce_1->btree_opf,
									 tce_1->btree_opintype,
									 tce_2->btree_opintype,
									 BTORDER_PROC);

	/* No such function, emit ERROR */
	if (!OidIsValid(cmp_proc_oid))
		goto fill_type_cmp_fmgr_info_error;

	/* Fill FmgrInfo struct */
	fmgr_info(cmp_proc_oid, finfo);

	return; /* everything is OK */

/* Handle errors (no such function) */
fill_type_cmp_fmgr_info_error:
	elog(ERROR, "missing comparison function for types %s & %s",
		 format_type_be(type1), format_type_be(type2));
}


/*
 * Get CSTRING representation of Datum using the type Oid.
 */
char *
datum_to_cstring(Datum datum, Oid typid)
{
	char	   *result;
	HeapTuple	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));

	if (HeapTupleIsValid(tup))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tup);
		result = OidOutputFunctionCall(typtup->typoutput, datum);
		ReleaseSysCache(tup);
	}
	else
		result = pstrdup("[error]");

	return result;
}


/*
 * Try casting value of type 'in_type' to 'out_type'.
 *
 * This function might emit ERROR.
 */
Datum
perform_type_cast(Datum value, Oid in_type, Oid out_type, bool *success)
{
	CoercionPathType	ret;
	Oid					castfunc = InvalidOid;

	/* Speculative success */
	if (success) *success = true;

	/* Fast and trivial path */
	if (in_type == out_type)
		return value;

	/* Check that types are binary coercible */
	if (IsBinaryCoercible(in_type, out_type))
		return value;

	/* If not, try to perform a type cast */
	ret = find_coercion_pathway(out_type, in_type,
								COERCION_EXPLICIT,
								&castfunc);

	/* Handle coercion paths */
	switch (ret)
	{
		/* There's a function */
		case COERCION_PATH_FUNC:
			{
				/* Perform conversion */
				Assert(castfunc != InvalidOid);
				return OidFunctionCall1(castfunc, value);
			}

		/* Types are binary compatible (no implicit cast) */
		case COERCION_PATH_RELABELTYPE:
			{
				/* We don't perform any checks here */
				return value;
			}

		/* TODO: implement these casts if needed */
		case COERCION_PATH_ARRAYCOERCE:
		case COERCION_PATH_COERCEVIAIO:

		/* There's no cast available */
		case COERCION_PATH_NONE:
		default:
			{
				/* Oops, something is wrong */
				if (success)
					*success = false;
				else
					elog(ERROR, "cannot cast %s to %s",
						 format_type_be(in_type),
						 format_type_be(out_type));

				return (Datum) 0;
			}
	}
}

