/*-------------------------------------------------------------------------
 *
 * freelist.c
 *	  routines for managing the buffer pool's replacement strategy.
 *
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/storage/buffer/freelist.c,v 1.67 2009/06/22 20:04:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/buf_internals.h"
#include "storage/bufmgr.h"
#include <string.h>

/*
 * CS186: This variable stores the current replacement policy in
 * use. It can have one of three possible values: POLICY_CLOCK,
 * POLICY_LRU, POLICY_MRU, or POLICY_2Q.
 */
int BufferReplacementPolicy;

int NextMRUBufIndex();
/*
 * CS186: Shared freelist control information. This is a data
 * structure that is kept in shared memory, and holds information used
 * by the replacement policy to decide buffer frame to replace.
 */
typedef struct
{
	/* Control information for the clock-based replacement policy */

	/* Clock sweep hand: index of next buffer to consider grabbing */
	int			nextVictimBuffer;

	int			firstFreeBuffer;	/* Head of list of unused buffers */
	int			lastFreeBuffer; /* Tail of list of unused buffers */

	/*
	 * NOTE: lastFreeBuffer is undefined when firstFreeBuffer is -1 (that is,
	 * when the list is empty)
	 */

	/*
	 * Statistics.	These counters should be wide enough that they can't
	 * overflow during a single bgwriter cycle.
	 */
	uint32		completePasses; /* Complete cycles of the clock sweep */
	uint32		numBufferAllocs;	/* Buffers allocated since last reset */

	/*
	 * CS186 TODO: Add any data you need to manage in order to implement
	 * your buffer replacement strategies here.
	 */

  int LRUBuffer;
  int MRUBuffer;
  int listCount;

} BufferStrategyControl;

/* Pointers to shared state */
static BufferStrategyControl *StrategyControl = NULL;

/*
 * Private (non-shared) state for managing a ring of shared buffers to re-use.
 * This is currently the only kind of BufferAccessStrategy object, but someday
 * we might have more kinds.
 */
typedef struct BufferAccessStrategyData
{
	/* Overall strategy type */
	BufferAccessStrategyType btype;
	/* Number of elements in buffers[] array */
	int			ring_size;

	/*
	 * Index of the "current" slot in the ring, ie, the one most recently
	 * returned by GetBufferFromRing.
	 */
	int			current;

	/*
	 * True if the buffer just returned by StrategyGetBuffer had been in the
	 * ring already.
	 */
	bool		current_was_in_ring;

	/*
	 * Array of buffer numbers.	 InvalidBuffer (that is, zero) indicates we
	 * have not yet selected a buffer for this ring slot.  For allocation
	 * simplicity this is palloc'd together with the fixed fields of the
	 * struct.
	 */
	Buffer		buffers[1];		/* VARIABLE SIZE ARRAY */
} BufferAccessStrategyData;


/* Prototypes for internal functions */
static volatile BufferDesc *GetBufferFromRing(BufferAccessStrategy strategy);
static void AddBufferToRing(BufferAccessStrategy strategy,
				volatile BufferDesc *buf);


/*
 * StrategyGetBuffer
 *
 *	Called by the bufmgr to get the next candidate buffer to use in
 *	BufferAlloc(). The only hard requirement BufferAlloc() has is that
 *	the selected buffer must not currently be pinned by anyone.
 *
 *	strategy is a BufferAccessStrategy object, or NULL for default strategy.
 *
 *	To ensure that no one else can pin the buffer before we do, we must
 *	return the buffer with the buffer header spinlock still held.  If
 *	*lock_held is set on exit, we have returned with the BufFreelistLock
 *	still held, as well; the caller must release that lock once the spinlock
 *	is dropped.	 We do it that way because releasing the BufFreelistLock
 *	might awaken other processes, and it would be bad to do the associated
 *	kernel calls while holding the buffer header spinlock.
 */
