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
// Manager of completed group
//

#ifndef _GROUP_COMPLETE_MANAGER_HPP_
#define _GROUP_COMPLETE_MANAGER_HPP_

#include "transaction_complete_manager.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>

#define er_log_group_complete_debug(...) if (prm_get_bool_value (PRM_ID_GROUP_COMPLETE_DEBUG)) _er_log_debug(__VA_ARGS__)

namespace cubtx
{
  /* Group state. */
  enum GROUP_STATE
  {
    GROUP_CLOSED = 0x01, /* Group closed. No other transaction can be included in a closed group. */
    GROUP_MVCC_COMPLETED = 0x02, /* MVCC completed. Set only for closed group. */
    GROUP_LOGGED = 0x04, /* Group log added. Set only for closed group. */
    GROUP_PREPARED_FOR_COMPLETE = 0x08,  /* Group prepared for complete. Set only for closed group. */
    GROUP_COMPLETE_STARTED = 0x10,  /* Group complete started. Set only for prepared group. */
    GROUP_COMPLETED = 0x20,  /* Group completed. Set only for complete started group. */

    GROUP_ALL_STATES = (GROUP_CLOSED | GROUP_MVCC_COMPLETED | GROUP_LOGGED | GROUP_PREPARED_FOR_COMPLETE
			| GROUP_COMPLETE_STARTED | GROUP_COMPLETED)
  };

  //
  // group_complete_manager is the common interface used by complete managers based on grouping the commits
  //
  class group_complete_manager : public complete_manager
  {
    public:
      group_complete_manager ()
	: m_current_group_id (1)
	, m_latest_closed_group_id (0)
	, m_latest_closed_group_state (GROUP_ALL_STATES)
      {

      }
      ~group_complete_manager () override;

      id_type register_transaction (int tran_index, MVCCID mvccid, TRAN_STATE state) override final;

      void complete_mvcc (id_type group_id) override final;

      void complete (id_type group_id) override final;

      void complete_logging (id_type group_id) override final;

    protected:
      bool has_transactions_in_current_group (const unsigned int count_transactions, id_type &current_group_id);

      bool close_current_group ();

      virtual void on_register_transaction () = 0;

      virtual bool can_close_current_group () = 0;

      virtual void do_prepare_complete (THREAD_ENTRY *thread_p) = 0;

      virtual void do_complete (THREAD_ENTRY *thread_p) = 0;

      void notify_group_mvcc_complete (const tx_group &closed_group);
      void notify_group_logged ();
      void notify_group_complete ();

      void mark_latest_closed_group_prepared_for_complete ();
      bool is_latest_closed_group_prepared_for_complete () const;

      /* TODO - consider a better name than latest_closed */
      bool starts_latest_closed_group_complete ();
      bool is_latest_closed_group_complete_started () const;

      bool is_latest_closed_group_mvcc_completed () const;
      bool is_latest_closed_group_logged () const;
      bool is_latest_closed_group_completed () const;

      bool is_current_group_empty () const;

      tx_group &get_latest_closed_group ();
      const tx_group &get_current_group () const;

      bool is_group_completed (id_type group_id) const;

    private:
      bool is_group_mvcc_completed (id_type group_id);
      bool is_group_logged (id_type group_id) const;

      void notify_all ();
      void execute_all ();

#if defined(SERVER_MODE)
      bool need_wait_for_complete ();
#endif

      /* Current group info - TODO Maybe better to use a structure here. */
      std::atomic<id_type> m_current_group_id;   // is also the group identifier
      tx_group m_current_group;
      std::mutex m_group_mutex;

      /* Latest closed group info - TODO Maybe better to use a structure here. */
      tx_group m_latest_closed_group;
      std::atomic<id_type> m_latest_closed_group_id;
      std::atomic<int> m_latest_closed_group_state;

      /* Wakeup info. */
      std::mutex m_group_complete_mutex;
      std::condition_variable m_group_complete_condvar;
  };
}
#endif // !_GROUP_COMPLETE_MANAGER_HPP_
