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

#include "thread_daemon.hpp"
#include "transaction_master_group_complete_manager.hpp"
#include "log_manager.h"

namespace cubtx
{
  tx_master_group_complete_manager::~tx_master_group_complete_manager ()
  {
    cubthread::get_manager ()->destroy_daemon (m_gc_daemon);
  }

  //
  // get global master instance
  //
  tx_master_group_complete_manager *tx_master_group_complete_manager::get_instance ()
  {
    if (gl_master_group == NULL)
      {
	gl_master_group = new tx_master_group_complete_manager ();
      }
    return gl_master_group;
  }

  //
  // init initialize master group commit
  //
  void tx_master_group_complete_manager::init ()
  {
#if defined (SERVER_MODE)
    cubthread::looper looper = cubthread::looper (std::chrono::milliseconds (10));
    gl_master_group = get_instance ();
    gl_master_group->m_gc_daemon = cubthread::get_manager ()->create_daemon (looper, gl_master_group);
    gl_master_group->m_latest_closed_group_stream_positon = 0;
#endif
  }

  //
  // final finalizes master group commit
  //
  void tx_master_group_complete_manager::final ()
  {
#if defined (SERVER_MODE)
    delete gl_master_group->m_gc_daemon;
    gl_master_group->m_gc_daemon = NULL;

    delete gl_master_group;
    gl_master_group = NULL;
#endif
  }

  //
  // notify_stream_ack notifies stream ack.
  //
  void tx_master_group_complete_manager::notify_stream_ack (const cubstream::stream_position stream_pos)
  {
    /* TO DO - disable it temporary since it is not tested */
    return;

    /* TODO - consider quorum. Consider multiple calls of same thread. */
    /* TODO - use m_latest_closed_group_stream_start_positon, m_latest_closed_group_stream_end_positon */
    if (stream_pos > m_latest_closed_group_stream_positon)
      {
	cubthread::entry *thread_p = &cubthread::get_entry ();
	do_complete (thread_p);
      }
  }

  //
  // execute is thread main method.
  //
  void tx_master_group_complete_manager::execute (cubthread::entry &thread_ref)
  {
    /* TO DO - disable it temporary since it is not tested */
    return;

    cubthread::entry *thread_p = &cubthread::get_entry ();
    prepare_complete (thread_p);
  }

  //
  // can_close_current_group check whether the current group can be closed.
  //
  bool tx_master_group_complete_manager::can_close_current_group ()
  {
    if (!is_latest_closed_group_completed ())
      {
	/* Can't advance to the next group since the current group was not committed yet. */
	return false;
      }

    if (is_current_group_empty ())
      {
	// no transaction, can't close the group.
	return false;
      }

    return true;
  }

  //
  // prepare_complete prepares group complete. Always should be called before do_complete.
  //
  void tx_master_group_complete_manager::prepare_complete (THREAD_ENTRY *thread_p)
  {
    if (close_current_group ())
      {
	cubstream::stream_position gc_position;
        tx_group & closed_group = get_last_closed_group ();

	/* TODO - Introduce parameter. For now complete group MVCC only here. Notify MVCC complete. */
	log_Gl.mvcc_table.complete_group_mvcc (closed_group);
	notify_group_mvcc_complete (closed_group);

	/* Pack group commit that internally wakeups senders. Get stream position of group complete. */
	logtb_get_tdes (thread_p)->replication_log_generator.pack_group_commit_entry (gc_position);
	m_latest_closed_group_stream_positon = gc_position;
      }
  }

  //
  // do_complete complete does group complete. Always should be called after prepare_complete.
  //
  void tx_master_group_complete_manager::do_complete (THREAD_ENTRY *thread_p)
  {
    tx_group closed_group;
    LOG_LSA closed_group_commit_lsa;
    LOG_TDES *tdes = logtb_get_tdes (&cubthread::get_entry ());
    bool has_postpone;

    if (is_latest_closed_group_completed ())
      {
	/* Latest closed group is already completed. */
	return;
      }

    /* TODO - consider parameter for MVCC complete here. */
    /* Add group commit log record and wakeup  log flush daemon. */
    log_append_group_commit (thread_p, tdes, m_latest_closed_group_stream_positon, closed_group,
			     &closed_group_commit_lsa, &has_postpone);
    log_wakeup_log_flush_daemon ();
    if (has_postpone)
      {
	/* Don't care about optimization here since postpone is a rare case. Wait to be sure that postpone is on disk. */
	logpb_flush_pages (thread_p, &closed_group_commit_lsa);
	/* Notify group postpone. */
	notify_group_logged ();
      }

    /* Finally, notify complete. */
    notify_group_complete ();

    /* wakeup GC thread */
#if defined (SERVER_MODE)
    get_instance ()->m_gc_daemon->wakeup ();
#endif
  }

  tx_master_group_complete_manager *tx_master_group_complete_manager::gl_master_group = NULL;
}