volatile BufferDesc *
StrategyGetBuffer(BufferAccessStrategy strategy, bool *lock_held)
{
	volatile BufferDesc *buf;
	volatile int bufIndex = -1;
	volatile int resultIndex = -1;
	int			trycounter;

	/* Lock the freelist */
	*lock_held = true;
	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);

	/*
	 * We count buffer allocation requests so that the bgwriter can estimate
	 * the rate of buffer consumption.
	 */
	StrategyControl->numBufferAllocs++;

	/*
	 * Try to get a buffer from the freelist.  Note that the freeNext fields
	 * are considered to be protected by the BufFreelistLock not the
	 * individual buffer spinlocks, so it's OK to manipulate them without
	 * holding the spinlock.
	 */
	while (StrategyControl->firstFreeBuffer >= 0)
	{
		bufIndex = StrategyControl->firstFreeBuffer;
		buf = &BufferDescriptors[bufIndex];
		Assert(buf->freeNext != FREENEXT_NOT_IN_LIST);

		/* Unconditionally remove buffer from freelist */
		StrategyControl->firstFreeBuffer = buf->freeNext;
		buf->freeNext = FREENEXT_NOT_IN_LIST;

		/*
		 * If the buffer is pinned or has a nonzero usage_count, we cannot use
		 * it; discard it and retry.  (This can only happen if VACUUM put a
		 * valid buffer in the freelist and then someone else used it before
		 * we got to it.  It's probably impossible altogether as of 8.3, but
		 * we'd better check anyway.)
		 */
		LockBufHdr(buf);
		if (buf->refcount == 0 && buf->usage_count == 0)
		{
			resultIndex = bufIndex;
			break;
		}
		UnlockBufHdr(buf);
	}

	/*
	 * Nothing on the freelist, so use the buffer replacement policy
	 * to select a buffer to evict.
	 */
	if (resultIndex == -1)
	{
		if (BufferReplacementPolicy == POLICY_CLOCK)
		{
			/* Run the "clock sweep" algorithm */
			trycounter = NBuffers;
			for (;;)
			{
				bufIndex = StrategyControl->nextVictimBuffer;
				buf = &BufferDescriptors[bufIndex];

				/*
				 * If the clock sweep hand has reached the end of the
				 * buffer pool, start back at the beginning.
				 */
				if (++StrategyControl->nextVictimBuffer >= NBuffers)
				{
					StrategyControl->nextVictimBuffer = 0;
					StrategyControl->completePasses++;
				}

				/*
				 * If the buffer is pinned or has a nonzero usage_count, we cannot use
				 * it; decrement the usage_count (unless pinned) and keep scanning.
				 */
				LockBufHdr(buf);
				if (buf->refcount == 0)
				{
					if (buf->usage_count > 0)
					{
						buf->usage_count--;
						trycounter = NBuffers;
					}
					else
					{
						/* Found a usable buffer */
						resultIndex = bufIndex;
						break;
					}
				}
				else if (--trycounter == 0)
				{
					/*
					 * We've scanned all the buffers without making any state changes,
					 * so all the buffers are pinned (or were when we looked at them).
					 * We could hope that someone will free one eventually, but it's
					 * probably better to fail than to risk getting stuck in an
					 * infinite loop.
					 */
					UnlockBufHdr(buf);
					elog(ERROR, "no unpinned buffers available");
				}
				UnlockBufHdr(buf);
			}
		}
		/*
		 * CS186 TODO: Add code here to implement the LRU, MRU and 2Q buffer
		 * replacement policies. Once you've selected a buffer to
		 * evict, assign its index in the BufferDescriptors array to
		 * "resultIndex". You can model your code on the CLOCK code
		 * above.
		 */
		else if (BufferReplacementPolicy == POLICY_LRU)
		{			
		        resultIndex = StrategyControl->LRUBuffer;

			if (resultIndex == -1){
			        // no unpinned buffers 
			        elog(ERROR, "no unpinned buffers available");
			}

			volatile BufferDesc *resBuf = &BufferDescriptors[resultIndex];

			
			while (resBuf->refcount > 0){
			  volatile BufferDesc *LRUBuf = &BufferDescriptors[StrategyControl->LRUBuffer];
			  StrategyControl->LRUBuffer = LRUBuf->moreRecentBuffer;
			  StrategyControl->listCount = StrategyControl->listCount-1;

			  resultIndex = StrategyControl->LRUBuffer;

			  if (resultIndex == -1){
			    // no unpinned buffers 
			    elog(ERROR, "no unpinned buffers available");
			  }
			  
			  resBuf = &BufferDescriptors[resultIndex];

			}

			volatile BufferDesc *LRUBuf = &BufferDescriptors[StrategyControl->LRUBuffer];
			StrategyControl->LRUBuffer = LRUBuf->moreRecentBuffer;
			StrategyControl->listCount = StrategyControl->listCount-1;

			if (StrategyControl->LRUBuffer == -1){
			        // Edge case where the list becomes empty.  Must update MRUBuffer as well.
			        StrategyControl->MRUBuffer = -1;				
			}
			
		}
		else if (BufferReplacementPolicy == POLICY_MRU)
		{
			resultIndex = StrategyControl->MRUBuffer;

			if (resultIndex == -1){
			  // no unpinned buffers
			  elog(ERROR, "no unpinned buffers available");
			}
			
			volatile BufferDesc *resBuf = &BufferDescriptors[resultIndex];
			
			while (resBuf->refcount > 0){
			  StrategyControl->MRUBuffer = NextMRUBufIndex();
			  StrategyControl->listCount = StrategyControl->listCount-1;
			  resultIndex = StrategyControl->MRUBuffer;

			  if (resultIndex == -1){
			    //no unpinned buffers
			    elog(ERROR, "no unpinned buffers available");
			  }

			  resBuf = &BufferDescriptors[resultIndex];
			}
			StrategyControl->MRUBuffer = NextMRUBufIndex();
			StrategyControl->listCount = StrategyControl->listCount-1;
			
			if (StrategyControl->MRUBuffer == -1){
			        // Edge case where the list becomes empty.  Must update LRUBuffer as well.
			        StrategyControl->LRUBuffer = -1;				
			}
			
		}
		else if (BufferReplacementPolicy == POLICY_2Q)
		{
			elog(ERROR, "2Q unimplemented");
		}
		else
		{
			elog(ERROR, "invalid buffer pool replacement policy %d", BufferReplacementPolicy);
		}

    /*
     * CS186 Grading LOG - DON'T TOUCH
     * Don't output logs starting with "GRADING" by yourself; they are for grading purposes only.
     */
    elog(LOG, "GRADING: EVICT %2d", resultIndex);  
    
  }

	if (resultIndex == -1)
		elog(ERROR, "reached end of StrategyGetBuffer() without selecting a buffer");
	
	return &BufferDescriptors[resultIndex];
}

