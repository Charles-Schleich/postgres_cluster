#include <unistd.h>
#include "postgres.h"

#include "funcapi.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "pgstat.h"

#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "access/clog.h"

#include "catalog/catversion.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"

#include "executor/spi.h"

#include "libpq/pqformat.h"

#include "mb/pg_wchar.h"

#include "parser/parse_type.h"

#include "replication/logical.h"
#include "replication/origin.h"

#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/lwlock.h"
#include "storage/bufmgr.h"
#include "storage/proc.h"

#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"

#include "utils/array.h"
#include "utils/tqual.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "parser/parse_relation.h"

#include "multimaster.h"

typedef struct TupleData
{
	Datum		values[MaxTupleAttributeNumber];
	bool		isnull[MaxTupleAttributeNumber];
	bool		changed[MaxTupleAttributeNumber];
} TupleData;

static Relation read_rel(StringInfo s, LOCKMODE mode);
static void read_tuple_parts(StringInfo s, Relation rel, TupleData *tup);
static EState* create_rel_estate(Relation rel);
static bool find_pkey_tuple(ScanKey skey, Relation rel, Relation idxrel,
                            TupleTableSlot *slot, bool lock, LockTupleMode mode);
static void build_index_scan_keys(EState *estate, ScanKey *scan_keys, TupleData *tup);
static bool build_index_scan_key(ScanKey skey, Relation rel, Relation idxrel, TupleData *tup);
static void UserTableUpdateOpenIndexes(EState *estate, TupleTableSlot *slot);
static void UserTableUpdateIndexes(EState *estate, TupleTableSlot *slot);

static void process_remote_begin(StringInfo s);
static void process_remote_commit(StringInfo s);
static void process_remote_insert(StringInfo s, Relation rel);
static void process_remote_update(StringInfo s, Relation rel);
static void process_remote_delete(StringInfo s, Relation rel);

static int MtmReplicationNode;

/*
 * Search the index 'idxrel' for a tuple identified by 'skey' in 'rel'.
 *
 * If a matching tuple is found setup 'tid' to point to it and return true,
 * false is returned otherwise.
 */
bool
find_pkey_tuple(ScanKey skey, Relation rel, Relation idxrel,
				TupleTableSlot *slot, bool lock, LockTupleMode mode)
{
	HeapTuple	scantuple;
	bool		found;
	IndexScanDesc scan;
	SnapshotData snap;
	TransactionId xwait;

	InitDirtySnapshot(snap);
	scan = index_beginscan(rel, idxrel,
						   &snap,
						   RelationGetNumberOfAttributes(idxrel),
						   0);

retry:
	found = false;

	index_rescan(scan, skey, RelationGetNumberOfAttributes(idxrel), NULL, 0);

	if ((scantuple = index_getnext(scan, ForwardScanDirection)) != NULL)
	{
		found = true;
		/* FIXME: Improve TupleSlot to not require copying the whole tuple */
		ExecStoreTuple(scantuple, slot, InvalidBuffer, false);
		ExecMaterializeSlot(slot);

		xwait = TransactionIdIsValid(snap.xmin) ?
			snap.xmin : snap.xmax;

		if (TransactionIdIsValid(xwait))
		{
			XactLockTableWait(xwait, NULL, NULL, XLTW_None);
			goto retry;
		}
	}

	if (lock && found)
	{
		Buffer buf;
		HeapUpdateFailureData hufd;
		HTSU_Result res;
		HeapTupleData locktup;

		ItemPointerCopy(&slot->tts_tuple->t_self, &locktup.t_self);

		PushActiveSnapshot(GetLatestSnapshot());

		res = heap_lock_tuple(rel, &locktup, GetCurrentCommandId(false), mode,
							  false /* wait */,
							  false /* don't follow updates */,
							  &buf, &hufd);
		/* the tuple slot already has the buffer pinned */
		ReleaseBuffer(buf);

		PopActiveSnapshot();

		switch (res)
		{
			case HeapTupleMayBeUpdated:
				break;
			case HeapTupleUpdated:
				/* XXX: Improve handling here */
				ereport(LOG,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("concurrent update, retrying")));
				goto retry;
			default:
				elog(ERROR, "unexpected HTSU_Result after locking: %u", res);
				break;
		}
	}

	index_endscan(scan);

	return found;
}

