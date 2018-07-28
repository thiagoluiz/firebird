/*
 *	PROGRAM:	JRD Access Method
 *	MODULE:		tpc.cpp
 *	DESCRIPTION:	TIP Cache for Database
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 */

#include "firebird.h"
#include "../jrd/jrd.h"
#include "../jrd/ods.h"
#include "../jrd/tra.h"
#include "../jrd/pag.h"
#include "../jrd/cch_proto.h"
#include "../jrd/lck_proto.h"
#include "../jrd/ods_proto.h"
#include "../jrd/tpc_proto.h"
#include "../jrd/tra_proto.h"
#include "../common/isc_proto.h"

#include <sys/stat.h>

using namespace Firebird;

namespace Jrd {

void TipCache::MemoryInitializer::mutexBug(int osErrorCode, const char* text) 
{
	string msg;
	msg.printf("TPC: mutex %s error, status = %d", text, osErrorCode);
	fb_utils::logAndDie(msg.c_str());
}

bool TipCache::GlobalTpcInitializer::initialize(Firebird::SharedMemoryBase* sm, bool initialize) 
{
	m_cache->m_tpcHeader = static_cast<Firebird::SharedMemory<GlobalTpcHeader>*>(sm);

	if (!initialize)
		return true;

	thread_db* tdbb = JRD_get_thread_data();

	GlobalTpcHeader* header = m_cache->m_tpcHeader->getHeader();

	// Initialize the shared data header
	header->mhb_type = SharedMemoryBase::SRAM_TPC_HEADER;
	header->mhb_version = TPC_VERSION;
	header->mhb_timestamp = TimeStamp::getCurrentTimeStamp().value();

	header->latest_commit_number.store(CN_PREHISTORIC, std::memory_order_relaxed);
	header->latest_statement_id.store(0, std::memory_order_relaxed);
	header->tpc_block_size = m_cache->m_dbb->dbb_config->getTpcBlockSize();

	m_cache->initTransactionsPerBlock();
	m_cache->loadInventoryPages(tdbb);

	return true;
}

bool TipCache::SnapshotsInitializer::initialize(Firebird::SharedMemoryBase* sm, bool initialize) 
{
	if (!initialize)
		return true;

	SnapshotList* header = static_cast<SnapshotList*>(sm->sh_mem_header);

	// Initialize the shared data header
	header->mhb_type = SharedMemoryBase::SRAM_TPC_SNAPSHOTS;
	header->mhb_version = TPC_VERSION;
	header->mhb_timestamp = TimeStamp::getCurrentTimeStamp().value();

	header->slots_used.store(0, std::memory_order_relaxed);
	header->min_free_slot = 0;
	const ULONG dataSize = sm->sh_mem_length_mapped - offsetof(SnapshotList, slots[0]);
	header->slots_allocated.store(dataSize / sizeof(SnapshotData), std::memory_order_relaxed);

	return true;
}

bool TipCache::MemBlockInitializer::initialize(Firebird::SharedMemoryBase* sm, bool initialize) 
{
	if (!initialize)
		return true;

	TransactionStatusBlock* header = static_cast<TransactionStatusBlock*>(sm->sh_mem_header);

	// Initialize the shared data header
	header->mhb_type = SharedMemoryBase::SRAM_TPC_BLOCK;
	header->mhb_version = TPC_VERSION;
	header->mhb_timestamp = TimeStamp::getCurrentTimeStamp().value();

	memset(const_cast<CommitNumber*>(header->data), 0, sm->sh_mem_length_mapped - offsetof(TransactionStatusBlock, data[0]));

	return true;
}

TipCache::TipCache(Database* dbb)
	: m_dbb(dbb), m_tpcHeader(NULL), m_snapshots(NULL), m_transactionsPerBlock(0), 
	globalTpcInitializer(this), snapshotsInitializer(this), memBlockInitializer(this),
	m_blocks_memory(*dbb->dbb_permanent)
{
}

TipCache::~TipCache() 
{
	// Make sure that object is finalized before being deleted
	fb_assert(!m_blocks_memory.getFirst());
	fb_assert(!m_snapshots);
	fb_assert(!m_tpcHeader);
	fb_assert(m_transactionsPerBlock == 0);
}

void TipCache::finalizeTpc(thread_db* tdbb) 
{
	// To avoid race conditions, this function might only
	// be called during database shutdown when AST delivery is already disabled

	// Release locks and deallocate all shared memory structures
	if (m_blocks_memory.getFirst()) 
		do 
		{
			StatusBlockData* cur = m_blocks_memory.current();
			delete cur;
		} while (m_blocks_memory.getNext());

	if (m_snapshots)
	{
		m_snapshots->removeMapFile();
		delete m_snapshots;
		m_snapshots = NULL;
	}

	if (m_tpcHeader)
	{
		m_tpcHeader->removeMapFile();
		delete m_tpcHeader;
		m_tpcHeader = NULL;
	}

	m_blocks_memory.clear();
	m_transactionsPerBlock = 0;
}

CommitNumber TipCache::cacheState(TraNumber number) 
{
	fb_assert(m_tpcHeader);

	TraNumber oldest = m_tpcHeader->getHeader()->oldest_transaction.load(std::memory_order_relaxed);

	if (number < oldest)
		return CN_PREHISTORIC;

	// It is possible to amortize barrier in getTransactionStatusBlock
	// over large number of operations, if our callers are made aware of
	// TransactionStatusBlock granularity and iterate over transactions 
	// directly. But since this function is not really called too frequently, 
	// it should not matter and we leave interface "as is" for now.
	int blockNumber = number / m_transactionsPerBlock;
	int offset = number % m_transactionsPerBlock;
	TransactionStatusBlock *block = getTransactionStatusBlock(blockNumber);

	// This should not really happen ever
	fb_assert(block);

	if (!block)
		return CN_PREHISTORIC;

	// Barrier is not needed here when we are reading state from cache
	// because all callers of this function are prepared to handle
	// slightly out-dated information and will take slow path if necessary
	CommitNumber state = block->data[offset];

	return state;
}

void TipCache::initializeTpc(thread_db *tdbb) 
{
	// Initialization can only be called on a TipCache that is not initialized
	fb_assert(!m_transactionsPerBlock);

	string fileName;
	fileName.printf(TPC_HDR_FILE, m_dbb->getUniqueFileId().c_str());
	try
	{
		// XXX: This will indirectly set m_tpcHeader and store the pointer to an object
		// This is quite dirty C++ and will need to be fixed eventually
		fb_assert(!m_tpcHeader);

		FB_NEW_POOL(*m_dbb->dbb_permanent) SharedMemory<GlobalTpcHeader>(
			fileName.c_str(), sizeof(GlobalTpcHeader), &globalTpcInitializer);

		fb_assert(m_tpcHeader);
	}
	catch (const Exception& ex)
	{
		m_tpcHeader = NULL; // This is to prevent double free due to the hack above
		iscLogException("TPC: Cannot initialize the shared memory region (header)", ex);
		finalizeTpc(tdbb);
		throw;
	}

	fb_assert(m_tpcHeader->getHeader()->mhb_version == TPC_VERSION);
	initTransactionsPerBlock();

	fileName.printf(SNAPSHOTS_FILE, m_dbb->getUniqueFileId().c_str());
	try
	{
		m_snapshots = FB_NEW_POOL(*m_dbb->dbb_permanent) SharedMemory<SnapshotList>(
			fileName.c_str(), m_dbb->dbb_config->getSnapshotsMemSize(), &snapshotsInitializer);
	}
	catch (const Exception& ex)
	{
		iscLogException("TPC: Cannot initialize the shared memory region (snapshots)", ex);
		finalizeTpc(tdbb);
		throw;
	}

	fb_assert(m_snapshots->getHeader()->mhb_version == TPC_VERSION);
}

void TipCache::initTransactionsPerBlock()
{
	if (m_transactionsPerBlock)
		return;

	const ULONG dataOffset = static_cast<ULONG>(offsetof(TransactionStatusBlock, data[0]));
	m_transactionsPerBlock = 
		(m_tpcHeader->getHeader()->tpc_block_size - dataOffset) / sizeof(CommitNumber);
}

void TipCache::loadInventoryPages(thread_db* tdbb) 
{
	// check the header page for the oldest and newest transaction numbers

#ifdef SUPERSERVER_V2
	const TraNumber top = m_dbb->dbb_next_transaction;
	const TraNumber hdr_oldest = m_dbb->dbb_oldest_transaction;
#else
	WIN window(HEADER_PAGE_NUMBER);
	const Ods::header_page* header_page = (Ods::header_page*) CCH_FETCH(tdbb, &window, LCK_read, pag_header);
	const TraNumber hdr_oldest_transaction = Ods::getOIT(header_page);
	const TraNumber hdr_next_transaction = Ods::getNT(header_page);
	const AttNumber hdr_attachment_id = Ods::getAttID(header_page);
	CCH_RELEASE(tdbb, &window);
#endif

	GlobalTpcHeader* header = m_tpcHeader->getHeader();
	header->oldest_transaction.store(hdr_oldest_transaction, std::memory_order_relaxed);
	header->latest_attachment_id.store(hdr_attachment_id, std::memory_order_relaxed);
	header->latest_transaction_id.store(hdr_next_transaction, std::memory_order_relaxed);

	// Check if TIP has any interesting transactions.
	// At database creation time, it doesn't and the code below breaks
	// if there isn't a single one transaction to care about.
	if (hdr_oldest_transaction >= hdr_next_transaction)
		return;

	// Round down the oldest to a multiple of four, which puts the 
	// transaction in temporary buffer on a byte boundary
	TraNumber base = hdr_oldest_transaction & ~TRA_MASK;

	const FB_SIZE_T buffer_length = (hdr_next_transaction + 1 - base + TRA_MASK) / 4;
	Firebird::Array<UCHAR> transactions(buffer_length);

	UCHAR *buffer = transactions.begin();
	TRA_get_inventory(tdbb, buffer, base, hdr_next_transaction);

	static const CommitNumber init_state_mapping[4] = {CN_ACTIVE, CN_LIMBO, CN_DEAD, CN_PREHISTORIC};

	int blockNumber = hdr_oldest_transaction / m_transactionsPerBlock;
	int transOffset = hdr_oldest_transaction % m_transactionsPerBlock;
	TransactionStatusBlock* statusBlock = getTransactionStatusBlock(blockNumber);

	for (TraNumber t = hdr_oldest_transaction; ; ) 
	{
		int state = TRA_state(buffer, base, t);
		CommitNumber cn = init_state_mapping[state];

		// Barrier is not needed as our thread is the only one here.
		// At the same time, simple write to a volatile variable is not good
		// as it is not deterministic. Some compilers generate barriers and some do not.
		((std::atomic<CommitNumber>*)(statusBlock->data + transOffset))->store(cn, std::memory_order_relaxed);

		if (++t > hdr_next_transaction)
			break;

		if (++transOffset == m_transactionsPerBlock)
		{
			blockNumber++;
			transOffset = 0;
			statusBlock = getTransactionStatusBlock(blockNumber);
		}
	}
}


TipCache::StatusBlockData::StatusBlockData(thread_db* tdbb, TipCache* tipCache, int blkNumber) :
	blockNumber(blkNumber),
	memory(NULL),
	existenceLock(tdbb, sizeof(SLONG), LCK_tpc_block, this, tpc_block_blocking_ast),
	cache(tipCache)
{
	Database* dbb = tipCache->m_dbb;
	MemoryPool* pool = dbb->dbb_permanent;

	blockNumber = blockNumber;
	memory = NULL;
	cache = tipCache;
	existenceLock.setKey(blockNumber);

	if (!LCK_lock(tdbb, &existenceLock, LCK_SR, LCK_WAIT))
		ERR_bugcheck_msg("Unable to obtain memory block lock");


	string fileName;
	fileName.printf(TPC_BLOCK_FILE, cache->m_dbb->getUniqueFileId().c_str(), blockNumber);
	try
	{
		memory = FB_NEW_POOL(*pool) Firebird::SharedMemory<TransactionStatusBlock>(
			fileName.c_str(), cache->m_tpcHeader->getHeader()->tpc_block_size, 
			&cache->memBlockInitializer, true);
	}
	catch (const Exception& ex)
	{
		iscLogException("TPC: Cannot initialize the shared memory region (header)", ex);
		LCK_release(tdbb, &existenceLock);
		throw;
	}

	fb_assert(memory->getHeader()->mhb_version == TPC_VERSION);
}

TipCache::StatusBlockData::~StatusBlockData()
{
	thread_db* tdbb = JRD_get_thread_data();
	clear(tdbb);
}

void TipCache::StatusBlockData::clear(thread_db* tdbb)
{
	// memory could be already released at tpc_block_blocking_ast
	if (memory)
	{
		memory->removeMapFile();
		delete memory;
		memory = NULL;
	}
	LCK_release(tdbb, &existenceLock);
}

TipCache::TransactionStatusBlock* TipCache::createTransactionStatusBlock(int blockNumber) 
{
	fb_assert(m_sync_status.ourExclusiveLock());

	thread_db* tdbb = JRD_get_thread_data();
	Database* dbb = tdbb->getDatabase();

	StatusBlockData* blockData = FB_NEW_POOL(*dbb->dbb_permanent) StatusBlockData(tdbb, this, blockNumber);

	m_blocks_memory.add(blockData);

	return blockData->memory->getHeader();
}

TipCache::TransactionStatusBlock* TipCache::getTransactionStatusBlock(int blockNumber) 
{
	// This is a double-checked locking pattern. SyncLockGuard uses atomic ops internally and should be cheap
	TransactionStatusBlock* block = NULL;
	{
		SyncLockGuard sync(&m_sync_status, SYNC_SHARED, "TipCache::getTransactionStatusBlock");
		BlocksMemoryMap::ConstAccessor acc(&m_blocks_memory);
		if (acc.locate(blockNumber))
			block = acc.current()->memory->getHeader();
	}

	if (!block)
	{
		SyncLockGuard sync(&m_sync_status, SYNC_EXCLUSIVE, "TipCache::getTransactionStatusBlock");
		BlocksMemoryMap::ConstAccessor acc(&m_blocks_memory);
		if (acc.locate(blockNumber))
			block = acc.current()->memory->getHeader();
		else
		{
			// Check if block might be too old to be created.
			TraNumber oldest = m_tpcHeader->getHeader()->oldest_transaction.load(std::memory_order_relaxed);
			if (blockNumber >= static_cast<int>(oldest / m_transactionsPerBlock)) 
			{
				block = createTransactionStatusBlock(blockNumber);
			}
		}
	}
	return block;
}

TraNumber TipCache::findStates(TraNumber minNumber, TraNumber maxNumber, ULONG mask, int& state)
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);

	TransactionStatusBlock* statusBlock;
	int blockNumber, transOffset;
	do 
	{
		TraNumber oldest = m_tpcHeader->getHeader()->oldest_transaction.load(std::memory_order_relaxed);

		if (minNumber < oldest) 
			minNumber = oldest;

		blockNumber = minNumber / m_transactionsPerBlock;
		transOffset = minNumber % m_transactionsPerBlock;
		statusBlock = getTransactionStatusBlock(blockNumber);
	} while (!statusBlock);

	for (TraNumber t = minNumber; ; ) 
	{
		// Barrier is not needed here. Slightly out-dated information shall be ok here.
		// Such transaction shall already be considered active by our caller.
		// TODO: check if this assumption is indeed correct.

		CommitNumber cn = ((std::atomic<CommitNumber>*)(statusBlock->data + transOffset))->load(std::memory_order_relaxed);
		switch (cn)
		{
		case CN_ACTIVE:
			state = tra_active;
			break;

		case CN_LIMBO:
			state = tra_limbo;
			break;

		case CN_DEAD:
			state = tra_dead;
			break;

		case CN_MAX_NUMBER:	
			fb_assert(false);	// fall thru

		default:
			state = tra_committed;
			break;
		}

		if (((1 << state) & mask) != 0)
			return t;

		if (++t >= maxNumber)
			break;

		if (++transOffset == m_transactionsPerBlock)
		{
			blockNumber++;
			transOffset = 0;
			statusBlock = getTransactionStatusBlock(blockNumber);
		}
	}

	return 0;
}