/*
 * loops through list to find second to last most recent buffer
 */
int
NextMRUBufIndex(){
  int MRUBufIndex = StrategyControl->MRUBuffer;
  if (MRUBufIndex == -1){
    return -1;
  }
  int currBufIndex = StrategyControl->LRUBuffer;
  volatile BufferDesc *currBuf = &BufferDescriptors[currBufIndex];
  if (currBuf->moreRecentBuffer == -1){
    return -1;
  }
  
  while (currBuf->moreRecentBuffer != MRUBufIndex){
    currBufIndex = currBuf->moreRecentBuffer;
    currBuf = &BufferDescriptors[currBufIndex];
  }
  return currBufIndex;
}

/*
 * CS186: Called when the specified buffer is unpinned and becomes
 * available for replacement. 
 */
void
BufferUnpinned(int bufIndex)
{
	volatile BufferDesc *buf = &BufferDescriptors[bufIndex];

	if (!LWLockConditionalAcquire(BufFreelistLock, LW_EXCLUSIVE))
		return;

	/*
	if (!(StrategyControl->LRUBuffer == -1 && StrategyControl->MRUBuffer == -1)){
	  //char listStr [200];
	  //char indexChar [2];
	  //sprintf(indexChar, "%2d", StrategyControl->LRUBuffer);
	  //strcat(listStr, indexChar);
	  elog(LOG, "LIST %2d", StrategyControl->LRUBuffer);
	
	  int currBufIndex = StrategyControl->LRUBuffer;
	  volatile BufferDesc *currBuf = &BufferDescriptors[currBufIndex];

	  while (currBuf->moreRecentBuffer != -1){
	    //sprintf(indexChar, "%2d", currBuf->moreRecentBuffer);
	    //strcat(listStr, indexChar);
	    
	    elog(LOG, "LIST %2d", currBuf->moreRecentBuffer);
	    volatile BufferDesc *moreRecentBuf = &BufferDescriptors[currBuf->moreRecentBuffer];
	    currBuf = moreRecentBuf;
	  }

	  //elog(LOG, listStr);
	  }*/

	/*
	 * CS186 TODO: When this function is called, the specified buffer has
	 * just been unpinned. That means you can start to manage this buffer 
   * using your buffer replacement policy. You can access the 
   * StrategyControl global variable from inside this function.
   * This function was added by the GSIs.
	 */
	int LRUBufIndex = StrategyControl->LRUBuffer;
	int MRUBufIndex = StrategyControl->MRUBuffer;
	volatile BufferDesc *LRUBuf = &BufferDescriptors[LRUBufIndex];
	//volatile BufferDesc *MRUBuf = &BufferDescriptors[MRUBufIndex];
	
	// LRU and MRU Buffer has not yet been initialized
	if (LRUBufIndex == -1 && MRUBufIndex == -1){
	  StrategyControl->LRUBuffer = bufIndex;
	  StrategyControl->listCount = StrategyControl->listCount+1;
	}
	// Already at front
	else if (LRUBufIndex == bufIndex){
	  if (MRUBufIndex != bufIndex){
	    StrategyControl->LRUBuffer = LRUBuf->moreRecentBuffer;
	  
	    volatile BufferDesc *mruBuf = 
	      &BufferDescriptors[StrategyControl->MRUBuffer];
	    mruBuf->moreRecentBuffer = bufIndex;
	  }
	  else{
	    //do nothing
	  }
	}
	// Already at back
	else if (MRUBufIndex == bufIndex){
	  // do nothing
	}
	// Normal case
	else{
	  int currBufIndex = StrategyControl->LRUBuffer;
	  volatile BufferDesc *currBuf = &BufferDescriptors[currBufIndex];
	  bool notFound = true;

	  // Loop through all indexes on the list and see if its already there
	  while (currBuf->moreRecentBuffer != -1){
	    volatile BufferDesc *moreRecentBuf = &BufferDescriptors[currBuf->moreRecentBuffer];

	    // if it is, move the pointers
	    if (currBuf->moreRecentBuffer == bufIndex){
	      currBuf->moreRecentBuffer = moreRecentBuf->moreRecentBuffer;

	      volatile BufferDesc *mruBuf = 
		&BufferDescriptors[StrategyControl->MRUBuffer];
	      mruBuf->moreRecentBuffer = bufIndex;

	      notFound = false;
	      
	      break;
	    }
	    currBuf = moreRecentBuf;
	  }
	  
	  // if not, then add it to the end
	  if (notFound){
	    volatile BufferDesc *mruBuf = 
		&BufferDescriptors[StrategyControl->MRUBuffer];
	    mruBuf->moreRecentBuffer = bufIndex;
	    StrategyControl->listCount = StrategyControl->listCount+1;
	  }
	}

	// reset MRUBuffer pointer, set tail to point to -1
	StrategyControl->MRUBuffer = bufIndex;
	buf->moreRecentBuffer = -1;

	LWLockRelease(BufFreelistLock);
}