static void
build_index_scan_keys(EState *estate, ScanKey *scan_keys, TupleData *tup)
{
	ResultRelInfo *relinfo;
	int i;

	relinfo = estate->es_result_relation_info;

	/* build scankeys for each index */
	for (i = 0; i < relinfo->ri_NumIndices; i++)
	{
		IndexInfo  *ii = relinfo->ri_IndexRelationInfo[i];

		/*
		 * Only unique indexes are of interest here, and we can't deal with
		 * expression indexes so far. FIXME: predicates should be handled
		 * better.
		 */
		if (!ii->ii_Unique || ii->ii_Expressions != NIL)
		{
			scan_keys[i] = NULL;
			continue;
		}

		scan_keys[i] = palloc(ii->ii_NumIndexAttrs * sizeof(ScanKeyData));

		/*
		 * Only return index if we could build a key without NULLs.
		 */
		if (build_index_scan_key(scan_keys[i],
								  relinfo->ri_RelationDesc,
								  relinfo->ri_IndexRelationDescs[i],
								  tup))
		{
			pfree(scan_keys[i]);
			scan_keys[i] = NULL;
			continue;
		}
	}
}

/*
 * Setup a ScanKey for a search in the relation 'rel' for a tuple 'key' that
 * is setup to match 'rel' (*NOT* idxrel!).
 *
 * Returns whether any column contains NULLs.
 */
static bool
build_index_scan_key(ScanKey skey, Relation rel, Relation idxrel, TupleData *tup)
{
	int			attoff;
	Datum		indclassDatum;
	Datum		indkeyDatum;
	bool		isnull;
	oidvector  *opclass;
	int2vector  *indkey;
	bool		hasnulls = false;

	indclassDatum = SysCacheGetAttr(INDEXRELID, idxrel->rd_indextuple,
									Anum_pg_index_indclass, &isnull);
	Assert(!isnull);
	opclass = (oidvector *) DatumGetPointer(indclassDatum);

	indkeyDatum = SysCacheGetAttr(INDEXRELID, idxrel->rd_indextuple,
									Anum_pg_index_indkey, &isnull);
	Assert(!isnull);
	indkey = (int2vector *) DatumGetPointer(indkeyDatum);


	for (attoff = 0; attoff < RelationGetNumberOfAttributes(idxrel); attoff++)
	{
		Oid			operator;
		Oid			opfamily;
		RegProcedure regop;
		int			pkattno = attoff + 1;
		int			mainattno = indkey->values[attoff];
		Oid			atttype = attnumTypeId(rel, mainattno);
		Oid			optype = get_opclass_input_type(opclass->values[attoff]);

		opfamily = get_opclass_family(opclass->values[attoff]);

		operator = get_opfamily_member(opfamily, optype,
									   optype,
									   BTEqualStrategyNumber);

		if (!OidIsValid(operator))
			elog(ERROR,
				 "could not lookup equality operator for type %u, optype %u in opfamily %u",
				 atttype, optype, opfamily);

		regop = get_opcode(operator);

		/* FIXME: convert type? */
		ScanKeyInit(&skey[attoff],
					pkattno,
					BTEqualStrategyNumber,
					regop,
					tup->values[mainattno - 1]);

		if (tup->isnull[mainattno - 1])
		{
			hasnulls = true;
			skey[attoff].sk_flags |= SK_ISNULL;
		}
	}
	return hasnulls;
}

static void
UserTableUpdateIndexes(EState *estate, TupleTableSlot *slot)
{
	/* HOT update does not require index inserts */
	if (HeapTupleIsHeapOnly(slot->tts_tuple))
		return;

	ExecOpenIndices(estate->es_result_relation_info, false);
	UserTableUpdateOpenIndexes(estate, slot);
	ExecCloseIndices(estate->es_result_relation_info);
}

static void
UserTableUpdateOpenIndexes(EState *estate, TupleTableSlot *slot)
{
	List	   *recheckIndexes = NIL;

	/* HOT update does not require index inserts */
	if (HeapTupleIsHeapOnly(slot->tts_tuple))
		return;

	if (estate->es_result_relation_info->ri_NumIndices > 0)
	{
		recheckIndexes = ExecInsertIndexTuples(slot,
											   &slot->tts_tuple->t_self,
											   estate, false, NULL, NIL);

		if (recheckIndexes != NIL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("bdr doesn't support index rechecks")));
	}

	/* FIXME: recheck the indexes */
	list_free(recheckIndexes);
}

