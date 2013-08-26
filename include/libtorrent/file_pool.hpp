/*

Copyright (c) 2006-2012, Arvid Norberg
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

#ifndef TORRENT_FILE_POOL_HPP
#define TORRENT_FILE_POOL_HPP

#include <map>
#include "libtorrent/file.hpp"
#include "libtorrent/ptime.hpp"
#include "libtorrent/thread.hpp"
#include "libtorrent/file_storage.hpp"

namespace libtorrent
{
	struct pool_file_status
	{
		// the index of the file this entry refers to into the ``file_storage``
		// file list of this torrent. This starts indexing at 0.
		int file_index;

		// a (high precision) timestamp of when the file was last used.
		ptime last_use;

		// ``open_mode`` is a bitmask of the file flags this file is currently opened with. These
		// are the flags used in the ``file::open()`` function. This enum is defined as a member
		// of the ``file`` class.
		// 
		// ::
		// 
		// 	enum
		// 	{
		// 		read_only = 0,
		// 		write_only = 1,
		// 		read_write = 2,
		// 		rw_mask = 3,
		// 		no_buffer = 4,
		// 		sparse = 8,
		// 		no_atime = 16,
		// 		random_access = 32,
		// 		lock_file = 64,
		// 	};
		// 
		// Note that the read/write mode is not a bitmask. The two least significant bits are used
		// to represent the read/write mode. Those bits can be masked out using the ``rw_mask`` constant.
		int open_mode;
	};

	struct TORRENT_EXPORT file_pool : boost::noncopyable
	{
		file_pool(int size = 40);
		~file_pool();

		file_handle open_file(void* st, std::string const& p
			, int file_index, file_storage const& fs, int m, error_code& ec);
		void release(void* st = NULL);
		void release(void* st, int file_index);
		void resize(int size);
		int size_limit() const { return m_size; }
		void set_low_prio_io(bool b) { m_low_prio_io = b; }
		void get_status(std::vector<pool_file_status>* files, void* st) const;

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		bool assert_idle_files(void* st) const;

		// remember that this storage has had
		// its files deleted. We may not open any
		// files from it again
		void mark_deleted(file_storage const& fs);
#endif

	private:

		void remove_oldest(mutex::scoped_lock& l);

		int m_size;
		bool m_low_prio_io;

		struct lru_file_entry
		{
			lru_file_entry(): key(0), last_use(time_now()), mode(0) {}
			mutable file_handle file_ptr;
			void* key;
			ptime last_use;
			int mode;
		};

		// maps storage pointer, file index pairs to the
		// lru entry for the file
		typedef std::map<std::pair<void*, int>, lru_file_entry> file_set;
		
		file_set m_files;

#if defined TORRENT_DEBUG || TORRENT_RELEASE_ASSERTS
		std::vector<std::pair<std::string, void const*> > m_deleted_storages;
#endif
		mutable mutex m_mutex;
	};
}

#endif
