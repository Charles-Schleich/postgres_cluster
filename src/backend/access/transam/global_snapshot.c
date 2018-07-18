/*
 * pg_dtm.c
 *
 * Pluggable distributed transaction manager
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <time.h>

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "access/global_snapshot.h"
#include "storage/s_lock.h"
#include "storage/spin.h"
#include "storage/lmgr.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "access/xlogdefs.h"
#include "access/xact.h"
#include "access/xtm.h"
#include "access/transam.h"
#include "access/subtrans.h"
#include "access/xlog.h"
#include "access/clog.h"
#include "access/twophase.h"
#include "executor/spi.h"
#include "utils/hsearch.h"
#include <utils/guc.h>
#include "utils/tqual.h"
#include "utils/builtins.h"

#define DTM_HASH_INIT_SIZE	1000000
#define INVALID_CID    0
#define MIN_WAIT_TIMEOUT 1000
#define MAX_WAIT_TIMEOUT 100000
#define HASH_PER_ELEM_OVERHEAD 64

#define USEC 1000000

#define TRACE_SLEEP_TIME 1

typedef uint64 timestamp_t;

/* Distributed transaction state kept in shared memory */
typedef struct DtmTransStatus
{
	TransactionId xid;
	XidStatus	status;
	int			nSubxids;
	cid_t		cid;			/* CSN */
	struct DtmTransStatus *next;/* pointer to next element in finished
								 * transaction list */
}	DtmTransStatus;

/* State of DTM node */
typedef struct
{
	cid_t		cid;			/* last assigned CSN; used to provide unique
								 * ascending CSNs */
	TransactionId oldest_xid;	/* XID of oldest transaction visible by any
								 * active transaction (local or global) */
	long		time_shift;		/* correction to system time */
	volatile slock_t lock;		/* spinlock to protect access to hash table  */
	DtmTransStatus *trans_list_head;	/* L1 list of finished transactions
										 * present in xid2status hash table.
										 * This list is used to perform
										 * cleanup of too old transactions */
	DtmTransStatus **trans_list_tail;
}	DtmNodeState;

/* Structure used to map global transaction identifier to XID */
typedef struct
{
	char		gtid[MAX_GTID_SIZE];
	TransactionId xid;
	TransactionId *subxids;
	int			nSubxids;
}	DtmTransId;


#define DTM_TRACE(x)
/* #define DTM_TRACE(x) fprintf x */

// static shmem_startup_hook_type prev_shmem_startup_hook;
static HTAB *xid2status;
static HTAB *gtid2xid;
static DtmNodeState *local;
static uint64 totalSleepInterrupts;
static int	DtmVacuumDelay = 2; /* sec */
static bool DtmRecordCommits = 0;

DtmCurrentTrans dtm_tx; // XXXX: make static

static Snapshot DtmGetSnapshot(Snapshot snapshot);
static TransactionId DtmGetOldestXmin(Relation rel, int flags);
static bool DtmXidInMVCCSnapshot(TransactionId xid, Snapshot snapshot);
static void DtmAdjustOldestXid(void);
static bool DtmDetectGlobalDeadLock(PGPROC *proc);
static void DtmAddSubtransactions(DtmTransStatus * ts, TransactionId *subxids, int nSubxids);
static char const *DtmGetName(void);
static size_t DtmGetTransactionStateSize(void);
static void DtmSerializeTransactionState(void* ctx);
static void DtmDeserializeTransactionState(void* ctx);


static TransactionManager DtmTM = {
	PgTransactionIdGetStatus,
	PgTransactionIdSetTreeStatus,
	DtmGetSnapshot,
	PgGetNewTransactionId,
	DtmGetOldestXmin,
	PgTransactionIdIsInProgress,
	PgGetGlobalTransactionId,
	DtmXidInMVCCSnapshot,
	DtmDetectGlobalDeadLock,
	DtmGetName,
	DtmGetTransactionStateSize,
	DtmSerializeTransactionState,
	DtmDeserializeTransactionState,
	PgInitializeSequence
};

void		_PG_init(void);
void		_PG_fini(void);


// static void dtm_shmem_startup(void);
static void dtm_xact_callback(XactEvent event, void *arg);
static timestamp_t dtm_get_current_time();
static void dtm_sleep(timestamp_t interval);
static cid_t dtm_get_cid();
static cid_t dtm_sync(cid_t cid);