static EState *
create_rel_estate(Relation rel)
{
	EState	   *estate;
	ResultRelInfo *resultRelInfo;

	estate = CreateExecutorState();

	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = rel;
	resultRelInfo->ri_TrigInstrument = NULL;

	estate->es_result_relations = resultRelInfo;
	estate->es_num_result_relations = 1;
	estate->es_result_relation_info = resultRelInfo;

	return estate;
}

static void
process_remote_begin(StringInfo s)
{
	GlobalTransactionId gtid;
	csn_t snapshot;

	gtid.node = pq_getmsgint(s, 4); 
	gtid.xid = pq_getmsgint(s, 4); 
	snapshot = pq_getmsgint64(s);    
    SetCurrentStatementStartTimestamp();     
	StartTransactionCommand();
    MtmJoinTransaction(&gtid, snapshot);

	MTM_TRACE("REMOTE begin node=%d xid=%d snapshot=%ld\n", gtid.node, gtid.xid, snapshot);
}

static void
read_tuple_parts(StringInfo s, Relation rel, TupleData *tup)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int			i;
	int			rnatts;
	char		action;

	action = pq_getmsgbyte(s);

	if (action != 'T')
		elog(ERROR, "expected TUPLE, got %c", action);

	memset(tup->isnull, 1, sizeof(tup->isnull));
	memset(tup->changed, 1, sizeof(tup->changed));

	rnatts = pq_getmsgint(s, 2);

	if (desc->natts != rnatts)
		elog(ERROR, "tuple natts mismatch, %u vs %u", desc->natts, rnatts);

	/* FIXME: unaligned data accesses */

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = desc->attrs[i];
		char		kind = pq_getmsgbyte(s);
		const char *data;
		int			len;

		switch (kind)
		{
			case 'n': /* null */
				/* already marked as null */
				tup->values[i] = 0xdeadbeef;
				break;
			case 'u': /* unchanged column */
				tup->isnull[i] = true;
				tup->changed[i] = false;
				tup->values[i] = 0xdeadbeef; /* make bad usage more obvious */
				break;

			case 'b': /* binary format */
				tup->isnull[i] = false;
				len = pq_getmsgint(s, 4); /* read length */

				data = pq_getmsgbytes(s, len);

				/* and data */
				if (att->attbyval)
					tup->values[i] = fetch_att(data, true, len);
				else
					tup->values[i] = PointerGetDatum(data);
				break;
			case 's': /* send/recv format */
				{
					Oid typreceive;
					Oid typioparam;
					StringInfoData buf;

					tup->isnull[i] = false;
					len = pq_getmsgint(s, 4); /* read length */

					getTypeBinaryInputInfo(att->atttypid,
										   &typreceive, &typioparam);

					/* create StringInfo pointing into the bigger buffer */
					initStringInfo(&buf);
					/* and data */
					buf.data = (char *) pq_getmsgbytes(s, len);
					buf.len = len;
					tup->values[i] = OidReceiveFunctionCall(
						typreceive, &buf, typioparam, att->atttypmod);

					if (buf.len != buf.cursor)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
								 errmsg("incorrect binary data format")));
					break;
				}
			case 't': /* text format */
				{
					Oid typinput;
					Oid typioparam;

					tup->isnull[i] = false;
					len = pq_getmsgint(s, 4); /* read length */

					getTypeInputInfo(att->atttypid, &typinput, &typioparam);
					/* and data */
					data = (char *) pq_getmsgbytes(s, len);
					tup->values[i] = OidInputFunctionCall(
						typinput, (char *) data, typioparam, att->atttypmod);
				}
				break;
			default:
				elog(ERROR, "unknown column type '%c'", kind);
		}

		if (att->attisdropped && !tup->isnull[i])
			elog(ERROR, "data for dropped column");
	}
}

