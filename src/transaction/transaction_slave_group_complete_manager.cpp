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

#include "boot_sr.h"
#include "log_manager.h"
#include "thread_entry_task.hpp"
#include "thread_manager.hpp"
#include "transaction_slave_group_complete_manager.hpp"

namespace cubtx
{
  slave_group_complete_manager *gl_slave_gcm = NULL;
  cubthread::daemon *gl_slave_gcm_daemon = NULL;

  //
  // slave_group_complete_task is class for slave group complete daemon
  //
  class slave_group_complete_task : public cubthread::entry_task
  {
    public:
      //
      // execute is thread main method.
      //
      void execute (cubthread::entry &thread_ref) override
      {
	if (!BO_IS_SERVER_RESTARTED ())
	  {
	    return;
	  }

	cubthread::entry *thread_p = &cubthread::get_entry ();
	slave_group_complete_manager *p_gl_slave_group = get_slave_gcm_instance ();
	p_gl_slave_group->do_prepare_complete (thread_p);
	p_gl_slave_group->do_complete (thread_p);
      }
  };

  slave_group_complete_manager::slave_group_complete_manager ()
  {
    m_current_group_expected_transactions = 0;
    m_latest_group_id = NULL_ID;
    m_latest_group_stream_position = 0;
    m_has_latest_group_close_info.store (false);
  }

  slave_group_complete_manager::~slave_group_complete_manager ()
  {
  }

  //
  // on_register_transaction - on register transaction specific to slave node.
  //  Note: This function must be called under group mutex protection.
  //
  void slave_group_complete_manager::on_register_transaction ()
  {
    /* This function is called after adding a transaction to the current group.
     * Currently, we wakeup GC thread when all expected transactions were added into current group.
     */
    assert (m_has_latest_group_close_info.load () == false
	    || get_current_group ().get_container ().size () <= (size_t) m_current_group_expected_transactions);
    if (m_has_latest_group_close_info.load () == true
	&& get_current_group ().get_container ().size () == (size_t) m_current_group_expected_transactions)
      {
	gl_slave_gcm_daemon->wakeup ();
      }
  }

  //
  // can_close_current_group check whether the current group can be closed.
  //  Note: This function must be called under group mutex protection.
  //
  bool slave_group_complete_manager::can_close_current_group ()
  {
    if (!is_latest_closed_group_completed ())
      {
	/* Can't advance to the next group since the current group was not completed yet. */
	return false;
      }

    /* Check whether close info for latest group were set. */
    if (m_has_latest_group_close_info.load () == false)
      {
	/* Can't close yet the current group. */
	if (!is_current_group_empty () && is_group_completed (m_latest_group_id))
	  {
	    /* Something wrong happens. The latest group was closed, but, we have a transaction
	     * waiting for another group. Forces a group complete to not stuck the system.
	     */
	    er_log_group_complete_debug (ARG_FILE_LINE,
					 "can_close_current_group: wrong transaction waiting beyond the latest "
					 "group id (%llu)", m_latest_group_id);
	    return true;
	  }

	return false;
      }

    /* Check whether all expected transactions already registered. */
    if (get_current_group ().get_container ().size () < (size_t) m_current_group_expected_transactions)
      {
	return false;
      }

    assert (get_current_group ().get_container ().size () == (size_t) m_current_group_expected_transactions);
    return true;
  }

  //
  // prepare_complete prepares group complete. Always should be called before do_complete.
  //
  void slave_group_complete_manager::do_prepare_complete (THREAD_ENTRY *thread_p)
  {
    /* TODO - consider whether stream position was saved on disk, when close the group */
    if (close_current_group ())
      {
	/* The new group does not have group close info yet - stream position, count transactions. */
	m_has_latest_group_close_info.store (false);
	const tx_group &closed_group = get_latest_closed_group ();

	/* TODO - Introduce parameter. For now complete group MVCC only here. Notify MVCC complete. */
	log_Gl.mvcc_table.complete_group_mvcc (thread_p, closed_group);
	notify_group_mvcc_complete (closed_group);

	mark_latest_closed_group_prepared_for_complete ();
      }
  }

