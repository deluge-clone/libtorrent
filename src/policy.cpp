/*

Copyright (c) 2003, Arvid Norberg
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

#include <iostream>

#include <boost/date_time/posix_time/posix_time.hpp>

#include "libtorrent/policy.hpp"
#include "libtorrent/torrent.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/peer_connection.hpp"

// TODO: move all alerts to a single header
// session.hpp is included just for the peer_error_alert
#include "libtorrent/session.hpp"

#if defined(_MSC_VER) && _MSC_VER < 1300
#	define for if (false) {} else for
#endif

namespace libtorrent
{
	class peer_connection;
}

namespace
{
	enum
	{
		// the limits of the download queue size
		max_request_queue = 16,
		min_request_queue = 2,

		// the amount of free upload allowed before
		// the peer is choked
		free_upload_amount = 4 * 16 * 1024
	};


	using namespace libtorrent;


	// TODO: replace these two functions with std::find_first_of
	template<class It1, class It2>
	bool has_intersection(It1 start1, It1 end1, It2 start2, It2 end2)
	{
		for (;start1 != end1; ++start1)
			for (;start2 != end2; ++start2)
				if (*start1 == *start2) return true;
		return false;
	}

	piece_block find_first_common(const std::deque<piece_block>& queue,
		const std::vector<piece_block>& busy)
	{
		for (std::deque<piece_block>::const_reverse_iterator i
			= queue.rbegin();
			i != queue.rend();
			++i)
		{
			for (std::vector<piece_block>::const_iterator j
				= busy.begin();
				j != busy.end();
				++j)
			{
				if ((*j) == (*i)) return *i;
			}
		}
		assert(false);
		return piece_block(-1, -1);
	}

	namespace
	{
		int to_seconds(const boost::posix_time::time_duration& d)
		{
			return d.hours() * 60 * 60 + d.minutes() * 60 + d.seconds();
		}
	}

	void request_a_block(torrent& t, peer_connection& c)
	{
		int desired_queue_size = max_request_queue + 1 - to_seconds(c.last_piece_time()) / 4;
		if (desired_queue_size > max_request_queue) desired_queue_size = max_request_queue;
		if (desired_queue_size < min_request_queue) desired_queue_size = min_request_queue;

		assert(desired_queue_size >= min_request_queue);

		int num_requests = desired_queue_size - c.download_queue().size();

		// if our request queue is already full, we
		// don't have to make any new requests yet
		if (num_requests <= 0) return;

		piece_picker& p = t.picker();
		std::vector<piece_block> interesting_pieces;
		interesting_pieces.reserve(100);

		// picks the interesting pieces from this peer
		// the integer is the number of pieces that
		// should be guaranteed to be available for download
		// (if num_requests is too big, too many pieces are
		// picked and cpu-time is wasted)
		p.pick_pieces(c.get_bitfield(), interesting_pieces, num_requests);

		// this vector is filled with the interesting pieces
		// that some other peer is currently downloading
		// we should then compare this peer's download speed
		// with the other's, to see if we should abort another
		// peer_connection in favour of this one
		std::vector<piece_block> busy_pieces;
		busy_pieces.reserve(10);

		for (std::vector<piece_block>::iterator i = interesting_pieces.begin();
			i != interesting_pieces.end();
			++i)
		{
			if (p.is_downloading(*i))
			{
				busy_pieces.push_back(*i);
				continue;
			}

			// ok, we found a piece that's not being downloaded
			// by somebody else. request it from this peer
			c.send_request(*i);
			num_requests--;
			if (num_requests <= 0) return;
		}

		if (busy_pieces.empty()) return;

		// first look for blocks that are just queued
		// and not actually sent to us yet
		// (then we can cancel those and request them
		// from this peer instead)

		peer_connection* peer = 0;
		float down_speed = -1.f;
		// find the peer with the lowest download
		// speed that also has a piece that this
		// peer could send us
		for (torrent::peer_iterator i = t.begin();
			i != t.end();
			++i)
		{
			const std::deque<piece_block>& queue = i->second->download_queue();
			if (i->second->statistics().down_peak() > down_speed
				&& has_intersection(busy_pieces.begin(),
					busy_pieces.end(),
					queue.begin(),
					queue.end()))
			{
				peer = i->second;
				down_speed = peer->statistics().down_peak();
			}
		}

		assert(peer != 0);

		// this peer doesn't have a faster connection than the
		// slowest peer. Don't take over any blocks
		if (c.statistics().down_peak() <= down_speed) return;

		// find a suitable block to take over from this peer
		piece_block block = find_first_common(peer->download_queue(), busy_pieces);
		peer->send_cancel(block);
		c.send_request(block);

		// the one we interrupted may need to request a new piece
		request_a_block(t, *peer);

		num_requests--;
	}


	int collect_free_download(
		torrent::peer_iterator start
		, torrent::peer_iterator end)
	{
		int accumulator = 0;
		for (torrent::peer_iterator i = start; i != end; ++i)
		{
			// if the peer is interested in us, it means it may
			// want to trade it's surplus uploads for downloads itself
			// (and we should consider it free). If the share diff is
			// negative, there's no free download to get from this peer.
			int diff = i->second->share_diff();
			if (i->second->is_peer_interested() || diff <= 0)
				continue;

			assert(diff > 0);
			i->second->add_free_upload(-diff);
			accumulator += diff;
			assert(accumulator > 0);
		}
		assert(accumulator >= 0);
		return accumulator;
	}


	// returns the amount of free upload left after
	// it has been distributed to the peers
	int distribute_free_upload(
		torrent::peer_iterator start
		, torrent::peer_iterator end
		, int free_upload)
	{
		if (free_upload <= 0) return free_upload;
		int num_peers = 0;
		int total_diff = 0;
		for (torrent::peer_iterator i = start; i != end; ++i)
		{
			total_diff += i->second->share_diff();
			if (!i->second->is_peer_interested() || i->second->share_diff() >= 0) continue;
			++num_peers;
		}

		if (num_peers == 0) return free_upload;
		int upload_share;
		if (total_diff >= 0)
		{
			upload_share = std::min(free_upload, total_diff) / num_peers;
		}
		else
		{
			upload_share = (free_upload + total_diff) / num_peers;
		}
		if (upload_share < 0) return free_upload;

		for (torrent::peer_iterator i = start; i != end; ++i)
		{
			peer_connection* p = i->second;
			if (!p->is_peer_interested() || p->share_diff() >= 0) continue;
			p->add_free_upload(upload_share);
			free_upload -= upload_share;
		}
		return free_upload;
	}

	struct match_peer_ip
	{
		match_peer_ip(const address& id)
			: m_id(id)
		{}

		bool operator()(const policy::peer& p) const
		{ return p.id.ip() == m_id.ip(); }

		address m_id;
	};

	struct match_peer_connection
	{
		match_peer_connection(const peer_connection& c)
			: m_conn(c)
		{}

		bool operator()(const policy::peer& p) const
		{ return p.connection == &m_conn; }

		const peer_connection& m_conn;
	};


}

namespace libtorrent
{
/*
	TODO: implement some kind of limit of the number of sockets
	opened, to use for systems where a user has a limited number
	of open file descriptors. and for windows which has a buggy tcp-stack.
	This means also to implement a 'connecion purger', that identifies
	more or less useless connections and closes them.
*/

	policy::policy(torrent* t)
		: m_num_peers(0)
		, m_torrent(t)
		, m_max_uploads(std::numeric_limits<int>::max())
		, m_num_unchoked(0)
		, m_available_free_upload(0)
	{}
	// finds the peer that has the worst download rate
	// and returns it. May return 0 if all peers are
	// choked.
	policy::peer* policy::find_choke_candidate()
	{
		peer* worst_peer = 0;
		int min_weight = std::numeric_limits<int>::max();

		// TODO: make this selection better

		for (std::vector<peer>::iterator i = m_peers.begin();
			i != m_peers.end();
			++i)
		{
			peer_connection* c = i->connection;

			if (c == 0) continue;
			if (c->is_choked()) continue;
			// if the peer isn't interested, just choke it
			if (!c->is_peer_interested())
				return &(*i);

			int diff = i->total_download()
				- i->total_upload();

			int weight = static_cast<int>(c->statistics().download_rate() * 10.f)
				+ diff
				+ (c->has_peer_choked()?-10:10)*1024;

			if (weight > min_weight) continue;

			min_weight = weight;
			worst_peer = &(*i);
			continue;
		}
		return worst_peer;
	}

	policy::peer* policy::find_unchoke_candidate()
	{
		// if all of our peers are unchoked, there's
		// no left to unchoke
		if (m_num_unchoked == m_torrent->num_peers())
			return 0;

		using namespace boost::posix_time;
		using namespace boost::gregorian;

		peer* unchoke_peer = 0;
		ptime min_time(date(9999,Jan,1));
		float max_down_speed = 0.f;

		// TODO: make this selection better

		for (std::vector<peer>::iterator i = m_peers.begin();
			i != m_peers.end();
			++i)
		{
			peer_connection* c = i->connection;
			if (c == 0) continue;
			if (!c->is_choked()) continue;
			if (!c->is_peer_interested()) continue;
			if (c->share_diff()
				< -free_upload_amount) continue;
			if (c->statistics().download_rate() < max_down_speed) continue;
//			if (i->last_optimistically_unchoked > min_time) continue;

			min_time = i->last_optimistically_unchoked;
			max_down_speed = c->statistics().download_rate();
			unchoke_peer = &(*i);
		}
		return unchoke_peer;
	}

	policy::peer *policy::find_connect_candidate()
	{
		boost::posix_time::ptime local_time=boost::posix_time::second_clock::local_time();
		boost::posix_time::ptime ptime(local_time);
		policy::peer *candidate=0;

		for (std::vector<peer>::iterator i = m_peers.begin();
			i != m_peers.end();
			++i)
		{
			if(i->connection) continue;
			if(i->banned) continue;
			if(!i->connectable) continue;

			assert(i->connected <= local_time);

			boost::posix_time::ptime next_connect=i->connected + boost::posix_time::seconds(10*60);

			if (next_connect <= ptime)
			{
				ptime=next_connect;
				candidate=&*i;
			}
		}
		
		assert(ptime <= local_time);

		return candidate;
	}

	void policy::pulse()
	{
		using namespace boost::posix_time;

/*		// remove old disconnected peers from the list
		m_peers.erase(
			std::remove_if(m_peers.begin()
			, m_peers.end()
			, old_disconnected_peer())
			, m_peers.end());
*/
		//while(connect_one_peer());
		connect_one_peer();


		// if the share ratio is 0 (infinite)
		// m_available_free_upload isn't used
		// because it isn't necessary
		if (m_torrent->ratio() != 0.f)
		{
			// accumulate all the free download we get
			// and add it to the available free upload
			m_available_free_upload
				+= collect_free_download(
					m_torrent->begin()
					, m_torrent->end());

			// distribute the free upload among the peers
			m_available_free_upload = distribute_free_upload(
				m_torrent->begin()
				, m_torrent->end()
				, m_available_free_upload);
		}

		// ------------------------
		// seed policy
		// ------------------------
		if (m_torrent->is_seed())
		{
			// make sure we have enough
			// unchoked peers
			while (m_num_unchoked < m_max_uploads)
			{
				peer* p = 0;
				for (std::vector<peer>::iterator i = m_peers.begin();
					i != m_peers.end();
					++i)
				{
					peer_connection* c = i->connection;
					if (c == 0) continue;
					if (!c->is_choked()) continue;
					if (!c->is_peer_interested()) continue;
// TODO: add some more criterions here. Maybe the peers
// that have less should be promoted? (to allow them to trade)
					p = &(*i);
				}

				if (p == 0) break;

				p->connection->send_unchoke();
				p->last_optimistically_unchoked = boost::posix_time::second_clock::local_time();
				++m_num_unchoked;
			}
		}

		// ------------------------
		// downloading policy
		// ------------------------
		else
		{
			// choke peers that have leeched too much without giving anything back
			for (std::vector<peer>::iterator i = m_peers.begin();
				i != m_peers.end();
				++i)
			{
				peer_connection* c = i->connection;
				if (c == 0) continue;

				int diff = i->connection->share_diff();
				if (diff < -free_upload_amount
					&& !c->is_choked())
				{
					// if we have uploaded more than a piece for free, choke peer and
					// wait until we catch up with our download.
					c->send_choke();
					--m_num_unchoked;
				}
			}
			
			// make sure we don't have too many
			// unchoked peers
			while (m_num_unchoked > m_max_uploads)
			{
				peer* p = find_choke_candidate();
				assert(p);
				p->connection->send_choke();
				--m_num_unchoked;
			}

			// optimistic unchoke. trade the 'worst'
			// unchoked peer with one of the choked
			assert(m_num_unchoked <= m_torrent->num_peers());
			peer* p = find_choke_candidate();
			if (p)
			{
				p->connection->send_choke();
				--m_num_unchoked;
				unchoke_one_peer();
			}


			// make sure we have enough
			// unchoked peers
			while (m_num_unchoked < m_max_uploads && unchoke_one_peer());
		}

#ifndef NDEBUG
		check_invariant();
#endif
	}

	void policy::ban_peer(const peer_connection& c)
	{
		std::vector<peer>::iterator i = std::find_if(
			m_peers.begin()
			, m_peers.end()
			, match_peer_connection(c));

		assert(i != m_peers.end());

		i->banned = true;
	}

	void policy::new_connection(peer_connection& c)
	{
		assert(!c.is_local());

		std::vector<peer>::iterator i = std::find_if(
			m_peers.begin()
			, m_peers.end()
			, match_peer_ip(c.get_socket()->sender()));

		if (i == m_peers.end())
		{
			using namespace boost::posix_time;
			using namespace boost::gregorian;

			// we don't have ny info about this peer.
			// add a new entry
			
			peer p(c.get_socket()->sender(), peer::not_connectable);
			m_peers.push_back(p);
			i = m_peers.end()-1;
		}
		else
		{
			if (i->connection != 0)
				throw protocol_error("duplicate connection, closing");
			if (i->banned)
				throw protocol_error("ip address banned, disconnected");
		}
		
		assert(i->connection == 0);
		i->connection = &c;
		i->connected = boost::posix_time::second_clock::local_time();
	}

	void policy::peer_from_tracker(const address& remote, const peer_id& id)
	{
		// just ignore the obviously invalid entries from the tracker
		if(remote.ip() == 0 || remote.port() == 0)
			return;

		try
		{
			std::vector<peer>::iterator i = std::find_if(
				m_peers.begin()
				, m_peers.end()
				, match_peer_ip(remote));
			
			if (i == m_peers.end())
			{
				using namespace boost::posix_time;
				using namespace boost::gregorian;

				// we don't have ny info about this peer.
				// add a new entry
				peer p(remote, peer::connectable);
				m_peers.push_back(p);
				// the iterator is invalid
				// because of the push_back()
				i = m_peers.end()-1;
			}
			else
			{
				i->type = peer::connectable;

				// in case we got the ip from a remote connection, port is
				// not known, so save it. Client may also have changed port
				// for some reason.
				i->id = remote;

				if (i->connection)
				{
					// this means we're already connected
					// to this peer. don't connect to
					// it again.
					assert(i->connection->associated_torrent() == m_torrent);
					return;
				}
			}

			if (i->banned) return;

			i->connection = &m_torrent->connect_to_peer(remote);
			i->connected = boost::posix_time::second_clock::local_time();
			return;
		}
		catch(network_error& e)
		{
			if (m_torrent->alerts().should_post(alert::debug))
			{
				m_torrent->alerts().post_alert(
					peer_error_alert(id, e.what()));
			}
		}
		catch(protocol_error& e)
		{
			if (m_torrent->alerts().should_post(alert::debug))
			{
				m_torrent->alerts().post_alert(
					peer_error_alert(id, e.what()));
			}
		}
	}

	// this is called when we are choked by a peer
	// i.e. a peer lets us know that we will not receive
	// anything for a while
	void policy::choked(peer_connection& c)
	{
	}

	void policy::piece_finished(int index, bool successfully_verified)
	{
		if (successfully_verified)
		{
			// have all peers update their interested-flag
			for (std::vector<peer>::iterator i = m_peers.begin();
				i != m_peers.end();
				++i)
			{
				if (i->connection == 0) continue;
				// if we're not interested, we will not become interested
				if (!i->connection->is_interesting()) continue;
				if (!i->connection->has_piece(index)) continue;

				bool interested = false;
				const std::vector<bool>& peer_has = i->connection->get_bitfield();
				const std::vector<bool>& we_have = m_torrent->pieces();
				assert(we_have.size() == peer_has.size());
				for (int j = 0; j != we_have.size(); ++j)
				{
					if (!we_have[j] && peer_has[j])
					{
						interested = true;
						break;
					}
				}
				if (!interested)
					i->connection->send_not_interested();
			}
		}
	}

	// TODO: we must be able to get interested
	// in a peer again, if a piece fails that
	// this peer has.
	void policy::block_finished(peer_connection& c, piece_block b)
	{
		// if the peer hasn't choked us, ask for another piece
		if (!c.has_peer_choked())
			request_a_block(*m_torrent, c);
	}

	// this is called when we are unchoked by a peer
	// i.e. a peer lets us know that we will receive
	// data from now on
	void policy::unchoked(peer_connection& c)
	{
		if (c.is_interesting())
		{
			request_a_block(*m_torrent, c);
		}
	}

	// called when a peer is interested in us
	void policy::interested(peer_connection& c)
	{
	}

	// called when a peer is no longer interested in us
	void policy::not_interested(peer_connection& c)
	{
		// TODO: return the diff() of this peer to the
		// pool of undistributed free upload
	}

	bool policy::unchoke_one_peer()
	{
		peer* p = find_unchoke_candidate();
		if (p == 0) return false;

		p->connection->send_unchoke();
		p->last_optimistically_unchoked = boost::posix_time::second_clock::local_time();
		++m_num_unchoked;
		return true;
	}

	bool policy::connect_one_peer()
	{
		peer* p = find_connect_candidate();
		if (p == 0) return false;
		assert(!p->banned);
		assert(!p->connection);
		assert(p->connectable);

		p->connection = &m_torrent->connect_to_peer(p->id);
		p->connected = boost::posix_time::second_clock::local_time();
		return true;
	}

	// this is called whenever a peer connection is closed
	void policy::connection_closed(const peer_connection& c)
	{
		std::vector<peer>::iterator i = std::find_if(
			m_peers.begin()
			, m_peers.end()
			, match_peer_connection(c));

		if (i == m_peers.end()) return;
		assert(i->connection == &c);

		i->connected = boost::posix_time::second_clock::local_time();
		i->prev_amount_download += c.statistics().total_payload_download();
		i->prev_amount_upload += c.statistics().total_payload_upload();
		if (!i->connection->is_choked() && !m_torrent->is_aborted())
		{
			--m_num_unchoked;
			unchoke_one_peer();
		}

		// if the share ratio is 0 (infinite), the
		// m_available_free_upload isn't used,
		// because it isn't necessary.
		if (m_torrent->ratio() != 0.f)
		{
			assert(i->connection->share_diff() < std::numeric_limits<int>::max());
			m_available_free_upload += i->connection->share_diff();
		}
		i->connection = 0;
	}

	void policy::set_max_uploads(int max_uploads)
	{
		assert(max_uploads > 1 || max_uploads == -1);
		if (max_uploads == -1) max_uploads = std::numeric_limits<int>::max();
		m_max_uploads = max_uploads;
	}

	void policy::peer_is_interesting(peer_connection& c)
	{
		c.send_interested();
		if (c.has_peer_choked()) return;
		request_a_block(*m_torrent, c);
	}