static Relation 
read_rel(StringInfo s, LOCKMODE mode)
{
	int			relnamelen;
	int			nspnamelen;
	RangeVar*	rv;
	Oid			relid;

	rv = makeNode(RangeVar);

	nspnamelen = pq_getmsgbyte(s);
	rv->schemaname = (char *) pq_getmsgbytes(s, nspnamelen);

	relnamelen = pq_getmsgbyte(s);
	rv->relname = (char *) pq_getmsgbytes(s, relnamelen);

	relid = RangeVarGetRelidExtended(rv, mode, false, false, NULL, NULL);

	return heap_open(relid, NoLock);
}

static void
MtmBeginSession(void)
{
	char slot_name[MULTIMASTER_MAX_SLOT_NAME_SIZE];
	MtmLockNode(MtmReplicationNode);
	sprintf(slot_name, MULTIMASTER_SLOT_PATTERN, MtmReplicationNode);
	Assert(replorigin_session_origin == InvalidRepOriginId);
	replorigin_session_origin = replorigin_by_name(slot_name, false); 
	MTM_TRACE("%d: Begin setup replorigin session: %d\n", MyProcPid, replorigin_session_origin);
	replorigin_session_setup(replorigin_session_origin);
	MTM_TRACE("%d: End setup replorigin session: %d\n", MyProcPid, replorigin_session_origin);
}

static void 
MtmEndSession(void)
{
	if (replorigin_session_origin != InvalidRepOriginId) { 
		MTM_TRACE("%d: Begin reset replorigin session: %d\n", MyProcPid, replorigin_session_origin);
		replorigin_session_origin = InvalidRepOriginId;
		replorigin_session_reset();
		MtmUnlockNode(MtmReplicationNode);
		MTM_TRACE("%d: End reset replorigin session: %d\n", MyProcPid, replorigin_session_origin);
	}
}

static void
process_remote_commit(StringInfo in)
{
	uint8 		flags;
	csn_t       csn;
	const char *gid = NULL;	
	bool        caughtUp;

	/* read flags */
	flags = pq_getmsgbyte(in);
	MtmReplicationNode = pq_getmsgbyte(in);
	caughtUp = pq_getmsgbyte(in) != 0;

	/* read fields */
	replorigin_session_origin_lsn = pq_getmsgint64(in); /* commit_lsn */
	pq_getmsgint64(in); /* end_lsn */
	replorigin_session_origin_timestamp = pq_getmsgint64(in); /* commit_time */
	
	Assert(replorigin_session_origin == InvalidRepOriginId);

	switch(PGLOGICAL_XACT_EVENT(flags))
	{
		case PGLOGICAL_COMMIT:
		{
			MTM_TRACE("%d: PGLOGICAL_COMMIT commit\n", MyProcPid);
			if (IsTransactionState()) {
				Assert(TransactionIdIsValid(MtmGetCurrentTransactionId()));
				MtmBeginSession();
				CommitTransactionCommand();
			}
			break;
		}
		case PGLOGICAL_PREPARE:
		{
			Assert(IsTransactionState() && TransactionIdIsValid(MtmGetCurrentTransactionId()));
			gid = pq_getmsgstring(in);
			/* prepare TBLOCK_INPROGRESS state for PrepareTransactionBlock() */
			MTM_TRACE("%d: PGLOGICAL_PREPARE commit: gid=%s\n", MyProcPid, gid);
			BeginTransactionBlock();
			CommitTransactionCommand();
			StartTransactionCommand();
			
			MtmBeginSession();
			/* PREPARE itself */
			MtmSetCurrentTransactionGID(gid);
			PrepareTransactionBlock(gid);
			CommitTransactionCommand();
			break;
		}
		case PGLOGICAL_COMMIT_PREPARED:
		{
			Assert(!TransactionIdIsValid(MtmGetCurrentTransactionId()));
			csn = pq_getmsgint64(in); 
			gid = pq_getmsgstring(in);
			MTM_TRACE("%d: PGLOGICAL_COMMIT_PREPARED commit: csn=%ld, gid=%s\n", MyProcPid, csn, gid);
			StartTransactionCommand();
			MtmBeginSession();
			MtmSetCurrentTransactionCSN(csn);
			MtmSetCurrentTransactionGID(gid);
			FinishPreparedTransaction(gid, true);
			CommitTransactionCommand();
			break;
		}
		case PGLOGICAL_ABORT_PREPARED:
		{
			Assert(!TransactionIdIsValid(MtmGetCurrentTransactionId()));
			gid = pq_getmsgstring(in);
			MTM_TRACE("%d: PGLOGICAL_ABORT_PREPARED commit: gid=%s\n", MyProcPid, gid);
			if (MtmGetGlobalTransactionStatus(gid) != TRANSACTION_STATUS_ABORTED) { 
				StartTransactionCommand();
				MtmSetCurrentTransactionGID(gid);
				FinishPreparedTransaction(gid, false);
				CommitTransactionCommand();
			}
			break;
		}
		default:
			Assert(false);
	}
	MtmEndSession(true);
	if (caughtUp) {
		MtmRecoveryCompleted();
	}
}