CommitNumber TipCache::setState(TraNumber number, SSHORT state)
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);

	// over large number of operations, if our callers are made aware of
	// TransactionStatusBlock granularity and iterate over transactions 
	// directly. But since this function is not really called too frequently, 
	// it should not matter and we leave interface "as is" for now.
	int blockNumber = number / m_transactionsPerBlock;
	int offset = number % m_transactionsPerBlock;
	TransactionStatusBlock *block = getTransactionStatusBlock(blockNumber);

	// This should not really happen
	if (!block) 
		ERR_bugcheck_msg("TPC: Attempt to change state of old transaction");

	std::atomic<CommitNumber>* statePtr = (std::atomic<CommitNumber>*)(block->data + offset);
	CommitNumber oldStateCn = statePtr->load(std::memory_order_relaxed);
	switch(state) 
	{
		case tra_committed:
		{
			if (oldStateCn == CN_DEAD)
				ERR_bugcheck_msg("TPC: Attempt to commit dead transaction");

			// If transaction is already committed - do nothing
			if (oldStateCn >= CN_PREHISTORIC && oldStateCn <= CN_MAX_NUMBER)
				return oldStateCn;

			// We verified for all other cases, transaction must either be Active or in Limbo
			fb_assert(oldStateCn == CN_ACTIVE || oldStateCn == CN_LIMBO);

			// Generate new commit number
			CommitNumber newCommitNumber = m_tpcHeader->getHeader()->latest_commit_number++ + 1;

			statePtr->store(newCommitNumber, std::memory_order_relaxed);
			return newCommitNumber;
		}

		case tra_limbo:
			if (oldStateCn != CN_ACTIVE)
				ERR_bugcheck_msg("TPC: Attempt to mark inactive transaction to be in limbo");

			if (oldStateCn != CN_LIMBO)
				statePtr->store(CN_LIMBO, std::memory_order_relaxed);

			return CN_LIMBO;

		case tra_dead:
			if (oldStateCn == CN_DEAD)
				return CN_DEAD;

			if (oldStateCn != CN_ACTIVE && oldStateCn != CN_LIMBO)
				ERR_bugcheck_msg("TPC: Attempt to mark inactive transaction to be dead");

			statePtr->store(CN_DEAD, std::memory_order_relaxed);

			return CN_DEAD;

		default:
			ERR_bugcheck_msg("TPC: Attempt to mark invalid transaction state");
			return CN_ACTIVE; // silence the compiler
	}
}