/*
 *	Time manipulation functions
 */

/* Get current time with microscond resolution */
static timestamp_t
dtm_get_current_time()
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (timestamp_t) tv.tv_sec * USEC + tv.tv_usec + local->time_shift;
}

/* Sleep for specified amount of time */
static void
dtm_sleep(timestamp_t interval)
{
	timestamp_t waketm = dtm_get_current_time() + interval;
	while (1948)
	{
		timestamp_t sleepfor = waketm - dtm_get_current_time();

		pg_usleep(sleepfor);
		if (dtm_get_current_time() < waketm)
		{
			totalSleepInterrupts += 1;
			Assert(errno == EINTR);
			continue;
		}
		break;
	}
}

/* Get unique ascending CSN.
 * This function is called inside critical section
 */
static cid_t
dtm_get_cid()
{
	cid_t		cid = dtm_get_current_time();

	if (cid <= local->cid)
	{
		cid = ++local->cid;
	}
	else
	{
		local->cid = cid;
	}
	return cid;
}

/*
 * Adjust system time
 */
static cid_t
dtm_sync(cid_t global_cid)
{
	cid_t		local_cid;

	while ((local_cid = dtm_get_cid()) < global_cid)
	{
		local->time_shift += global_cid - local_cid;
	}
	return local_cid;
}

/*
 * Estimate shared memory space needed.
 */
Size
GlobalSnapshotShmemSize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(DtmNodeState));
	size = add_size(size, (sizeof(DtmTransId) + sizeof(DtmTransStatus) + HASH_PER_ELEM_OVERHEAD * 2) * DTM_HASH_INIT_SIZE);

	return size;
}

static void
dtm_xact_callback(XactEvent event, void *arg)
{
	DTM_TRACE((stderr, "Backend %d dtm_xact_callback %d\n", getpid(), event));
	switch (event)
	{
		case XACT_EVENT_START:
			DtmLocalBegin(&dtm_tx);
			break;

		case XACT_EVENT_ABORT:
			DtmLocalAbort(&dtm_tx);
			DtmLocalEnd(&dtm_tx);
			break;

		case XACT_EVENT_COMMIT:
			DtmLocalCommit(&dtm_tx);
			DtmLocalEnd(&dtm_tx);
			break;

		case XACT_EVENT_ABORT_PREPARED:
			DtmLocalAbortPrepared(&dtm_tx);
			break;

		case XACT_EVENT_COMMIT_PREPARED:
			DtmLocalCommitPrepared(&dtm_tx);
			break;

		case XACT_EVENT_PRE_PREPARE:
			DtmLocalSavePreparedState(&dtm_tx);
			DtmLocalEnd(&dtm_tx);
			break;

		default:
			break;
	}
}

/*
 *	***************************************************************************
 */

static uint32
dtm_xid_hash_fn(const void *key, Size keysize)
{
	return (uint32) *(TransactionId *) key;
}

static int
dtm_xid_match_fn(const void *key1, const void *key2, Size keysize)
{
	return *(TransactionId *) key1 - *(TransactionId *) key2;
}

static uint32
dtm_gtid_hash_fn(const void *key, Size keysize)
{
	GlobalTransactionId id = (GlobalTransactionId) key;
	uint32		h = 0;

	while (*id != 0)
	{
		h = h * 31 + *id++;
	}
	return h;
}

static void *
dtm_gtid_keycopy_fn(void *dest, const void *src, Size keysize)
{
	return strcpy((char *) dest, (GlobalTransactionId) src);
}

static int
dtm_gtid_match_fn(const void *key1, const void *key2, Size keysize)
{
	return strcmp((GlobalTransactionId) key1, (GlobalTransactionId) key2);
}

static char const *
DtmGetName(void)
{
	return "pg_tsdtm";
}

static void
DtmTransactionListAppend(DtmTransStatus * ts)
{
	ts->next = NULL;
	*local->trans_list_tail = ts;
	local->trans_list_tail = &ts->next;
}

static void
DtmTransactionListInsertAfter(DtmTransStatus * after, DtmTransStatus * ts)
{
	ts->next = after->next;
	after->next = ts;
	if (local->trans_list_tail == &after->next)
	{
		local->trans_list_tail = &ts->next;
	}
}