static void
process_remote_insert(StringInfo s, Relation rel)
{
	EState *estate;
	TupleData new_tuple;
	TupleTableSlot *newslot;
	TupleTableSlot *oldslot;
	ResultRelInfo *relinfo;
	ScanKey	*index_keys;
	char* relname = RelationGetRelationName(rel);
	int	i;

	estate = create_rel_estate(rel);
	newslot = ExecInitExtraTupleSlot(estate);
	oldslot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(newslot, RelationGetDescr(rel));
	ExecSetSlotDescriptor(oldslot, RelationGetDescr(rel));

	read_tuple_parts(s, rel, &new_tuple);
	{
		HeapTuple tup;
		tup = heap_form_tuple(RelationGetDescr(rel),
							  new_tuple.values, new_tuple.isnull);
		ExecStoreTuple(tup, newslot, InvalidBuffer, true);
	}

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "unexpected relkind '%c' rel \"%s\"",
			 rel->rd_rel->relkind, RelationGetRelationName(rel));

	/* debug output */
#ifdef VERBOSE_INSERT
	log_tuple("INSERT:%s", RelationGetDescr(rel), newslot->tts_tuple);
#endif

	/*
	 * Search for conflicting tuples.
	 */
	ExecOpenIndices(estate->es_result_relation_info, false);
	relinfo = estate->es_result_relation_info;
	index_keys = palloc0(relinfo->ri_NumIndices * sizeof(ScanKeyData*));

	build_index_scan_keys(estate, index_keys, &new_tuple);

	/* do a SnapshotDirty search for conflicting tuples */
	for (i = 0; i < relinfo->ri_NumIndices; i++)
	{
		IndexInfo  *ii = relinfo->ri_IndexRelationInfo[i];
		bool found = false;

		/*
		 * Only unique indexes are of interest here, and we can't deal with
		 * expression indexes so far. FIXME: predicates should be handled
		 * better.
		 *
		 * NB: Needs to match expression in build_index_scan_key
		 */
		if (!ii->ii_Unique || ii->ii_Expressions != NIL)
			continue;

		if (index_keys[i] == NULL)
			continue;

		Assert(ii->ii_Expressions == NIL);

		/* if conflict: wait */
		found = find_pkey_tuple(index_keys[i],
								rel, relinfo->ri_IndexRelationDescs[i],
								oldslot, true, LockTupleExclusive);

		/* alert if there's more than one conflicting unique key */
		if (found)
		{
			/* TODO: Report tuple identity in log */
			ereport(ERROR,
                    (errcode(ERRCODE_UNIQUE_VIOLATION),
                     errmsg("Unique constraints violated by remotely INSERTed tuple"),
                     errdetail("Cannot apply transaction because remotely INSERTed tuple conflicts with a local tuple on UNIQUE constraint and/or PRIMARY KEY")));
		}
		CHECK_FOR_INTERRUPTS();
	}

	simple_heap_insert(rel, newslot->tts_tuple);
    UserTableUpdateOpenIndexes(estate, newslot);

	ExecCloseIndices(estate->es_result_relation_info);

    heap_close(rel, NoLock);
    ExecResetTupleTable(estate->es_tupleTable, true);
    FreeExecutorState(estate);

	CommandCounterIncrement();

	if (strcmp(relname, MULTIMASTER_DDL_TABLE) == 0) { 
		char* ddl = TextDatumGetCString(new_tuple.values[Anum_mtm_ddl_log_query-1]);
		int rc;
		SPI_connect();
		MTM_TRACE("%d: Execute utility statement %s\n", MyProcPid, ddl);
		rc = SPI_execute(ddl, false, 0);
        SPI_finish();
		if (rc != SPI_OK_UTILITY) { 
			elog(ERROR, "Failed to execute utility statement %s", ddl);
		}
	}

}

