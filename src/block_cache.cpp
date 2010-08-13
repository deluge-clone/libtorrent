/*

Copyright (c) 2010, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/block_cache.hpp"
#include "libtorrent/disk_buffer_pool.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/time.hpp"
#include "libtorrent/disk_io_job.hpp"
#include "libtorrent/storage.hpp"
#include "libtorrent/io_service.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/disk_io_thread.hpp" // disk_operation_failed
#include "libtorrent/invariant_check.hpp"

#define DEBUG_CACHE 0

#define DLOG if (DEBUG_CACHE) fprintf

namespace libtorrent {

const int block_size = 16 * 1024;

struct update_last_use
{
	update_last_use(int exp): expire(exp) {}
	void operator()(block_cache::cached_piece_entry& p)
	{
		TORRENT_ASSERT(p.storage);
		p.expire = time(0) + expire;
	}
	int expire;
};

block_cache::cached_piece_entry::cached_piece_entry()
	: piece(0)
	, storage()
	, expire(0)
	, refcount(0)
	, num_dirty(0)
	, num_blocks(0)
	, blocks_in_piece(0)
	, marked_for_deletion(false)
	, blocks()
	, jobs()
{}

block_cache::block_cache(disk_buffer_pool& p)
	: m_max_size(0)
	, m_cache_size(0)
	, m_read_cache_size(0)
	, m_write_cache_size(0)
	, m_blocks_read(0)
	, m_blocks_read_hit(0)
	, m_buffer_pool(p)
{}

// returns:
// -1: not in cache
// -2: no memory
int block_cache::try_read(disk_io_job& j)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(j.buffer == 0);
	TORRENT_ASSERT(j.cache_min_time >= 0);

	cache_piece_index_t& idx = m_pieces.get<0>();
	cache_piece_index_t::iterator p = find_piece(j);

	int ret = 0;

	// if the piece cannot be found in the cache,
	// it's a cache miss
	if (p == idx.end()) return -1;

	ret = copy_from_piece(p, j);
	if (ret < 0) return ret;
	if (p->num_blocks == 0) idx.erase(p);
	else idx.modify(p, update_last_use(j.cache_min_time));

	ret = j.buffer_size;
	++m_blocks_read;
	++m_blocks_read_hit;
	return ret;
}

block_cache::iterator block_cache::allocate_piece(disk_io_job const& j)
{
	INVARIANT_CHECK;

	cache_piece_index_t& idx = m_pieces.get<0>();
	cache_piece_index_t::iterator p = find_piece(j);
	if (p == idx.end())
	{
		int piece_size = j.storage->info()->piece_size(j.piece);
		int blocks_in_piece = (piece_size + block_size - 1) / block_size;

		cached_piece_entry pe;
		pe.piece = j.piece;
		pe.storage = j.storage;
		pe.expire = time(0) + j.cache_min_time;
		pe.blocks_in_piece = blocks_in_piece;
		pe.blocks.reset(new (std::nothrow) cached_block_entry[blocks_in_piece]);
		TORRENT_ASSERT(pe.blocks);
		if (!pe.blocks) return idx.end();
		p = idx.insert(pe).first;
	}
	return p;
}

block_cache::iterator block_cache::add_dirty_block(disk_io_job const& j)
{
	INVARIANT_CHECK;

	iterator p = allocate_piece(j);
	TORRENT_ASSERT(p != end());
	if (p == end()) return p;

	int block = j.offset / block_size;
	TORRENT_ASSERT((j.offset % block_size) == 0);

	if (m_cache_size + 1 > m_max_size)
		try_evict_blocks(m_cache_size + 1 - m_max_size, 1, p);

	cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*p);
	TORRENT_ASSERT(block < pe->blocks_in_piece);
	TORRENT_ASSERT(pe->blocks[block].pending == false);
	TORRENT_ASSERT(pe->blocks[block].dirty == false);
	TORRENT_ASSERT(pe->blocks[block].buf == 0);
	TORRENT_ASSERT(j.piece == pe->piece);
	TORRENT_ASSERT(!pe->marked_for_deletion);

	pe->blocks[block].buf = j.buffer;

	pe->blocks[block].dirty = true;
	++pe->num_blocks;
	++pe->num_dirty;
	++m_write_cache_size;
	++m_cache_size;
	pe->jobs.push_back(j);
	pe->jobs.back().buffer = 0;
	pe->expire = (std::max)(pe->expire, time(0) + j.cache_min_time);

	TORRENT_ASSERT(m_cache_size <= m_buffer_pool.in_use());
	TORRENT_ASSERT(m_read_cache_size <= m_buffer_pool.in_use());
	return p;
}

block_cache::iterator block_cache::end()
{
	cache_piece_index_t& idx = m_pieces.get<0>();
	return idx.end();
}

std::pair<block_cache::iterator, block_cache::iterator> block_cache::all_pieces()
{
	cache_piece_index_t& idx = m_pieces.get<0>();
	return std::make_pair(idx.begin(), idx.end());
}

std::pair<block_cache::lru_iterator, block_cache::lru_iterator> block_cache::all_lru_pieces()
{
	cache_lru_index_t& idx = m_pieces.get<1>();
	return std::make_pair(idx.begin(), idx.end());
}

std::pair<block_cache::iterator, block_cache::iterator> block_cache::pieces_for_storage(void* st)
{
	cache_piece_index_t& idx = m_pieces.get<0>();
	return idx.equal_range(boost::make_tuple(st));
}

void block_cache::mark_for_deletion(iterator p)
{
	INVARIANT_CHECK;

	cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*p);

	std::vector<char*> to_delete;
	to_delete.reserve(pe->blocks_in_piece);
	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		if (pe->blocks[i].buf == 0 || pe->blocks[i].refcount > 0) continue;
		TORRENT_ASSERT(pe->blocks[i].buf != 0);
		to_delete.push_back(pe->blocks[i].buf);
		pe->blocks[i].buf = 0;
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		TORRENT_ASSERT(m_cache_size > 0);
		--m_cache_size;
		if (!pe->blocks[i].dirty)
		{
			TORRENT_ASSERT(m_read_cache_size > 0);
			--m_read_cache_size;
		}
		else --pe->num_dirty;
	}
	if (!to_delete.empty()) m_buffer_pool.free_multiple_buffers(&to_delete[0], to_delete.size());
	pe->marked_for_deletion = true;

	if (pe->refcount == 0)
	{
		cache_piece_index_t& idx = m_pieces.get<0>();
		idx.erase(p);
		return;
	}

	TORRENT_ASSERT(m_cache_size <= m_buffer_pool.in_use());
	TORRENT_ASSERT(m_read_cache_size <= m_buffer_pool.in_use());
}

int block_cache::try_evict_blocks(int num, int prio, iterator ignore)
{
	INVARIANT_CHECK;

	if (num <= 0) return 0;

	DLOG(stderr, "[%p] try_evict_blocks: %d\n", &m_buffer_pool, num);

	cache_lru_index_t& idx = m_pieces.get<1>();

	std::vector<char*> to_free;
	to_free.reserve(num);

	// iterate over all blocks in order of last being used (oldest first) and as
	// long as we still have blocks to evict
	for (cache_lru_index_t::iterator i = idx.begin(); i != idx.end() && num > 0;)
	{
		if (ignore != end() && i->storage == ignore->storage && i->piece == ignore->piece)
		{
			++i;
			continue;
		}

		cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*i);

		if (pe->num_blocks == 0)
		{
			idx.erase(i++);
			continue;
		}

		// all blocks in this piece are dirty
		if (pe->num_dirty == pe->num_blocks)
		{
			++i;
			continue;
		}

		// go through the blocks and evict the ones
		// that are not dirty and not referenced
		for (int j = 0; j < pe->blocks_in_piece && num > 0; ++j)
		{
			cached_block_entry& b = pe->blocks[j];
			if (b.buf == 0 || b.refcount > 0 || b.dirty || b.uninitialized || b.pending) continue;
			
			to_free.push_back(b.buf);
			b.buf = 0;
			--pe->num_blocks;
			--m_read_cache_size;
			--m_cache_size;
			--num;
		}

		if (pe->num_blocks == 0) idx.erase(i++);
		else ++i;
	}

	if (to_free.empty()) return num;

	DLOG(stderr, "[%p]    removed %d blocks\n", &m_buffer_pool, int(to_free.size()));

	m_buffer_pool.free_multiple_buffers(&to_free[0], to_free.size());

	TORRENT_ASSERT(m_cache_size <= m_buffer_pool.in_use());
	TORRENT_ASSERT(m_read_cache_size <= m_buffer_pool.in_use());

	return num;
}

// the priority controls which other blocks these new blocks
// are allowed to evict from the cache.
// 0 = regular read job
// 1 = write jobs
// 2 = required read jobs (like for read and hash)

// returns the number of blocks in the given range that are pending
// if this is > 0, it's safe to append the disk_io_job to the piece
// and it will be invoked once the pending blocks complete
// negative return values indicate different errors
// -1 = out of memory
// -2 = out of cache space

int block_cache::allocate_pending(block_cache::iterator p
	, int begin, int end, disk_io_job const& j, int prio)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(begin >= 0);
	TORRENT_ASSERT(end <= p->blocks_in_piece);
	TORRENT_ASSERT(begin < end);
	TORRENT_ASSERT(p->piece == j.piece);
	TORRENT_ASSERT(p->storage == j.storage);

	int ret = 0;

	cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*p);

	int blocks_to_allocate = end - begin;
	if (m_cache_size + blocks_to_allocate > m_max_size)
	{
		if (try_evict_blocks(m_cache_size + blocks_to_allocate - m_max_size, prio, p) > 0)
		{
			// we couldn't evict enough blocks to make room for this piece
			// we cannot return -1 here, since that means we're out of
			// memory. We're just out of cache space. -2 will tell the caller
			// to read the piece directly instead of going through the cache
			return -2;
		}
	}

	for (int i = begin; i < end; ++i)
	{
		if (pe->blocks[i].buf) continue;
		if (pe->blocks[i].pending) continue;
		pe->blocks[i].buf = m_buffer_pool.allocate_buffer("pending read");
		if (pe->blocks[i].buf == 0)
		{
			std::vector<char*> to_delete;
			to_delete.reserve(end - begin);
			for (int j = begin; j < end; ++j)
			{
				cached_block_entry& bl = pe->blocks[j];
				if (!bl.uninitialized) continue;
				TORRENT_ASSERT(bl.buf != 0);
				to_delete.push_back(bl.buf);
				bl.buf = 0;
				bl.uninitialized = false;
				TORRENT_ASSERT(m_read_cache_size > 0);
				--m_read_cache_size;
				TORRENT_ASSERT(m_cache_size > 0);
				--m_cache_size;
				TORRENT_ASSERT(bl.refcount == 1);
				TORRENT_ASSERT(bl.refcount > 0);
				--bl.refcount;
				TORRENT_ASSERT(pe->refcount > 0);
				--pe->refcount;
				TORRENT_ASSERT(pe->num_blocks > 0);
				--pe->num_blocks;
			}
			if (p->num_blocks == 0)
			{
				cache_piece_index_t& idx = m_pieces.get<0>();
				idx.erase(p);
			}
			if (!to_delete.empty()) m_buffer_pool.free_multiple_buffers(&to_delete[0], to_delete.size());

			TORRENT_ASSERT(m_cache_size <= m_buffer_pool.in_use());
			TORRENT_ASSERT(m_read_cache_size <= m_buffer_pool.in_use());

			return -1;
		}
		++pe->num_blocks;
		// this signals the disk_io_thread that this buffer should
		// be read in io_range()
		pe->blocks[i].uninitialized = true;
		++m_read_cache_size;
		++m_cache_size;
		++ret;
	}
	
	TORRENT_ASSERT(j.piece == pe->piece);
	if (ret > 0)
	{
		// in case this was marked for deletion
		// don't do that anymore
		pe->marked_for_deletion = false;
		pe->jobs.push_back(j);
	}

	TORRENT_ASSERT(m_cache_size <= m_buffer_pool.in_use());
	TORRENT_ASSERT(m_read_cache_size <= m_buffer_pool.in_use());

	return ret;
}

void block_cache::mark_as_done(block_cache::iterator p, int begin, int end
	, io_service& ios, int queue_buffer_size, error_code const& ec)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(begin >= 0);
	TORRENT_ASSERT(end <= p->blocks_in_piece);
	TORRENT_ASSERT(begin < end);

	cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*p);

	DLOG(stderr, "%p block_cache mark_as_done error: %s\n", &m_buffer_pool, ec.message().c_str());

	std::vector<char*> to_delete;
	to_delete.reserve(pe->blocks_in_piece);
	if (ec)
	{
		// fail all jobs for this piece with this error
		// and clear blocks

		for (int i = begin; i < end; ++i)
		{
			cached_block_entry& bl = pe->blocks[i];
			TORRENT_ASSERT(bl.refcount == 1);
			TORRENT_ASSERT(bl.refcount > 0);
			--bl.refcount;
			TORRENT_ASSERT(pe->refcount > 0);
			--pe->refcount;

			// we can't free blocks that are in use by some 
			// async. operation
			if (bl.refcount > 0) continue;

			// if this block isn't pending, it was here before
			// this operation failed
			if (!bl.pending && !bl.dirty) continue;

			if (bl.dirty)
			{
				TORRENT_ASSERT(pe->num_dirty > 0);
				--pe->num_dirty;
				bl.dirty = false;
				TORRENT_ASSERT(m_write_cache_size > 0);
				--m_write_cache_size;
			}
			else
			{
				TORRENT_ASSERT(m_read_cache_size > 0);
				--m_read_cache_size;
			}
			TORRENT_ASSERT(bl.buf != 0);
			to_delete.push_back(bl.buf);
			bl.buf = 0;
			bl.pending = false;
			TORRENT_ASSERT(pe->num_blocks > 0);
			--pe->num_blocks;
			TORRENT_ASSERT(m_cache_size > 0);
			--m_cache_size;
		}
	}
	else
	{
		for (int i = begin; i < end; ++i)
		{
			TORRENT_ASSERT(pe->blocks[i].refcount == 1);
			TORRENT_ASSERT(pe->blocks[i].refcount > 0);
			--pe->blocks[i].refcount;
			TORRENT_ASSERT(pe->refcount > 0);
			--pe->refcount;
			pe->blocks[i].pending = false;
			if (pe->blocks[i].dirty)
			{
				TORRENT_ASSERT(pe->num_dirty > 0);
				--pe->num_dirty;
				pe->blocks[i].dirty = false;
				TORRENT_ASSERT(m_write_cache_size > 0);
				--m_write_cache_size;
				++m_read_cache_size;
			}
#if TORRENT_DISK_STATS
			rename_buffer(pe->blocks[i].buf, "read cache");
#endif
		}

		for (int i = begin; i < end; ++i)
		{
			if (!pe->blocks[i].dirty) continue;
			// turn this block into a read cache in case
			// it was a write cache
			pe->blocks[i].dirty = false;
			TORRENT_ASSERT(pe->num_dirty > 0);
			--pe->num_dirty;
		}
	}

	for (std::list<disk_io_job>::iterator i = pe->jobs.begin();
		i != pe->jobs.end();)
	{
		TORRENT_ASSERT(i->piece == p->piece);
		i->error = ec;
		i->outstanding_writes = queue_buffer_size;
		int ret = i->buffer_size;
		if (!ec)
		{
			// if the job overlaps any blocks that are still pending,
			// leave it in the list
			int first_block = i->offset / block_size;
			int last_block = (i->offset + i->buffer_size - 1) / block_size;
			TORRENT_ASSERT(first_block >= 0);
			TORRENT_ASSERT(last_block < pe->blocks_in_piece);
			TORRENT_ASSERT(first_block <= last_block);
			if (pe->blocks[first_block].pending || pe->blocks[last_block].pending)
			{
				DLOG(stderr, "%p block_cache mark_done leaving job (overlap) "
					"piece: %d start: %d end: %d\n", &m_buffer_pool, pe->piece, begin, end);
				++i;
				continue;
			}

			if (i->action == disk_io_job::read_and_hash
				&& p->num_blocks != p->blocks_in_piece)
			{
				DLOG(stderr, "%p block_cache mark_done leaving job (read_and_hash) "
					"piece: %d num_blocks: %d blocks_in_piece: %d\n"
					, &m_buffer_pool, pe->piece, p->num_blocks, p->blocks_in_piece);
				// this job is waiting for some all blocks to be read
				++i;
				continue;
			}

			if (i->action == disk_io_job::hash
				&& p->num_dirty > 0)
			{
				DLOG(stderr, "%p block_cache mark_done leaving job (hash) "
					"piece: %d num_dirty: %d\n"
					, &m_buffer_pool, pe->piece, p->num_dirty);
				// this job is waiting for some all blocks to be written
				++i;
				continue;
			}

			if (i->action == disk_io_job::read
				|| i->action == disk_io_job::read_and_hash)
			{
				ret = copy_from_piece(p, *i);
				if (ret == -1)
				{
					TORRENT_ASSERT(false);
					// this job is waiting for some other
					// blocks from this piece, we have to
					// leave it in here. It's not clear if this
					// would ever happen and in that case why
					++i;
					continue;
				}
				else if (ret == -2)
				{
					ret = disk_io_thread::disk_operation_failed;
					i->error = error::no_memory;
				}
				else
				{
					ret = i->buffer_size;
				}
			}

			if (ret >= 0
				&& i->action == disk_io_job::read_and_hash
				&& !i->storage->get_storage_impl()->settings().disable_hash_checks)
			{
				// #error do this in a hasher thread!
				hasher sha1;
				int size = i->storage->info()->piece_size(p->piece);
				for (int k = 0; k < p->blocks_in_piece; ++k)
				{
					TORRENT_ASSERT(size > 0);
					sha1.update(p->blocks[k].buf, (std::min)(block_size, size));
					size -= block_size;
				}
				sha1_hash h = sha1.final();
				ret = (i->storage->info()->hash_for_piece(i->piece) == h)?ret:-3;
				if (ret == -3) i->storage->mark_failed(i->piece);
			}
			else if (ret >= 0
				&& i->action == disk_io_job::hash
				&& !i->storage->get_storage_impl()->settings().disable_hash_checks)
			{
				// #error replace this with an asynchronous call which uses a worker thread
				// to do the hashing. This would make better use of parallel systems
				sha1_hash h = i->storage->hash_for_piece_impl(i->piece, i->error);
				if (i->error)
				{
					i->storage->mark_failed(i->piece);
					ret = -1;
				}
				else
				{
					ret = (i->storage->info()->hash_for_piece(i->piece) == h)?0:-2;
					if (ret == -2) i->storage->mark_failed(i->piece);
				}
			}
		}
		else
		{
			// there was a read error, regardless of which blocks
			// this job is waiting for just return the failure
			ret = -1;
		}

		if (!to_delete.empty()) m_buffer_pool.free_multiple_buffers(&to_delete[0], to_delete.size());
		TORRENT_ASSERT(i->piece == pe->piece);
		DLOG(stderr, "%p block_cache mark_done post job "
			"piece: %d offset: %d\n", &m_buffer_pool, i->piece, i->offset);
		if (i->callback) ios.post(boost::bind(i->callback, ret, *i));
		i = pe->jobs.erase(i);
	}

	if (pe->jobs.empty() && pe->storage->has_fence())
	{
		DLOG(stderr, "%p piece out of jobs. Count total jobs\n", &m_buffer_pool);
		// this piece doesn't have any outstanding jobs anymore
		// and we have a fence on the storage. Are all outstanding
		// jobs complete for this storage?
		std::pair<iterator, iterator> range = pieces_for_storage(pe->storage.get());
		int has_jobs = false;
		for (iterator i = range.first; i != range.second; ++i)
		{
			if (i->jobs.empty()) continue;
			has_jobs = true;
			break;
		}

		if (!has_jobs)
		{
			DLOG(stderr, "%p no more jobs. lower fence\n", &m_buffer_pool);
			// yes, all outstanding jobs are done, lower the fence
			pe->storage->lower_fence();
		}
	}

	if (pe->marked_for_deletion && pe->refcount == 0)
	{
		cache_piece_index_t& idx = m_pieces.get<0>();
		free_piece(p);
		idx.erase(p);
	}

	TORRENT_ASSERT(m_cache_size <= m_buffer_pool.in_use());
	TORRENT_ASSERT(m_read_cache_size <= m_buffer_pool.in_use());
}

void block_cache::abort_dirty(iterator p, io_service& ios)
{
	INVARIANT_CHECK;

	cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*p);
	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		if (!pe->blocks[i].dirty || pe->blocks[i].refcount > 0) continue;
		TORRENT_ASSERT(!pe->blocks[i].pending);
		m_buffer_pool.free_buffer(pe->blocks[i].buf);
		pe->blocks[i].buf = 0;
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		TORRENT_ASSERT(m_cache_size > 0);
		--m_cache_size;
		TORRENT_ASSERT(pe->num_dirty > 0);
		--pe->num_dirty;
	}

	for (std::list<disk_io_job>::iterator i = pe->jobs.begin();
		i != pe->jobs.end();)
	{
		if (i->action != disk_io_job::write) { ++i; continue; }
		i->error.assign(libtorrent::error::operation_aborted, get_system_category());
		if (i->callback) ios.post(boost::bind(i->callback, -1, *i));
		i = pe->jobs.erase(i);
	}

	TORRENT_ASSERT(m_cache_size <= m_buffer_pool.in_use());
	TORRENT_ASSERT(m_read_cache_size <= m_buffer_pool.in_use());
}

// frees all buffers associated with this piece. May only
// be called for pieces with a refcount of 0
void block_cache::free_piece(iterator p)
{
	INVARIANT_CHECK;

	cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*p);
	TORRENT_ASSERT(pe->refcount == 0);
	// build a vector of all the buffers we need to free
	// and free them all in one go
	std::vector<char*> buffers;
	for (int i = 0; i < pe->blocks_in_piece; ++i)
	{
		if (pe->blocks[i].buf == 0) continue;
		TORRENT_ASSERT(pe->blocks[i].pending == false);
		TORRENT_ASSERT(pe->blocks[i].refcount == 0);
		buffers.push_back(pe->blocks[i].buf);
		pe->blocks[i].buf = 0;
		TORRENT_ASSERT(pe->num_blocks > 0);
		--pe->num_blocks;
		TORRENT_ASSERT(m_cache_size > 0);
		--m_cache_size;
		if (!pe->blocks[i].dirty)
		{
			TORRENT_ASSERT(m_read_cache_size > 0);
			--m_read_cache_size;
		}
		else --pe->num_dirty;
	}
	if (!buffers.empty()) m_buffer_pool.free_multiple_buffers(&buffers[0], buffers.size());

	TORRENT_ASSERT(m_cache_size <= m_buffer_pool.in_use());
	TORRENT_ASSERT(m_read_cache_size <= m_buffer_pool.in_use());
}

int block_cache::drain_piece_bufs(cached_piece_entry& p, std::vector<char*>& buf)
{
	int piece_size = p.storage->info()->piece_size(p.piece);
	int blocks_in_piece = (piece_size + block_size - 1) / block_size;
	int ret = 0;

	for (int i = 0; i < blocks_in_piece; ++i)
	{
		if (p.blocks[i].buf == 0) continue;
		buf.push_back(p.blocks[i].buf);
		++ret;
		p.blocks[i].buf = 0;
		--p.num_blocks;
		--m_cache_size;
		--m_read_cache_size;
	}
	TORRENT_ASSERT(m_cache_size <= m_buffer_pool.in_use());
	TORRENT_ASSERT(m_read_cache_size <= m_buffer_pool.in_use());
	return ret;
}

void block_cache::get_stats(cache_status* ret) const
{
	ret->blocks_read_hit = m_blocks_read_hit;
	ret->cache_size = m_cache_size;
	ret->read_cache_size = m_read_cache_size;
}

#ifdef TORRENT_DEBUG
void block_cache::check_invariant() const
{
	int cached_write_blocks = 0;
	int cached_read_blocks = 0;
	cache_piece_index_t const& idx = m_pieces.get<0>();
	for (cache_piece_index_t::const_iterator i = idx.begin()
		, end(idx.end()); i != end; ++i)
	{
		cached_piece_entry const& p = *i;
		TORRENT_ASSERT(p.blocks);
		
		TORRENT_ASSERT(p.storage);
		int piece_size = p.storage->info()->piece_size(p.piece);
		int blocks_in_piece = (piece_size + block_size - 1) / block_size;
		int num_blocks = 0;
		int num_dirty = 0;
		int num_pending = 0;
		for (int k = 0; k < blocks_in_piece; ++k)
		{
			if (p.blocks[k].buf)
			{
#ifndef TORRENT_DISABLE_POOL_ALLOCATOR
				TORRENT_ASSERT(is_disk_buffer(p.blocks[k].buf));
#endif
				++num_blocks;
				if (p.blocks[k].dirty)
				{
					++num_dirty;
					++cached_write_blocks;
				}
				else
				{
					++cached_read_blocks;
				}
				if (p.blocks[k].pending) ++num_pending;
			}
			else
			{
				TORRENT_ASSERT(!p.blocks[k].dirty);
				TORRENT_ASSERT(!p.blocks[k].pending);
			}
		}
		TORRENT_ASSERT(num_blocks == p.num_blocks);
		TORRENT_ASSERT(num_dirty == p.num_dirty);
		TORRENT_ASSERT(num_pending <= p.refcount);
	}
	TORRENT_ASSERT(m_read_cache_size == cached_read_blocks);
	TORRENT_ASSERT(m_cache_size == cached_read_blocks + cached_write_blocks);

#ifdef TORRENT_DISK_STATS
	int read_allocs = m_categories.find(std::string("read cache"))->second;
	int write_allocs = m_categories.find(std::string("write cache"))->second;
	TORRENT_ASSERT(cached_read_blocks == read_allocs);
	TORRENT_ASSERT(cached_write_blocks == write_allocs);
#endif
}
#endif

// returns
// -1: block not in caceh
// -2: out of memory

int block_cache::copy_from_piece(iterator p, disk_io_job& j)
{
	INVARIANT_CHECK;

	TORRENT_ASSERT(j.buffer == 0);

	cached_piece_entry* pe = const_cast<cached_piece_entry*>(&*p);

	// copy from the cache and update the last use timestamp
	int block = j.offset / block_size;
	int block_offset = j.offset & (block_size-1);
	int buffer_offset = 0;
	int size = j.buffer_size;
	int min_blocks_to_read = block_offset > 0 ? 2 : 1;
	TORRENT_ASSERT(size <= block_size);
	int start_block = block;
	if (pe->blocks[start_block].buf != 0
		&& !pe->blocks[start_block].pending
		&& min_blocks_to_read > 1)
		++start_block;
	// if block_offset > 0, we need to read two blocks, and then
	// copy parts of both, because it's not aligned to the block
	// boundaries
	if (pe->blocks[start_block].buf == 0
		|| pe->blocks[start_block].pending) return -1;

	j.buffer = m_buffer_pool.allocate_buffer("send buffer");
	if (j.buffer == 0) return -2;

	// build a vector of all the buffers we need to free
	// and free them all in one go
	std::vector<char*> buffers;

	while (size > 0)
	{
		TORRENT_ASSERT(pe->blocks[block].buf);
		int to_copy = (std::min)(block_size
				- block_offset, size);
		std::memcpy(j.buffer + buffer_offset
			, pe->blocks[block].buf + block_offset
			, to_copy);
		size -= to_copy;
		block_offset = 0;
		buffer_offset += to_copy;
		// #error this breaks if there are multiple requests to the same block
		// the first request will go through, but the second one will read a NULL pointer
/*
		if (j.flags & disk_io_job::volatile_read)
		{
			// if volatile read cache is set, the assumption is
			// that no other peer is likely to request the same
			// piece. Therefore, for each request out of the cache
			// we clear the block that was requested and any blocks
			// the peer skipped
			for (int i = block; i >= 0 && pe->blocks[i].buf; --i)
			{
				if (pe->blocks[i].refcount > 0) continue;

				buffers.push_back(pe->blocks[i].buf);
				pe->blocks[i].buf = 0;
				TORRENT_ASSERT(pe->num_blocks > 0);
				--pe->num_blocks;
				TORRENT_ASSERT(m_cache_size > 0);
				--m_cache_size;
				TORRENT_ASSERT(m_read_cache_size > 0);
				--m_read_cache_size;
			}
		}
*/
		++block;
	}
	if (!buffers.empty()) m_buffer_pool.free_multiple_buffers(&buffers[0], buffers.size());
	return j.buffer_size;
}

block_cache::iterator block_cache::find_piece(
	disk_io_job const& j)
{
	cache_piece_index_t& idx = m_pieces.get<0>();
	cache_piece_index_t::iterator i
		= idx.find(boost::make_tuple((void*)j.storage.get(), j.piece));
	TORRENT_ASSERT(i == idx.end() || (i->storage == j.storage && i->piece == j.piece));
	return i;
}

}	