CommitNumber TipCache::snapshotState(thread_db* tdbb, TraNumber number) 
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);

	// Get data from cache
	CommitNumber stateCn = cacheState(number);

	// Transaction is committed or dead?
	if (stateCn == CN_DEAD || (stateCn >= CN_PREHISTORIC && stateCn <= CN_MAX_NUMBER))
		return stateCn;

	// We excluded all other cases above
	fb_assert(stateCn == CN_ACTIVE || stateCn == CN_LIMBO);

	// Query lock data for a transaction; if we can then we know it is still active.
	//
	// This logic differs from original TPC implementation as follows:
	// 1. We use read_data instead of taking a lock, to avoid possible race conditions
	//    (they were probably not causing much harm, but consistency is a good thing)
	// 2. Old TPC returned tra_active for transactions in limbo, which was not correct
	Lock temp_lock(tdbb, sizeof(TraNumber), LCK_tra);
	temp_lock.setKey(number);

	if (LCK_read_data(tdbb, &temp_lock))
		return stateCn;

	// Go to disk, and obtain state of our transaction from TIP
	int state = TRA_fetch_state(tdbb, number);

	// We already know for sure that this transaction cannot be active, so mark it dead now
	// to avoid more work in the future
	if (state == tra_active) 
	{
		TRA_set_state(tdbb, 0, number, tra_dead); // This will update TIP cache
		return CN_DEAD;
	}

	// Update cache and return new state
	stateCn = setState(number, state);
	return stateCn;
}

