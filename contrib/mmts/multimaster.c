/*
 * multimaster.c
 *
 * Multimaster based on logical replication
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "postgres.h"
#include "funcapi.h"
#include "fmgr.h"
#include "miscadmin.h"

#include "libpq-fe.h"
#include "common/username.h"

#include "postmaster/postmaster.h"
#include "postmaster/bgworker.h"
#include "storage/lwlock.h"
#include "storage/s_lock.h"
#include "storage/spin.h"
#include "storage/lmgr.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "access/xlogdefs.h"
#include "access/xact.h"
#include "access/xtm.h"
#include "access/transam.h"
#include "access/subtrans.h"
#include "access/commit_ts.h"
#include "access/xlog.h"
#include "storage/proc.h"
#include "executor/executor.h"
#include "access/twophase.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/tqual.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "commands/dbcommands.h"
#include "postmaster/autovacuum.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "replication/slot.h"
#include "replication/message.h"
#include "port/atomics.h"
#include "tcop/utility.h"
#include "nodes/makefuncs.h"
#include "access/htup_details.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "pglogical_output/hooks.h"
#include "parser/analyze.h"
#include "parser/parse_relation.h"

#include "multimaster.h"
#include "ddd.h"
#include "raftable_wrapper.h"
#include "raftable.h"
#include "worker.h"

typedef struct { 
    TransactionId xid;    /* local transaction ID   */
	GlobalTransactionId gtid; /* global transaction ID assigned by coordinator of transaction */
	bool  isReplicated;   /* transaction on replica */
	bool  isDistributed;  /* transaction performed INSERT/UPDATE/DELETE and has to be replicated to other nodes */
	bool  isPrepared;     /* transaction is perpared at first stage of 2PC */
    bool  isTransactionBlock; /* is transaction block */
	bool  containsDML;    /* transaction contains DML statements */
	XidStatus status;     /* transaction status */
    csn_t snapshot;       /* transaction snaphsot */
	csn_t csn;            /* CSN */
	char  gid[MULTIMASTER_MAX_GID_SIZE]; /* global transaction identifier (used by 2pc) */
} MtmCurrentTrans;

typedef struct {
	char gid[MULTIMASTER_MAX_GID_SIZE];
	bool abort;
	XidStatus status;
	MtmTransState* state;
} MtmTransMap;

/* #define USE_SPINLOCK 1 */

typedef enum 
{
	MTM_STATE_LOCK_ID
} MtmLockIds;

#define MTM_SHMEM_SIZE (64*1024*1024)
#define MTM_HASH_SIZE  100003
#define MTM_MAP_SIZE   1003
#define MIN_WAIT_TIMEOUT 1000
#define MAX_WAIT_TIMEOUT 100000
#define MAX_WAIT_LOOPS   100
#define STATUS_POLL_DELAY USECS_PER_SEC

void _PG_init(void);
void _PG_fini(void);

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(mtm_start_replication);
PG_FUNCTION_INFO_V1(mtm_stop_replication);
PG_FUNCTION_INFO_V1(mtm_drop_node);
PG_FUNCTION_INFO_V1(mtm_add_node);
PG_FUNCTION_INFO_V1(mtm_poll_node);
PG_FUNCTION_INFO_V1(mtm_recover_node);
PG_FUNCTION_INFO_V1(mtm_get_snapshot);
PG_FUNCTION_INFO_V1(mtm_get_csn);
PG_FUNCTION_INFO_V1(mtm_get_last_csn);
PG_FUNCTION_INFO_V1(mtm_get_nodes_state);
PG_FUNCTION_INFO_V1(mtm_get_cluster_state);
PG_FUNCTION_INFO_V1(mtm_get_cluster_info);
PG_FUNCTION_INFO_V1(mtm_make_table_local);
PG_FUNCTION_INFO_V1(mtm_dump_lock_graph);
PG_FUNCTION_INFO_V1(mtm_inject_2pc_error);

static Snapshot MtmGetSnapshot(Snapshot snapshot);
static void MtmInitialize(void);
static void MtmXactCallback(XactEvent event, void *arg);
static void MtmBeginTransaction(MtmCurrentTrans* x);
static void MtmPrePrepareTransaction(MtmCurrentTrans* x);
static void MtmPostPrepareTransaction(MtmCurrentTrans* x);
static void MtmAbortPreparedTransaction(MtmCurrentTrans* x);
static void MtmEndTransaction(MtmCurrentTrans* x, bool commit);
static bool MtmTwoPhaseCommit(MtmCurrentTrans* x);
static TransactionId MtmGetOldestXmin(Relation rel, bool ignoreVacuum);
static bool MtmXidInMVCCSnapshot(TransactionId xid, Snapshot snapshot);
static TransactionId MtmAdjustOldestXid(TransactionId xid);
static bool MtmDetectGlobalDeadLock(PGPROC* proc);
static void MtmAddSubtransactions(MtmTransState* ts, TransactionId* subxids, int nSubxids);
static char const* MtmGetName(void);
static size_t MtmGetTransactionStateSize(void);
static void MtmSerializeTransactionState(void* ctx);
static void MtmDeserializeTransactionState(void* ctx);
static void MtmInitializeSequence(int64* start, int64* step);

static void MtmCheckClusterLock(void);
static void MtmCheckSlots(void);
static void MtmAddSubtransactions(MtmTransState* ts, TransactionId *subxids, int nSubxids);

static void MtmShmemStartup(void);

static BgwPool* MtmPoolConstructor(void);
static bool MtmRunUtilityStmt(PGconn* conn, char const* sql, char **errmsg);
static void MtmBroadcastUtilityStmt(char const* sql, bool ignoreError);
static bool MtmProcessDDLCommand(char const* queryString);

MtmState* Mtm;

HTAB* MtmXid2State;
static HTAB* MtmGid2State;
static HTAB* MtmLocalTables;

static bool MtmIsRecoverySession;
static MtmConnectionInfo* MtmConnections;

static MtmCurrentTrans MtmTx;

static dlist_head MtmLsnMapping = DLIST_STATIC_INIT(MtmLsnMapping);

static TransactionManager MtmTM = { 
	PgTransactionIdGetStatus, 
	PgTransactionIdSetTreeStatus,
	MtmGetSnapshot, 
	PgGetNewTransactionId, 
	MtmGetOldestXmin, 
	PgTransactionIdIsInProgress, 
	PgGetGlobalTransactionId, 
	MtmXidInMVCCSnapshot, 
	MtmDetectGlobalDeadLock, 
	MtmGetName,
	MtmGetTransactionStateSize,
	MtmSerializeTransactionState,
	MtmDeserializeTransactionState,
	MtmInitializeSequence
};

char const* const MtmNodeStatusMnem[] = 
{ 
	"Initialization", 
	"Offline", 
	"Connected",
	"Online",
	"Recovery",
	"InMinor",
	"OutOfService"
};

bool  MtmDoReplication;
char* MtmDatabaseName;
char* MtmDatabaseUser;

int   MtmNodes;
int   MtmNodeId;
int   MtmReplicationNodeId;
int   MtmArbiterPort;
int   MtmRaftablePort;
int   MtmConnectTimeout;
int   MtmReconnectTimeout;
int   MtmNodeDisableDelay;
int   MtmTransSpillThreshold;
int   MtmMaxNodes;
int   MtmHeartbeatSendTimeout;
int   MtmHeartbeatRecvTimeout;
bool  MtmUseRaftable;
bool  MtmUseDtm;
bool  MtmVolksWagenMode;

static char* MtmConnStrs;
static int   MtmQueueSize;
static int   MtmWorkers;
static int   MtmVacuumDelay;
static int   MtmMinRecoveryLag;
static int   MtmMaxRecoveryLag;
static int   Mtm2PCPrepareRatio;
static int   Mtm2PCMinTimeout;
static int   MtmGcPeriod;
static bool  MtmIgnoreTablesWithoutPk;

static ExecutorFinish_hook_type PreviousExecutorFinishHook;
static ProcessUtility_hook_type PreviousProcessUtilityHook;
static shmem_startup_hook_type PreviousShmemStartupHook;


static void MtmExecutorFinish(QueryDesc *queryDesc);
static void MtmProcessUtility(Node *parsetree, const char *queryString,
							 ProcessUtilityContext context, ParamListInfo params,
							 DestReceiver *dest, char *completionTag);

/*
 * -------------------------------------------
 * Synchronize access to MTM structures.
 * Using LWLock seems to be  more efficient (at our benchmarks)
 * -------------------------------------------
 */
void MtmLock(LWLockMode mode)
{
#ifdef USE_SPINLOCK
	SpinLockAcquire(&Mtm->spinlock);
#else
	timestamp_t start, stop;
	start = MtmGetSystemTime();
	LWLockAcquire((LWLockId)&Mtm->locks[MTM_STATE_LOCK_ID], mode);
	stop = MtmGetSystemTime();
	if (stop > start + MSEC_TO_USEC(MtmHeartbeatSendTimeout)) { 
		MTM_LOG1("%d: obtaining %s lock takes %ld microseconds", MyProcPid, (mode == LW_EXCLUSIVE ? "exclusive" : "shared"), stop - start);
	}	
#endif
	Mtm->lastLockHolder = MyProcPid;
}

void MtmUnlock(void)
{
#ifdef USE_SPINLOCK
	SpinLockRelease(&Mtm->spinlock);
#else
	LWLockRelease((LWLockId)&Mtm->locks[MTM_STATE_LOCK_ID]);
#endif
	Mtm->lastLockHolder = 0;
}

void MtmLockNode(int nodeId)
{
	Assert(nodeId > 0 && nodeId <= Mtm->nAllNodes);
	LWLockAcquire((LWLockId)&Mtm->locks[nodeId], LW_EXCLUSIVE);
}

void MtmUnlockNode(int nodeId)
{
	Assert(nodeId > 0 && nodeId <= Mtm->nAllNodes);
	LWLockRelease((LWLockId)&Mtm->locks[nodeId]);	
}

/*
 * -------------------------------------------
 * System time manipulation functions
 * -------------------------------------------
 */


timestamp_t MtmGetSystemTime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (timestamp_t)tv.tv_sec*USECS_PER_SEC + tv.tv_usec;
}

timestamp_t MtmGetCurrentTime(void)
{
    return MtmGetSystemTime() + Mtm->timeShift;
}

void MtmSleep(timestamp_t interval)
{
    struct timespec ts;
    struct timespec rem;
    ts.tv_sec = interval/USECS_PER_SEC;
    ts.tv_nsec = interval%USECS_PER_SEC*1000;

    while (nanosleep(&ts, &rem) < 0) { 
        Assert(errno == EINTR);
        ts = rem;
    }
}
    
/** 
 * Return ascending unique timestamp which is used as CSN
 */
csn_t MtmAssignCSN()
{
    csn_t csn = MtmGetCurrentTime();
    if (csn <= Mtm->csn) { 
        csn = ++Mtm->csn;
    } else { 
        Mtm->csn = csn;
    }
    return csn;
}

/**
 * "Adjust" system clock if we receive message from future 
 */
csn_t MtmSyncClock(csn_t global_csn)
{
    csn_t local_csn;
    while ((local_csn = MtmAssignCSN()) < global_csn) { 
        Mtm->timeShift += global_csn - local_csn;
    }
    return local_csn;
}

/*
 * Distribute transaction manager functions
 */ 
static char const* MtmGetName(void)
{
	return MULTIMASTER_NAME;
}

static size_t 
MtmGetTransactionStateSize(void)
{
	return sizeof(MtmTx);
}

static void
MtmSerializeTransactionState(void* ctx)
{
	memcpy(ctx, &MtmTx, sizeof(MtmTx));
}

static void
MtmDeserializeTransactionState(void* ctx)
{
	memcpy(&MtmTx, ctx, sizeof(MtmTx));
}


static void
MtmInitializeSequence(int64* start, int64* step)
{
	if (MtmVolksWagenMode)
	{
		*start = 1;
		*step  = 1;
	}
	else
	{
		*start = MtmNodeId;
		*step  = MtmMaxNodes;
	}
}


/*
 * -------------------------------------------
 * Visibility&snapshots
 * -------------------------------------------
 */

csn_t MtmTransactionSnapshot(TransactionId xid)
{
	csn_t snapshot = INVALID_CSN;
	
	MtmLock(LW_SHARED);
	if (Mtm->status == MTM_ONLINE) {
		MtmTransState* ts = hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
		if (ts != NULL && !ts->isLocal) { 
			snapshot = ts->snapshot;
			Assert(ts->gtid.node == MtmNodeId || MtmIsRecoverySession); 		
		}
	}
	MtmUnlock();
    return snapshot;
}


Snapshot MtmGetSnapshot(Snapshot snapshot)
{
    snapshot = PgGetSnapshotData(snapshot);
	RecentGlobalDataXmin = RecentGlobalXmin = Mtm->oldestXid;
    return snapshot;
}


TransactionId MtmGetOldestXmin(Relation rel, bool ignoreVacuum)
{
    TransactionId xmin = PgGetOldestXmin(NULL, false); /* consider all backends */
	if (TransactionIdIsValid(xmin)) { 
		MtmLock(LW_EXCLUSIVE);
		xmin = MtmAdjustOldestXid(xmin);
		MtmUnlock();
	}
	return xmin;
}

bool MtmXidInMVCCSnapshot(TransactionId xid, Snapshot snapshot)
{	
#if TRACE_SLEEP_TIME
    static timestamp_t firstReportTime;
    static timestamp_t prevReportTime;
    static timestamp_t totalSleepTime;
    static timestamp_t maxSleepTime;
#endif
    timestamp_t delay = MIN_WAIT_TIMEOUT;
	int i;
    Assert(xid != InvalidTransactionId);
	
	if (!MtmUseDtm) { 
		return PgXidInMVCCSnapshot(xid, snapshot);
	}
	MtmLock(LW_SHARED);

#if TRACE_SLEEP_TIME
    if (firstReportTime == 0) {
        firstReportTime = MtmGetCurrentTime();
    }
#endif
    
	for (i = 0; i < MAX_WAIT_LOOPS; i++)
    {
        MtmTransState* ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
        if (ts != NULL/* && ts->status != TRANSACTION_STATUS_IN_PROGRESS*/)
        {
            if (ts->csn > MtmTx.snapshot) { 
                MTM_LOG4("%d: tuple with xid=%d(csn=%ld) is invisibile in snapshot %ld",
						 MyProcPid, xid, ts->csn, MtmTx.snapshot);
                MtmUnlock();
                return true;
            }
            if (ts->status == TRANSACTION_STATUS_UNKNOWN)
            {
                MTM_LOG3("%d: wait for in-doubt transaction %u in snapshot %lu", MyProcPid, xid, MtmTx.snapshot);
                MtmUnlock();
#if TRACE_SLEEP_TIME
                {
                timestamp_t delta, now = MtmGetCurrentTime();
#endif
                MtmSleep(delay);
#if TRACE_SLEEP_TIME
                delta = MtmGetCurrentTime() - now;
                totalSleepTime += delta;
                if (delta > maxSleepTime) {
                    maxSleepTime = delta;
                }
                if (now > prevReportTime + USECS_PER_SEC*10) { 
                    prevReportTime = now;
                    if (firstReportTime == 0) { 
                        firstReportTime = now;
                    } else { 
                        MTM_LOG3("Snapshot sleep %lu of %lu usec (%f%%), maximum=%lu", totalSleepTime, now - firstReportTime, totalSleepTime*100.0/(now - firstReportTime), maxSleepTime);
                    }
                }
                }
#endif
                if (delay*2 <= MAX_WAIT_TIMEOUT) {
                    delay *= 2;
                }
				MtmLock(LW_SHARED);
            }
            else
            {
                bool invisible = ts->status != TRANSACTION_STATUS_COMMITTED;
                MTM_LOG4("%d: tuple with xid=%d(csn= %ld) is %s in snapshot %ld",
						 MyProcPid, xid, ts->csn, invisible ? "rollbacked" : "committed", MtmTx.snapshot);
                MtmUnlock();
                return invisible;
            }
        }
        else
        {
            MTM_LOG4("%d: visibility check is skept for transaction %u in snapshot %lu", MyProcPid, xid, MtmTx.snapshot);
			MtmUnlock();
			return PgXidInMVCCSnapshot(xid, snapshot);
        }
    }
	MtmUnlock();
	elog(ERROR, "Failed to get status of XID %d", xid);
	return true;
}    



