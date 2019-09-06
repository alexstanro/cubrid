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
 * log_consumer.cpp
 */

#ident "$Id$"

#include "log_consumer.hpp"

#include "error_manager.h"
#include "locator_sr.h"
#include "multi_thread_stream.hpp"
#include "replication_common.hpp"
#include "replication_stream_entry.hpp"
#include "replication_subtran_apply.hpp"
#include "stream_entry_fetcher.hpp"
#include "stream_file.hpp"
#include "string_buffer.hpp"
#include "system_parameter.h"
#include "thread_daemon.hpp"
#include "thread_entry_task.hpp"
#include "thread_task.hpp"
#include "transaction_slave_group_complete_manager.hpp"
#include <unordered_map>

namespace cubreplication
{
  class applier_worker_task : public cubthread::entry_task
  {
    public:
      applier_worker_task (stream_entry *repl_stream_entry, log_consumer &lc)
	: m_lc (lc)
      {
	add_repl_stream_entry (repl_stream_entry);
      }

      ~applier_worker_task ()
      {
	for (stream_entry *&se : m_repl_stream_entries)
	  {
	    delete se;
	    se = NULL;
	  }
      }

      void execute (cubthread::entry &thread_ref) final
      {
	(void) locator_repl_start_tran (&thread_ref);

	for (stream_entry *&curr_stream_entry : m_repl_stream_entries)
	  {
	    curr_stream_entry->unpack ();

	    if (prm_get_bool_value (PRM_ID_DEBUG_REPLICATION_DATA))
	      {
		string_buffer sb;
		curr_stream_entry->stringify (sb, stream_entry::detailed_dump);
		_er_log_debug (ARG_FILE_LINE, "applier_worker_task execute:\n%s", sb.get_buffer ());
	      }

	    for (int i = 0; i < curr_stream_entry->get_packable_entry_count_from_header (); i++)
	      {
		replication_object *obj = curr_stream_entry->get_object_at (i);

		/* clean error code */
		er_clear ();

		int err = obj->apply ();
		if (err != NO_ERROR)
		  {
		    /* TODO[replication] : error handling */
		  }
	      }

	    delete curr_stream_entry;
	    curr_stream_entry = NULL;
	  }

	m_repl_stream_entries.clear ();

	(void) locator_repl_end_tran (&thread_ref, true);
	m_lc.end_one_task ();
      }

      void add_repl_stream_entry (stream_entry *repl_stream_entry)
      {
	m_repl_stream_entries.push_back (repl_stream_entry);
      }

      size_t get_entries_cnt (void)
      {
	return m_repl_stream_entries.size ();
      }

      void stringify (string_buffer &sb)
      {
	sb ("apply_task: stream_entries:%d\n", get_entries_cnt ());
	for (auto it = m_repl_stream_entries.begin (); it != m_repl_stream_entries.end (); it++)
	  {
	    (*it)->stringify (sb, stream_entry::detailed_dump);
	  }
      }

    private:
      std::vector<stream_entry *> m_repl_stream_entries;
      log_consumer &m_lc;
  };

  class dispatch_daemon_task : public cubthread::entry_task
  {
    public:
      dispatch_daemon_task (log_consumer &lc)
	: m_filtered_apply_end (log_Gl.hdr.m_ack_stream_position)
	, m_entry_fetcher (*lc.get_stream ())
	, m_lc (lc)
	, m_stop (false)
	, m_prev_group_stream_position (0)
	, m_curr_group_stream_position (0)
      {
	m_p_dispatch_consumer = cubtx::get_slave_gcm_instance ();
      }