/*
 * StrategyFreeBuffer: put a buffer on the freelist
 */
void
StrategyFreeBuffer(volatile BufferDesc *buf)
{
	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);

	/*
	 * It is possible that we are told to put something in the freelist that
	 * is already in it; don't screw up the list if so.
	 */
	if (buf->freeNext == FREENEXT_NOT_IN_LIST)
	{
		buf->freeNext = StrategyControl->firstFreeBuffer;
		if (buf->freeNext < 0)
			StrategyControl->lastFreeBuffer = buf->buf_id;
		StrategyControl->firstFreeBuffer = buf->buf_id;
	}

	LWLockRelease(BufFreelistLock);
}

/*
 * StrategySyncStart -- tell BufferSync where to start syncing
 *
 * The result is the buffer index of the best buffer to sync first.
 * BufferSync() will proceed circularly around the buffer array from there.
 *
 * In addition, we return the completed-pass count (which is effectively
 * the higher-order bits of nextVictimBuffer) and the count of recent buffer
 * allocs if non-NULL pointers are passed.	The alloc count is reset after
 * being read.
 */
int
StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc)
{
	int			result;

	LWLockAcquire(BufFreelistLock, LW_EXCLUSIVE);
	result = StrategyControl->nextVictimBuffer;
	if (complete_passes)
		*complete_passes = StrategyControl->completePasses;
	if (num_buf_alloc)
	{
		*num_buf_alloc = StrategyControl->numBufferAllocs;
		StrategyControl->numBufferAllocs = 0;
	}
	LWLockRelease(BufFreelistLock);
	return result;
}