/*
 * There can be different oldest XIDs at different cluster node.
 * We collest oldest CSNs from all nodes and choose minimum from them.
 * If no such XID can be located, then return previously observed oldest XID
 */
static TransactionId 
MtmAdjustOldestXid(TransactionId xid)
{
	int i;   
	csn_t oldestSnapshot = INVALID_CSN;
	MtmTransState *prev = NULL;
	MtmTransState *ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
	MTM_LOG2("%d: MtmAdjustOldestXid(%d): snapshot=%ld, csn=%ld, status=%d", MyProcPid, xid, ts != NULL ? ts->snapshot : 0, ts != NULL ? ts->csn : 0, ts != NULL ? ts->status : -1);
	Mtm->gcCount = 0;

	if (ts != NULL) { 
		oldestSnapshot = ts->snapshot;
		Assert(oldestSnapshot != INVALID_CSN);
		if (Mtm->nodes[MtmNodeId-1].oldestSnapshot < oldestSnapshot) { 
			Mtm->nodes[MtmNodeId-1].oldestSnapshot = oldestSnapshot;
		} else {
			oldestSnapshot = Mtm->nodes[MtmNodeId-1].oldestSnapshot;
		}
		for (i = 0; i < Mtm->nAllNodes; i++) { 
			if (!BIT_CHECK(Mtm->disabledNodeMask, i)
				&& Mtm->nodes[i].oldestSnapshot < oldestSnapshot) 
			{ 
				oldestSnapshot = Mtm->nodes[i].oldestSnapshot;
			}
		}
		if (oldestSnapshot > MtmVacuumDelay*USECS_PER_SEC) { 
			oldestSnapshot -= MtmVacuumDelay*USECS_PER_SEC;
		} else { 
			oldestSnapshot = 0;
		}
		
		for (ts = Mtm->transListHead; 
			 ts != NULL 
				 && ts->csn < oldestSnapshot
				 && TransactionIdPrecedes(ts->xid, xid);
/*
				 && (ts->status == TRANSACTION_STATUS_COMMITTED ||
					 ts->status == TRANSACTION_STATUS_ABORTED);
*/
			 prev = ts, ts = ts->next) 
		{ 
			if (prev != NULL) { 
				/* Remove information about too old transactions */
				hash_search(MtmXid2State, &prev->xid, HASH_REMOVE, NULL);
			}
		}
	} 

	if (MtmUseDtm) 
	{ 
		if (prev != NULL) { 
			MTM_LOG1("%d: MtmAdjustOldestXid: oldestXid=%d, prev->xid=%d, prev->status=%d, prev->snapshot=%ld, ts->xid=%d, ts->status=%d, ts->snapshot=%ld, oldestSnapshot=%ld", 
					 MyProcPid, xid, prev->xid, prev->status, prev->snapshot, (ts ? ts->xid : 0), (ts ? ts->status : -1), (ts ? ts->snapshot : -1), oldestSnapshot);
			Mtm->transListHead = prev;
			Mtm->oldestXid = xid = prev->xid;            
		} else if (TransactionIdPrecedes(Mtm->oldestXid, xid)) {  
			xid = Mtm->oldestXid;
		}
	} else { 
		if (prev != NULL) { 
			Mtm->transListHead = prev;
		}
	}
    return xid;
}
/*
 * -------------------------------------------
 * Transaction list manipulation
 * -------------------------------------------
 */


static void MtmTransactionListAppend(MtmTransState* ts)
{
	if (!ts->isEnqueued) { 
		ts->isEnqueued = true;
		ts->next = NULL;
		ts->nSubxids = 0;
		*Mtm->transListTail = ts;
		Mtm->transListTail = &ts->next;
	}
}

static void MtmTransactionListInsertAfter(MtmTransState* after, MtmTransState* ts)
{
    ts->next = after->next;
    after->next = ts;
	ts->isEnqueued = true;
    if (Mtm->transListTail == &after->next) { 
        Mtm->transListTail = &ts->next;
    }
}

static void MtmAddSubtransactions(MtmTransState* ts, TransactionId* subxids, int nSubxids)
{
    int i;
	ts->nSubxids = nSubxids;
    for (i = 0; i < nSubxids; i++) { 
        bool found;
		MtmTransState* sts;
		Assert(TransactionIdIsValid(subxids[i]));
        sts = (MtmTransState*)hash_search(MtmXid2State, &subxids[i], HASH_ENTER, &found);
        Assert(!found);
        sts->status = ts->status;
        sts->csn = ts->csn;
		sts->votingCompleted = true;
        MtmTransactionListInsertAfter(ts, sts);
    }
}

void MtmAdjustSubtransactions(MtmTransState* ts)
{
	int i;
	int nSubxids = ts->nSubxids;
	MtmTransState* sts = ts;

    for (i = 0; i < nSubxids; i++) {
		sts = sts->next;
		sts->status = ts->status;
		sts->csn = ts->csn;
	}
}

/*
 * -------------------------------------------
 * Transaction control
 * -------------------------------------------
 */


static void
MtmXactCallback(XactEvent event, void *arg)
{
    switch (event) 
    {
	  case XACT_EVENT_START:
	    MtmBeginTransaction(&MtmTx);
        break;
	  case XACT_EVENT_PRE_PREPARE:
		MtmPrePrepareTransaction(&MtmTx);
		break;
	  case XACT_EVENT_POST_PREPARE:
		MtmPostPrepareTransaction(&MtmTx);
		break;
	  case XACT_EVENT_ABORT_PREPARED:
		MtmAbortPreparedTransaction(&MtmTx);
		break;
	  case XACT_EVENT_COMMIT:
		MtmEndTransaction(&MtmTx, true);
		break;
	  case XACT_EVENT_ABORT: 
		MtmEndTransaction(&MtmTx, false);
		break;
	  case XACT_EVENT_COMMIT_COMMAND:
		if (!MtmTx.isTransactionBlock) { 
			MtmTwoPhaseCommit(&MtmTx);
		}
		break;
	  default:
        break;
	}
}

/* 
 * Check if this is "normal" user trnsaction which shoudl be distributed to other nodes
 */
static bool
MtmIsUserTransaction()
{
	return !IsAutoVacuumLauncherProcess() && IsNormalProcessingMode() && MtmDoReplication && !am_walsender && !IsBackgroundWorker && !IsAutoVacuumWorkerProcess();
}

static void 
MtmResetTransaction(MtmCurrentTrans* x)
{
	x->snapshot = INVALID_CSN;
	x->xid = InvalidTransactionId;
	x->gtid.xid = InvalidTransactionId;
	x->isDistributed = false;
	x->isPrepared = false;
	x->isPrepared = false;
	x->status = TRANSACTION_STATUS_UNKNOWN;
}


static const char* const isoLevelStr[] = 
{
	"read uncommitted", 
	"read committed", 
	"repeatable read", 
	"serializable"
};

static void 
MtmBeginTransaction(MtmCurrentTrans* x)
{
    if (x->snapshot == INVALID_CSN) { 
		TransactionId xmin = (Mtm->gcCount >= MtmGcPeriod) ? PgGetOldestXmin(NULL, false) : InvalidTransactionId; /* Get oldest xmin outside critical section */

		MtmLock(LW_EXCLUSIVE);	
		if (TransactionIdIsValid(xmin) && Mtm->gcCount >= MtmGcPeriod) {
			MtmAdjustOldestXid(xmin);
		}
		x->xid = GetCurrentTransactionIdIfAny();
        x->isReplicated = false;
        x->isDistributed = MtmIsUserTransaction();
		x->isPrepared = false;
		x->isTransactionBlock = IsTransactionBlock();
		/* Application name can be changed usnig PGAPPNAME environment variable */
		if (x->isDistributed && Mtm->status != MTM_ONLINE && strcmp(application_name, MULTIMASTER_ADMIN) != 0) { 
			/* Reject all user's transactions at offline cluster. 
			 * Allow execution of transaction by bg-workers to make it possible to perform recovery.
			 */
			MtmUnlock();			
			elog(ERROR, "Multimaster node is not online: current status %s", MtmNodeStatusMnem[Mtm->status]);
		}
		if (x->isDistributed && XactIsoLevel != XACT_REPEATABLE_READ) { 
			elog(LOG, "Isolation level %s is not supported by multimaster", isoLevelStr[XactIsoLevel]);
		}
		x->containsDML = false;
        x->snapshot = MtmAssignCSN();	
		x->gtid.xid = InvalidTransactionId;
		x->gid[0] = '\0';
		x->status = TRANSACTION_STATUS_IN_PROGRESS;
		MtmUnlock();

        MTM_LOG3("%d: MtmLocalTransaction: %s transaction %u uses local snapshot %lu", 
				 MyProcPid, x->isDistributed ? "distributed" : "local", x->xid, x->snapshot);
    }
}


static MtmTransState* 
MtmCreateTransState(MtmCurrentTrans* x)
{
	bool found;
	MtmTransState* ts = hash_search(MtmXid2State, &x->xid, HASH_ENTER, &found);
	ts->status = TRANSACTION_STATUS_IN_PROGRESS;
	ts->snapshot = x->snapshot;
	ts->isLocal = true;
	if (!found) {
		ts->isEnqueued = false;
	}
	if (TransactionIdIsValid(x->gtid.xid)) { 		
		Assert(x->gtid.node != MtmNodeId);
		ts->gtid = x->gtid;
		strcpy(ts->gid, x->gid);
	} else { 
		/* I am coordinator of transaction */
		ts->gtid.xid = x->xid;
		ts->gtid.node = MtmNodeId;
		strcpy(ts->gid, x->gid);
	}
	return ts;
}

	
	
/* 
 * Prepare transaction for two-phase commit.
 * This code is executed by PRE_PREPARE hook before PREPARE message is sent to replicas by logical replication
 */
static void
MtmPrePrepareTransaction(MtmCurrentTrans* x)
{ 
	MtmTransState* ts;
	TransactionId* subxids;
	MTM_TXTRACE(x, "PrePrepareTransaction Start");

	if (!x->isDistributed) {
		return;
	}


	if (Mtm->inject2PCError == 1) { 
		Mtm->inject2PCError = 0;
		elog(ERROR, "ERROR INJECTION for transaction %d (%s)", x->xid, x->gid);
	}
	x->xid = GetCurrentTransactionId();
	Assert(TransactionIdIsValid(x->xid));

	if (Mtm->disabledNodeMask != 0) { 
		timestamp_t now = MtmGetSystemTime();		
		if (Mtm->lastClusterStatusUpdate + MSEC_TO_USEC(MtmHeartbeatRecvTimeout) < now) { 
			Mtm->lastClusterStatusUpdate = now;
			MtmRefreshClusterStatus(true);
		}
		if (!IsBackgroundWorker && Mtm->status != MTM_ONLINE) { 
			/* Do not take in account bg-workers which are performing recovery */
			elog(ERROR, "Abort current transaction because this cluster node is in %s status", MtmNodeStatusMnem[Mtm->status]);			
		}
	}

	MtmLock(LW_EXCLUSIVE);
	/*
	 * Check if there is global multimaster lock preventing new transaction from commit to make a chance to wal-senders to catch-up.
	 * Only "own" transactions are blacked. Transactions replicated from other nodes (including recovered transaction) should be proceeded
	 * and should not cause cluster status change.
	 */
	if (!x->isReplicated) { 
		MtmCheckClusterLock();
	}
	ts = MtmCreateTransState(x);
	/* 
	 * Invalid CSN prevent replication of transaction by logical replication 
	 */	   
	ts->isLocal = x->isReplicated || !x->containsDML;
	ts->snapshot = x->snapshot;
	ts->csn = MtmAssignCSN();	
	ts->procno = MyProc->pgprocno;
	ts->nVotes = 1; /* I am voted myself */
	ts->votingCompleted = false;
	ts->cmd = MSG_INVALID;
	ts->nSubxids = xactGetCommittedChildren(&subxids);
	Mtm->nActiveTransactions += 1;

	x->isPrepared = true;
	x->csn = ts->csn;
	
	Mtm->transCount += 1;
	Mtm->gcCount += 1;

	MtmTransactionListAppend(ts);
	MtmAddSubtransactions(ts, subxids, ts->nSubxids);
	MTM_LOG3("%d: MtmPrePrepareTransaction prepare commit of %d (gtid.xid=%d, gtid.node=%d, CSN=%ld)", 
			 MyProcPid, x->xid, ts->gtid.xid, ts->gtid.node, ts->csn);
	MtmUnlock();
	MTM_TXTRACE(x, "PrePrepareTransaction Finish");
}

/*
 * Check heartbeats
 */
bool MtmWatchdog(timestamp_t now)
{
	int i, n = Mtm->nAllNodes;
	bool allAlive = true;
	for (i = 0; i < n; i++) { 
		if (i+1 != MtmNodeId && !BIT_CHECK(Mtm->disabledNodeMask, i)) {
			if (Mtm->nodes[i].lastHeartbeat != 0
				&& now > Mtm->nodes[i].lastHeartbeat + MSEC_TO_USEC(MtmHeartbeatRecvTimeout)) 
			{ 
				elog(WARNING, "Heartbeat is not received from node %d during %d msec", 
					 i+1, (int)USEC_TO_MSEC(now - Mtm->nodes[i].lastHeartbeat));
				MtmOnNodeDisconnect(i+1);				
				allAlive = false;
			}
		}
	}
	return allAlive;
}


static void
MtmPostPrepareTransaction(MtmCurrentTrans* x)
{ 
	MtmTransState* ts;
	MTM_TXTRACE(x, "PostPrepareTransaction Start");

	if (!x->isDistributed) {
		return;
	}

	if (Mtm->inject2PCError == 2) { 
		Mtm->inject2PCError = 0;
		elog(ERROR, "ERROR INJECTION for transaction %d (%s)", x->xid, x->gid);
	}
	MtmLock(LW_EXCLUSIVE);
	ts = hash_search(MtmXid2State, &x->xid, HASH_FIND, NULL);
	Assert(ts != NULL);
	if (x->gid[0]) MTM_LOG1("Preparing transaction %d (%s) at %ld", x->xid, x->gid, MtmGetCurrentTime());
	if (!MtmIsCoordinator(ts) || Mtm->status == MTM_RECOVERY) {
		bool found;
		MtmTransMap* tm = (MtmTransMap*)hash_search(MtmGid2State, x->gid, HASH_ENTER, &found);
		Assert(x->gid[0]);
		tm->state = ts;	
		ts->votingCompleted = true;
		if (!found) { 
			ts->isEnqueued = false;
		}
		if (Mtm->status != MTM_RECOVERY) { 
			MtmSendNotificationMessage(ts, MSG_READY); /* send notification to coordinator */
			if (!MtmUseDtm) { 
				ts->status = TRANSACTION_STATUS_UNKNOWN;
			}
		} else {
			ts->status = TRANSACTION_STATUS_UNKNOWN;
		}
		MtmUnlock();
		MtmResetTransaction(x);
	} else { 
		time_t transTimeout = Max(MSEC_TO_USEC(Mtm2PCMinTimeout), (ts->csn - ts->snapshot)*Mtm2PCPrepareRatio/100); 
		int result = 0;
		int nConfigChanges = Mtm->nConfigChanges;

		timestamp_t start = MtmGetSystemTime();	
		/* Wait votes from all nodes until: */
		while (!ts->votingCompleted                           /* all nodes voted */
			   && nConfigChanges == Mtm->nConfigChanges       /* configarion is changed */
			   && Mtm->status == MTM_ONLINE                   /* node is not online */
			   && ts->status != TRANSACTION_STATUS_ABORTED    /* transaction is aborted */
			   && start + transTimeout >= MtmGetSystemTime()) /* timeout is expired */
		{
			MtmUnlock();
			MTM_TXTRACE(x, "PostPrepareTransaction WaitLatch Start");
			result = WaitLatch(&MyProc->procLatch, WL_LATCH_SET|WL_TIMEOUT, MtmHeartbeatRecvTimeout);
			MTM_TXTRACE(x, "PostPrepareTransaction WaitLatch Finish");
			if (result & WL_LATCH_SET) { 
				ResetLatch(&MyProc->procLatch);			
			} 
			MtmLock(LW_SHARED);
		}
		if (ts->status != TRANSACTION_STATUS_ABORTED && (!ts->votingCompleted || nConfigChanges != Mtm->nConfigChanges)) {  
			if (nConfigChanges != Mtm->nConfigChanges) {
				elog(WARNING, "Transaction %d is aborted because cluster configuration is changed during commit", x->xid);
			} else {
				elog(WARNING, "Transaction %d is aborted because of %d msec timeout expiration, prepare time %d msec", x->xid, (int)transTimeout, (int)USEC_TO_MSEC(ts->csn - x->snapshot));
			}
			MtmAbortTransaction(ts);
		}
		x->status = ts->status;
		MTM_LOG3("%d: Result of vote: %d", MyProcPid, ts->status);
		MtmUnlock();
	}
	if (x->gid[0]) MTM_LOG1("Prepared transaction %d (%s) csn=%ld at %ld: %d", x->xid, x->gid, ts->csn, MtmGetCurrentTime(), ts->status);
	if (Mtm->inject2PCError == 3) { 
		Mtm->inject2PCError = 0;
		elog(ERROR, "ERROR INJECTION for transaction %d (%s)", x->xid, x->gid);
	}

	MTM_TXTRACE(x, "PostPrepareTransaction Finish");
}


