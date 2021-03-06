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

//
// Manager of completed group on a HA master node
//

#ifndef _MASTER_GROUP_COMPLETE_MANAGER_HPP_
#define _MASTER_GROUP_COMPLETE_MANAGER_HPP_

#include "cubstream.hpp"
#include "stream_transfer_sender.hpp"
#include "transaction_group_complete_manager.hpp"

namespace cubtx
{
  //
  // master_group_complete_manager is a manager for group commits on master node
  //    Implements complete_manager interface used by transaction threads.
  //    Implements stream_ack interface used by stream senders.
  //
  class master_group_complete_manager : public group_complete_manager, public cubstream::stream_ack
  {
    public:
      master_group_complete_manager ();
      ~master_group_complete_manager () override;

      /* group_complete_manager methods */
      void do_prepare_complete (THREAD_ENTRY *thread_p) override;
      void do_complete (THREAD_ENTRY *thread_p) override;

      /* stream_ack methods */
      void notify_stream_ack (const cubstream::stream_position stream_pos) override;

      int get_manager_type () const override;

    protected:
      bool can_close_current_group () override;
      void on_register_transaction () override;

    private:
      std::atomic<cubstream::stream_position> m_latest_closed_group_start_stream_position;
      std::atomic<cubstream::stream_position> m_latest_closed_group_end_stream_position;
  };

  void initialize_master_gcm ();
  void finalize_master_gcm ();
  master_group_complete_manager *get_master_gcm_instance ();
}
#endif // !_MASTER_GROUP_COMPLETE_MANAGER_HPP_