/*
 * StrategyShmemSize
 *
 * estimate the size of shared memory used by the freelist-related structures.
 *
 * Note: for somewhat historical reasons, the buffer lookup hashtable size
 * is also determined here.
 */
Size
StrategyShmemSize(void)
{
	Size		size = 0;

	/* size of lookup hash table ... see comment in StrategyInitialize */
	size = add_size(size, BufTableShmemSize(NBuffers + NUM_BUFFER_PARTITIONS));

	/* size of the shared replacement strategy control block */
	size = add_size(size, MAXALIGN(sizeof(BufferStrategyControl)));

	return size;
}

/*
 * StrategyInitialize -- initialize the buffer cache replacement
 *		strategy.
 *
 * Assumes: All of the buffers are already built into a linked list.
 *		Only called by postmaster and only during initialization.
 */
void
StrategyInitialize(bool init)
{
	bool		found;

	/*
	 * Initialize the shared buffer lookup hashtable.
	 *
	 * Since we can't tolerate running out of lookup table entries, we must be
	 * sure to specify an adequate table size here.	 The maximum steady-state
	 * usage is of course NBuffers entries, but BufferAlloc() tries to insert
	 * a new entry before deleting the old.	 In principle this could be
	 * happening in each partition concurrently, so we could need as many as
	 * NBuffers + NUM_BUFFER_PARTITIONS entries.
	 */
	InitBufTable(NBuffers + NUM_BUFFER_PARTITIONS);

	/*
	 * Get or create the shared strategy control block
	 */
	StrategyControl = (BufferStrategyControl *)
		ShmemInitStruct("Buffer Strategy Status",
						sizeof(BufferStrategyControl),
						&found);

	if (!found)
	{
		/*
		 * Only done once, usually in postmaster
		 */
		Assert(init);

		/*
		 * Grab the whole linked list of free buffers for our strategy. We
		 * assume it was previously set up by InitBufferPool().
		 */
		StrategyControl->firstFreeBuffer = 0;
		StrategyControl->lastFreeBuffer = NBuffers - 1;

		/* Initialize the clock sweep pointer */
		StrategyControl->nextVictimBuffer = 0;

		/* Clear statistics */
		StrategyControl->completePasses = 0;
		StrategyControl->numBufferAllocs = 0;

		/* CS186 TODO: Initialize any data you added to StrategyControlData here */
		StrategyControl->LRUBuffer = -1;
		StrategyControl->MRUBuffer = -1;
	}
	else
		Assert(!init);
}


/* ----------------------------------------------------------------
 *				Backend-private buffer ring management
 * ----------------------------------------------------------------
 */


/*
 * GetAccessStrategy -- create a BufferAccessStrategy object
 *
 * The object is allocated in the current memory context.
 */
BufferAccessStrategy
GetAccessStrategy(BufferAccessStrategyType btype)
{
	BufferAccessStrategy strategy;
	int			ring_size;

	/*
	 * Select ring size to use.	 See buffer/README for rationales.
	 *
	 * Note: if you change the ring size for BAS_BULKREAD, see also
	 * SYNC_SCAN_REPORT_INTERVAL in access/heap/syncscan.c.
	 */
	switch (btype)
	{
		case BAS_NORMAL:
			/* if someone asks for NORMAL, just give 'em a "default" object */
			return NULL;

		case BAS_BULKREAD:
			ring_size = 256 * 1024 / BLCKSZ;
			break;
		case BAS_BULKWRITE:
			ring_size = 16 * 1024 * 1024 / BLCKSZ;
			break;
		case BAS_VACUUM:
			ring_size = 256 * 1024 / BLCKSZ;
			break;

		default:
			elog(ERROR, "unrecognized buffer access strategy: %d",
				 (int) btype);
			return NULL;		/* keep compiler quiet */
	}

	/* Make sure ring isn't an undue fraction of shared buffers */
	ring_size = Min(NBuffers / 8, ring_size);

	/* Allocate the object and initialize all elements to zeroes */
	strategy = (BufferAccessStrategy)
		palloc0(offsetof(BufferAccessStrategyData, buffers) +
				ring_size * sizeof(Buffer));

	/* Set fields that don't start out zero */
	strategy->btype = btype;
	strategy->ring_size = ring_size;

	return strategy;
}