static void 
MtmAbortPreparedTransaction(MtmCurrentTrans* x)
{
	MtmTransMap* tm;

	if (Mtm->status == MTM_RECOVERY) { 
		return;
	}
	if (x->status != TRANSACTION_STATUS_ABORTED) { 
		MtmLock(LW_EXCLUSIVE);
		tm = (MtmTransMap*)hash_search(MtmGid2State, x->gid, HASH_REMOVE, NULL);
		Assert(tm != NULL && tm->state != NULL);
		MTM_LOG1("Abort prepared transaction %d with gid='%s'", x->xid, x->gid);
		MtmAbortTransaction(tm->state);
		MtmUnlock();
		x->status = TRANSACTION_STATUS_ABORTED;
	} else { 
		MTM_LOG1("Transaction %d with gid='%s' is already aborted", x->xid, x->gid);
	}
}

static void 
MtmEndTransaction(MtmCurrentTrans* x, bool commit)
{
	MTM_LOG3("%d: End transaction %d, prepared=%d, replicated=%d, distributed=%d, gid=%s -> %s", 
			 MyProcPid, x->xid, x->isPrepared, x->isReplicated, x->isDistributed, x->gid, commit ? "commit" : "abort");
	if (x->status != TRANSACTION_STATUS_ABORTED && x->isDistributed && (x->isPrepared || x->isReplicated)) {
		MtmTransState* ts = NULL;
		MtmLock(LW_EXCLUSIVE);
		if (x->isPrepared) { 
			ts = hash_search(MtmXid2State, &x->xid, HASH_FIND, NULL);
			Assert(ts != NULL);
		} else if (x->gid[0]) { 
			MtmTransMap* tm = (MtmTransMap*)hash_search(MtmGid2State, x->gid, HASH_REMOVE, NULL);
			if (tm != NULL) {
				ts = tm->state;
			} else { 
				MTM_LOG3("%d: GID %s not found", MyProcPid, x->gid);
			}
		}
		if (ts != NULL) { 
			if (commit) {
				/* Assert(ts->status == TRANSACTION_STATUS_UNKNOWN); */
				Assert(ts->status == TRANSACTION_STATUS_UNKNOWN 
					   || (ts->status == TRANSACTION_STATUS_IN_PROGRESS && Mtm->status == MTM_RECOVERY)); /* ??? Why there is commit without prepare */
				if (x->csn > ts->csn || Mtm->status == MTM_RECOVERY) {
					ts->csn = x->csn;
					MtmSyncClock(ts->csn);
				}
				Mtm->lastCsn = ts->csn;
				ts->status = TRANSACTION_STATUS_COMMITTED;
				MtmAdjustSubtransactions(ts);
				Assert(Mtm->nActiveTransactions != 0);
				Mtm->nActiveTransactions -= 1;
			} else { 
				MTM_LOG1("%d: abort transaction %d gid='%s' is called from MtmEndTransaction", MyProcPid, x->xid, x->gid);
				MtmAbortTransaction(ts);
			}
		}
		if (!commit && x->isReplicated && TransactionIdIsValid(x->gtid.xid)) { 
			Assert(Mtm->status != MTM_RECOVERY || Mtm->recoverySlot != MtmNodeId);
			/* 
			 * Send notification only if ABORT happens during transaction processing at replicas, 
			 * do not send notification if ABORT is received from master 
			 */
			MTM_LOG1("%d: send ABORT notification abort transaction %d to coordinator %d", MyProcPid, x->gtid.xid, x->gtid.node);
			if (ts == NULL) { 
				bool found;
				Assert(TransactionIdIsValid(x->xid));
				ts = hash_search(MtmXid2State, &x->xid, HASH_ENTER, &found);
				if (!found) { 
					ts->isEnqueued = false;
				}
				ts->status = TRANSACTION_STATUS_ABORTED;
				ts->isLocal = true;
				ts->snapshot = x->snapshot;
				ts->csn = MtmAssignCSN();	
				ts->gtid = x->gtid;
				ts->nSubxids = 0;
				ts->cmd = MSG_INVALID;				
				ts->votingCompleted = true;
				MtmTransactionListAppend(ts);
			}
			MtmSendNotificationMessage(ts, MSG_ABORTED); /* send notification to coordinator */
		} else if (x->status == TRANSACTION_STATUS_ABORTED && x->isReplicated && !x->isPrepared) {
			hash_search(MtmXid2State, &x->xid, HASH_REMOVE, NULL);
		}
		MtmUnlock();
	}
	MtmResetTransaction(x);
	if (!MyReplicationSlot) { 
		MtmCheckSlots();
	}
}

void MtmSendNotificationMessage(MtmTransState* ts, MtmMessageCode cmd)
{
	MtmTransState* votingList;

	votingList = Mtm->votingTransactions;
	ts->nextVoting = votingList;
	ts->cmd = cmd;
	Mtm->votingTransactions = ts;

	if (votingList == NULL) { 
		/* singal semaphore only once for the whole list */
		PGSemaphoreUnlock(&Mtm->votingSemaphore);
	}
}


static void MtmStartRecovery()
{
	MtmLock(LW_EXCLUSIVE);
	BIT_SET(Mtm->disabledNodeMask, MtmNodeId-1);
	MtmSwitchClusterMode(MTM_RECOVERY);
	MtmUnlock();
}

void MtmJoinTransaction(GlobalTransactionId* gtid, csn_t globalSnapshot)
{
	MtmTx.gtid = *gtid;
	MtmTx.xid = GetCurrentTransactionId();
	MtmTx.isReplicated = true;
	MtmTx.isDistributed = true;
	MtmTx.containsDML = true;

	if (globalSnapshot != INVALID_CSN) {
		MtmLock(LW_EXCLUSIVE);
		MtmSyncClock(globalSnapshot);	
		MtmTx.snapshot = globalSnapshot;	
		if (Mtm->status != MTM_RECOVERY) { 
			MtmCreateTransState(&MtmTx); /* we need local->remote xid mapping for deadlock detection */
		}
		MtmUnlock();
	} else { 
		globalSnapshot = MtmTx.snapshot;
	}
	if (!TransactionIdIsValid(gtid->xid)) { 
		/* In case of recovery InvalidTransactionId is passed */
		if (Mtm->status != MTM_RECOVERY) { 
			elog(WARNING, "Node %d tries to recover node %d which is in %s mode", gtid->node, MtmNodeId,  MtmNodeStatusMnem[Mtm->status]);
			MtmStartRecovery();
		}
	} else if (Mtm->status == MTM_RECOVERY) { 
		/* When recovery is completed we get normal transaction ID and switch to normal mode */
		MtmRecoveryCompleted();
	}
}

void  MtmSetCurrentTransactionGID(char const* gid)
{
	MTM_LOG3("Set current transaction xid=%d GID %s", MtmTx.xid, gid);
	strcpy(MtmTx.gid, gid);
	MtmTx.isDistributed = true;
	MtmTx.isReplicated = true;
}

TransactionId MtmGetCurrentTransactionId(void)
{
	return MtmTx.xid;
}

XidStatus MtmGetCurrentTransactionStatus(void)
{
	return MtmTx.status;
}

XidStatus MtmExchangeGlobalTransactionStatus(char const* gid, XidStatus new_status)
{
	MtmTransMap* tm;
	bool found;
	XidStatus old_status = TRANSACTION_STATUS_IN_PROGRESS;

	Assert(gid[0]);
	MtmLock(LW_EXCLUSIVE);
	tm = (MtmTransMap*)hash_search(MtmGid2State, gid, HASH_ENTER, &found);
	if (found) {
		old_status = tm->status;
		if (old_status != TRANSACTION_STATUS_ABORTED) { 
			tm->status = new_status;
		}
	} else { 
		tm->state = NULL;
		tm->status = new_status;
	}
	MtmUnlock();
	return old_status;
}

void  MtmSetCurrentTransactionCSN(csn_t csn)
{
	MTM_LOG3("Set current transaction CSN %ld", csn);
	MtmTx.csn = csn;
	MtmTx.isDistributed = true;
	MtmTx.isReplicated = true;
}


csn_t MtmGetTransactionCSN(TransactionId xid)
{
	MtmTransState* ts;
	csn_t csn;
	MtmLock(LW_SHARED);
	ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
	Assert(ts != NULL);
	csn = ts->csn;
	MtmUnlock();
	return csn;
}
	
void MtmWakeUpBackend(MtmTransState* ts)
{
	if (!ts->votingCompleted) {
		MTM_TXTRACE(ts, "MtmWakeUpBackend");
		MTM_LOG3("Wakeup backed procno=%d, pid=%d", ts->procno, ProcGlobal->allProcs[ts->procno].pid);
		ts->votingCompleted = true;
		SetLatch(&ProcGlobal->allProcs[ts->procno].procLatch); 
	}
}

void MtmAbortTransaction(MtmTransState* ts)
{	
	if (ts->status != TRANSACTION_STATUS_ABORTED) { 
		MTM_LOG1("Rollback active transaction %d:%d (local xid %d)", ts->gtid.node, ts->gtid.xid, ts->xid);
		ts->status = TRANSACTION_STATUS_ABORTED;
		MtmAdjustSubtransactions(ts);
		Mtm->nActiveTransactions -= 1;
	}
}

/*
 * -------------------------------------------
 * HA functions
 * -------------------------------------------
 */

void MtmHandleApplyError(void)
{
	ErrorData *edata = CopyErrorData();
	switch (edata->sqlerrcode) { 
		case ERRCODE_DISK_FULL:
		case ERRCODE_INSUFFICIENT_RESOURCES:
		case ERRCODE_IO_ERROR:
		case ERRCODE_DATA_CORRUPTED:
		case ERRCODE_INDEX_CORRUPTED:
		case ERRCODE_SYSTEM_ERROR:
		case ERRCODE_INTERNAL_ERROR:
		case ERRCODE_OUT_OF_MEMORY:
			elog(WARNING, "Node is excluded from cluster because of non-recoverable error %d, %s, pid=%u",
				edata->sqlerrcode, edata->message, getpid());
			// MtmSwitchClusterMode(MTM_OUT_OF_SERVICE);
			// kill(PostmasterPid, SIGQUIT);
			break;
	}
	FreeErrorData(edata);
}


static void MtmDisableNode(int nodeId)
{
	BIT_SET(Mtm->disabledNodeMask, nodeId-1);
	Mtm->nodes[nodeId-1].lastStatusChangeTime = MtmGetSystemTime();
	Mtm->nodes[nodeId-1].lastHeartbeat = 0; /* defuse watchdog until first heartbeat is received */
	if (nodeId != MtmNodeId) { 
		Mtm->nLiveNodes -= 1;
	}
	elog(WARNING, "Disable node %d at xlog position %lx", nodeId, GetXLogInsertRecPtr());
} 
	
static void MtmEnableNode(int nodeId)
{ 
	BIT_CLEAR(Mtm->disabledNodeMask, nodeId-1);
	BIT_CLEAR(Mtm->reconnectMask, nodeId-1);
	Mtm->nodes[nodeId-1].lastStatusChangeTime = MtmGetSystemTime();
	Mtm->nodes[nodeId-1].lastHeartbeat = 0; /* defuse watchdog until first heartbeat is received */
	if (nodeId != MtmNodeId) { 
		Mtm->nLiveNodes += 1;			
	}
	elog(WARNING, "Enable node %d at xlog position %lx", nodeId, GetXLogInsertRecPtr());
}

void MtmRecoveryCompleted(void)
{
	int i;
	MTM_LOG1("Recovery of node %d is completed, disabled mask=%lx, reconnect mask=%lx, live nodes=%d", 
			 MtmNodeId, Mtm->disabledNodeMask, Mtm->reconnectMask, Mtm->nLiveNodes);
	MtmLock(LW_EXCLUSIVE);
	Mtm->recoverySlot = 0;
	Mtm->nodes[MtmNodeId-1].lastStatusChangeTime = MtmGetSystemTime();
	BIT_CLEAR(Mtm->disabledNodeMask, MtmNodeId-1);
	for (i = 0; i < Mtm->nAllNodes; i++) { 
		Mtm->nodes[i].lastHeartbeat = 0; /* defuse watchdog until first heartbeat is received */
	}
	/* Mode will be changed to online once all logical reciever are connected */
	MtmSwitchClusterMode(MTM_CONNECTED);
	MtmUnlock();
}



/**
 * Check state of replication slots. If some of them are too much lag behind wal, then drop this slots to avoid 
 * WAL overflow
 */
static void 
MtmCheckSlots()
{
	if (MtmMaxRecoveryLag != 0 && Mtm->disabledNodeMask != 0) 
	{
		int i;
		for (i = 0; i < max_replication_slots; i++) { 
			ReplicationSlot* slot = &ReplicationSlotCtl->replication_slots[i];
			int nodeId;
			if (slot->in_use 
				&& sscanf(slot->data.name.data, MULTIMASTER_SLOT_PATTERN, &nodeId) == 1
				&& BIT_CHECK(Mtm->disabledNodeMask, nodeId-1)
				&& slot->data.confirmed_flush + MtmMaxRecoveryLag < GetXLogInsertRecPtr()) 
			{
				elog(WARNING, "Drop slot for node %d which lag %ld is larger than threshold %d", 
						 nodeId,
						 GetXLogInsertRecPtr() - slot->data.restart_lsn,
						 MtmMaxRecoveryLag);
				ReplicationSlotDrop(slot->data.name.data);
			}
		}
	}
}

static int64 MtmGetSlotLag(int nodeId)
{
	int i;
	for (i = 0; i < max_replication_slots; i++) { 
		ReplicationSlot* slot = &ReplicationSlotCtl->replication_slots[i];
		int node;
		if (slot->in_use 
			&& sscanf(slot->data.name.data, MULTIMASTER_SLOT_PATTERN, &node) == 1
			&& node == nodeId)
		{
			return GetXLogInsertRecPtr() - slot->data.confirmed_flush;
		}
	}
	return -1;
}


/*
 * This function is called by WAL sender when start sending new transaction.
 * It returns true if specified node is in recovery mode. In this case we should send all transactions from WAL, 
 * not only coordinated by self node as in normal mode.
 */
