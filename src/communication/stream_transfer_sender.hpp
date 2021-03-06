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
 * stream_transfer_[sender/receiver].hpp - transfer stream through the network
 *                                         see stream_transfer_sender.cpp commentary for more details
 */

#ifndef _STREAM_TRANSFER_SENDER_HPP_
#define _STREAM_TRANSFER_SENDER_HPP_

#include "communication_channel.hpp"
#include "cubstream.hpp"

#include <atomic>     // for atomic_bool

namespace cubthread
{
  class daemon;
};

namespace cubstream
{

  /* Interface used by stream senders/receivers to notify threads. */
  class stream_ack
  {
    public:
      virtual void notify_stream_ack (const cubstream::stream_position stream_pos) = 0;
  };

  class transfer_sender
  {

    public:

      transfer_sender (cubcomm::channel &&chn,
		       cubstream::stream &stream,
		       stream_position begin_sending_position = 0);
      virtual ~transfer_sender ();
      int read_action (char *ptr, const size_t byte_count);

      stream_position get_last_sent_position ();
      cubcomm::channel &get_channel ();

      inline cubthread::daemon *get_daemon ()
      {
	return m_sender_daemon;
      }

      void enter_termination_phase ()
      {
	m_is_termination_phase.store (true);
      }

      bool is_termination_phase ()
      {
	return m_is_termination_phase.load ();
      }

      void register_stream_ack (stream_ack *stream_ack)
      {
	m_p_stream_ack = stream_ack;
      }

    private:

      friend class transfer_sender_task;

      cubcomm::channel m_channel;
      cubstream::stream &m_stream;
      stream_position m_last_sent_position;
      cubthread::daemon *m_sender_daemon;
      char m_buffer[cubcomm::MTU];

      std::atomic_bool m_is_termination_phase;

      /* TO DO - move p_stream_ack in new receiver threads on master node. */
      stream_ack *m_p_stream_ack;

    protected:
      cubstream::stream::read_func_t m_read_action_function;
  };

} // namespace cubstream
#endif /* _STREAM_TRANSFER_SENDER_HPP_ */
