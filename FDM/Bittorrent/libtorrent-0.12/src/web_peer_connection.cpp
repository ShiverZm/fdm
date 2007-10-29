/*
  Free Download Manager Copyright (c) 2003-2007 FreeDownloadManager.ORG
*/  



#include <vector>
#include <iostream>
#include <iomanip>
#include <limits>
#include <boost/bind.hpp>
#include <sstream>

#include "libtorrent/web_peer_connection.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/identify_client.hpp"
#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/invariant_check.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/version.hpp"
#include "libtorrent/aux_/session_impl.hpp"

using namespace boost::posix_time;
using boost::bind;
using boost::shared_ptr;
using libtorrent::aux::session_impl;

namespace libtorrent
{
	web_peer_connection::web_peer_connection(
		session_impl& ses
		, boost::weak_ptr<torrent> t
		, boost::shared_ptr<stream_socket> s
		, tcp::endpoint const& remote
		, tcp::endpoint const& proxy
		, std::string const& url)
		: peer_connection(ses, t, s, remote, proxy)
		, m_url(url)
		, m_first_request(true)
	{
		INVARIANT_CHECK;

		
		
		prefer_whole_pieces(true);
		
		
		request_large_blocks(true);
		
		set_non_prioritized(true);
		shared_ptr<torrent> tor = t.lock();
		assert(tor);
		int blocks_per_piece = tor->torrent_file().piece_length() / tor->block_size();
		
		
		
		m_max_out_request_queue = ses.settings().urlseed_pipeline_size
			* blocks_per_piece;

		
		
		set_timeout(ses.settings().urlseed_timeout);
#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << "*** web_peer_connection\n";
#endif

		std::string protocol;
		boost::tie(protocol, m_host, m_port, m_path)
			= parse_url_components(url);
			
		m_server_string = "URL seed @ ";
		m_server_string += m_host;
	}

	web_peer_connection::~web_peer_connection()
	{}
	
	boost::optional<piece_block_progress>
	web_peer_connection::downloading_piece_progress() const
	{
		if (!m_parser.header_finished() || m_requests.empty())
			return boost::optional<piece_block_progress>();

		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);

		buffer::const_interval http_body = m_parser.get_body();
		piece_block_progress ret;