bool MtmIsRecoveredNode(int nodeId)
{
	if (BIT_CHECK(Mtm->disabledNodeMask, nodeId-1)) { 
		if (!MtmIsRecoverySession) { 
			elog(ERROR, "Node %d is marked as disabled but is not in recovery mode", nodeId);
		}
		return true;
	} else { 
		MtmIsRecoverySession = false; /* recovery is completed */
		return false;
	}
}


bool MtmRecoveryCaughtUp(int nodeId, XLogRecPtr slotLSN)
{
	bool caughtUp = false;
	MtmLock(LW_EXCLUSIVE);
	if (MtmIsRecoveredNode(nodeId)) { 
		XLogRecPtr walLSN = GetXLogInsertRecPtr();
		if (slotLSN == walLSN && Mtm->nActiveTransactions == 0) {
			if (BIT_CHECK(Mtm->nodeLockerMask, nodeId-1)) { 
				MTM_LOG1("Node %d is caught-up", nodeId);	
				BIT_CLEAR(Mtm->walSenderLockerMask, MyWalSnd - WalSndCtl->walsnds);
				BIT_CLEAR(Mtm->nodeLockerMask, nodeId-1);
				Mtm->nLockers -= 1;
			} else { 
				MTM_LOG1("%d: node %d is caugth-up without locking cluster", MyProcPid, nodeId);	
				/* We are lucky: caugth-up without locking cluster! */
			}
			MtmEnableNode(nodeId);
			Mtm->nConfigChanges += 1;
			caughtUp = true;
		} else if (!BIT_CHECK(Mtm->nodeLockerMask, nodeId-1)
				   && slotLSN + MtmMinRecoveryLag > walLSN) 
		{ 
			/*
			 * Wal sender almost catched up.
			 * Lock cluster preventing new transaction to start until wal is completely replayed.
			 * We have to maintain two bitmasks: one is marking wal sender, another - correspondent nodes. 
			 * Is there some better way to establish mapping between nodes ad WAL-seconder?
			 */
			MTM_LOG1("Node %d is almost caught-up: slot position %lx, WAL position %lx, active transactions %d", 
				 nodeId, slotLSN, walLSN, Mtm->nActiveTransactions);
			Assert(MyWalSnd != NULL); /* This function is called by WAL-sender, so it should not be NULL */
			BIT_SET(Mtm->nodeLockerMask, nodeId-1);
			BIT_SET(Mtm->walSenderLockerMask, MyWalSnd - WalSndCtl->walsnds);
			Mtm->nLockers += 1;
		} else { 
			MTM_LOG2("Continue recovery of node %d, slot position %lx, WAL position %lx, WAL sender position %lx, lockers %d, active transactions %d", nodeId, slotLSN, walLSN, MyWalSnd->sentPtr, Mtm->nLockers, Mtm->nActiveTransactions);
		}
	}
	MtmUnlock();
	return caughtUp;
}

void MtmSwitchClusterMode(MtmNodeStatus mode)
{
	Mtm->status = mode;
	MTM_LOG1("Switch to %s mode", MtmNodeStatusMnem[mode]);
	/* ??? Something else to do here? */
}


/*
 * If there are recovering nodes which are catching-up WAL, check the status and prevent new transaction from commit to give
 * WAL-sender a chance to catch-up WAL, completely synchronize replica and switch it to normal mode.
 * This function is called before transaction prepare with multimaster lock set.
 */
static void 
MtmCheckClusterLock()
{	
	timestamp_t delay = MIN_WAIT_TIMEOUT;
	while (true)
	{
		nodemask_t mask = Mtm->walSenderLockerMask;
		if (mask != 0) {
			if (Mtm->nActiveTransactions == 0) { 
				XLogRecPtr currLogPos = GetXLogInsertRecPtr();
				int i;
				for (i = 0; mask != 0; i++, mask >>= 1) { 
					if (mask & 1) { 
						if (WalSndCtl->walsnds[i].sentPtr != currLogPos) {
							/* recovery is in progress */
							break;
						} else { 
							/* recovered replica catched up with master */
							MTM_LOG1("WAL-sender %d complete recovery", i);
							BIT_CLEAR(Mtm->walSenderLockerMask, i);
						}
					}
				}
			}
			if (mask != 0) { 
				/* some "almost catch-up" wal-senders are still working. */
				/* Do not start new transactions until them are completed. */
				MtmUnlock();
				MtmSleep(delay);
				if (delay*2 <= MAX_WAIT_TIMEOUT) { 
					delay *= 2;
				}
				MtmLock(LW_EXCLUSIVE);
				continue;
			} else {  
				/* All lockers are synchronized their logs */
				/* Remove lock and mark them as receovered */
				MTM_LOG1("Complete recovery of %d nodes (node mask %lx)", Mtm->nLockers, (long) Mtm->nodeLockerMask);
				Assert(Mtm->walSenderLockerMask == 0);
				Assert((Mtm->nodeLockerMask & Mtm->disabledNodeMask) == Mtm->nodeLockerMask);
				Mtm->disabledNodeMask &= ~Mtm->nodeLockerMask;
				Mtm->nLiveNodes += Mtm->nLockers;
				Mtm->nLockers = 0;
				Mtm->nodeLockerMask = 0;
				MtmCheckQuorum();
			}
		}
		break;
	}
}	

/**
 * Build internode connectivity mask. 1 - means that node is disconnected.
 */
static bool 
MtmBuildConnectivityMatrix(nodemask_t* matrix, bool nowait)
{
	int i, j, n = Mtm->nAllNodes;
	fprintf(stderr, "Connectivity matrix:\n");
	for (i = 0; i < n; i++) { 
		if (i+1 != MtmNodeId) { 
			void* data = RaftableGet(psprintf("node-mask-%d", i+1), NULL, NULL, nowait);
			if (data == NULL) { 
				return false;
			}
			matrix[i] = *(nodemask_t*)data;
		} else { 
			matrix[i] = Mtm->connectivityMask;
		}
		for (j = 0; j < n; j++) { 
			putc(BIT_CHECK(matrix[i], j) ? 'X' : '+', stderr);
		}
		putc('\n', stderr);
	}
	fputs("-----------------------\n", stderr);
	/* make matrix symetric: required for Bron–Kerbosch algorithm */
	for (i = 0; i < n; i++) { 
		for (j = 0; j < i; j++) { 
			matrix[i] |= ((matrix[j] >> i) & 1) << j;
			matrix[j] |= ((matrix[i] >> j) & 1) << i;
		}
		matrix[i] &= ~((nodemask_t)1 << i);
	}
	return true;
}	


/**
 * Build connectivity graph, find clique in it and extend disabledNodeMask by nodes not included in clique.
 * This function returns false if current node is excluded from cluster, true otherwise
 */
bool MtmRefreshClusterStatus(bool nowait)
{
	nodemask_t mask, clique, disabled;
	nodemask_t matrix[MAX_NODES];
	MtmTransState *ts;
	int clique_size;
	int i;

	if (!MtmBuildConnectivityMatrix(matrix, nowait)) { 
		/* RAFT is not available */
		return false;
	}

	clique = MtmFindMaxClique(matrix, Mtm->nAllNodes, &clique_size);
	if (clique_size >= Mtm->nAllNodes/2+1) { /* have quorum */
		fprintf(stderr, "Old mask: ");
		for (i = 0; i <  Mtm->nAllNodes; i++) { 
			putc(BIT_CHECK(Mtm->disabledNodeMask, i) ? '-' : '+', stderr);
		}
		putc('\n', stderr);
		fprintf(stderr, "New mask: ");
		for (i = 0; i <  Mtm->nAllNodes; i++) { 
			putc(BIT_CHECK(clique, i) ? '+' : '-', stderr);
		}
		putc('\n', stderr);

		MTM_LOG1("Find clique %lx, disabledNodeMask %lx", (long) clique, (long) Mtm->disabledNodeMask);
		MtmLock(LW_EXCLUSIVE);
		disabled = ~clique & (((nodemask_t)1 << Mtm->nAllNodes)-1) & ~Mtm->disabledNodeMask; /* new disabled nodes mask */
		
		for (i = 0, mask = disabled; mask != 0; i++, mask >>= 1) {
			if (mask & 1) { 
				MtmDisableNode(i+1);
			}
		}		
#if 0	/* Do  not enable nodes here: them will be enabled after completion of recovery */
		enabled = clique & Mtm->disabledNodeMask; /* new enabled nodes mask */		
		for (i = 0, mask = enabled; mask != 0; i++, mask >>= 1) {
			if (mask & 1) { 
				MtmEnableNode(i+1);
			}
		}
		Mtm->reconnectMask |= clique & Mtm->disabledNodeMask; /* new enabled nodes mask */		
#endif

		if (disabled) { 
			MtmCheckQuorum();
		}
		/* Interrupt voting for active transaction and abort them */
		for (ts = Mtm->transListHead; ts != NULL; ts = ts->next) { 
			MTM_LOG3("Active transaction gid='%s', coordinator=%d, xid=%d, status=%d, gtid.xid=%d",
					 ts->gid, ts->gtid.nхode, ts->xid, ts->status, ts->gtid.xid);
			if (MtmIsCoordinator(ts)) { 
				if (!ts->votingCompleted && disabled != 0 && ts->status != TRANSACTION_STATUS_ABORTED) {
					MtmAbortTransaction(ts);
					MtmWakeUpBackend(ts);
				}
#if 0
			} else if (TransactionIdIsValid(ts->gtid.xid) && BIT_CHECK(disabled, ts->gtid.node-1)) { /* coordinator of transaction is on disabled node */
				if (ts->gid[0]) { 
					if (ts->status == TRANSACTION_STATUS_UNKNOWN || ts->status == TRANSACTION_STATUS_IN_PROGRESS) {
						MTM_LOG1("%d: Abort trasaction %s because its coordinator is at disabled node %d", MyProcPid, ts->gid, ts->gtid.node);
						MtmAbortTransaction(ts);
						MtmTx.status = TRANSACTION_STATUS_ABORTED; /* prevent recursive invocation of MtmAbortPreparedTransaction */
						FinishPreparedTransaction(ts->gid, false);
					}
				}
#endif
			}
		}
		MtmUnlock();
		if (BIT_CHECK(Mtm->disabledNodeMask, MtmNodeId-1)) { 
			if (Mtm->status == MTM_ONLINE) {
				/* I was excluded from cluster:( */
				MtmSwitchClusterMode(MTM_OFFLINE);
			}
		} else if (Mtm->status == MTM_OFFLINE) {
			/* Should we somehow restart logical receivers? */ 			
			MtmStartRecovery();
		}
	} else { 
		MTM_LOG1("Clique %lx has no quorum", (long) clique);
		MtmSwitchClusterMode(MTM_IN_MINORITY);
	}
	return true;
}

void MtmCheckQuorum(void)
{
	Mtm->nConfigChanges += 1;
	if (Mtm->nLiveNodes < Mtm->nAllNodes/2+1) {
		if (Mtm->status == MTM_ONLINE) { /* out of quorum */
			elog(WARNING, "Node is in minority: disabled mask %lx", (long) Mtm->disabledNodeMask);
			MtmSwitchClusterMode(MTM_IN_MINORITY);
		}
	} else {
		if (Mtm->status == MTM_IN_MINORITY) { 
			MTM_LOG1("Node is in majority: disabled mask %lx", (long) Mtm->disabledNodeMask);
			MtmSwitchClusterMode(MTM_ONLINE);
		}
	}
}
			
void MtmOnNodeDisconnect(int nodeId)
{ 
	MtmTransState *ts;

	if (BIT_CHECK(Mtm->disabledNodeMask, nodeId-1))
	{
		/* Node is already disabled */
		return;
	}
	if (Mtm->nodes[nodeId-1].lastStatusChangeTime + MSEC_TO_USEC(MtmNodeDisableDelay) > MtmGetSystemTime()) 
	{ 
		/* Avoid false detection of node failure and prevent node status blinking */
		return;
	}
	MtmLock(LW_EXCLUSIVE);
	BIT_SET(Mtm->connectivityMask, nodeId-1);
	BIT_SET(Mtm->reconnectMask, nodeId-1);
	MtmUnlock();

	if (!RaftableSet(psprintf("node-mask-%d", MtmNodeId), &Mtm->connectivityMask, sizeof Mtm->connectivityMask, false))
	{
		elog(WARNING, "Disable node which is in minority according to RAFT");
		MtmLock(LW_EXCLUSIVE);
		if (Mtm->status == MTM_ONLINE) { 
			MtmSwitchClusterMode(MTM_IN_MINORITY);
		}
		MtmUnlock();
		return;
	}

	MtmSleep(MSEC_TO_USEC(MtmHeartbeatSendTimeout));

	if (!MtmUseRaftable) 
	{
		MtmLock(LW_EXCLUSIVE);
		if (!BIT_CHECK(Mtm->disabledNodeMask, nodeId-1)) { 
			MtmDisableNode(nodeId);
			MtmCheckQuorum();
			/* Interrupt voting for active transaction and abort them */
			for (ts = Mtm->transListHead; ts != NULL; ts = ts->next) { 
				if (MtmIsCoordinator(ts)) { 
					if (!ts->votingCompleted && ts->status != TRANSACTION_STATUS_ABORTED) {
						MtmAbortTransaction(ts);
						MtmWakeUpBackend(ts);
					}
#if 0
				} else if (TransactionIdIsValid(ts->gtid.xid) && ts->gtid.node == nodeId) { /* coordinator of transaction is on disabled node */
					if (ts->gid[0] && ts->status != TRANSACTION_STATUS_ABORTED) {
						MtmAbortTransaction(ts);
						MtmTx.status = TRANSACTION_STATUS_ABORTED; /* prevent recursive invocation of MtmAbortPreparedTransaction */
						FinishPreparedTransaction(ts->gid, false);
					}
#endif
				}
			}
		}
		MtmUnlock();
	} else { 
		MtmRefreshClusterStatus(false);
    }
}

void MtmOnNodeConnect(int nodeId)
{
	MtmLock(LW_EXCLUSIVE);	
	BIT_CLEAR(Mtm->connectivityMask, nodeId-1);
	BIT_CLEAR(Mtm->reconnectMask, nodeId-1);
	MtmUnlock();

	MTM_LOG1("Reconnect node %d", nodeId);
	RaftableSet(psprintf("node-mask-%d", MtmNodeId), &Mtm->connectivityMask, sizeof Mtm->connectivityMask, false); 
}



/*
 * -------------------------------------------
 * Node initialization
 * -------------------------------------------
 */


static HTAB* 
MtmCreateXidMap(void)
{
	HASHCTL info;
	HTAB* htab;
	Assert(MtmMaxNodes > 0);
	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(TransactionId);
	info.entrysize = sizeof(MtmTransState) + (MtmMaxNodes-1)*sizeof(TransactionId);
	htab = ShmemInitHash(
		"MtmXid2State",
		MTM_HASH_SIZE, MTM_HASH_SIZE,
		&info,
		HASH_ELEM | HASH_BLOBS
	);
	return htab;
}

static HTAB* 
MtmCreateGidMap(void)
{
	HASHCTL info;
	HTAB* htab;
	memset(&info, 0, sizeof(info));
	info.keysize = MULTIMASTER_MAX_GID_SIZE;
	info.entrysize = sizeof(MtmTransMap);
	htab = ShmemInitHash(
		"MtmGid2State",
		MTM_MAP_SIZE, MTM_MAP_SIZE,
		&info,
		HASH_ELEM 
	);
	return htab;
}

static HTAB* 
MtmCreateLocalTableMap(void)
{
	HASHCTL info;
	HTAB* htab;
	memset(&info, 0, sizeof(info));
	info.keysize = sizeof(Oid);
	htab = ShmemInitHash(
		"MtmLocalTables",
		MULTIMASTER_MAX_LOCAL_TABLES, MULTIMASTER_MAX_LOCAL_TABLES,
		&info,
		0 
	);
	return htab;
}

static void MtmMakeRelationLocal(Oid relid)
{
	if (OidIsValid(relid)) { 
		MtmLock(LW_EXCLUSIVE);		
		hash_search(MtmLocalTables, &relid, HASH_ENTER, NULL);
		MtmUnlock();		
	}
}	