static void
process_remote_update(StringInfo s, Relation rel)
{
	char		action;
	EState	   *estate;
	TupleTableSlot *newslot;
	TupleTableSlot *oldslot;
	bool		pkey_sent;
	bool		found_tuple;
	TupleData   old_tuple;
	TupleData   new_tuple;
	Oid			idxoid;
	Relation	idxrel;
	ScanKeyData skey[INDEX_MAX_KEYS];
	HeapTuple	remote_tuple = NULL;

	action = pq_getmsgbyte(s);

	/* old key present, identifying key changed */
	if (action != 'K' && action != 'N')
		elog(ERROR, "expected action 'N' or 'K', got %c",
			 action);

	estate = create_rel_estate(rel);
	oldslot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(oldslot, RelationGetDescr(rel));
	newslot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(newslot, RelationGetDescr(rel));

	if (action == 'K')
	{
		pkey_sent = true;
		read_tuple_parts(s, rel, &old_tuple);
		action = pq_getmsgbyte(s);
	}
	else
		pkey_sent = false;

	/* check for new  tuple */
	if (action != 'N')
		elog(ERROR, "expected action 'N', got %c",
			 action);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "unexpected relkind '%c' rel \"%s\"",
			 rel->rd_rel->relkind, RelationGetRelationName(rel));

	/* read new tuple */
	read_tuple_parts(s, rel, &new_tuple);

	/* lookup index to build scankey */
	if (rel->rd_indexvalid == 0)
		RelationGetIndexList(rel);
	idxoid = rel->rd_replidindex;
	if (!OidIsValid(idxoid))
	{
		elog(ERROR, "could not find primary key for table with oid %u",
			 RelationGetRelid(rel));
		return;
	}

	/* open index, so we can build scan key for row */
	idxrel = index_open(idxoid, RowExclusiveLock);

	Assert(idxrel->rd_index->indisunique);

	/* Use columns from the new tuple if the key didn't change. */
	build_index_scan_key(skey, rel, idxrel,
						 pkey_sent ? &old_tuple : &new_tuple);

	PushActiveSnapshot(GetTransactionSnapshot());

	/* look for tuple identified by the (old) primary key */
	found_tuple = find_pkey_tuple(skey, rel, idxrel, oldslot, true,
						pkey_sent ? LockTupleExclusive : LockTupleNoKeyExclusive);

	if (found_tuple)
	{
		remote_tuple = heap_modify_tuple(oldslot->tts_tuple,
										 RelationGetDescr(rel),
										 new_tuple.values,
										 new_tuple.isnull,
										 new_tuple.changed);

		ExecStoreTuple(remote_tuple, newslot, InvalidBuffer, true);

#ifdef VERBOSE_UPDATE
		{
			StringInfoData o;
			initStringInfo(&o);
			tuple_to_stringinfo(&o, RelationGetDescr(rel), oldslot->tts_tuple);
			appendStringInfo(&o, " to");
			tuple_to_stringinfo(&o, RelationGetDescr(rel), remote_tuple);
			elog(DEBUG1, "UPDATE:%s", o.data);
			resetStringInfo(&o);
		}
#endif

        simple_heap_update(rel, &oldslot->tts_tuple->t_self, newslot->tts_tuple);
        UserTableUpdateIndexes(estate, newslot);
	}
	else
	{
        ereport(ERROR,
                (errcode(ERRCODE_NO_DATA_FOUND),
                 errmsg("Record with specified key can not be located at this node"),
                 errdetail("Most likely we have DELETE-UPDATE conflict")));

	}
    
	PopActiveSnapshot();
    
	/* release locks upon commit */
	index_close(idxrel, NoLock);
	heap_close(rel, NoLock);
    
	ExecResetTupleTable(estate->es_tupleTable, true);
	FreeExecutorState(estate);

	CommandCounterIncrement();
}