      void execute (cubthread::entry &thread_ref) override
      {
	using tasks_map = std::unordered_map<MVCCID, applier_worker_task *>;
	tasks_map repl_tasks;
	tasks_map nonexecutable_repl_tasks;
	int count_expected_transaction = 0;

	m_lc.wait_for_fetch_resume ();

	assert (m_p_dispatch_consumer != NULL);
	while (!m_stop)
	  {
	    stream_entry *se = NULL;
	    int err = m_entry_fetcher.fetch_entry (se);
	    if (err != NO_ERROR)
	      {
		if (err == ER_STREAM_NO_MORE_DATA)
		  {
		    ASSERT_ERROR ();
		    m_stop = true;
		    delete se;
		    break;
		  }
		else
		  {
		    ASSERT_ERROR ();
		    // should not happen
		    assert (false);
		    m_stop = true;
		    delete se;
		    break;
		  }
	      }

	    if (filter_out_stream_entry (se))
	      {
		delete se;
		continue;
	      }

	    if (se->is_tran_state_end_of_stream ())
	      {
		m_stop = true;
		delete se;
		break;
	      }

	    if (se->is_group_commit ())
	      {
		se->unpack ();
		assert (se->get_stream_entry_end_position () > se->get_stream_entry_start_position ());
		m_lc.ack_produce (se->get_stream_entry_end_position ());
	      }

	    if (prm_get_bool_value (PRM_ID_DEBUG_REPLICATION_DATA))
	      {
		string_buffer sb;
		se->stringify (sb, stream_entry::short_dump);
		_er_log_debug (ARG_FILE_LINE, "dispatch_daemon_task execute pop_entry:\n%s", sb.get_buffer ());
	      }

	    /* TODO[replication] : on-the-fly applier & multi-threaded applier */
	    if (se->is_group_commit ())
	      {
		applier_worker_task *my_repl_applier_worker_task;
		const repl_gc_info *gc_entry;
		tx_group transactions_group;
		TRAN_STATE tran_state;

		assert (se->count_entries () == 1);
		assert (se->get_stream_entry_start_position () < se->get_stream_entry_end_position ());
		gc_entry = dynamic_cast <const repl_gc_info *> (se->get_object_at (0));
		transactions_group = gc_entry->as_tx_group ();

		er_log_debug_replication (ARG_FILE_LINE, "dispatch_daemon_task wait for all working tasks to finish\n");
		assert (se->get_stream_entry_start_position () < se->get_stream_entry_end_position ());

		if (count_expected_transaction > 0)
		  {
		    /* We have to wait for previous group. */
		    m_prev_group_stream_position = m_curr_group_stream_position;
		    m_curr_group_stream_position = se->get_stream_entry_end_position ();

		    // Noramlly, is enough to wait for group to complete. However, for safety reason, it is better to
		    // start wait for workers first, then wait for complete manager.
		    m_lc.wait_for_tasks ();

		    // We need to wait for previous group to complete. Otherwise, we mix transactions from previous and
		    // current groups.
		    m_p_dispatch_consumer->complete_upto_stream_position (m_prev_group_stream_position);
		  }

		// apply all sub-transaction first
		m_lc.get_subtran_applier ().apply ();

		// apply the transaction in current group
		count_expected_transaction = 0;
		for (tasks_map::iterator it = repl_tasks.begin (); it != repl_tasks.end (); it++)
		  {
		    tran_state = TRAN_ACTIVE;
		    /* Get transaction state. Would be better to include it in a method in tx_group. */
		    for (const tx_group::node_info &transaction_info : transactions_group.get_container ())
		      {
			if (transaction_info.m_mvccid == it->first)
			  {
			    tran_state = transaction_info.m_tran_state;
			    break;
			  }
		      }

		    /* check last stream entry of task */
		    my_repl_applier_worker_task = it->second;

		    /* We don't need to check all commit/abort states, but is not wrong. */
		    if (LOG_ISTRAN_STATE_COMMIT (tran_state))
		      {
			m_lc.execute_task (it->second);
			count_expected_transaction++;
		      }
		    else if (LOG_ISTRAN_STATE_ABORT (tran_state))
		      {
			/* TODO[replication] : when on-fly apply is active, we need to abort the transaction;
			 * for now, we are sure that no change has been made on slave on behalf of this task,
			 * just drop the task */
			delete it->second;
			it->second = NULL;
		      }
		    else
		      {
			/* tasks without commit or abort are postponed to next group commit */
			assert (it->second->get_entries_cnt () > 0);
			nonexecutable_repl_tasks.insert (std::make_pair (it->first, it->second));
		      }
		  }

		repl_tasks.clear ();

		if (nonexecutable_repl_tasks.size () > 0)
		  {
		    repl_tasks = nonexecutable_repl_tasks;
		    nonexecutable_repl_tasks.clear ();
		  }

		/* delete the group commit stream entry */
		assert (se->is_group_commit ());
		delete se;

		// The transactions started but can't complete yet since it waits for the current group complete.
		// But the current group can't complete, since GC thread is waiting for close info.
		// Now is safe to set close info.
		if (count_expected_transaction > 0)
		  {
		    /* We have committed transaction with replication entries. */
		    m_p_dispatch_consumer->set_close_info_for_current_group (m_curr_group_stream_position,
			count_expected_transaction);
		  }
		else
		  {
		    er_log_debug (ARG_FILE_LINE, "No replication entries in group having stream position (%llu, %llu)\n",
				  se->get_stream_entry_start_position (), se->get_stream_entry_end_position ());
		  }
	      }
	    else if (se->is_new_master ())
	      {
		for (auto &repl_task : repl_tasks)
		  {
		    delete repl_task.second;
		    repl_task.second = NULL;
		  }
		repl_tasks.clear ();
	      }
	    else if (se->is_subtran_commit ())
	      {
		m_lc.get_subtran_applier ().insert_stream_entry (se);
	      }
	    else if (se->get_packable_entry_count_from_header () > 0)
	      {
		MVCCID mvccid = se->get_mvccid ();
		auto it = repl_tasks.find (mvccid);

		if (it != repl_tasks.end ())
		  {
		    /* already a task with same MVCCID, add it to existing task */
		    applier_worker_task *my_repl_applier_worker_task = it->second;
		    my_repl_applier_worker_task->add_repl_stream_entry (se);

		    assert (my_repl_applier_worker_task->get_entries_cnt () > 0);
		  }
		else
		  {
		    applier_worker_task *my_repl_applier_worker_task = new applier_worker_task (se, m_lc);
		    repl_tasks.insert (std::make_pair (mvccid, my_repl_applier_worker_task));

		    assert (my_repl_applier_worker_task->get_entries_cnt () > 0);
		  }

		/* stream entry is deleted by applier task thread */
	      }
	    else
	      {
		/* Skip entry without data, to avoid commit issues. Readers do not wait for group complete. */
		assert (se->get_packable_entry_count_from_header () == 0);
		delete se;
	      }
	  }

	// delete unapplied tasks
	for (auto &repl_task : repl_tasks)
	  {
	    delete repl_task.second;
	    repl_task.second = NULL;
	  }
	// wait for the applying tasks
	m_lc.wait_for_tasks ();
	m_lc.set_dispatcher_finished ();
      }