void MtmMakeTableLocal(char* schema, char* name)
{
	RangeVar* rv = makeRangeVar(schema, name, -1);
	Oid relid = RangeVarGetRelid(rv, NoLock, true);
	MtmMakeRelationLocal(relid);
}


typedef struct { 
	NameData schema;
	NameData name;
} MtmLocalTablesTuple;

static void MtmLoadLocalTables(void)
{
	RangeVar	   *rv;
	Relation		rel;
	SysScanDesc		scan;
	HeapTuple		tuple;

	Assert(IsTransactionState());

	rv = makeRangeVar(MULTIMASTER_SCHEMA_NAME, MULTIMASTER_LOCAL_TABLES_TABLE, -1);
	rel = heap_openrv_extended(rv, RowExclusiveLock, true);
	if (rel != NULL) { 
		scan = systable_beginscan(rel, 0, true, NULL, 0, NULL);
		
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			MtmLocalTablesTuple	*t = (MtmLocalTablesTuple*) GETSTRUCT(tuple);
			MtmMakeTableLocal(NameStr(t->schema), NameStr(t->name));
		}

		systable_endscan(scan);
		heap_close(rel, RowExclusiveLock);
	}
}
	
static void MtmRaftableInitialize()
{
	int i;

	for (i = 0; i < MtmNodes; i++)
	{
		int port = MtmConnections[i].raftablePort;
		if (port == 0) {
			port = MtmRaftablePort + i;
		}
		raftable_peer(i, MtmConnections[i].hostName, port);
	}
	raftable_start(MtmNodeId - 1);
}


static void MtmInitialize()
{
	bool found;
	int i;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	Mtm = (MtmState*)ShmemInitStruct(MULTIMASTER_NAME, sizeof(MtmState) + sizeof(MtmNodeInfo)*(MtmMaxNodes-1), &found);
	if (!found)
	{
		Mtm->status = MTM_INITIALIZATION;
		Mtm->recoverySlot = 0;
		Mtm->locks = GetNamedLWLockTranche(MULTIMASTER_NAME);
		Mtm->csn = MtmGetCurrentTime();
		Mtm->lastClusterStatusUpdate = MtmGetSystemTime();
		Mtm->lastCsn = INVALID_CSN;
		Mtm->oldestXid = FirstNormalTransactionId;
        Mtm->nLiveNodes = MtmNodes;
        Mtm->nAllNodes = MtmNodes;
		Mtm->disabledNodeMask = 0;
		Mtm->connectivityMask = 0;
		Mtm->pglogicalNodeMask = 0;
		Mtm->walSenderLockerMask = 0;
		Mtm->nodeLockerMask = 0;
		Mtm->reconnectMask = 0;
		Mtm->nLockers = 0;
		Mtm->nActiveTransactions = 0;
		Mtm->votingTransactions = NULL;
        Mtm->transListHead = NULL;
        Mtm->transListTail = &Mtm->transListHead;		
        Mtm->nReceivers = 0;
		Mtm->timeShift = 0;		
		Mtm->transCount = 0;
		Mtm->gcCount = 0;
		Mtm->nConfigChanges = 0;
		Mtm->recoveryCount = 0;
		Mtm->localTablesHashLoaded = false;
		Mtm->inject2PCError = 0;
		for (i = 0; i < MtmNodes; i++) {
			Mtm->nodes[i].oldestSnapshot = 0;
			Mtm->nodes[i].transDelay = 0;
			Mtm->nodes[i].lastStatusChangeTime = MtmGetSystemTime();
			Mtm->nodes[i].con = MtmConnections[i];
			Mtm->nodes[i].flushPos = 0;
			Mtm->nodes[i].lastHeartbeat = 0;
			Mtm->nodes[i].restartLsn = 0;
			Mtm->nodes[i].originId = InvalidRepOriginId;
		}
		PGSemaphoreCreate(&Mtm->votingSemaphore);
		PGSemaphoreReset(&Mtm->votingSemaphore);
		SpinLockInit(&Mtm->spinlock);
		BgwPoolInit(&Mtm->pool, MtmExecutor, MtmDatabaseName, MtmDatabaseUser, MtmQueueSize, MtmWorkers);
		RegisterXactCallback(MtmXactCallback, NULL);
		MtmTx.snapshot = INVALID_CSN;
		MtmTx.xid = InvalidTransactionId;		
	}
	MtmXid2State = MtmCreateXidMap();
	MtmGid2State = MtmCreateGidMap();
	MtmLocalTables = MtmCreateLocalTableMap();
    MtmDoReplication = true;
	TM = &MtmTM;
	LWLockRelease(AddinShmemInitLock);
}

static void 
MtmShmemStartup(void)
{
	if (PreviousShmemStartupHook) {
		PreviousShmemStartupHook();
	}
	MtmInitialize();
}

void MtmUpdateNodeConnectionInfo(MtmConnectionInfo* conn, char const* connStr)
{
	char const* host;
	char const* end;
	int         hostLen;
	char*       port;
	int         connStrLen = (int)strlen(connStr);

	if (connStrLen >= MULTIMASTER_MAX_CONN_STR_SIZE) {
		elog(ERROR, "Too long (%d) connection string '%s': limit is %d", 
			 connStrLen, connStr, MULTIMASTER_MAX_CONN_STR_SIZE-1);
	}
	strcpy(conn->connStr, connStr);

	host = strstr(connStr, "host=");
	if (host == NULL) {
		elog(ERROR, "Host not specified in connection string: '%s'", connStr);
	}
	host += 5;
	for (end = host; *end != ' ' && *end != '\0'; end++);
	hostLen = end - host;
	if (hostLen >= MULTIMASTER_MAX_HOST_NAME_SIZE) {
		elog(ERROR, "Too long (%d) host name '%.*s': limit is %d", 
			 hostLen, hostLen, host, MULTIMASTER_MAX_HOST_NAME_SIZE-1);
	}
	memcpy(conn->hostName, host, hostLen);
	conn->hostName[hostLen] = '\0';

	port = strstr(connStr, "raftport=");
	if (port != NULL) {
		int n;
		if (sscanf(port+9, "%d%n", &conn->raftablePort, &n) != 1) { 
			elog(ERROR, "Invalid raftable port: %s", port+9);
		}
		n += 9;
	} else { 
		conn->raftablePort = 0;
	}

	port = strstr(connStr, "arbiterport=");
	if (port != NULL) {
		int n;
		if (sscanf(port+12, "%d%n", &conn->arbiterPort, &n) != 1) { 
			elog(ERROR, "Invalid arbiter port: %s", port+12);
		}
		n += 12;
	} else { 
		conn->arbiterPort = 0;
	}
}

static void MtmSplitConnStrs(void)
{
	int i;
	char* copy =  pstrdup(MtmConnStrs);
    char* connStr = copy;
    char* connStrEnd = connStr + strlen(connStr);

	for (i = 0; connStr < connStrEnd; i++) { 
		char* p = strchr(connStr, ',');
        if (p == NULL) { 
            p = connStrEnd;
        }
		connStr = p + 1;
	}
	if (i > MAX_NODES) { 
		elog(ERROR, "Multimaster with more than %d nodes is not currently supported", MAX_NODES);
	}
	if (MtmNodeId > i) {
		elog(ERROR, "Multimaster node id %d is out of range [%d..%d]", MtmNodeId, 1, i);
	}
	if (i < 2) { 
        elog(ERROR, "Multimaster should have at least two nodes");
	}	
	if (MtmMaxNodes == 0) {
		MtmMaxNodes = i;
	} else if (MtmMaxNodes < i) { 
        elog(ERROR, "More than %d nodes are specified", MtmMaxNodes);
	}			
	MtmNodes = i;
	MtmConnections = (MtmConnectionInfo*)palloc(MtmMaxNodes*sizeof(MtmConnectionInfo));
	connStr = copy;

	for (i = 0; connStr < connStrEnd; i++) { 
        char* p = strchr(connStr, ',');
        if (p == NULL) { 
            p = connStrEnd;
        }
		*p = '\0';

		MtmUpdateNodeConnectionInfo(&MtmConnections[i], connStr);

		if (i+1 == MtmNodeId) {
			char* dbName = strstr(connStr, "dbname="); // XXX: shoud we care about string 'itisnotdbname=xxx'?
			char* dbUser = strstr(connStr, "user=");
			char* end;
			size_t len;

			if (dbName == NULL)
				elog(ERROR, "Database is not specified in connection string: '%s'", connStr);

			if (dbUser == NULL)
			{
				char *errstr;
				const char *username = get_user_name(&errstr);
				if (!username)
					elog(FATAL, "Database user is not specified in connection string '%s', fallback failed: %s", connStr, errstr);
				else
					elog(WARNING, "Database user is not specified in connection string '%s', fallback to '%s'", connStr, username);
				MtmDatabaseUser = pstrdup(username);
			}
			else
			{
				dbUser += 5;
				end = strchr(dbUser, ' ');
				if (!end) end = strchr(dbUser, '\0');
				Assert(end != NULL);
				len = end - dbUser;
				MtmDatabaseUser = pnstrdup(dbUser, len);
			}

			dbName += 7;
			end = strchr(dbName, ' ');
			if (!end) end = strchr(dbName, '\0');
			Assert(end != NULL);
			len = end - dbName;
			MtmDatabaseName = pnstrdup(dbName, len);
		}
		connStr = p + 1;
    }
	pfree(copy);
}		

static bool ConfigIsSane(void)
{
	bool ok = true;

	if (DefaultXactIsoLevel != XACT_REPEATABLE_READ)
	{
		elog(WARNING, "multimaster requires default_transaction_isolation = 'repeatable read'");
		ok = false;
	}

	if (MtmMaxNodes < 1)
	{
		elog(WARNING, "multimaster requires multimaster.max_nodes > 0");
		ok = false;
	}

	if (max_prepared_xacts < 1)
	{
		elog(WARNING,
			 "multimaster requires max_prepared_transactions > 0, "
			 "because all transactions are implicitly two-phase");
		ok = false;
	}

	{
		int workers_required = 2 * MtmMaxNodes + MtmWorkers + 1;
		if (max_worker_processes < workers_required)
		{
			elog(WARNING,
				 "multimaster requires max_worker_processes >= %d",
				 workers_required);
			ok = false;
		}
	}

	if (wal_level != WAL_LEVEL_LOGICAL)
	{
		elog(WARNING,
			 "multimaster requires wal_level = 'logical', "
			 "because it is build on top of logical replication");
		ok = false;
	}

	if (max_wal_senders < MtmMaxNodes)
	{
		elog(WARNING,
			 "multimaster requires max_wal_senders >= %d (multimaster.max_nodes), ",
			 MtmMaxNodes);
		ok = false;
	}

	if (max_replication_slots < MtmMaxNodes)
	{
		elog(WARNING,
			 "multimaster requires max_replication_slots >= %d (multimaster.max_nodes), ",
			 MtmMaxNodes);
		ok = false;
	}

	return ok;
}