/*
 * There can be different oldest XIDs at different cluster node.
 * Seince we do not have centralized aribiter, we have to rely in DtmVacuumDelay.
 * This function takes XID which PostgreSQL consider to be the latest and try to find XID which
 * is older than it more than DtmVacuumDelay.
 * If no such XID can be located, then return previously observed oldest XID
 */
static void
DtmAdjustOldestXid()
{
	DtmTransStatus *ts,
				*prev = NULL;

	timestamp_t cutoff_time = dtm_get_current_time() - DtmVacuumDelay * USEC;
	int total = 0, deleted = 0;

	SpinLockAcquire(&local->lock);

	for (ts = local->trans_list_head; ts != NULL; ts = ts->next)
		total++;

	for (ts = local->trans_list_head; ts != NULL && ts->cid < cutoff_time; prev = ts, ts = ts->next)
	{
		if (prev != NULL)
		{
			hash_search(xid2status, &prev->xid, HASH_REMOVE, NULL);
			deleted++;
		}
	}

	if (prev != NULL)
		local->trans_list_head = prev;

	if (ts != NULL)
		local->oldest_xid = ts->xid;
	else
		local->oldest_xid = InvalidTransactionId;

	SpinLockRelease(&local->lock);

	// elog(LOG, "DtmAdjustOldestXid total=%d, deleted=%d, xid=%d, prev=%p, ts=%p", total, deleted, local->oldest_xid, prev, ts);
}

Snapshot
DtmGetSnapshot(Snapshot snapshot)
{
	snapshot = PgGetSnapshotData(snapshot);
	// RecentGlobalDataXmin = RecentGlobalXmin = DtmAdjustOldestXid(RecentGlobalDataXmin);
	SpinLockAcquire(&local->lock);

	if (TransactionIdIsValid(local->oldest_xid) &&
		TransactionIdPrecedes(local->oldest_xid, RecentGlobalXmin))
		RecentGlobalXmin = local->oldest_xid;

	if (TransactionIdIsValid(local->oldest_xid) &&
		TransactionIdPrecedes(local->oldest_xid, RecentGlobalDataXmin))
		RecentGlobalDataXmin = local->oldest_xid;

	SpinLockRelease(&local->lock);
	return snapshot;
}

TransactionId
DtmGetOldestXmin(Relation rel, int flags)
{
	TransactionId xmin = PgGetOldestXmin(rel, flags);

	// xmin = DtmAdjustOldestXid(xmin);

	SpinLockAcquire(&local->lock);

	if (TransactionIdIsValid(local->oldest_xid) &&
		TransactionIdPrecedes(local->oldest_xid, xmin))
		xmin = local->oldest_xid;

	SpinLockRelease(&local->lock);

	return xmin;
}

/*
 * Check tuple bisibility based on CSN of current transaction.
 * If there is no niformation about transaction with this XID, then use standard PostgreSQL visibility rules.
 */
bool
DtmXidInMVCCSnapshot(TransactionId xid, Snapshot snapshot)
{
	timestamp_t delay = MIN_WAIT_TIMEOUT;

	Assert(xid != InvalidTransactionId);

	SpinLockAcquire(&local->lock);

	while (true)
	{
		DtmTransStatus *ts = (DtmTransStatus *) hash_search(xid2status, &xid, HASH_FIND, NULL);

		if (ts != NULL)
		{
			if (ts->cid > dtm_tx.snapshot)
			{
				DTM_TRACE((stderr, "%d: tuple with xid=%d(csn=%lld) is invisibile in snapshot %lld\n",
						   getpid(), xid, ts->cid, dtm_tx.snapshot));
				SpinLockRelease(&local->lock);
				return true;
			}
			if (ts->status == TRANSACTION_STATUS_IN_PROGRESS)
			{
				DTM_TRACE((stderr, "%d: wait for in-doubt transaction %u in snapshot %lu\n", getpid(), xid, dtm_tx.snapshot));
				SpinLockRelease(&local->lock);

				dtm_sleep(delay);

				if (delay * 2 <= MAX_WAIT_TIMEOUT)
					delay *= 2;
				SpinLockAcquire(&local->lock);
			}
			else
			{
				bool		invisible = ts->status == TRANSACTION_STATUS_ABORTED;

				DTM_TRACE((stderr, "%d: tuple with xid=%d(csn= %lld) is %s in snapshot %lld\n",
						   getpid(), xid, ts->cid, invisible ? "rollbacked" : "committed", dtm_tx.snapshot));
				SpinLockRelease(&local->lock);
				return invisible;
			}
		}
		else
		{
			DTM_TRACE((stderr, "%d: visibility check is skept for transaction %u in snapshot %lu\n", getpid(), xid, dtm_tx.snapshot));
			break;
		}
	}
	SpinLockRelease(&local->lock);
	return PgXidInMVCCSnapshot(xid, snapshot);
}