void TipCache::updateOldestTransaction(thread_db *tdbb, TraNumber oldest, TraNumber oldestSnapshot) 
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);

	TraNumber oldestNew = MIN(oldest, oldestSnapshot);
	TraNumber oldestNow = m_tpcHeader->getHeader()->oldest_transaction.load(std::memory_order_relaxed);
	if (oldestNew > oldestNow)
	{
		m_tpcHeader->getHeader()->oldest_transaction.store(oldestNew, std::memory_order_relaxed);
		releaseSharedMemory(tdbb, oldestNow, oldestNew);
	}
}

int TipCache::tpc_block_blocking_ast(void* arg) 
{
	StatusBlockData* data = static_cast<StatusBlockData*>(arg);

	Database* const dbb = data->existenceLock.lck_dbb;
	AsyncContextHolder tdbb(dbb, FB_FUNCTION, &data->existenceLock);

	TipCache* cache = data->cache;
	TraNumber oldest = cache->m_tpcHeader->getHeader()->oldest_transaction.load(std::memory_order_relaxed);

	// Release shared memory
	data->clear(tdbb);

	// Check if there is a bug in cleanup code and we were requested to 
	// release memory that might be in use
	if (data->blockNumber >= static_cast<int>(oldest / cache->m_transactionsPerBlock))
		ERR_bugcheck_msg("Incorrect attempt to release shared memory");

	return 0;
}