void
_PG_init(void)
{
	/*
	 * In order to create our shared memory area, we have to be loaded via
	 * shared_preload_libraries.  If not, fall out without hooking into any of
	 * the main system.  (We don't throw error here because it seems useful to
	 * allow the cs_* functions to be created even when the
	 * module isn't active.  The functions must protect themselves against
	 * being called then, however.)
	 */
	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomIntVariable(
		"multimaster.heartbeat_send_timeout", 
		"Timeout in milliseconds of sending heartbeat messages",
		"Period of broadcasting heartbeat messages by arbiter to all nodes",
		&MtmHeartbeatSendTimeout,
		1000,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.heartbeat_recv_timeout", 
		"Timeout in milliseconds of receiving heartbeat messages",
		"If no heartbeat message is received from node within this period, it assumed to be dead",
		&MtmHeartbeatRecvTimeout,
		10000,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.gc_period",
		"Number of distributed transactions after which garbage collection is started",
		"Multimaster is building xid->csn hash map which has to be cleaned to avoid hash overflow. This parameter specifies interval of invoking garbage collector for this map",
		&MtmGcPeriod,
		MTM_HASH_SIZE/10,
		1,
	    INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.max_nodes",
		"Maximal number of cluster nodes",
		"This parameters allows to add new nodes to the cluster, default value 0 restricts number of nodes to one specified in multimaster.conn_strings",
		&MtmMaxNodes,
		0,
		0,
		MAX_NODES,
		PGC_POSTMASTER,
		0,
		NULL,
		NULL,
		NULL
	);
	DefineCustomIntVariable(
		"multimaster.trans_spill_threshold",
		"Maximal size (Mb) of transaction after which transaction is written to the disk",
		NULL,
		&MtmTransSpillThreshold,
		1000, /* 1Gb */
		0,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.twopc_min_timeout",
		"Minamal amount of time (milliseconds) to wait 2PC confirmation from all nodes",
		"Timeout for 2PC is calculated as MAX(prepare_time*2pc_prepare_ratio/100,2pc_min_timeout)",
		&Mtm2PCMinTimeout,
		10000, /* 10 seconds */
		0,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.twopc_prepare_ratio",
		"Percent of prepare time for maximal time of second phase of two-pahse commit",
		"Timeout for 2PC is calculated as MAX(prepare_time*2pc_prepare_ratio/100,2pc_min_timeout)",
		&Mtm2PCPrepareRatio,
		1000, /* 10 times */
		0,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);



	DefineCustomIntVariable(
		"multimaster.node_disable_delay",
		"Minimal amount of time (msec) between node status change",
		"This delay is used to avoid false detection of node failure and to prevent blinking of node status node",
		&MtmNodeDisableDelay,
		1000,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.min_recovery_lag",
		"Minimal lag of WAL-sender performing recovery after which cluster is locked until recovery is completed",
		"When wal-sender almost catch-up WAL current position we need to stop 'Achilles tortile competition' and "
		"temporary stop commit of new transactions until node will be completely repared",
		&MtmMinRecoveryLag,
		100000,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.max_recovery_lag",
		"Maximal lag of replication slot of failed node after which this slot is dropped to avoid transaction log overflow",
		"Dropping slot makes it not possible to recover node using logical replication mechanism, it will be ncessary to completely copy content of some other nodes " 
		"using basebackup or similar tool. Zero value of parameter disable dropping slot.",
		&MtmMaxRecoveryLag,
		100000000,
		0,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.use_raftable",
		"Use raftable plugin for internode communication",
		NULL,
		&MtmUseRaftable,
		true,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.ignore_tables_without_pk",
		"Do not replicate tables withpout primary key",
		NULL,
		&MtmIgnoreTablesWithoutPk,
		false,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.use_dtm",
		"Use distributed transaction manager",
		NULL,
		&MtmUseDtm,
		true,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomBoolVariable(
		"multimaster.volkswagen_mode",
		"Pretend to be normal postgres. This means skip some NOTICE's and use local sequences. Default false.",
		NULL,
		&MtmVolksWagenMode,
		false,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.workers",
		"Number of multimaster executor workers per node",
		NULL,
		&MtmWorkers,
		8,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.vacuum_delay",
		"Minimal age of records which can be vacuumed (seconds)",
		NULL,
		&MtmVacuumDelay,
		1,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.queue_size",
		"Multimaster queue size",
		NULL,
		&MtmQueueSize,
		256*1024*1024,
	    1024*1024,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.arbiter_port",
		"Base value for assigning arbiter ports",
		NULL,
		&MtmArbiterPort,
		54320,
	    0,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.raftable_port",
		"Base value for assigning raftable ports",
		NULL,
		&MtmRaftablePort,
		6543,
	    0,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomStringVariable(
		"multimaster.conn_strings",
		"Multimaster node connection strings separated by commas, i.e. 'replication=database dbname=postgres host=localhost port=5001,replication=database dbname=postgres host=localhost port=5002'",
		NULL,
		&MtmConnStrs,
		"",
		PGC_BACKEND, /* context */
		0,           /* flags */
		NULL,        /* GucStringCheckHook check_hook */
		NULL,        /* GucStringAssignHook assign_hook */
		NULL         /* GucShowHook show_hook */
	);
    
	DefineCustomIntVariable(
		"multimaster.node_id",
		"Multimaster node ID",
		NULL,
		&MtmNodeId,
		INT_MAX,
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.connect_timeout",
		"Multimaster nodes connect timeout",
		"Interval in milliseconds for establishing connection with cluster node",
		&MtmConnectTimeout,
		10000, /* 10 seconds */
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	DefineCustomIntVariable(
		"multimaster.reconnect_timeout",
		"Multimaster nodes reconnect timeout",
		"Interval in milliseconds for establishing connection with cluster node",
		&MtmReconnectTimeout,
		5000, /* 5 seconds */
		1,
		INT_MAX,
		PGC_BACKEND,
		0,
		NULL,
		NULL,
		NULL
	);

	if (!ConfigIsSane()) {
		elog(ERROR, "Multimaster config is insane, refusing to work");
	}

	/* This will also perform some checks on connection strings */
	MtmSplitConnStrs();

    MtmStartReceivers();

	/*
	 * Request additional shared resources.  (These are no-ops if we're not in
	 * the postmaster process.)  We'll allocate or attach to the shared
	 * resources in mtm_shmem_startup().
	 */
	RequestAddinShmemSpace(MTM_SHMEM_SIZE + MtmQueueSize);
	RequestNamedLWLockTranche(MULTIMASTER_NAME, 1 + MtmMaxNodes);

    BgwPoolStart(MtmWorkers, MtmPoolConstructor);

	if (MtmUseRaftable)
		MtmRaftableInitialize();
	MtmArbiterInitialize();

	/*
	 * Install hooks.
	 */
	PreviousShmemStartupHook = shmem_startup_hook;
	shmem_startup_hook = MtmShmemStartup;

	PreviousExecutorFinishHook = ExecutorFinish_hook;
	ExecutorFinish_hook = MtmExecutorFinish;

	PreviousProcessUtilityHook = ProcessUtility_hook;
	ProcessUtility_hook = MtmProcessUtility;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	shmem_startup_hook = PreviousShmemStartupHook;
	ExecutorFinish_hook = PreviousExecutorFinishHook;
	ProcessUtility_hook = PreviousProcessUtilityHook;	
}


/*
 * This functions is called by pglogical receiver main function when receiver background worker is started.
 * We switch to ONLINE mode when all receviers are connected.
 * As far as background worker can be restarted multiple times, use node bitmask.
 */
void MtmReceiverStarted(int nodeId)
{
	MtmLock(LW_EXCLUSIVE);
	if (!BIT_CHECK(Mtm->pglogicalNodeMask, nodeId-1)) { 
		BIT_SET(Mtm->pglogicalNodeMask, nodeId-1);
		if (BIT_CHECK(Mtm->disabledNodeMask, nodeId-1)) {
			MtmEnableNode(nodeId);
			MtmCheckQuorum();
		}
		if (++Mtm->nReceivers == Mtm->nLiveNodes-1) {
			if (Mtm->status == MTM_CONNECTED) { 
				MtmSwitchClusterMode(MTM_ONLINE);
			}
		}
	}
	MtmUnlock();
}

/* 
 * Determine when and how we should open replication slot.
 * Druing recovery we need to open only one replication slot from which node should receive all transactions.
 * Slots at other nodes should be removed 
 */
MtmReplicationMode MtmGetReplicationMode(int nodeId, sig_atomic_t volatile* shutdown)
{
	int i;
	bool recovery = false;

	while (Mtm->status != MTM_CONNECTED && Mtm->status != MTM_ONLINE) 
	{ 	
		if (*shutdown) 
		{ 
			return REPLMODE_EXIT;
		}
		MTM_LOG2("%d: receiver slot mode %s", MyProcPid, MtmNodeStatusMnem[Mtm->status]);
		MtmLock(LW_EXCLUSIVE);
		if (Mtm->status == MTM_RECOVERY) { 
			recovery = true;
			if (Mtm->recoverySlot == 0 || Mtm->recoverySlot == nodeId) { 
				/* Choose for recovery first available slot */
				elog(WARNING, "Process %d starts recovery from node %d", MyProcPid, nodeId);
				Mtm->recoverySlot = nodeId;
				Mtm->nReceivers = 0;
				Mtm->recoveryCount += 1;
				Mtm->pglogicalNodeMask = 0;
				for (i = 0; i < Mtm->nAllNodes; i++) { 
					Mtm->nodes[i].restartLsn = InvalidXLogRecPtr;
				}
				MtmUnlock();
				FinishAllPreparedTransactions(false);
				return REPLMODE_RECOVERY;
			}
		}
		MtmUnlock();
		/* delay opening of other slots until recovery is completed */
		MtmSleep(STATUS_POLL_DELAY);
	}
	if (recovery) { 
		MTM_LOG1("%d: Restart replication for node %d after end of recovery", MyProcPid, nodeId);
	} else { 
		MTM_LOG1("%d: Continue replication slot for node %d", MyProcPid, nodeId);
	}
	/* After recovery completion we need to drop all other slots to avoid receive of redundant data */
	return recovery ? REPLMODE_RECOVERED : REPLMODE_NORMAL;
}
			
static bool MtmIsBroadcast() 
{
	return application_name != NULL && strcmp(application_name, MULTIMASTER_BROADCAST_SERVICE) == 0;
}

void MtmRecoverNode(int nodeId)
{
	if (nodeId <= 0 || nodeId > Mtm->nLiveNodes) 
	{ 
		elog(ERROR, "NodeID %d is out of range [1,%d]", nodeId, Mtm->nLiveNodes);
	}
	if (!BIT_CHECK(Mtm->disabledNodeMask, nodeId-1)) { 
		elog(ERROR, "Node %d was not disabled", nodeId);
	}
	if (!MtmIsBroadcast())
	{
		MtmBroadcastUtilityStmt(psprintf("select pg_create_logical_replication_slot('" MULTIMASTER_SLOT_PATTERN "', '" MULTIMASTER_NAME "')", nodeId), true);
	}
}
	
	
void MtmDropNode(int nodeId, bool dropSlot)
{
	MtmLock(LW_EXCLUSIVE);

	if (!BIT_CHECK(Mtm->disabledNodeMask, nodeId-1))
	{
		if (nodeId <= 0 || nodeId > Mtm->nLiveNodes) 
		{ 
			MtmUnlock();
			elog(ERROR, "NodeID %d is out of range [1,%d]", nodeId, Mtm->nLiveNodes);
		}
		MtmDisableNode(nodeId);
		MtmCheckQuorum();
		if (!MtmIsBroadcast())
		{
			MtmBroadcastUtilityStmt(psprintf("select mtm.drop_node(%d,%s)", nodeId, dropSlot ? "true" : "false"), true);
		}
		if (dropSlot) 
		{
			ReplicationSlotDrop(psprintf(MULTIMASTER_SLOT_PATTERN, nodeId));
		}		
	}

	MtmUnlock();
}
static void
MtmOnProcExit(int code, Datum arg)
{
	if (MtmReplicationNodeId > 0) { 
		Mtm->nodes[MtmReplicationNodeId-1].senderPid = -1;
		MTM_LOG1("WAL-sender to %d is terminated", MtmReplicationNodeId); 
		/* MtmOnNodeDisconnect(MtmReplicationNodeId); */
	}
}

static void 
MtmReplicationStartupHook(struct PGLogicalStartupHookArgs* args)
{
	ListCell *param;
	bool recoveryCompleted = false;
	MtmIsRecoverySession = false;
	Mtm->nodes[MtmReplicationNodeId-1].senderPid = MyProcPid;
	Mtm->nodes[MtmReplicationNodeId-1].senderStartTime = MtmGetSystemTime();
	foreach(param, args->in_params)
	{
		DefElem    *elem = lfirst(param);
		if (strcmp("mtm_replication_mode", elem->defname) == 0) { 
			if (elem->arg != NULL && strVal(elem->arg) != NULL) { 
				if (strcmp(strVal(elem->arg), "recovery") == 0) { 
					MtmIsRecoverySession = true;
				} else if (strcmp(strVal(elem->arg), "recovered") == 0) { 
					recoveryCompleted = true;
				} else if (strcmp(strVal(elem->arg), "normal") != 0) { 
					elog(ERROR, "Illegal recovery mode %s", strVal(elem->arg));
				}
			} else { 
				elog(ERROR, "Replication mode is not specified");
			}				
			break;
		}
	}
	MtmLock(LW_EXCLUSIVE);
	if (MtmIsRecoverySession) {
		MTM_LOG1("%d: Node %d start recovery of node %d", MyProcPid, MtmNodeId, MtmReplicationNodeId);
		if (!BIT_CHECK(Mtm->disabledNodeMask,  MtmReplicationNodeId-1)) {
			MtmDisableNode(MtmReplicationNodeId);
			MtmCheckQuorum();
		}
	} else if (BIT_CHECK(Mtm->disabledNodeMask,  MtmReplicationNodeId-1)) {
		if (recoveryCompleted) { 
			MTM_LOG1("Node %d consider that recovery of node %d is completed: start normal replication", MtmNodeId, MtmReplicationNodeId); 
			MtmEnableNode(MtmReplicationNodeId);
			MtmCheckQuorum();
		} else {
			MtmUnlock();
			elog(ERROR, "Disabled node %d tries to reconnect without recovery", MtmReplicationNodeId); 
		}
	} else {
		MTM_LOG1("Node %d start logical replication to node %d in normal mode", MtmNodeId, MtmReplicationNodeId); 
	}
	MtmUnlock();
	on_shmem_exit(MtmOnProcExit, 0);
}

XLogRecPtr MtmGetFlushPosition(int nodeId)
{
	return Mtm->nodes[nodeId-1].flushPos;
}

void  MtmUpdateLsnMapping(int node_id, XLogRecPtr end_lsn)
{
	dlist_mutable_iter iter;
	MtmFlushPosition* flushpos;
	XLogRecPtr local_flush = GetFlushRecPtr();
	MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);

	if (end_lsn != InvalidXLogRecPtr) {
		/* Track commit lsn */
		flushpos = (MtmFlushPosition *) palloc(sizeof(MtmFlushPosition));
		flushpos->node_id = node_id;
		flushpos->local_end = XactLastCommitEnd;
		flushpos->remote_end = end_lsn;
		dlist_push_tail(&MtmLsnMapping, &flushpos->node);
	}
	MtmLock(LW_EXCLUSIVE);
	dlist_foreach_modify(iter, &MtmLsnMapping)
	{
		flushpos = dlist_container(MtmFlushPosition, node, iter.cur);
		if (flushpos->local_end <= local_flush)
		{
			if (Mtm->nodes[node_id-1].flushPos < flushpos->remote_end) { 
				Mtm->nodes[node_id-1].flushPos = flushpos->remote_end;
			}
			dlist_delete(iter.cur);
			pfree(flushpos);
		} else { 
			break;
		}
	}
	MtmUnlock();
	MemoryContextSwitchTo(old_context);
}


static void 
MtmReplicationShutdownHook(struct PGLogicalShutdownHookArgs* args)
{
	if (MtmReplicationNodeId >= 0) { 
		MTM_LOG1("Logical replication to node %d is stopped", MtmReplicationNodeId); 
		/* MtmOnNodeDisconnect(MtmReplicationNodeId); */
		MtmReplicationNodeId = -1; /* defuse on_proc_exit hook */
	}
}

static bool 
MtmReplicationTxnFilterHook(struct PGLogicalTxnFilterArgs* args)
{
	bool res = Mtm->status != MTM_RECOVERY
		&& (args->origin_id == InvalidRepOriginId 
			|| MtmIsRecoveredNode(MtmReplicationNodeId));
	return res;
}

static bool 
MtmReplicationRowFilterHook(struct PGLogicalRowFilterArgs* args)
{
	bool isDistributed;
	MtmLock(LW_SHARED);
	if (!Mtm->localTablesHashLoaded) { 
		MtmUnlock();
		MtmLock(LW_EXCLUSIVE);
		if (!Mtm->localTablesHashLoaded) { 
			MtmLoadLocalTables();
			Mtm->localTablesHashLoaded = true;
		}
	}
	isDistributed = hash_search(MtmLocalTables, &RelationGetRelid(args->changed_rel), HASH_FIND, NULL) == NULL;
	MtmUnlock();
	return isDistributed;
}

void MtmSetupReplicationHooks(struct PGLogicalHooks* hooks)
{
	hooks->startup_hook = MtmReplicationStartupHook;
	hooks->shutdown_hook = MtmReplicationShutdownHook;
	hooks->txn_filter_hook = MtmReplicationTxnFilterHook;
	hooks->row_filter_hook = MtmReplicationRowFilterHook;
}

	

/*
 * -------------------------------------------
 * SQL API functions
 * -------------------------------------------
 */


Datum
mtm_start_replication(PG_FUNCTION_ARGS)
{
    MtmDoReplication = true;
    PG_RETURN_VOID();
}

Datum
mtm_stop_replication(PG_FUNCTION_ARGS)
{
    MtmDoReplication = false;
    MtmTx.isDistributed = false;
    PG_RETURN_VOID();
}

Datum
mtm_drop_node(PG_FUNCTION_ARGS)
{
	int nodeId = PG_GETARG_INT32(0);
	bool dropSlot = PG_GETARG_BOOL(1);
	MtmDropNode(nodeId, dropSlot);
    PG_RETURN_VOID();
}

Datum
mtm_add_node(PG_FUNCTION_ARGS)
{
	char* connStr = PG_GETARG_CSTRING(0);

	if (Mtm->nAllNodes == MtmMaxNodes) { 
		elog(ERROR, "Maximal number of nodes %d is reached", MtmMaxNodes);
	}
	if (!MtmIsBroadcast())
	{
		MtmBroadcastUtilityStmt(psprintf("select mtm.add_node('%s')", connStr), true);
	} 
	else 
	{ 
		int nodeId;
		MtmLock(LW_EXCLUSIVE);	
		nodeId = Mtm->nAllNodes;
		elog(NOTICE, "Add node %d: '%s'", nodeId+1, connStr);
		MtmUpdateNodeConnectionInfo(&Mtm->nodes[nodeId].con, connStr);
		Mtm->nodes[nodeId].transDelay = 0;
		Mtm->nodes[nodeId].lastStatusChangeTime = MtmGetSystemTime();
		Mtm->nodes[nodeId].flushPos = 0;
		Mtm->nodes[nodeId].oldestSnapshot = 0;

		BIT_SET(Mtm->disabledNodeMask, nodeId);
		Mtm->nAllNodes += 1;
		MtmUnlock();

		MtmStartReceiver(nodeId+1, true);
	}
    PG_RETURN_VOID();
}
	
Datum
mtm_poll_node(PG_FUNCTION_ARGS)
{
	int nodeId = PG_GETARG_INT32(0);
	bool nowait = PG_GETARG_BOOL(1);
	bool online = true;
	while ((nodeId == MtmNodeId && Mtm->status != MTM_ONLINE)
		   || (nodeId != MtmNodeId && BIT_CHECK(Mtm->disabledNodeMask, nodeId-1))) 
	{ 
		if (nowait) { 
			online = false;
			break;
		} else { 
			MtmSleep(STATUS_POLL_DELAY);
		}
	}
	if (!nowait) { 
		/* Just wait some time until logical repication channels will be reestablished */
		MtmSleep(MSEC_TO_USEC(MtmNodeDisableDelay));
	}
    PG_RETURN_BOOL(online);
}
	
Datum
mtm_recover_node(PG_FUNCTION_ARGS)
{
	int nodeId = PG_GETARG_INT32(0);
	MtmRecoverNode(nodeId);
    PG_RETURN_VOID();
}
	
Datum
mtm_get_snapshot(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(MtmTx.snapshot);
}

Datum
mtm_get_last_csn(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(Mtm->lastCsn);
}

Datum
mtm_get_csn(PG_FUNCTION_ARGS)
{
	TransactionId xid = PG_GETARG_INT32(0);
	MtmTransState* ts;
	csn_t csn = INVALID_CSN;

	MtmLock(LW_SHARED);
    ts = hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
    if (ts != NULL) { 
		csn = ts->csn;
	}
	MtmUnlock();

    return csn;
}

typedef struct
{
	int       nodeId;
	TupleDesc desc;
    Datum     values[Natts_mtm_nodes_state];
    bool      nulls[Natts_mtm_nodes_state];
} MtmGetNodeStateCtx;

Datum
mtm_get_nodes_state(PG_FUNCTION_ARGS)
{
    FuncCallContext* funcctx;
	MtmGetNodeStateCtx* usrfctx;
	MemoryContext oldcontext;
	int64 lag;
    bool is_first_call = SRF_IS_FIRSTCALL();

    if (is_first_call) { 
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);       
		usrfctx = (MtmGetNodeStateCtx*)palloc(sizeof(MtmGetNodeStateCtx));
		get_call_result_type(fcinfo, NULL, &usrfctx->desc);
		usrfctx->nodeId = 1;
		memset(usrfctx->nulls, false, sizeof(usrfctx->nulls));
		funcctx->user_fctx = usrfctx;
		MemoryContextSwitchTo(oldcontext);      
    }
    funcctx = SRF_PERCALL_SETUP();	
	usrfctx = (MtmGetNodeStateCtx*)funcctx->user_fctx;
	if (usrfctx->nodeId > Mtm->nAllNodes) {
		SRF_RETURN_DONE(funcctx);      
	}
	usrfctx->values[0] = Int32GetDatum(usrfctx->nodeId);
	usrfctx->values[1] = BoolGetDatum(BIT_CHECK(Mtm->disabledNodeMask, usrfctx->nodeId-1));
	usrfctx->values[2] = BoolGetDatum(BIT_CHECK(Mtm->connectivityMask, usrfctx->nodeId-1));
	usrfctx->values[3] = BoolGetDatum(BIT_CHECK(Mtm->nodeLockerMask, usrfctx->nodeId-1));
	lag = MtmGetSlotLag(usrfctx->nodeId);
	usrfctx->values[4] = Int64GetDatum(lag);
	usrfctx->nulls[4] = lag < 0;
	usrfctx->values[5] = Int64GetDatum(Mtm->transCount ? Mtm->nodes[usrfctx->nodeId-1].transDelay/Mtm->transCount : 0);
	usrfctx->values[6] = TimestampTzGetDatum(time_t_to_timestamptz(Mtm->nodes[usrfctx->nodeId-1].lastStatusChangeTime/USECS_PER_SEC));
	usrfctx->values[7] = Int64GetDatum(Mtm->nodes[usrfctx->nodeId-1].oldestSnapshot);
	usrfctx->values[8] = Int32GetDatum(Mtm->nodes[usrfctx->nodeId-1].senderPid);
	usrfctx->values[9] = TimestampTzGetDatum(time_t_to_timestamptz(Mtm->nodes[usrfctx->nodeId-1].senderStartTime/USECS_PER_SEC));
	usrfctx->values[10] = Int32GetDatum(Mtm->nodes[usrfctx->nodeId-1].receiverPid);
	usrfctx->values[11] = TimestampTzGetDatum(time_t_to_timestamptz(Mtm->nodes[usrfctx->nodeId-1].receiverStartTime/USECS_PER_SEC));   
	usrfctx->values[12] = CStringGetTextDatum(Mtm->nodes[usrfctx->nodeId-1].con.connStr);
	usrfctx->nodeId += 1;

	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(usrfctx->desc, usrfctx->values, usrfctx->nulls)));
}


