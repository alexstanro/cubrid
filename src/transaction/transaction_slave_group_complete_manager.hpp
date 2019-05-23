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
// Manager of completed group on a HA slave node
//
#include "transaction_group_complete_manager.hpp"

#include "cubstream.hpp"
#include "log_consumer.hpp"
#include "thread_daemon.hpp"
#include "thread_entry_task.hpp"

namespace cubtx
{
  //
  // slave_group_complete_manager is a manager for group commits on slave node
  //    Implements group complete_manager interface used by transaction threads.
  //    Implements dispatch_consumer interface used by dispatch thread.
  //
  class slave_group_complete_manager : public group_complete_manager, public cubreplication::dispatch_consumer
  {
    public:
      ~slave_group_complete_manager () override;

      static slave_group_complete_manager *get_instance ();
      static void init ();
      static void final ();

      /* group complete methods */
      virtual void do_prepare_complete (THREAD_ENTRY *thread_p) override;
      virtual void do_complete (THREAD_ENTRY *thread_p) override;

      /* dispatch complete methods */
      void wait_for_complete_stream_position (cubstream::stream_position stream_position) override;
      void set_close_info_for_current_group (cubstream::stream_position stream_position,
					     int count_expected_transactions) override;

    protected:
      /* group_complete_manager methods */
      bool can_close_current_group () override;
      void on_register_transaction () override;

    private:
      static slave_group_complete_manager *gl_slave_group;
      static cubthread::daemon *gl_slave_group_complete_daemon;

      /* Latest recorded stream position and corresponding id. */
      id_type m_latest_group_id;
      std::atomic<cubstream::stream_position> m_latest_group_stream_positon;

      /* has_latest_group_close_info - true, if stream position and count expected transactions were set. */
      std::atomic<bool> has_latest_group_close_info;

      friend class slave_group_complete_task;
  };

  //
  // slave_group_complete_task is class for slave group complete daemon
  //
  class slave_group_complete_task : public cubthread::entry_task
  {
    public:
      /* entry_task methods */
      void execute (cubthread::entry &thread_ref) override;
  };
}