void TipCache::releaseSharedMemory(thread_db *tdbb, TraNumber oldest_old, TraNumber oldest_new) 
{
	int lastInterestingBlockNumber = oldest_new / m_transactionsPerBlock;

	// If we didn't cross block boundary - there is nothing to do.
	// Note that due to the fuzziness of our caller's memory access to variables
	// there is an unlikely possibility that we might lose one such event.
	// This is not too bad, and next such event will clean things up.
	if (oldest_old / m_transactionsPerBlock == lastInterestingBlockNumber)
		return;

	// Populate array of blocks that might be unmapped and deleted
	// We scan for blocks to clean up in descending order, but delete them in
	// ascending order to ensure for robust operation.
	string fileName;
	Firebird::Array<int> blocksToCleanup;
	for (int blockNumber = lastInterestingBlockNumber - SAFETY_GAP_BLOCKS - 1; 
		blockNumber >= 0; blockNumber--) 
	{
		fileName.printf(TPC_BLOCK_FILE, m_dbb->getUniqueFileId().c_str(), blockNumber);
		TEXT expanded_filename[MAXPATHLEN];
		iscPrefixLock(expanded_filename, fileName.c_str(), false);

		struct stat st;
		// If file is not found -- look no further
		if (stat(expanded_filename, &st) != 0)
			break;

		blocksToCleanup.add(blockNumber);
	}

	if (blocksToCleanup.isEmpty())
		return;

	SyncLockGuard sync(&m_sync_status, SYNC_EXCLUSIVE, "TipCache::releaseSharedMemory");
	for (int *itr = blocksToCleanup.end() - 1; itr >= blocksToCleanup.begin(); itr--)
	{
		int blockNumber = *itr;

		if (m_blocks_memory.locate(blockNumber))
		{
			StatusBlockData* cur = m_blocks_memory.current();
			m_blocks_memory.fastRemove();

			delete cur;
		}

		// Signal other processes to release resources
		Lock temp(tdbb, sizeof(SLONG), LCK_tpc_block);
		temp.setKey(blockNumber);
		if (!LCK_lock(tdbb, &temp, LCK_EX, LCK_WAIT))
		{
			gds__log("TPC BUG: Unable to obtain cleanup lock for block %d. Please report this error to developers", *itr);
			fb_assert(false);
			break;
		}
		LCK_release(tdbb, &temp);
	}
}