    private:
      bool is_filtered_apply_segment (cubstream::stream_position stream_entry_end) const
      {
	return stream_entry_end <= m_filtered_apply_end;
      }

      bool filter_out_stream_entry (stream_entry *repl_stream_entry) const
      {
	if (is_filtered_apply_segment (repl_stream_entry->get_stream_entry_end_position ()))
	  {
	    MVCCID mvccid = repl_stream_entry->get_mvccid ();
	    return repl_stream_entry->is_group_commit ()
		   || log_Gl.m_repl_rv.m_active_mvcc_ids.find (mvccid) == log_Gl.m_repl_rv.m_active_mvcc_ids.end ();
	  }

	if (!log_Gl.m_repl_rv.m_active_mvcc_ids.empty ())
	  {
	    log_Gl.m_repl_rv.m_active_mvcc_ids.clear ();
	  }
	return false;
      }

      cubstream::stream_position m_filtered_apply_end;
      stream_entry_fetcher m_entry_fetcher;
      log_consumer &m_lc;
      bool m_stop;
      cubstream::stream_position m_prev_group_stream_position;
      cubstream::stream_position m_curr_group_stream_position;
      group_completion *m_p_dispatch_consumer;
  };

  log_consumer::~log_consumer ()
  {
    stop ();

    if (m_use_daemons)
      {
	cubthread::get_manager ()->destroy_daemon (m_dispatch_daemon);
	cubthread::get_manager ()->destroy_worker_pool (m_applier_workers_pool);
      }

    delete m_subtran_applier; // must be deleted after worker pool

    get_stream ()->start ();
  }

  void log_consumer::start_daemons (void)
  {
    m_subtran_applier = new subtran_applier (*this);

    m_dispatch_daemon = cubthread::get_manager ()->create_daemon (cubthread::delta_time (0),
			new dispatch_daemon_task (*this),
			"apply_stream_entry_daemon");

    m_applier_workers_pool = cubthread::get_manager ()->create_worker_pool (m_applier_worker_threads_count,
			     m_applier_worker_threads_count,
			     "replication_apply_workers",
			     NULL, 1, 1);

    m_use_daemons = true;
  }

  void log_consumer::wait_dispatcher_applied_all ()
  {
    std::unique_lock<std::mutex> ul (m_dispatch_finished_mtx);
    m_dispatch_finished_cv.wait (ul, [this] ()
    {
      return m_dispatch_finished;
    });
  }

  void log_consumer::set_dispatcher_finished ()
  {
    m_dispatch_finished = true;
    m_dispatch_finished_cv.notify_one ();
  }

  void log_consumer::push_task (cubthread::entry_task *task)
  {
    /* Increase m_started_task before starting the task. */
    m_started_tasks++;

    cubthread::get_manager ()->push_task (m_applier_workers_pool, task);
  }

  void log_consumer::execute_task (applier_worker_task *task)
  {
    if (prm_get_bool_value (PRM_ID_DEBUG_REPLICATION_DATA))
      {
	string_buffer sb;
	task->stringify (sb);
	_er_log_debug (ARG_FILE_LINE, "log_consumer::execute_task:\n%s", sb.get_buffer ());
      }

    push_task (task);
  }

  void log_consumer::wait_for_tasks (void)
  {
    /* First, check without mutex. */
    if (m_started_tasks == 0)
      {
	return;
      }

    std::unique_lock<std::mutex> ulock (m_join_tasks_mutex);
    m_join_tasks_cv.wait (ulock, [this] {return m_started_tasks == 0;});
  }

  void log_consumer::stop (void)
  {
    /* wakeup fetch daemon to allow it time to detect it is stopped */
    fetch_resume ();

    get_stream ()->stop ();
  }

  void log_consumer::fetch_suspend (void)
  {
    m_fetch_suspend.clear ();
  }

  void log_consumer::fetch_resume (void)
  {
    m_fetch_suspend.set ();
  }

  void log_consumer::wait_for_fetch_resume (void)
  {
    m_fetch_suspend.wait ();
  }

  subtran_applier &log_consumer::get_subtran_applier ()
  {
    return *m_subtran_applier;
  }

} /* namespace cubreplication */