		ret.piece_index = m_requests.front().piece;
		ret.bytes_downloaded = http_body.left() % t->block_size();
		ret.block_index = (m_requests.front().start + ret.bytes_downloaded) / t->block_size();
		ret.full_block_bytes = t->block_size();
		const int last_piece = t->torrent_file().num_pieces() - 1;
		if (ret.piece_index == last_piece && ret.block_index
			== t->torrent_file().piece_size(last_piece) / t->block_size())
			ret.full_block_bytes = t->torrent_file().piece_size(last_piece) % t->block_size();
		return ret;
	}

	void web_peer_connection::on_connected()
	{
		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);
	
		
		incoming_bitfield(std::vector<bool>(
			t->torrent_file().num_pieces(), true));
		
		incoming_unchoke();
		
		reset_recv_buffer(t->torrent_file().piece_length() + 1024 * 2);
	}

	void web_peer_connection::write_request(peer_request const& r)
	{
		INVARIANT_CHECK;

		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);

		assert(t->valid_metadata());

		bool single_file_request = false;
		if (!m_path.empty() && m_path[m_path.size() - 1] != '/')
			single_file_request = true;

		torrent_info const& info = t->torrent_file();
		
		std::string request;

		int size = r.length;
		const int block_size = t->block_size();
		while (size > 0)
		{
			int request_size = std::min(block_size, size);
			peer_request pr = {r.piece, r.start + r.length - size
				,  request_size};
			m_requests.push_back(pr);
			size -= request_size;
		}

		bool using_proxy = false;
		if (!m_ses.settings().proxy_ip.empty())
			using_proxy = true;

		if (single_file_request)
		{
			request += "GET ";
			
			
			request += using_proxy ? m_url : m_path;
			request += " HTTP/1.1\r\n";
			request += "Host: ";
			request += m_host;
			if (m_first_request)
			{
				request += "\r\nUser-Agent: ";
				request += m_ses.settings().user_agent;
			}
			if (using_proxy && !m_ses.settings().proxy_login.empty())
			{
				request += "\r\nProxy-Authorization: Basic ";
				request += base64encode(m_ses.settings().proxy_login + ":"
					+ m_ses.settings().proxy_password);
			}
			if (using_proxy)
			{
				request += "\r\nProxy-Connection: keep-alive";
			}
			request += "\r\nRange: bytes=";
			request += boost::lexical_cast<std::string>(r.piece
				* info.piece_length() + r.start);
			request += "-";
			request += boost::lexical_cast<std::string>(r.piece
				* info.piece_length() + r.start + r.length - 1);
			if (m_first_request || using_proxy)
				request += "\r\nConnection: keep-alive";
			request += "\r\n\r\n";
			m_first_request = false;
			m_file_requests.push_back(0);
		}
		else
		{
			std::vector<file_slice> files = info.map_block(r.piece, r.start
				, r.length);

			for (std::vector<file_slice>::iterator i = files.begin();
				i != files.end(); ++i)
			{
				file_slice const& f = *i;

				request += "GET ";
				if (using_proxy)
				{
					request += m_url;
					std::string path = info.file_at(f.file_index).path.string();
					request += escape_path(path.c_str(), path.length());
				}
				else
				{
					std::string path = m_path;
					path += info.file_at(f.file_index).path.string();
					request += escape_path(path.c_str(), path.length());
				}
				request += " HTTP/1.1\r\n";
				request += "Host: ";
				request += m_host;
				if (m_first_request)
				{
					request += "\r\nUser-Agent: ";
					request += m_ses.settings().user_agent;
				}
				if (using_proxy && !m_ses.settings().proxy_login.empty())
				{
					request += "\r\nProxy-Authorization: Basic ";
					request += base64encode(m_ses.settings().proxy_login + ":"
						+ m_ses.settings().proxy_password);
				}
				if (using_proxy)
				{
					request += "\r\nProxy-Connection: keep-alive";
				}
				request += "\r\nRange: bytes=";
				request += boost::lexical_cast<std::string>(f.offset);
				request += "-";
				request += boost::lexical_cast<std::string>(f.offset + f.size - 1);
				if (m_first_request || using_proxy)
					request += "\r\nConnection: keep-alive";
				request += "\r\n\r\n";
				m_first_request = false;
				m_file_requests.push_back(f.file_index);
			}
		}

#ifdef TORRENT_VERBOSE_LOGGING
		(*m_logger) << request << "\n";