SnapshotHandle TipCache::allocateSnapshotSlot() 
{
	// Try finding available slot
	SnapshotList* snapshots = m_snapshots->getHeader();

	// Scan previously used slots first
	SnapshotHandle slotNumber;

	ULONG slots_used = snapshots->slots_used.load(std::memory_order_relaxed);
	for (slotNumber = snapshots->min_free_slot; slotNumber < slots_used; slotNumber++) 
	{
		if (!snapshots->slots[slotNumber].attachment_id.load(std::memory_order_relaxed))
			return slotNumber;
	}

	// See if we have some space left in the snapshots block
	if (slotNumber < snapshots->slots_allocated.load(std::memory_order_relaxed)) 
	{
		snapshots->slots_used.store(slotNumber + 1, std::memory_order_release);
		return slotNumber;
	}

#ifdef HAVE_OBJECT_MAP
	LocalStatus ls;
	CheckStatusWrapper localStatus(&ls);
	if (!m_snapshots->remapFile(&localStatus, m_snapshots->sh_mem_length_mapped * 2, true))
	{
		status_exception::raise(&localStatus);
	}

	snapshots = m_snapshots->getHeader();
	snapshots->slots_allocated.store(
		static_cast<ULONG>((m_snapshots->sh_mem_length_mapped - offsetof(SnapshotList, slots[0])) / sizeof(SnapshotData)),
		std::memory_order_release);
#else
	// NS: I do not intend to assign a code to this condition, because I think that we do not 
	// support platforms without HAVE_OBJECT_MAP capability, and the code below needs to be cleaned out
	// sooner or later. And even if need to support such a platform suddenly appears we shall make it 
	// fail in remapFile code and not here.
	(Arg::Gds(isc_random) << 
		"Snapshots shared memory block is full on a platform that does not support shared memory remapping").raise();
#endif

	fb_assert(slotNumber < snapshots->slots_allocated.load(std::memory_order_relaxed));
	snapshots->slots_used.store(slotNumber + 1, std::memory_order_release);
	return slotNumber;
}

void TipCache::remapSnapshots(bool sync) 
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);

	SnapshotList* snapshots = m_snapshots->getHeader();
	if (snapshots->slots_allocated.load(std::memory_order_acquire) != 
		(m_snapshots->sh_mem_length_mapped - offsetof(SnapshotList, slots[0])) / sizeof(SnapshotData))
	{
		SharedMutexGuard guard(m_snapshots, false);
		if (sync) 
			guard.lock();

		LocalStatus ls;
		CheckStatusWrapper localStatus(&ls);
		if (!m_snapshots->remapFile(&localStatus,
			static_cast<ULONG>(
				snapshots->slots_allocated.load(std::memory_order_relaxed) * sizeof(SnapshotData) + 
					offsetof(SnapshotList, slots[0])), false))
		{
			status_exception::raise(&localStatus);
		}
	}
}



SnapshotHandle TipCache::beginSnapshot(thread_db* tdbb, AttNumber attachmentId, CommitNumber *commitNumber_out)
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);
	fb_assert(attachmentId);

	// Lock mutex
	SharedMutexGuard guard(m_snapshots);

	// Remap snapshot list if it has been grown by someone else
	remapSnapshots(false);

	SnapshotHandle slotNumber = allocateSnapshotSlot();

	// Note, that allocateSnapshotSlot might remap memory and thus invalidate pointers
	SnapshotList *snapshots = m_snapshots->getHeader();

	// Store snapshot commit number and return handle
	SnapshotData *slot = snapshots->slots + slotNumber;

	*commitNumber_out = m_tpcHeader->getHeader()->latest_commit_number.load(std::memory_order_acquire);
	slot->snapshot.store(*commitNumber_out, std::memory_order_release);

	// Only assign attachment_id after we completed all other work
	slot->attachment_id.store(attachmentId, std::memory_order_release);

	// And now move allocator watermark position after we used slot for sure
	snapshots->min_free_slot = slotNumber + 1;

	return slotNumber;
}