Datum
mtm_get_cluster_state(PG_FUNCTION_ARGS)
{
	TupleDesc desc;
    Datum     values[Natts_mtm_cluster_state];
    bool      nulls[Natts_mtm_cluster_state] = {false};
	get_call_result_type(fcinfo, NULL, &desc);

	values[0] = CStringGetTextDatum(MtmNodeStatusMnem[Mtm->status]);
	values[1] = Int64GetDatum(Mtm->disabledNodeMask);
	values[2] = Int64GetDatum(Mtm->connectivityMask);
	values[3] = Int64GetDatum(Mtm->nodeLockerMask);
	values[4] = Int32GetDatum(Mtm->nLiveNodes);
	values[5] = Int32GetDatum(Mtm->nAllNodes);
	values[6] = Int32GetDatum((int)Mtm->pool.active);
	values[7] = Int32GetDatum((int)Mtm->pool.pending);
	values[8] = Int64GetDatum(BgwPoolGetQueueSize(&Mtm->pool));
	values[9] = Int64GetDatum(Mtm->transCount);
	values[10] = Int64GetDatum(Mtm->timeShift);
	values[11] = Int32GetDatum(Mtm->recoverySlot);
	values[12] = Int64GetDatum(hash_get_num_entries(MtmXid2State));
	values[13] = Int64GetDatum(hash_get_num_entries(MtmGid2State));
	values[14] = Int32GetDatum(Mtm->oldestXid);
	values[15] = Int32GetDatum(Mtm->nConfigChanges);

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}


typedef struct
{
	int       nodeId;
} MtmGetClusterInfoCtx;

static void erase_option_from_connstr(const char *option, char *connstr)
{
	char *needle = psprintf("%s=", option);
	while (1) {
		char *found = strstr(connstr, needle);
		if (found == NULL) break;
		while (*found != '\0' && *found != ' ') {
			*found = ' ';
			found++;
		}
	}
	pfree(needle);
}

PGconn *PQconnectdb_safe(const char *conninfo)
{
	PGconn *conn;
	char *safe_connstr = pstrdup(conninfo);
	erase_option_from_connstr("raftport", safe_connstr);
	erase_option_from_connstr("arbiterport", safe_connstr);

	conn = PQconnectdb(safe_connstr);

	pfree(safe_connstr);
	return conn;
}

Datum
mtm_get_cluster_info(PG_FUNCTION_ARGS)
{

    FuncCallContext* funcctx;
	MtmGetClusterInfoCtx* usrfctx;
	MemoryContext oldcontext;
	TupleDesc desc;
    bool is_first_call = SRF_IS_FIRSTCALL();
	int i;
	PGconn* conn;
	PGresult *result;
	char* values[Natts_mtm_cluster_state];
	HeapTuple tuple;

    if (is_first_call) { 
		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);       
		usrfctx = (MtmGetClusterInfoCtx*)palloc(sizeof(MtmGetNodeStateCtx));
		get_call_result_type(fcinfo, NULL, &desc);
		funcctx->attinmeta = TupleDescGetAttInMetadata(desc);
		usrfctx->nodeId = 0;
		funcctx->user_fctx = usrfctx;
		MemoryContextSwitchTo(oldcontext);      
    }
    funcctx = SRF_PERCALL_SETUP();	
	usrfctx = (MtmGetClusterInfoCtx*)funcctx->user_fctx;
	while (++usrfctx->nodeId <= Mtm->nAllNodes && BIT_CHECK(Mtm->disabledNodeMask, usrfctx->nodeId-1));
	if (usrfctx->nodeId > Mtm->nAllNodes) {
		SRF_RETURN_DONE(funcctx);      
	}	
	conn = PQconnectdb_safe(Mtm->nodes[usrfctx->nodeId-1].con.connStr);
	if (PQstatus(conn) != CONNECTION_OK) {
		elog(ERROR, "Failed to establish connection '%s' to node %d: error = %s", Mtm->nodes[usrfctx->nodeId-1].con.connStr, usrfctx->nodeId, PQerrorMessage(conn));
	}
	result = PQexec(conn, "select * from mtm.get_cluster_state()");

	if (PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) != 1) { 
		elog(ERROR, "Failed to receive data from %d", usrfctx->nodeId);
	}

	for (i = 0; i < Natts_mtm_cluster_state; i++) { 
		values[i] = PQgetvalue(result, 0, i);
	}
	tuple = BuildTupleFromCStrings(funcctx->attinmeta, values);
	PQclear(result);
	PQfinish(conn);
	SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
}


Datum mtm_make_table_local(PG_FUNCTION_ARGS)
{
	Oid	reloid = PG_GETARG_OID(1);
	RangeVar   *rv;
	Relation	rel;
	TupleDesc	tupDesc;
	HeapTuple	tup;
	Datum		values[Natts_mtm_local_tables];
	bool		nulls[Natts_mtm_local_tables];

	MtmMakeRelationLocal(reloid);
	
	rv = makeRangeVar(MULTIMASTER_SCHEMA_NAME, MULTIMASTER_LOCAL_TABLES_TABLE, -1);
	rel = heap_openrv(rv, RowExclusiveLock);
	if (rel != NULL) {
		char* tableName = get_rel_name(reloid);
		Oid   schemaid = get_rel_namespace(reloid);
		char* schemaName = get_namespace_name(schemaid);

		tupDesc = RelationGetDescr(rel);

		/* Form a tuple. */
		memset(nulls, false, sizeof(nulls));
		
		values[Anum_mtm_local_tables_rel_schema - 1] = CStringGetTextDatum(schemaName);
		values[Anum_mtm_local_tables_rel_name - 1] = CStringGetTextDatum(tableName);

		tup = heap_form_tuple(tupDesc, values, nulls);
		
		/* Insert the tuple to the catalog. */
		simple_heap_insert(rel, tup);
		
		/* Update the indexes. */
		CatalogUpdateIndexes(rel, tup);
		
		/* Cleanup. */
		heap_freetuple(tup);
		heap_close(rel, RowExclusiveLock);

		MtmTx.containsDML = true;
	}
	return false;
}

Datum mtm_dump_lock_graph(PG_FUNCTION_ARGS)
{
	StringInfo s = makeStringInfo();
	int i;
	for (i = 0; i < Mtm->nAllNodes; i++)
	{
		size_t size;
		char *data = RaftableGet(psprintf("lock-graph-%d", i+1), &size, NULL, false);
		if (data) { 
			GlobalTransactionId *gtid = (GlobalTransactionId *)data;
			GlobalTransactionId *last = (GlobalTransactionId *)(data + size);
			appendStringInfo(s, "node-%d lock graph: ", i+1);
			while (gtid != last) { 
				GlobalTransactionId *src = gtid++;
				appendStringInfo(s, "%d:%d -> ", src->node, src->xid);
				while (gtid->node != 0) {
					GlobalTransactionId *dst = gtid++;
					appendStringInfo(s, "%d:%d, ", dst->node, dst->xid);
				}
				gtid += 1;
			}
			appendStringInfo(s, "\n");
		}
	}
	return CStringGetTextDatum(s->data);
}

Datum mtm_inject_2pc_error(PG_FUNCTION_ARGS)
{
	Mtm->inject2PCError = PG_GETARG_INT32(0);
    PG_RETURN_VOID();
}

/*
 * -------------------------------------------
 * Broadcast utulity statements
 * -------------------------------------------
 */

/*
 * Execute statement with specified parameters and check its result
 */
static bool MtmRunUtilityStmt(PGconn* conn, char const* sql, char **errmsg)
{
	PGresult *result = PQexec(conn, sql);
	int status = PQresultStatus(result);

	bool ret = status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;

	if (!ret) {
		char *errstr = PQresultErrorMessage(result);
		int errlen = strlen(errstr);
		if (errlen > 9) { 
			*errmsg = palloc0(errlen);

			/* Strip "ERROR:  " from beginning and "\n" from end of error string */
			strncpy(*errmsg, errstr + 8, errlen - 1 - 8);
		}
	}

	PQclear(result);
	return ret;
}

static void 
MtmNoticeReceiver(void *i, const PGresult *res)
{
	char *notice = PQresultErrorMessage(res);
	char *stripped_notice;
	int len = strlen(notice);

	/* Skip notices from other nodes */
	if ( (*(int *)i) != MtmNodeId - 1)
		return;

	stripped_notice = palloc0(len);

	if (*notice == 'N')
	{
		/* Strip "NOTICE:  " from beginning and "\n" from end of error string */
		strncpy(stripped_notice, notice + 9, len - 1 - 9);
		elog(NOTICE, "%s", stripped_notice);
	}
	else if (*notice == 'W')
	{
		/* Strip "WARNING:  " from beginning and "\n" from end of error string */
		strncpy(stripped_notice, notice + 10, len - 1 - 10);
		elog(WARNING, "%s", stripped_notice);
	}
	else
	{
		stripped_notice = notice;
		elog(WARNING, "%s", stripped_notice);
	}

	MTM_LOG1("%s", stripped_notice);
	pfree(stripped_notice);
}

static void MtmBroadcastUtilityStmt(char const* sql, bool ignoreError)
{
	int i = 0;
	nodemask_t disabledNodeMask = Mtm->disabledNodeMask;
	int failedNode = -1;
	char const* errorMsg = NULL;
	PGconn **conns = palloc0(sizeof(PGconn*)*Mtm->nAllNodes);
	char* utility_errmsg;
	int nNodes = Mtm->nAllNodes;

	for (i = 0; i < nNodes; i++) 
	{ 
		if (!BIT_CHECK(disabledNodeMask, i)) 
		{
			conns[i] = PQconnectdb_safe(psprintf("%s application_name=%s", Mtm->nodes[i].con.connStr, MULTIMASTER_BROADCAST_SERVICE));
			if (PQstatus(conns[i]) != CONNECTION_OK)
			{
				if (ignoreError) 
				{ 
					PQfinish(conns[i]);
					conns[i] = NULL;
				} else { 
					failedNode = i;
					do { 
						PQfinish(conns[i]);
					} while (--i >= 0);                             
					elog(ERROR, "Failed to establish connection '%s' to node %d, error = %s", Mtm->nodes[failedNode].con.connStr, failedNode+1, PQerrorMessage(conns[i]));
				}
			}
			PQsetNoticeReceiver(conns[i], MtmNoticeReceiver, &i);
		}
	}
	Assert(i == nNodes);
    
	for (i = 0; i < nNodes; i++) 
	{ 
		if (conns[i]) 
		{
			if (!MtmRunUtilityStmt(conns[i], "BEGIN TRANSACTION", &utility_errmsg) && !ignoreError)
			{
				errorMsg = "Failed to start transaction at node %d";
				failedNode = i;
				break;
			}
			if (!MtmRunUtilityStmt(conns[i], sql, &utility_errmsg) && !ignoreError)
			{
				if (i + 1 == MtmNodeId)
					errorMsg = utility_errmsg;
				else
				{
					elog(ERROR, "%s", utility_errmsg);
					errorMsg = "Failed to run command at node %d";
				}

				failedNode = i;
				break;
			}
		}
	}
	if (failedNode >= 0 && !ignoreError)  
	{
		for (i = 0; i < nNodes; i++) 
		{ 
			if (conns[i])
			{
				MtmRunUtilityStmt(conns[i], "ROLLBACK TRANSACTION", &utility_errmsg);
			}
		}
	} else { 
		for (i = 0; i < nNodes; i++) 
		{ 
			if (conns[i] && !MtmRunUtilityStmt(conns[i], "COMMIT TRANSACTION", &utility_errmsg) && !ignoreError) 
			{ 
				errorMsg = "Commit failed at node %d";
				failedNode = i;
			}
		}
	}                       
	for (i = 0; i < nNodes; i++) 
	{ 
		if (conns[i])
		{
			PQfinish(conns[i]);
		}
	}
	if (!ignoreError && failedNode >= 0) 
	{ 
		elog(ERROR, errorMsg, failedNode+1);
	}
}

/*
 * Genenerate global transaction identifier for two-pahse commit.
 * It should be unique for all nodes
 */
static void
MtmGenerateGid(char* gid)
{
	static int localCount;
	sprintf(gid, "MTM-%d-%d-%d", MtmNodeId, MyProcPid, ++localCount);
}