  //
  // do_complete complete does group complete. Always should be called after prepare_complete.
  //
  void slave_group_complete_manager::do_complete (THREAD_ENTRY *thread_p)
  {
    LOG_LSA closed_group_start_complete_lsa;
    LOG_TDES *tdes = logtb_get_tdes (thread_p);
    bool has_postpone = false;

    if (is_latest_closed_group_completed ())
      {
	/* Latest closed group is already completed. */
	return;
      }

    while (!is_latest_closed_group_prepared_for_complete ())
      {
	/* It happens rare. */
	thread_sleep (10);
      }

    if (!starts_latest_closed_group_complete ())
      {
	return;
      }

    tx_group &closed_group = get_latest_closed_group ();
    /* TODO - consider parameter for MVCC complete here. */
    /* Add group commit log record and wakeup  log flush daemon. */

    log_append_group_complete (thread_p, tdes, m_latest_group_stream_position,
			       closed_group, &closed_group_start_complete_lsa, NULL);

    log_wakeup_log_flush_daemon ();
    if (has_postpone)
      {
	/* Notify group postpone. For consistency, we need preserve the order: log GC with postpone first and then
	 * RUN_POSTPONE. The transaction having postpone must wait for GC with postpone log record to be appended.
	 * It seems that we don't need to wait for log flush here.
	 */
	notify_group_logged ();
      }

    /* Finally, notify complete. TODO - consider notify log and complete together. Consider complete MVCC case 2. */
    notify_group_complete ();
  }

  //
  // complete_upto_stream_position - complete up to stream position
  //
  void slave_group_complete_manager::complete_upto_stream_position (cubstream::stream_position stream_position)
  {
    assert (stream_position <= m_latest_group_stream_position);
    complete (m_latest_group_id);
  }

  //
  // set_close_info_for_current_group - set close info for current group.
  //
  void slave_group_complete_manager::set_close_info_for_current_group (cubstream::stream_position stream_position,
      int count_expected_transactions)
  {
    bool has_group_enough_transactions = false;
    /* TODO - Can we set close info twice? */

    m_latest_group_stream_position = stream_position;
    m_current_group_expected_transactions = count_expected_transactions;
    has_group_enough_transactions = has_transactions_in_current_group (count_expected_transactions, m_latest_group_id);
    m_has_latest_group_close_info.store (true);
    er_log_group_complete_debug (ARG_FILE_LINE, "set_close_info_for_current_group sp=%llu, latest_group_id = %llu,"
				 "count_expected_transaction = %d\n", stream_position, m_latest_group_id,
				 count_expected_transactions);
    if (has_group_enough_transactions)
      {
	/* Wakeup group complete thread, since we have all informations that allows group close. */
	gl_slave_gcm_daemon->wakeup ();
      }
  }

  //
  // get_manager_type - get manager type.
  //
  int slave_group_complete_manager::get_manager_type () const
  {
    return LOG_TRAN_COMPLETE_MANAGER_SLAVE_NODE;
  }

  void initialize_slave_gcm ()
  {
    /* Creates slave group complete manager object. */
    assert (gl_slave_gcm == NULL);
    cubthread::looper looper = cubthread::looper (std::chrono::milliseconds (10));
    gl_slave_gcm = new slave_group_complete_manager ();
    er_log_group_complete_debug (ARG_FILE_LINE, "slave_group_complete_manager:init created slave group complete manager\n");

    /* Creates a daemon thread that executes complete on slave gcm. */
    gl_slave_gcm_daemon = cubthread::get_manager ()->create_daemon ((looper),
			  new slave_group_complete_task (), "slave_group_complete_daemon");
  }

  void finalize_slave_gcm ()
  {
    if (gl_slave_gcm_daemon != NULL)
      {
	cubthread::get_manager ()->destroy_daemon (gl_slave_gcm_daemon);
	gl_slave_gcm_daemon = NULL;
      }

    delete gl_slave_gcm;
    gl_slave_gcm = NULL;
  }

  slave_group_complete_manager *get_slave_gcm_instance ()
  {
    assert (gl_slave_gcm != NULL);
    return gl_slave_gcm;
  }
}

