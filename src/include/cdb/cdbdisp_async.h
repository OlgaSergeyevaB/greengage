/*-------------------------------------------------------------------------
 *
 * cdbdisp_async.h
 * routines for asynchronous implementation of dispatching commands
 * to the qExec processes.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 *
 *
 * IDENTIFICATION
 *	    src/include/cdb/cdbdisp_async.h
 *
 *-------------------------------------------------------------------------
 */
#include "cdb/cdbdisp.h"

#ifndef CDBDISP_ASYNC_H
#define CDBDISP_ASYNC_H

extern DispatcherInternalFuncs DispatcherAsyncFuncs;

/*
 * Hook invoked on the coordinator when a QE sends an unknown NOTIFY message
 * during dispatch result processing.  It is used by pg_pathman to implement
 * on-demand partition creation: the QE notifies the coordinator about a range
 * that lacks a partition, the coordinator creates the partition relation
 * locally (without dispatching), and returns the full set of assigned OIDs
 * (serialized List of OidAssignment) so the QE can recreate an identical
 * relation via AddPreassignedOids().
 *
 * The hook returns a palloc'd binary buffer and stores its length in *len.
 * If *len is set to a negative value, the notify is considered "not handled"
 * and no response is sent back to the QE (the message is just logged).
 */
typedef char *(*post_parse_notify_hook_type) (char *relname, char *extra, int *len);
extern PGDLLIMPORT post_parse_notify_hook_type post_parse_notify_hook;


#endif