#endif

		send_buffer(request.c_str(), request.c_str() + request.size());
	}

	
	
	

	namespace
	{
		bool range_contains(peer_request const& range, peer_request const& req)
		{
			return range.start <= req.start
				&& range.start + range.length >= req.start + req.length;
		}
	}

	
	void web_peer_connection::on_receive(asio::error_code const& error
		, std::size_t bytes_transferred)
	{
		INVARIANT_CHECK;

		if (error) return;

		boost::shared_ptr<torrent> t = associated_torrent().lock();
		assert(t);

		incoming_piece_fragment();

		for (;;)
		{
			buffer::const_interval recv_buffer = receive_buffer();

			int payload;
			int protocol;
			bool header_finished = m_parser.header_finished();
			boost::tie(payload, protocol) = m_parser.incoming(recv_buffer);
			m_statistics.received_bytes(payload, protocol);
			
			assert(recv_buffer.left() <= packet_size());
			assert (recv_buffer.left() < packet_size()
				|| m_parser.finished());
			
			
			if (m_parser.status_code() == -1) break;

			
			if (m_parser.status_code() != 206 
				&& m_parser.status_code() != 200 
				&& !(m_parser.status_code() >= 300 
					&& m_parser.status_code() < 400))
			{
				
				t->remove_url_seed(m_url);
				std::string error_msg = boost::lexical_cast<std::string>(m_parser.status_code())
					+ " " + m_parser.message();
				if (m_ses.m_alerts.should_post(alert::warning))
				{
					session_impl::mutex_t::scoped_lock l(m_ses.m_mutex);
					m_ses.m_alerts.post_alert(url_seed_alert(t->get_handle(), url()
						, error_msg));
				}
				throw std::runtime_error(error_msg);
			}

			if (!m_parser.header_finished()) break;

			
			if (!header_finished)
			{
				if (m_parser.status_code() >= 300 && m_parser.status_code() < 400)
				{
					
					
					std::string location = m_parser.header<std::string>("location");

					if (location.empty())
					{
						
						t->remove_url_seed(m_url);
						throw std::runtime_error("got HTTP redirection status without location header");
					}
					
					bool single_file_request = false;
					if (!m_path.empty() && m_path[m_path.size() - 1] != '/')
						single_file_request = true;

					
					if (!single_file_request)
					{
						assert(!m_file_requests.empty());
						int file_index = m_file_requests.front();

						torrent_info const& info = t->torrent_file();
						std::string path = info.file_at(file_index).path.string();
						path = escape_path(path.c_str(), path.length());
						size_t i = location.rfind(path);
						if (i == std::string::npos)
						{
							t->remove_url_seed(m_url);
							throw std::runtime_error("got invalid HTTP redirection location (\"" + location + "\") "
								"expected it to end with: " + path);
						}
						location.resize(i);
					}
					t->add_url_seed(location);
					t->remove_url_seed(m_url);
					throw std::runtime_error("redirecting to " + location);
				}

				std::string server_version = m_parser.header<std::string>("server");
				if (!server_version.empty())
				{
					m_server_string = "URL seed @ ";
					m_server_string += m_host;
					m_server_string += " (";
					m_server_string += server_version;
					m_server_string += ")";
				}

			}

			buffer::const_interval http_body = m_parser.get_body();

			size_type range_start;
			size_type range_end;
			if (m_parser.status_code() == 206)
			{
				std::stringstream range_str(m_parser.header<std::string>("content-range"));
				char dummy;
				std::string bytes;
				range_str >> bytes >> range_start >> dummy >> range_end;
				if (!range_str)
				{
					
					t->remove_url_seed(m_url);
					throw std::runtime_error("invalid range in HTTP response: " + range_str.str());
				}
				
				range_end++;
			}
			else
			{
				range_start = 0;
				range_end = m_parser.header<size_type>("content-length");
				if (range_end == -1)
				{
					
					t->remove_url_seed(m_url);
					throw std::runtime_error("no content-length in HTTP response");
				}
			}

			torrent_info const& info = t->torrent_file();

			if (m_requests.empty() || m_file_requests.empty())
				throw std::runtime_error("unexpected HTTP response");

			int file_index = m_file_requests.front();
			peer_request in_range = info.map_file(file_index, range_start
				, range_end - range_start);

			peer_request front_request = m_requests.front();
			if (in_range.piece != front_request.piece
				|| in_range.start > front_request.start + int(m_piece.size()))
			{
				throw std::runtime_error("invalid range in HTTP response");
			}

			front_request = m_requests.front();

			
			
			
			assert(in_range.start - int(m_piece.size()) <= front_request.start);
			http_body.begin += front_request.start - in_range.start + int(m_piece.size());

			
			
			
			
			

			bool range_overlaps_request = in_range.start + in_range.length
				> front_request.start + int(m_piece.size());

			
			
			
			
			if (range_overlaps_request && !range_contains(in_range, front_request))
			{
				
				
				
				
				
				
				m_piece.reserve(info.piece_length());
				int copy_size = std::min(front_request.length - int(m_piece.size())
					, http_body.left());
				std::copy(http_body.begin, http_body.begin + copy_size, std::back_inserter(m_piece));
				assert(int(m_piece.size()) <= front_request.length);
				http_body.begin += copy_size;
				int piece_size = int(m_piece.size());
				if (piece_size < front_request.length)
					return;

				
				
				
				

				m_requests.pop_front();
				incoming_piece(front_request, &m_piece[0]);
				if (associated_torrent().expired()) return;
				m_piece.clear();
			}

			
			while (!m_requests.empty()
				&& range_contains(in_range, m_requests.front())
				&& http_body.left() >= m_requests.front().length)
			{
				peer_request r = m_requests.front();
				m_requests.pop_front();
				assert(http_body.begin == recv_buffer.begin + m_parser.body_start()
					+ r.start - in_range.start);
				assert(http_body.left() >= r.length);

				incoming_piece(r, http_body.begin);
				if (associated_torrent().expired()) return;
				http_body.begin += r.length;
			}

			if (!m_requests.empty())
			{
				range_overlaps_request = in_range.start + in_range.length
					> m_requests.front().start + int(m_piece.size());

				if (in_range.start + in_range.length < m_requests.front().start + m_requests.front().length
					&& m_parser.finished())
				{
					m_piece.reserve(info.piece_length());
					int copy_size = std::min(m_requests.front().length - int(m_piece.size())
						, http_body.left());
					std::copy(http_body.begin, http_body.begin + copy_size, std::back_inserter(m_piece));
					http_body.begin += copy_size;
				}
			}

			if (m_parser.finished())
			{
				m_file_requests.pop_front();
				assert(http_body.left() == 0);
				m_parser.reset();
				assert(recv_buffer.end == http_body.end || *http_body.end == 'H');
				cut_receive_buffer(http_body.end - recv_buffer.begin
					, t->torrent_file().piece_length() + 1024 * 2);
				continue;
			}
			break;
		}
	}

	void web_peer_connection::get_peer_info(peer_info& p) const
	{
		assert(!associated_torrent().expired());

		p.down_speed = statistics().download_rate();
		p.up_speed = statistics().upload_rate();
		p.payload_down_speed = statistics().download_payload_rate();
		p.payload_up_speed = statistics().upload_payload_rate();
		p.pid = pid();
		p.ip = remote();
		
		p.country[0] = m_country[0];
		p.country[1] = m_country[1];

		p.total_download = statistics().total_payload_download();
		p.total_upload = statistics().total_payload_upload();

		if (m_bandwidth_limit[upload_channel].throttle() == bandwidth_limit::inf)
			p.upload_limit = -1;
		else
			p.upload_limit = m_bandwidth_limit[upload_channel].throttle();

		if (m_bandwidth_limit[download_channel].throttle() == bandwidth_limit::inf)
			p.download_limit = -1;
		else
			p.download_limit = m_bandwidth_limit[download_channel].throttle();

		p.load_balancing = total_free_upload();

		p.download_queue_length = (int)download_queue().size();
		p.upload_queue_length = (int)upload_queue().size();

		if (boost::optional<piece_block_progress> ret = downloading_piece_progress())
		{
			p.downloading_piece_index = ret->piece_index;
			p.downloading_block_index = ret->block_index;
			p.downloading_progress = ret->bytes_downloaded;
			p.downloading_total = ret->full_block_bytes;
		}
		else
		{
			p.downloading_piece_index = -1;
			p.downloading_block_index = -1;
			p.downloading_progress = 0;
			p.downloading_total = 0;
		}

		p.flags = 0;
		if (is_interesting()) p.flags |= peer_info::interesting;
		if (is_choked()) p.flags |= peer_info::choked;
		if (is_peer_interested()) p.flags |= peer_info::remote_interested;
		if (has_peer_choked()) p.flags |= peer_info::remote_choked;
		if (is_local()) p.flags |= peer_info::local_connection;
		if (!is_connecting() && m_server_string.empty())
			p.flags |= peer_info::handshake;
		if (is_connecting() && !is_queued()) p.flags |= peer_info::connecting;
		if (is_queued()) p.flags |= peer_info::queued;
		
		p.pieces = get_bitfield();
		p.seed = is_seed();

		p.client = m_server_string;
		p.connection_type = peer_info::web_seed;
	}

	bool web_peer_connection::in_handshake() const
	{
		return m_server_string.empty();
	}

	
	void web_peer_connection::on_sent(asio::error_code const& error
		, std::size_t bytes_transferred)
	{
		INVARIANT_CHECK;

		if (error) return;
		m_statistics.sent_bytes(0, bytes_transferred);
	} 

#ifndef NDEBUG
	void web_peer_connection::check_invariant() const
	{
	}
#endif

}