void TipCache::deallocateSnapshotSlot(SnapshotHandle slotNumber) 
{
	// Note: callers of this function assume that it cannot remap
	// shared memory (as they keep shared memory pointers).

	SnapshotList *snapshots = m_snapshots->getHeader();

	// At first, make slot available for allocator (this is always safe)
	if (snapshots->min_free_slot > slotNumber)
		snapshots->min_free_slot = slotNumber;

	SnapshotData *slot = snapshots->slots + slotNumber;

	slot->snapshot.store(0, std::memory_order_release);
	slot->attachment_id.store(0, std::memory_order_release);

	// After we completed deallocation, update used slots count, if necessary
	if (slotNumber == snapshots->slots_used.load(std::memory_order_relaxed) - 1) 
	{
		do {
			slot--;
		} while (slot >= snapshots->slots && slot->attachment_id.load(std::memory_order_relaxed) == 0);
		snapshots->slots_used.store(static_cast<ULONG>(slot - snapshots->slots + 1), std::memory_order_release);
	}
}

void TipCache::endSnapshot(thread_db* tdbb, SnapshotHandle handle, AttNumber attachmentId)
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);

	// Lock mutex
	SharedMutexGuard guard(m_snapshots);

	// We don't care to perform remap here, because we release a slot that was 
	// allocated by this process and we do not access any data past it during 
	// deallocation.

	// Perform some sanity checks on a handle
	SnapshotList *snapshots = m_snapshots->getHeader();
	SnapshotData *slot = snapshots->slots + handle;

	if (handle >= snapshots->slots_used.load(std::memory_order_relaxed))
	{
		ERR_bugcheck_msg("Incorrect snapshot deallocation - too few slots");
	}

	if (slot->attachment_id.load(std::memory_order_relaxed) != attachmentId)
	{
		ERR_bugcheck_msg("Incorrect snapshot deallocation - attachment mismatch");
	}

	// Deallocate slot
	deallocateSnapshotSlot(handle);

	// Increment release event count
	m_tpcHeader->getHeader()->snapshot_release_count++;
}