/*
 * FreeAccessStrategy -- release a BufferAccessStrategy object
 *
 * A simple pfree would do at the moment, but we would prefer that callers
 * don't assume that much about the representation of BufferAccessStrategy.
 */
void
FreeAccessStrategy(BufferAccessStrategy strategy)
{
	/* don't crash if called on a "default" strategy */
	if (strategy != NULL)
		pfree(strategy);
}

/*
 * GetBufferFromRing -- returns a buffer from the ring, or NULL if the
 *		ring is empty.
 *
 * The bufhdr spin lock is held on the returned buffer.
 */
static volatile BufferDesc *
GetBufferFromRing(BufferAccessStrategy strategy)
{
	volatile BufferDesc *buf;
	Buffer		bufnum;

	/* Advance to next ring slot */
	if (++strategy->current >= strategy->ring_size)
		strategy->current = 0;

	/*
	 * If the slot hasn't been filled yet, tell the caller to allocate a new
	 * buffer with the normal allocation strategy.	He will then fill this
	 * slot by calling AddBufferToRing with the new buffer.
	 */
	bufnum = strategy->buffers[strategy->current];
	if (bufnum == InvalidBuffer)
	{
		strategy->current_was_in_ring = false;
		return NULL;
	}

	/*
	 * If the buffer is pinned we cannot use it under any circumstances.
	 *
	 * If usage_count is 0 or 1 then the buffer is fair game (we expect 1,
	 * since our own previous usage of the ring element would have left it
	 * there, but it might've been decremented by clock sweep since then). A
	 * higher usage_count indicates someone else has touched the buffer, so we
	 * shouldn't re-use it.
	 */
	buf = &BufferDescriptors[bufnum - 1];
	LockBufHdr(buf);
	if (buf->refcount == 0 && buf->usage_count <= 1)
	{
		strategy->current_was_in_ring = true;
		return buf;
	}
	UnlockBufHdr(buf);

	/*
	 * Tell caller to allocate a new buffer with the normal allocation
	 * strategy.  He'll then replace this ring element via AddBufferToRing.
	 */
	strategy->current_was_in_ring = false;
	return NULL;
}

/*
 * AddBufferToRing -- add a buffer to the buffer ring
 *
 * Caller must hold the buffer header spinlock on the buffer.  Since this
 * is called with the spinlock held, it had better be quite cheap.
 */
static void
AddBufferToRing(BufferAccessStrategy strategy, volatile BufferDesc *buf)
{
	strategy->buffers[strategy->current] = BufferDescriptorGetBuffer(buf);
}

/*
 * StrategyRejectBuffer -- consider rejecting a dirty buffer
 *
 * When a nondefault strategy is used, the buffer manager calls this function
 * when it turns out that the buffer selected by StrategyGetBuffer needs to
 * be written out and doing so would require flushing WAL too.	This gives us
 * a chance to choose a different victim.
 *
 * Returns true if buffer manager should ask for a new victim, and false
 * if this buffer should be written and re-used.
 */
bool
StrategyRejectBuffer(BufferAccessStrategy strategy, volatile BufferDesc *buf)
{
	/* We only do this in bulkread mode */
	if (strategy->btype != BAS_BULKREAD)
		return false;

	/* Don't muck with behavior of normal buffer-replacement strategy */
	if (!strategy->current_was_in_ring ||
	  strategy->buffers[strategy->current] != BufferDescriptorGetBuffer(buf))
		return false;

	/*
	 * Remove the dirty buffer from the ring; necessary to prevent infinite
	 * loop if all ring members are dirty.
	 */
	strategy->buffers[strategy->current] = InvalidBuffer;

	return true;
}

const char *
get_buffer_policy_str(PolicyKind policy)
{
	switch (policy)
	{
		case POLICY_CLOCK:
			return "clock";

		case POLICY_LRU:
			return "lru";

		case POLICY_MRU:
			return "mru";

		case POLICY_2Q:
			return "2q";

		default:
			elog(ERROR, "invalid replacement policy: %d", policy);
			return "unknown";
	}
}
