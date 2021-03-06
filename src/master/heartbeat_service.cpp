/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * heartbeat_service.cpp - heartbeat communication service
 */

#include "heartbeat_service.hpp"

#include "udp_rpc.hpp"

namespace cubhb
{

  heartbeat_service::heartbeat_service (ha_server &server, cluster &cluster_)
    : m_server (server)
    , m_cluster (cluster_)
  {
    ha_server::server_request_handler handler = std::bind (&heartbeat_service::handle_heartbeat, std::ref (*this),
	std::placeholders::_1);
    m_server.register_handler (HEARTBEAT, handler);
  }

  void
  heartbeat_service::handle_heartbeat (ha_server::server_request &request)
  {
    heartbeat_arg request_arg;
    request.get_message (request_arg);

    // process heartbeat request
    m_cluster.receive_heartbeat (request_arg, request.get_remote_ip_address ());

    // send heartbeat back
    reply_heartbeat (request, request_arg.get_orig_hostname ());
  }

  void
  heartbeat_service::send_heartbeat (const cubbase::hostname_type &node_hostname)
  {
    heartbeat_arg request_arg (node_hostname, m_cluster);
    ha_server::client_request request = m_server.create_client_request (node_hostname);

    request.set_message (message_type::HEARTBEAT, request_arg);
    request.end ();
  }

  void
  heartbeat_service::reply_heartbeat (ha_server::server_request &request, const cubbase::hostname_type &node_hostname)
  {
    // must send heartbeat response in order to avoid split-brain when heartbeat configuration changed
    if (request.is_response_requested () && !m_cluster.hide_to_demote)
      {
	heartbeat_arg response_arg (node_hostname, m_cluster);
	request.get_response ().set_message (response_arg);
      }
  }

  heartbeat_arg::heartbeat_arg ()
    : m_state (node_state::UNKNOWN)
    , m_group_id ()
    , m_orig_hostname ()
    , m_dest_hostname ()
  {
    //
  }

  heartbeat_arg::heartbeat_arg (const cubbase::hostname_type &dest_hostname, const cluster &cluster_)
    : m_state (cluster_.get_state ())
    , m_group_id (cluster_.get_group_id ())
    , m_orig_hostname ()
    , m_dest_hostname (dest_hostname)
  {
    if (cluster_.get_myself_node () != NULL)
      {
	m_orig_hostname = cluster_.get_myself_node ()->get_hostname ();
      }
  }

  int
  heartbeat_arg::get_state () const
  {
    return m_state;
  }

  const std::string &
  heartbeat_arg::get_group_id () const
  {
    return m_group_id;
  }

  const cubbase::hostname_type &
  heartbeat_arg::get_orig_hostname () const
  {
    return m_orig_hostname;
  }

  const cubbase::hostname_type &
  heartbeat_arg::get_dest_hostname () const
  {
    return m_dest_hostname;
  }

  size_t
  heartbeat_arg::get_packed_size (cubpacking::packer &serializator, std::size_t start_offset) const
  {
    size_t size = serializator.get_packed_int_size (start_offset); // m_state
    size += serializator.get_packed_string_size (m_group_id, size);
    size += m_orig_hostname.get_packed_size (serializator, size);
    size += m_dest_hostname.get_packed_size (serializator, size);

    return size;
  }

  void
  heartbeat_arg::pack (cubpacking::packer &serializator) const
  {
    serializator.pack_int (m_state);
    serializator.pack_string (m_group_id);
    m_orig_hostname.pack (serializator);
    m_dest_hostname.pack (serializator);
  }

  void
  heartbeat_arg::unpack (cubpacking::unpacker &deserializator)
  {
    deserializator.unpack_int (m_state);
    assert (m_state >= node_state::UNKNOWN && m_state < node_state::MAX);

    deserializator.unpack_string (m_group_id);
    m_orig_hostname.unpack (deserializator);
    m_dest_hostname.unpack (deserializator);
  }

} /* namespace cubhb */