static void
process_remote_delete(StringInfo s, Relation rel)
{
	EState	   *estate;
	TupleData   oldtup;
	TupleTableSlot *oldslot;
	Oid			idxoid;
	Relation	idxrel;
	ScanKeyData skey[INDEX_MAX_KEYS];
	bool		found_old;

	estate = create_rel_estate(rel);
	oldslot = ExecInitExtraTupleSlot(estate);
	ExecSetSlotDescriptor(oldslot, RelationGetDescr(rel));

	read_tuple_parts(s, rel, &oldtup);

	/* lookup index to build scankey */
	if (rel->rd_indexvalid == 0)
		RelationGetIndexList(rel);
	idxoid = rel->rd_replidindex;
	if (!OidIsValid(idxoid))
	{
		elog(ERROR, "could not find primary key for table with oid %u",
			 RelationGetRelid(rel));
		return;
	}

	/* Now open the primary key index */
	idxrel = index_open(idxoid, RowExclusiveLock);

	if (rel->rd_rel->relkind != RELKIND_RELATION)
		elog(ERROR, "unexpected relkind '%c' rel \"%s\"",
			 rel->rd_rel->relkind, RelationGetRelationName(rel));

#ifdef VERBOSE_DELETE
	{
		HeapTuple tup;
		tup = heap_form_tuple(RelationGetDescr(rel),
							  oldtup.values, oldtup.isnull);
		ExecStoreTuple(tup, oldslot, InvalidBuffer, true);
	}
	log_tuple("DELETE old-key:%s", RelationGetDescr(rel), oldslot->tts_tuple);
#endif

	PushActiveSnapshot(GetTransactionSnapshot());

	build_index_scan_key(skey, rel, idxrel, &oldtup);

	/* try to find tuple via a (candidate|primary) key */
	found_old = find_pkey_tuple(skey, rel, idxrel, oldslot, true, LockTupleExclusive);

	if (found_old)
	{
		simple_heap_delete(rel, &oldslot->tts_tuple->t_self);
	}
	else
	{
        ereport(ERROR,
                (errcode(ERRCODE_NO_DATA_FOUND),
                 errmsg("Record with specified key can not be located at this node"),
                 errdetail("Most likely we have DELETE-DELETE conflict")));
	}

	PopActiveSnapshot();

	index_close(idxrel, NoLock);
	heap_close(rel, NoLock);

	ExecResetTupleTable(estate->es_tupleTable, true);
	FreeExecutorState(estate);

	CommandCounterIncrement();
}

static MemoryContext ApplyContext;

void MtmExecutor(int id, void* work, size_t size)
{
    StringInfoData s;
    Relation rel = NULL;
    s.data = work;
    s.len = size;
    s.maxlen = -1;
	s.cursor = 0;

    if (ApplyContext == NULL) {
        ApplyContext = AllocSetContextCreate(TopMemoryContext,
										   "MessageContext",
										   ALLOCSET_DEFAULT_MINSIZE,
										   ALLOCSET_DEFAULT_INITSIZE,
										   ALLOCSET_DEFAULT_MAXSIZE);
    }
    MemoryContextSwitchTo(ApplyContext);
	replorigin_session_origin = InvalidRepOriginId;
    PG_TRY();
    {    
        while (true) { 
            char action = pq_getmsgbyte(&s);
            MTM_TRACE("%d: REMOTE process actiob %c\n", MyProcPid, action);
            switch (action) {
                /* BEGIN */
            case 'B':
                process_remote_begin(&s);
                continue;
                /* COMMIT */
            case 'C':
                process_remote_commit(&s);
                break;
                /* INSERT */
            case 'I':
                process_remote_insert(&s, rel);
                continue;
                /* UPDATE */
            case 'U':
                process_remote_update(&s, rel);
                continue;
                /* DELETE */
            case 'D':
                process_remote_delete(&s, rel);
                continue;
            case 'R':
                rel = read_rel(&s, RowExclusiveLock);
                continue;
            default:
                elog(ERROR, "unknown action of type %c", action);
            }        
            break;
        }
    }
    PG_CATCH();
    {
		EmitErrorReport();
        FlushErrorState();
		MTM_INFO("%d: REMOTE begin abort transaction %d\n", MyProcPid, MtmGetCurrentTransactionId());
		MtmEndSession(false);
        AbortCurrentTransaction();
		MTM_INFO("%d: REMOTE end abort transaction %d\n", MyProcPid, MtmGetCurrentTransactionId());
    }
    PG_END_TRY();

    MemoryContextResetAndDeleteChildren(ApplyContext);
}
    
