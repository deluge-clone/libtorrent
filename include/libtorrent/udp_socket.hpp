/*

Copyright (c) 2007, Arvid Norberg
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

#ifndef TORRENT_UDP_SOCKET_HPP_INCLUDED
#define TORRENT_UDP_SOCKET_HPP_INCLUDED

#include "libtorrent/socket.hpp"
#include "libtorrent/io_service.hpp"
#include "libtorrent/error_code.hpp"
#include "libtorrent/session_settings.hpp"
#include "libtorrent/buffer.hpp"
#include "libtorrent/thread.hpp"
#include "libtorrent/deadline_timer.hpp"

#include <list>
#include <boost/function/function4.hpp>

namespace libtorrent
{
	class connection_queue;

	class udp_socket
	{
	public:
		typedef boost::function<void(error_code const& ec
			, udp::endpoint const&, char const* buf, int size)> callback_t;
		typedef boost::function<void(error_code const& ec
			, char const*, char const* buf, int size)> callback2_t;

		udp_socket(io_service& ios, callback_t const& c, callback2_t const& c2, connection_queue& cc);
		~udp_socket();

		bool is_open() const
		{
			return m_ipv4_sock.is_open()
#if TORRENT_USE_IPV6
				|| m_ipv6_sock.is_open()
#endif
				;
		}
		io_service& get_io_service() { return m_ipv4_sock.get_io_service(); }

		// this is only valid when using a socks5 proxy
		void send_hostname(char const* hostname, int port, char const* p, int len, error_code& ec);

		void send(udp::endpoint const& ep, char const* p, int len, error_code& ec);
		void bind(udp::endpoint const& ep, error_code& ec);
		void bind(int port);
		void close();
		int local_port() const { return m_bind_port; }

		void set_proxy_settings(proxy_settings const& ps);
		proxy_settings const& get_proxy_settings() { return m_proxy_settings; }

		bool is_closed() const { return m_abort; }
		tcp::endpoint local_endpoint() const
		{
			error_code ec;
			udp::endpoint ep = m_ipv4_sock.local_endpoint(ec);
			return tcp::endpoint(ep.address(), ep.port());
		}

	protected:

		struct queued_packet
		{
			udp::endpoint ep;
			char* hostname;
			buffer buf;
		};

	private:

		// callback for regular incoming packets
		callback_t m_callback;

		// callback for proxied incoming packets with a domain
		// name as source
		callback2_t m_callback2;

		void on_read(udp::socket* sock, error_code const& e, std::size_t bytes_transferred);
		void on_name_lookup(error_code const& e, tcp::resolver::iterator i);
		void on_timeout();
		void on_connect(int ticket);
		void on_connected(error_code const& ec);
		void handshake1(error_code const& e);
		void handshake2(error_code const& e);
		void handshake3(error_code const& e);
		void handshake4(error_code const& e);
		void socks_forward_udp(mutex::scoped_lock& l);
		void connect1(error_code const& e);
		void connect2(error_code const& e);
		void hung_up(error_code const& e);

		void wrap(udp::endpoint const& ep, char const* p, int len, error_code& ec);
		void wrap(char const* hostname, int port, char const* p, int len, error_code& ec);
		void unwrap(error_code const& e, char const* buf, int size);

		mutable mutex m_mutex;

		udp::socket m_ipv4_sock;
		udp::endpoint m_v4_ep;
		char m_v4_buf[1600];

#if TORRENT_USE_IPV6
		udp::socket m_ipv6_sock;
		udp::endpoint m_v6_ep;
		char m_v6_buf[1600];
#endif

		int m_bind_port;
		char m_outstanding;

		tcp::socket m_socks5_sock;
		int m_connection_ticket;
		proxy_settings m_proxy_settings;
		connection_queue& m_cc;
		tcp::resolver m_resolver;
		char m_tmp_buf[270];
		bool m_queue_packets;
		bool m_tunnel_packets;
		bool m_abort;
		udp::endpoint m_proxy_addr;
		// while we're connecting to the proxy
		// we have to queue the packets, we'll flush
		// them once we're connected
		std::list<queued_packet> m_queue;
#ifdef TORRENT_DEBUG
		bool m_started;
		int m_magic;
		int m_outstanding_when_aborted;
#endif
	};

	struct rate_limited_udp_socket : public udp_socket
	{
		rate_limited_udp_socket(io_service& ios, callback_t const& c, callback2_t const& c2, connection_queue& cc);
		void set_rate_limit(int limit) { m_rate_limit = limit; }
		bool can_send() const { return int(m_queue.size()) >= m_queue_size_limit; }
		bool send(udp::endpoint const& ep, char const* p, int len, error_code& ec, int flags = 0);
		void close();

	private:
		void on_tick(error_code const& e);

		deadline_timer m_timer;
		int m_queue_size_limit;
		int m_rate_limit;
		int m_quota;
		ptime m_last_tick;
		std::list<queued_packet> m_queue;
	};
}

#endif