void
DtmInitialize()
{
	bool		found;
	static HASHCTL info;

	info.keysize = sizeof(TransactionId);
	info.entrysize = sizeof(DtmTransStatus);
	info.hash = dtm_xid_hash_fn;
	info.match = dtm_xid_match_fn;
	xid2status = ShmemInitHash("xid2status",
							   DTM_HASH_INIT_SIZE, DTM_HASH_INIT_SIZE,
							   &info,
							   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	info.keysize = MAX_GTID_SIZE;
	info.entrysize = sizeof(DtmTransId);
	info.hash = dtm_gtid_hash_fn;
	info.match = dtm_gtid_match_fn;
	info.keycopy = dtm_gtid_keycopy_fn;
	gtid2xid = ShmemInitHash("gtid2xid",
							 DTM_HASH_INIT_SIZE, DTM_HASH_INIT_SIZE,
							 &info,
					HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_KEYCOPY);

	TM = &DtmTM;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	local = (DtmNodeState *) ShmemInitStruct("dtm", sizeof(DtmNodeState), &found);
	if (!found)
	{
		local->time_shift = 0;
		local->oldest_xid = FirstNormalTransactionId;
		local->cid = dtm_get_current_time();
		local->trans_list_head = NULL;
		local->trans_list_tail = &local->trans_list_head;
		SpinLockInit(&local->lock);
		RegisterXactCallback(dtm_xact_callback, NULL);
	}
	LWLockRelease(AddinShmemInitLock);
}

/*
 * Start transaction at local node.
 * Associate local snapshot (current time) with this transaction.
 */
void
DtmLocalBegin(DtmCurrentTrans * x)
{
	if (!TransactionIdIsValid(x->xid))
	{
		SpinLockAcquire(&local->lock);
		// x->xid = GetCurrentTransactionIdIfAny();
		x->cid = INVALID_CID;
		x->is_global = false;
		x->is_prepared = false;
		x->snapshot = dtm_get_cid();
		SpinLockRelease(&local->lock);
		DTM_TRACE((stderr, "DtmLocalBegin: transaction %u uses local snapshot %lu\n", x->xid, x->snapshot));
	}
}

/*
 * Transaction is going to be distributed.
 * Returns snapshot of current transaction.
 */
cid_t
DtmLocalExtend(DtmCurrentTrans * x, GlobalTransactionId gtid)
{
	if (gtid != NULL)
	{
		SpinLockAcquire(&local->lock);
		{
			DtmTransId *id = (DtmTransId *) hash_search(gtid2xid, gtid, HASH_ENTER, NULL);

			id->xid = x->xid;
			id->nSubxids = 0;
			id->subxids = 0;
		}
		strncpy(x->gtid, gtid, MAX_GTID_SIZE);
		SpinLockRelease(&local->lock);
	}
	x->is_global = true;
	return x->snapshot;
}

/*
 * This function is executed on all nodes joining distributed transaction.
 * global_cid is snapshot taken from node initiated this transaction
 */
cid_t
DtmLocalAccess(DtmCurrentTrans * x, GlobalTransactionId gtid, cid_t global_cid)
{
	cid_t		local_cid;

	SpinLockAcquire(&local->lock);
	{
		if (gtid != NULL)
		{
			DtmTransId *id = (DtmTransId *) hash_search(gtid2xid, gtid, HASH_ENTER, NULL);

			id->xid = x->xid;
			id->nSubxids = 0;
			id->subxids = 0;
		}
		local_cid = dtm_sync(global_cid);
		x->snapshot = global_cid;
		x->is_global = true;
	}
	strncpy(x->gtid, gtid, MAX_GTID_SIZE);
	SpinLockRelease(&local->lock);
	if (global_cid < local_cid - DtmVacuumDelay * USEC)
	{
		elog(ERROR, "Too old snapshot: requested %ld, current %ld", global_cid, local_cid);
	}
	return global_cid;
}

/*
 * Set transaction status to in-doubt. Now all transactions accessing tuples updated by this transaction have to
 * wait until it is either committed either aborted
 */
void
DtmLocalBeginPrepare(GlobalTransactionId gtid)
{
	SpinLockAcquire(&local->lock);
	{
		DtmTransStatus *ts;
		DtmTransId *id;

		id = (DtmTransId *) hash_search(gtid2xid, gtid, HASH_FIND, NULL);
		Assert(id != NULL);
		Assert(TransactionIdIsValid(id->xid));
		ts = (DtmTransStatus *) hash_search(xid2status, &id->xid, HASH_ENTER, NULL);
		ts->status = TRANSACTION_STATUS_IN_PROGRESS;
		ts->cid = dtm_get_cid();
		ts->nSubxids = id->nSubxids;
		DtmTransactionListAppend(ts);
		DtmAddSubtransactions(ts, id->subxids, id->nSubxids);
	}
	SpinLockRelease(&local->lock);
}

/*
 * Choose maximal CSN among all nodes.
 * This function returns maximum of passed (global) and local (current time) CSNs.
 */
cid_t
DtmLocalPrepare(GlobalTransactionId gtid, cid_t global_cid)
{
	cid_t		local_cid;

	SpinLockAcquire(&local->lock);
	local_cid = dtm_get_cid();
	if (local_cid > global_cid)
	{
		global_cid = local_cid;
	}
	SpinLockRelease(&local->lock);
	return global_cid;
}

/*
 * Adjust system time according to the received maximal CSN
 */
void
DtmLocalEndPrepare(GlobalTransactionId gtid, cid_t cid)
{
	SpinLockAcquire(&local->lock);
	{
		DtmTransStatus *ts;
		DtmTransId *id;
		int			i;

		id = (DtmTransId *) hash_search(gtid2xid, gtid, HASH_FIND, NULL);

		ts = (DtmTransStatus *) hash_search(xid2status, &id->xid, HASH_FIND, NULL);
		Assert(ts != NULL);
		ts->cid = cid;
		for (i = 0; i < ts->nSubxids; i++)
		{
			ts = ts->next;
			ts->cid = cid;
		}
		dtm_sync(cid);

		DTM_TRACE((stderr, "Prepare transaction %u(%s) with CSN %lu\n", id->xid, gtid, cid));
	}
	SpinLockRelease(&local->lock);

	/*
	 * Record commit in pg_committed_xact table to be make it possible to
	 * perform recovery in case of crash of some of cluster nodes
	 */
	if (DtmRecordCommits)
	{
		char		stmt[MAX_GTID_SIZE + 64];
		int			rc;

		sprintf(stmt, "insert into pg_committed_xacts values ('%s')", gtid);
		SPI_connect();
		rc = SPI_execute(stmt, true, 0);
		SPI_finish();
		if (rc != SPI_OK_INSERT)
		{
			elog(ERROR, "Failed to insert GTID %s in table pg_committed_xacts", gtid);
		}
	}
}

/*
 * Mark tranasction as prepared
 */
void
DtmLocalCommitPrepared(DtmCurrentTrans * x)
{
	if (!x->gtid[0])
		return;

	Assert(x->gtid != NULL);

	SpinLockAcquire(&local->lock);
	{
		DtmTransId *id = (DtmTransId *) hash_search(gtid2xid, x->gtid, HASH_REMOVE, NULL);

		Assert(id != NULL);

		x->is_global = true;
		x->is_prepared = true;
		x->xid = id->xid;
		free(id->subxids);

		DTM_TRACE((stderr, "Global transaction %u(%s) is precommitted\n", x->xid, gtid));
	}
	SpinLockRelease(&local->lock);

	DtmAdjustOldestXid();
	// elog(LOG, "DtmLocalCommitPrepared %d", x->xid);
}

/*
 * Set transaction status to committed
 */
void
DtmLocalCommit(DtmCurrentTrans * x)
{
	// if (!x->is_global)
	// 	return;

	SpinLockAcquire(&local->lock);
	if (TransactionIdIsValid(x->xid))
	{
		bool		found;
		DtmTransStatus *ts;

		ts = (DtmTransStatus *) hash_search(xid2status, &x->xid, HASH_ENTER, &found);
		ts->status = TRANSACTION_STATUS_COMMITTED;
		if (x->is_prepared)
		{
			int			i;
			DtmTransStatus *sts = ts;

			Assert(found);
			Assert(x->is_global);
			for (i = 0; i < ts->nSubxids; i++)
			{
				sts = sts->next;
				Assert(sts->cid == ts->cid);
				sts->status = TRANSACTION_STATUS_COMMITTED;
			}
		}
		else
		{
			TransactionId *subxids;

			Assert(!found);
			ts->cid = dtm_get_cid();
			DtmTransactionListAppend(ts);
			ts->nSubxids = xactGetCommittedChildren(&subxids);
			DtmAddSubtransactions(ts, subxids, ts->nSubxids);
		}
		x->cid = ts->cid;
		DTM_TRACE((stderr, "Local transaction %u is committed at %lu\n", x->xid, x->cid));
	}
	SpinLockRelease(&local->lock);

	DtmAdjustOldestXid();
	// elog(LOG, "DtmLocalCommit %d", x->xid);
}

/*
 * Mark tranasction as prepared
 */
void
DtmLocalAbortPrepared(DtmCurrentTrans * x)
{
	if (!x->gtid[0])
		return;

	Assert(x->gtid != NULL);

	SpinLockAcquire(&local->lock);
	{
		DtmTransId *id = (DtmTransId *) hash_search(gtid2xid, x->gtid, HASH_REMOVE, NULL);

		Assert(id != NULL);
		x->is_global = true;
		x->is_prepared = true;
		x->xid = id->xid;
		free(id->subxids);
		DTM_TRACE((stderr, "Global transaction %u(%s) is preaborted\n", x->xid, gtid));
	}
	SpinLockRelease(&local->lock);
}

/*
 * Set transaction status to aborted
 */
void
DtmLocalAbort(DtmCurrentTrans * x)
{
	if (!TransactionIdIsValid(x->xid))
		return;

	SpinLockAcquire(&local->lock);
	{
		bool		found;
		DtmTransStatus *ts;

		Assert(TransactionIdIsValid(x->xid));
		ts = (DtmTransStatus *) hash_search(xid2status, &x->xid, HASH_ENTER, &found);
		if (x->is_prepared)
		{
			Assert(found);
			Assert(x->is_global);
		}
		else
		{
			Assert(!found);
			ts->cid = dtm_get_cid();
			ts->nSubxids = 0;
			DtmTransactionListAppend(ts);
		}
		x->cid = ts->cid;
		ts->status = TRANSACTION_STATUS_ABORTED;
		DTM_TRACE((stderr, "Local transaction %u is aborted at %lu\n", x->xid, x->cid));
	}
	SpinLockRelease(&local->lock);
}

/*
 * Cleanup dtm_tx structure
 */
void
DtmLocalEnd(DtmCurrentTrans * x)
{
	x->is_global = false;
	x->is_prepared = false;
	x->xid = InvalidTransactionId;
	x->cid = INVALID_CID;
}

/*
 * Now only timestapm based dealock detection is supported for pg_tsdtm.
 * Please adjust "deadlock_timeout" parameter in postresql.conf to avoid false
 * deadlock detection.
 */
bool
DtmDetectGlobalDeadLock(PGPROC *proc)
{
	elog(WARNING, "Global deadlock?");
	return true;
}

static size_t
DtmGetTransactionStateSize(void)
{
	return sizeof(dtm_tx);
}

static void
DtmSerializeTransactionState(void* ctx)
{
	memcpy(ctx, &dtm_tx, sizeof(dtm_tx));
}

static void
DtmDeserializeTransactionState(void* ctx)
{
	memcpy(&dtm_tx, ctx, sizeof(dtm_tx));
}


cid_t
DtmGetCsn(TransactionId xid)
{
	cid_t		csn = 0;

	SpinLockAcquire(&local->lock);
	{
		DtmTransStatus *ts = (DtmTransStatus *) hash_search(xid2status, &xid, HASH_FIND, NULL);

		if (ts != NULL)
		{
			csn = ts->cid;
		}
	}
	SpinLockRelease(&local->lock);
	return csn;
}

/*
 * Save state of parepared transaction
 */
void
DtmLocalSavePreparedState(DtmCurrentTrans * x)
{
	// x->is_prepared = true;

	if (x->gtid[0])
	{
		SpinLockAcquire(&local->lock);
		{
			DtmTransId *id = (DtmTransId *) hash_search(gtid2xid, x->gtid, HASH_FIND, NULL);

			if (id != NULL)
			{
				TransactionId *subxids;
				int			nSubxids = xactGetCommittedChildren(&subxids);

				id->xid = GetCurrentTransactionId();
				if (nSubxids != 0)
				{
					id->subxids = (TransactionId *) malloc(nSubxids * sizeof(TransactionId));
					id->nSubxids = nSubxids;
					memcpy(id->subxids, subxids, nSubxids * sizeof(TransactionId));
				}
			}
		}
		SpinLockRelease(&local->lock);
	}
}

/*
 * Add subtransactions to finished transactions list.
 * Copy CSN and status of parent transaction.
 */
static void
DtmAddSubtransactions(DtmTransStatus * ts, TransactionId *subxids, int nSubxids)
{
	int			i;

	for (i = 0; i < nSubxids; i++)
	{
		bool		found;
		DtmTransStatus *sts;

		Assert(TransactionIdIsValid(subxids[i]));
		sts = (DtmTransStatus *) hash_search(xid2status, &subxids[i], HASH_ENTER, &found);
		Assert(!found);
		sts->status = ts->status;
		sts->cid = ts->cid;
		sts->nSubxids = 0;
		DtmTransactionListInsertAfter(ts, sts);
	}
}


/*
 *
 * SQL functions for global snapshot mamagement.
 *
 */

Datum
pg_global_snaphot_create(PG_FUNCTION_ARGS)
{
	GlobalTransactionId gtid = text_to_cstring(PG_GETARG_TEXT_PP(0));
	cid_t		cid = DtmLocalExtend(&dtm_tx, gtid);

	DTM_TRACE((stderr, "Backend %d extends transaction %u(%s) to global with cid=%lu\n", getpid(), dtm_tx.xid, gtid, cid));
	PG_RETURN_INT64(cid);
}

Datum
pg_global_snaphot_join(PG_FUNCTION_ARGS)
{
	cid_t		cid = PG_GETARG_INT64(0);
	GlobalTransactionId gtid = text_to_cstring(PG_GETARG_TEXT_PP(1));

	DTM_TRACE((stderr, "Backend %d joins transaction %u(%s) with cid=%lu\n", getpid(), dtm_tx.xid, gtid, cid));
	cid = DtmLocalAccess(&dtm_tx, gtid, cid);
	PG_RETURN_INT64(cid);
}

Datum
pg_global_snaphot_begin_prepare(PG_FUNCTION_ARGS)
{
	GlobalTransactionId gtid = text_to_cstring(PG_GETARG_TEXT_PP(0));

	DtmLocalBeginPrepare(gtid);
	DTM_TRACE((stderr, "Backend %d begins prepare of transaction %s\n", getpid(), gtid));
	PG_RETURN_VOID();
}

Datum
pg_global_snaphot_prepare(PG_FUNCTION_ARGS)
{
	GlobalTransactionId gtid = text_to_cstring(PG_GETARG_TEXT_PP(0));
	cid_t		cid = PG_GETARG_INT64(1);

	cid = DtmLocalPrepare(gtid, cid);
	DTM_TRACE((stderr, "Backend %d prepares transaction %s with cid=%lu\n", getpid(), gtid, cid));
	PG_RETURN_INT64(cid);
}

Datum
pg_global_snaphot_end_prepare(PG_FUNCTION_ARGS)
{
	GlobalTransactionId gtid = text_to_cstring(PG_GETARG_TEXT_PP(0));
	cid_t		cid = PG_GETARG_INT64(1);

	DTM_TRACE((stderr, "Backend %d ends prepare of transactions %s with cid=%lu\n", getpid(), gtid, cid));
	DtmLocalEndPrepare(gtid, cid);
	PG_RETURN_VOID();
}