static bool MtmTwoPhaseCommit(MtmCurrentTrans* x)
{
	if (MyXactAccessedTempRel)
	{
		/*
		 * XXX: this tx anyway goes to subscribers later, but without
		 * surrounding begin/commit. Now it will be filtered out on receiver side.
		 * Probably there is more clever way to do that.
		 */
		x->isDistributed = false;
		if (!MtmVolksWagenMode)
			elog(NOTICE, "MTM: Transaction was not replicated as it accesed temporary relation");
		return false;
	}

	if (!x->isReplicated && (x->isDistributed && x->containsDML)) { 
		MtmGenerateGid(x->gid);
		if (!x->isTransactionBlock) { 
			BeginTransactionBlock();
			x->isTransactionBlock = true;
			CommitTransactionCommand();
			StartTransactionCommand();
		}
		if (!PrepareTransactionBlock(x->gid))
		{
			elog(WARNING, "Failed to prepare transaction %s", x->gid);
			/* ??? Should we do explicit rollback */
		} else { 	
			CommitTransactionCommand();
			StartTransactionCommand();
			if (MtmGetCurrentTransactionStatus() == TRANSACTION_STATUS_ABORTED) { 
				FinishPreparedTransaction(x->gid, false);
				elog(ERROR, "Transaction %s is aborted by DTM", x->gid);
			} else {
				FinishPreparedTransaction(x->gid, true);
			}
		}
		return true;
	}
	return false;
}


/*
 * -------------------------------------------
 * GUC Context Handling
 * -------------------------------------------
 */

// XXX: is it defined somewhere?
#define GUC_KEY_MAXLEN 255

#define MTM_GUC_HASHSIZE 20

typedef struct MtmGucHashEntry
{
	char	key[GUC_KEY_MAXLEN];
	char   *value;
} MtmGucHashEntry;

static HTAB *MtmGucHash = NULL;

static void MtmGucHashInit(void)
{
	HASHCTL		hash_ctl;

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = GUC_KEY_MAXLEN;
	hash_ctl.entrysize = sizeof(MtmGucHashEntry);
	hash_ctl.hcxt = TopMemoryContext;
	MtmGucHash = hash_create("MtmGucHash",
						MTM_GUC_HASHSIZE,
						&hash_ctl,
						HASH_ELEM | HASH_CONTEXT);
}

static void MtmGucSet(VariableSetStmt *stmt, const char *queryStr)
{
	MemoryContext oldcontext;
	MtmGucHashEntry *hentry;
	bool found;
	char *key;

	if (!MtmGucHash)
		MtmGucHashInit();

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	switch (stmt->kind)
	{
		case VAR_SET_VALUE:
		case VAR_SET_DEFAULT:
		case VAR_SET_CURRENT:
			{
				char *value;

				key = pstrdup(stmt->name);
				hash_search(MtmGucHash, key, HASH_FIND,  &found);
				value = ExtractSetVariableArgs(stmt);

				fprintf(stderr, ":MtmGucSet: %s -> %s\n", key, value);

				if (value)
				{
					hentry = (MtmGucHashEntry *) hash_search(MtmGucHash, key,
															 HASH_ENTER,  &found);

					// if (found)
						// pfree(hentry->value);

					hentry->value = palloc(strlen(value) + 1);
					strcpy(hentry->value, value);
				}
				else if (found)
				{
					/* That was SET TO DEFAULT and we already had some value */
					hash_search(MtmGucHash, key, HASH_REMOVE, NULL);
				}
			}
			break;

		case VAR_RESET:
			{
				key = pstrdup(stmt->name);
				hash_search(MtmGucHash, key, HASH_REMOVE, NULL);
			}
			break;
		case VAR_RESET_ALL:
			break;

		case VAR_SET_MULTI:
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

static void MtmGucDiscard(DiscardStmt *stmt)
{

}

static void MtmGucClear(void)
{

}

static char * MtmGucSerialize(void)
{
	HASH_SEQ_STATUS status;
	MtmGucHashEntry *hentry;
	StringInfo serialized_gucs;

	serialized_gucs = makeStringInfo();
	appendStringInfoString(serialized_gucs, "RESET SESSION AUTHORIZATION; reset all; ");

	if (MtmGucHash)
	{
		hash_seq_init(&status, MtmGucHash);
		while ((hentry = (MtmGucHashEntry *) hash_seq_search(&status)) != NULL)
		{
			appendStringInfoString(serialized_gucs, "SET ");
			appendStringInfoString(serialized_gucs, hentry->key);
			appendStringInfoString(serialized_gucs, " TO ");
			appendStringInfoString(serialized_gucs, hentry->value);
			appendStringInfoString(serialized_gucs, "; ");
		}
	}

	return serialized_gucs->data;
}

/*
 * -------------------------------------------
 * DDL Handling
 * -------------------------------------------
 */

static bool MtmProcessDDLCommand(char const* queryString)
{
	char	   *queryWithContext;
	char	   *gucContext;

	/* Append global GUC to utility stmt. */
	gucContext = MtmGucSerialize();
	if (gucContext)
	{
		queryWithContext = palloc(strlen(gucContext) + strlen(queryString) +  1);
		strcpy(queryWithContext, gucContext);
		strcat(queryWithContext, queryString);
	}
	else
	{
		queryWithContext = (char *) queryString;
	}

	MTM_LOG1("Sending utility: %s", queryWithContext);
	LogLogicalMessage("MTM:GUC", queryWithContext, strlen(queryWithContext), true);

	MtmTx.containsDML = true;
	return false;
}

static void MtmProcessUtility(Node *parsetree, const char *queryString,
							 ProcessUtilityContext context, ParamListInfo params,
							 DestReceiver *dest, char *completionTag)
{
	bool skipCommand = false;

	MTM_LOG3("%d: Process utility statement %s", MyProcPid, queryString);
	switch (nodeTag(parsetree))
	{
	    case T_TransactionStmt:
			{
				TransactionStmt *stmt = (TransactionStmt *) parsetree;
				switch (stmt->kind)
				{					
				case TRANS_STMT_BEGIN:
  				    MtmTx.isTransactionBlock = true;
				    break;
				case TRANS_STMT_COMMIT:
  				    if (MtmTwoPhaseCommit(&MtmTx)) { 
						return;
					}
					break;
				case TRANS_STMT_PREPARE:
					elog(ERROR, "Two phase commit is not supported by multimaster");
					break;
				case TRANS_STMT_COMMIT_PREPARED:
				case TRANS_STMT_ROLLBACK_PREPARED:
  				    skipCommand = true;
					break;
				default:
					break;
				}
			}
			/* no break */
		case T_PlannedStmt:
		case T_ClosePortalStmt:
		case T_FetchStmt:
		case T_DoStmt:
		case T_CreateTableSpaceStmt:
		case T_AlterTableSpaceOptionsStmt:
		case T_TruncateStmt:
		case T_CommentStmt:
		case T_PrepareStmt:
		case T_ExecuteStmt:
		case T_DeallocateStmt:
		case T_NotifyStmt:
		case T_ListenStmt:
		case T_UnlistenStmt:
		case T_LoadStmt:
		case T_ClusterStmt:
		case T_VacuumStmt:
		case T_ExplainStmt:
		case T_VariableShowStmt:
		case T_ReassignOwnedStmt:
		case T_LockStmt:
		case T_CheckPointStmt:
		case T_ReindexStmt:
		case T_RefreshMatViewStmt:
			skipCommand = true;
			break;

		/* Save GUC context for consequent DDL execution */
		case T_DiscardStmt:
			{
				DiscardStmt *stmt = (DiscardStmt *) parsetree;

				if (!IsTransactionBlock())
				{
					skipCommand = true;
					MtmGucDiscard(stmt);
				}
			}
			break;
		case T_VariableSetStmt:
			{
				VariableSetStmt *stmt = (VariableSetStmt *) parsetree;

				/* Prevent SET TRANSACTION from replication */
				if (stmt->kind == VAR_SET_MULTI)
					skipCommand = true;

				if (stmt->kind == VAR_RESET && strcmp(stmt->name, "session_authorization") == 0)
					MtmGucClear();

				if (!IsTransactionBlock())
				{
					skipCommand = true;
					MtmGucSet(stmt, queryString);
				}
			}
			break;

		/* Copy need some special care */
	    case T_CopyStmt:
		{
			CopyStmt *copyStatement = (CopyStmt *) parsetree;
			skipCommand = true;
			if (copyStatement->is_from) { 
				RangeVar *relation = copyStatement->relation;
				
				if (relation != NULL)
				{
					Oid relid = RangeVarGetRelid(relation, NoLock, true);
					if (OidIsValid(relid))
					{
						Relation rel = heap_open(relid, ShareLock);
						if (RelationNeedsWAL(rel)) {
							MtmTx.containsDML = true;
						}	
						heap_close(rel, ShareLock);
					}
				}
			}
			break;
		}

	    default:
			skipCommand = false;
			break;
	}

	if (context == PROCESS_UTILITY_TOPLEVEL) // || context == PROCESS_UTILITY_QUERY)
	{
		if (!skipCommand && !MtmTx.isReplicated) {
			if (MtmProcessDDLCommand(queryString)) {
				return;
			}
		}
	}

	if (PreviousProcessUtilityHook != NULL)
	{
		PreviousProcessUtilityHook(parsetree, queryString, context,
								   params, dest, completionTag);
	}
	else
	{
		standard_ProcessUtility(parsetree, queryString, context,
								params, dest, completionTag);
	}
}


static void
MtmExecutorFinish(QueryDesc *queryDesc)
{
    if (MtmDoReplication) { 
        CmdType operation = queryDesc->operation;
        EState *estate = queryDesc->estate;
        if (estate->es_processed != 0 && (operation == CMD_INSERT || operation == CMD_UPDATE || operation == CMD_DELETE)) { 			
			int i;
			for (i = 0; i < estate->es_num_result_relations; i++) { 
				Relation rel = estate->es_result_relations[i].ri_RelationDesc;
				if (RelationNeedsWAL(rel)) {
					if (MtmIgnoreTablesWithoutPk) {
						if (!rel->rd_indexvalid) {
							RelationGetIndexList(rel);
						}
						if (rel->rd_replidindex == InvalidOid) { 
							MtmMakeRelationLocal(RelationGetRelid(rel));
							continue;
						}
					}
					MtmTx.containsDML = true;
					break;
				}
			}
        }
    }
    if (PreviousExecutorFinishHook != NULL)
    {
        PreviousExecutorFinishHook(queryDesc);
    }
    else
    {
        standard_ExecutorFinish(queryDesc);
    }
}        

/*
 * -------------------------------------------
 * Executor pool interface
 * -------------------------------------------
 */

void MtmExecute(void* work, int size)
{
	if (Mtm->status == MTM_RECOVERY) { 
		/* During recovery apply changes sequentially to preserve commit order */
		MtmExecutor(0, work, size);
	} else { 
		BgwPoolExecute(&Mtm->pool, work, size);
	}
}
    
static BgwPool* 
MtmPoolConstructor(void)
{
    return &Mtm->pool;
}

/*
 * -------------------------------------------
 * Deadlock detection
 * -------------------------------------------
 */

static void
MtmGetGtid(TransactionId xid, GlobalTransactionId* gtid)
{
	MtmTransState* ts;

	MtmLock(LW_SHARED);
	ts = (MtmTransState*)hash_search(MtmXid2State, &xid, HASH_FIND, NULL);
	if (ts != NULL) { 
		*gtid = ts->gtid;
	} else { 
		gtid->node = MtmNodeId;
		gtid->xid = xid;
	}
	MtmUnlock();
}


static void 
MtmSerializeLock(PROCLOCK* proclock, void* arg)
{
    ByteBuffer* buf = (ByteBuffer*)arg;
    LOCK* lock = proclock->tag.myLock;
    PGPROC* proc = proclock->tag.myProc; 
	GlobalTransactionId gtid;
    if (lock != NULL) {
        PGXACT* srcPgXact = &ProcGlobal->allPgXact[proc->pgprocno];
        
        if (TransactionIdIsValid(srcPgXact->xid) && proc->waitLock == lock) { 
            LockMethod lockMethodTable = GetLocksMethodTable(lock);
            int numLockModes = lockMethodTable->numLockModes;
            int conflictMask = lockMethodTable->conflictTab[proc->waitLockMode];
            SHM_QUEUE *procLocks = &(lock->procLocks);
            int lm;

			MtmGetGtid(srcPgXact->xid, &gtid);  /* waiting transaction */
			
            ByteBufferAppendInt32(buf, gtid.node);
            ByteBufferAppendInt32(buf, gtid.xid); 

            proclock = (PROCLOCK *) SHMQueueNext(procLocks, procLocks,
                                                 offsetof(PROCLOCK, lockLink));
            while (proclock)
            {
                if (proc != proclock->tag.myProc) { 
                    PGXACT* dstPgXact = &ProcGlobal->allPgXact[proclock->tag.myProc->pgprocno];
                    if (TransactionIdIsValid(dstPgXact->xid)) { 
                        Assert(srcPgXact->xid != dstPgXact->xid);
                        for (lm = 1; lm <= numLockModes; lm++)
                        {
                            if ((proclock->holdMask & LOCKBIT_ON(lm)) && (conflictMask & LOCKBIT_ON(lm)))
                            {
                                MTM_LOG3("%d: %u(%u) waits for %u(%u)", MyProcPid, srcPgXact->xid, proc->pid, dstPgXact->xid, proclock->tag.myProc->pid);
                                MtmGetGtid(dstPgXact->xid, &gtid); /* transaction holding lock */
								ByteBufferAppendInt32(buf, gtid.node); 
								ByteBufferAppendInt32(buf, gtid.xid); 
                                break;
                            }
                        }
                    }
                }
                proclock = (PROCLOCK *) SHMQueueNext(procLocks, &proclock->lockLink,
                                                     offsetof(PROCLOCK, lockLink));
            }
            ByteBufferAppendInt32(buf, 0); /* end of lock owners list */
            ByteBufferAppendInt32(buf, 0); /* end of lock owners list */
        }
    }
}

static bool 
MtmDetectGlobalDeadLock(PGPROC* proc)
{
    ByteBuffer buf;
    PGXACT* pgxact = &ProcGlobal->allPgXact[proc->pgprocno];
	bool hasDeadlock = false;

    if (TransactionIdIsValid(pgxact->xid)) { 
		MtmGraph graph;
		GlobalTransactionId gtid; 
		int i;
		
        ByteBufferAlloc(&buf);
        EnumerateLocks(MtmSerializeLock, &buf);
		RaftableSet(psprintf("lock-graph-%d", MtmNodeId), buf.data, buf.used, false);
		MtmGraphInit(&graph);
		MtmGraphAdd(&graph, (GlobalTransactionId*)buf.data, buf.used/sizeof(GlobalTransactionId));
        ByteBufferFree(&buf);
		for (i = 0; i < Mtm->nAllNodes; i++) { 
			if (i+1 != MtmNodeId && !BIT_CHECK(Mtm->disabledNodeMask, i)) { 
				size_t size;
				void* data = RaftableGet(psprintf("lock-graph-%d", i+1), &size, NULL, false);
				if (data == NULL) { 
					return true; /* If using Raftable is disabled */
				} else { 
					MtmGraphAdd(&graph, (GlobalTransactionId*)data, size/sizeof(GlobalTransactionId));
				}
			}
		}
		MtmGetGtid(pgxact->xid, &gtid);
		hasDeadlock = MtmGraphFindLoop(&graph, &gtid);
		elog(WARNING, "Distributed deadlock check for %u:%u = %d", gtid.node, gtid.xid, hasDeadlock);
		if (!hasDeadlock) { 
			/* There is no deadlock loop in graph, but deadlock can be caused by lack of apply workers: if all of them are busy, then some transactions
			 * can not be appied just because there are no vacant workers and it cause additional dependency between transactions which is not 
			 * refelected in lock graph 
			 */
			timestamp_t lastPeekTime = BgwGetLastPeekTime(&Mtm->pool);
			if (lastPeekTime != 0 && MtmGetSystemTime() - lastPeekTime >= MSEC_TO_USEC(DeadlockTimeout)) { 
				hasDeadlock = true;
				elog(WARNING, "Apply workers were blocked more than %d msec", 
					 (int)USEC_TO_MSEC(MtmGetSystemTime() - lastPeekTime));
			}
		}
	}
    return hasDeadlock;
}