void TipCache::updateActiveSnapshots(thread_db* tdbb, ActiveSnapshots* activeSnapshots) 
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);
	fb_assert(activeSnapshots);

	// This function is quite tricky as it reads snapshots list without locks (using atomics)

	if (activeSnapshots->m_lastCommit == CN_ACTIVE) 
	{
		// Slow path. Initialization.

		GlobalTpcHeader *header = m_tpcHeader->getHeader();
		activeSnapshots->m_lastCommit = header->latest_commit_number.load(std::memory_order_acquire);
		activeSnapshots->m_releaseCount = header->snapshot_release_count.load(std::memory_order_relaxed);

		SnapshotList *snapshots = m_snapshots->getHeader();

		// If new slots are allocated past this value - we don't care as we preserved
		// lastCommit and new snapshots will have numbers >= lastCommit and we don't
		// GC them anyways
		ULONG slots_used_org = snapshots->slots_used.load(std::memory_order_acquire);

		// Remap snapshot list if it has been grown by someone else
		remapSnapshots(true);

		snapshots = m_snapshots->getHeader();

		Firebird::GenericMap<Pair<NonPooled<AttNumber, bool> > > att_states;

		// We modify snapshots list only while holding a mutex
		SharedMutexGuard guard(m_snapshots, false);

		activeSnapshots->m_snapshots.clear();
		for (ULONG slotNumber = 0; slotNumber < slots_used_org; slotNumber++) 
		{
			SnapshotData *slot = snapshots->slots + slotNumber;
			AttNumber slot_attachment_id = slot->attachment_id.load(std::memory_order_acquire);
			if (slot_attachment_id) 
			{
				bool isAttachmentDead;
				if (!att_states.get(slot_attachment_id, isAttachmentDead)) 
				{
					ThreadStatusGuard temp_status(tdbb);
					Lock temp_lock(tdbb, sizeof(AttNumber), LCK_attachment);
					temp_lock.setKey(slot_attachment_id);
					if ((isAttachmentDead = LCK_lock(tdbb, &temp_lock, LCK_EX, LCK_NO_WAIT)))
						LCK_release(tdbb, &temp_lock);
					att_states.put(slot_attachment_id, isAttachmentDead);
				}

				if (isAttachmentDead)
				{
					if (!guard.isLocked())
					{
						guard.lock();

						// Check if slot was reused while we waited for the mutex
						AttNumber slot_attachment_id2 = slot->attachment_id.load(std::memory_order_acquire);
						if (slot_attachment_id != slot_attachment_id2)
						{
							slotNumber--;
							continue;
						}
					}

					deallocateSnapshotSlot(slotNumber);
				}
				else
				{
					CommitNumber slot_snapshot = slot->snapshot.load(std::memory_order_acquire);
					if (slot_snapshot)
						activeSnapshots->m_snapshots.set(slot_snapshot);
				}
			}
		}
	} 
	else 
	{
		// Fast path. Quick update.

		GlobalTpcHeader *header = m_tpcHeader->getHeader();
		SnapshotList *snapshots = m_snapshots->getHeader();

		// Update m_lastCommit unconditionally, to prevent active snapshots list from
		// stalling when there is no snapshots created\released since last update.
		// Stalled list of active snapshots could stop intermediate garbage collection
		// by current list owner (attachment).
		// It is important to ensure that no snapshot with CN less than m_lastCommit 
		// could be missed at activeSnapshots, therefore we read slots_used after 
		// latest_commit_number using appropriate memory barriers.

		activeSnapshots->m_lastCommit = header->latest_commit_number.load(std::memory_order_acquire);
		ULONG slots_used_org = snapshots->slots_used.load(std::memory_order_acquire);
		ULONG release_count = header->snapshot_release_count.load(std::memory_order_relaxed);

		// If no snapshots were released since we were last called - do nothing
		// Do not care about race issues here, because worst-case consequences are benign
		if (activeSnapshots->m_releaseCount == release_count &&
			activeSnapshots->m_slots_used == slots_used_org)
		{
			return;
		}

		activeSnapshots->m_slots_used == slots_used_org;
		activeSnapshots->m_releaseCount = release_count;

		// Remap snapshot list if it has been grown by someone else
		remapSnapshots(true);

		snapshots = m_snapshots->getHeader();

		activeSnapshots->m_snapshots.clear();
		for (SnapshotData *slot = snapshots->slots, 
			*end = snapshots->slots + slots_used_org; 
			slot < end;
			slot++) 
		{
			if (slot->attachment_id.load(std::memory_order_acquire))
			{
				CommitNumber slot_snapshot = slot->snapshot.load(std::memory_order_acquire);
				if (slot_snapshot)
					activeSnapshots->m_snapshots.set(slot_snapshot);
			}
		}
	}
}

TraNumber TipCache::generateTransactionId() 
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);

	// No barrier here, because inconsistency in transaction number order does not matter
	// for read-only databases where this function is used.
	TraNumber transaction_id = m_tpcHeader->getHeader()->latest_transaction_id++ + 1;
	return transaction_id;
}

AttNumber TipCache::generateAttachmentId() 
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);

	// No barrier here, because attachment id order does not generally matter
	// especially for read-only databases where this function is used.
	AttNumber attachment_id = m_tpcHeader->getHeader()->latest_attachment_id++ + 1;
	return attachment_id;
}

StmtNumber TipCache::generateStatementId() 
{
	// Can only be called on initialized TipCache
	fb_assert(m_tpcHeader);

	// No barrier here, because statement id order does not generally matter
	StmtNumber statement_id = m_tpcHeader->getHeader()->latest_statement_id++ + 1;
	return statement_id;
}

//void TipCache::assignLatestTransactionId(TraNumber number) {
//	// XXX: there is no paired acquire because value assigned here is not really used for now
//	atomic_int_store_release(&m_tpcHeader->getHeader()->latest_transaction_id, number);
//}

void TipCache::assignLatestAttachmentId(AttNumber number)
{
	// XXX: there is no paired acquire because value assigned here is not really used for now
	m_tpcHeader->getHeader()->latest_attachment_id.store(number, std::memory_order_release);
}

int TPC_snapshot_state(thread_db* tdbb, TraNumber number)
{
	TipCache* cache = tdbb->getDatabase()->dbb_tip_cache;
	// This function might be called early during database initialization when
	// TIP cache does not exist yet.
	if (!cache)
		return TRA_fetch_state(tdbb, number);

	CommitNumber stateCn = cache->snapshotState(tdbb, number);
	switch(stateCn) 
	{
	case CN_ACTIVE: 
		return tra_active;

	case CN_LIMBO: 
		return tra_limbo;

	case CN_DEAD: 
		return tra_dead;

	default: 
		return tra_committed;
	}
}


} // namespace Jrd