#ifndef NDEBUG
	bool policy::has_connection(const peer_connection* c)
	{
		return std::find_if(
			m_peers.begin()
			, m_peers.end()
			, match_peer_ip(c->get_socket()->sender())) != m_peers.end();
	}

	void policy::check_invariant()
	{
		assert(m_max_uploads >= 2);
		int actual_unchoked = 0;
		for (std::vector<peer>::iterator i = m_peers.begin();
			i != m_peers.end();
			++i)
		{
			if (!i->connection) continue;
			if (!i->connection->is_choked()) actual_unchoked++;
		}
		assert(actual_unchoked <= m_max_uploads);
	}
#endif

	policy::peer::peer(const address& pid, peer::connection_type t)
		: id(pid)
		, type(t)
		, last_optimistically_unchoked(
			boost::gregorian::date(1970,boost::gregorian::Jan,1))
		, connected(boost::gregorian::date(1970,boost::gregorian::Jan,1))
		, prev_amount_upload(0)
		, prev_amount_download(0)
		, banned(false)
		, connection(0)
	{
		assert(connected < boost::posix_time::second_clock::local_time());
	}

	int policy::peer::total_download() const
	{
		if (connection != 0)
		{
			return connection->statistics().total_payload_download()
				+ prev_amount_download;
		}
		else
		{
			return prev_amount_download;
		}
	}

	int policy::peer::total_upload() const
	{
		if (connection != 0)
		{
			return connection->statistics().total_payload_upload()
				+ prev_amount_upload;
		}
		else
		{
			return prev_amount_upload;
		}
	}
}
