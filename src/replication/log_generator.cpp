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
 * log_generator.cpp
 */

#include "log_generator.hpp"

#include "error_manager.h"
#include "heap_file.h"
#include "log_impl.h"
#include "multi_thread_stream.hpp"
#include "replication.h"
#include "replication_stream_entry.hpp"
#include "string_buffer.hpp"
#include "system_parameter.h"
#include "thread_manager.hpp"

namespace cubreplication
{
  static bool enable_log_generator_logging = false;

  log_generator::~log_generator()
  {
    LOG_LSA start_lsa;
    LSA_SET_NULL (&start_lsa);
    m_stream_entry.destroy_objects_after_lsa (start_lsa);
  }

  void log_generator::set_tran_repl_info(MVCCID mvccid, stream_entry_header::TRAN_STATE state)
  {

    if (prm_get_bool_value(PRM_ID_REPL_LOG_LOCAL_DEBUG) == true)
      {
      }
    assert(m_has_stream);
    m_stream_entry.set_mvccid(mvccid);
    m_stream_entry.set_state(state);
  }  

  void
    log_generator::add_statement(repl_info_sbr &stmt_info)
  {
    LOG_LSA *p_lsa;
    if (is_replication_disabled())
    {
      return;
    }

    cubthread::entry *thread_p = &cubthread::get_entry();
    p_lsa = logtb_find_current_tran_lsa(thread_p);
    assert(p_lsa != NULL);
    
    sbr_repl_entry *repl_obj = new sbr_repl_entry (stmt_info.stmt_text, stmt_info.db_user, stmt_info.sys_prm_context, *p_lsa);
    append_repl_object (*repl_obj);
  }

  void
    log_generator::add_delete_row(const DB_VALUE &key, const OID &class_oid)
  {    
    if (is_row_replication_disabled())
    {
      return;
    }

    LOG_LSA *p_lsa;
    /* Currently we set last lsa. Later we can adapt it to match the corresponding stream position. */
    char *classname = get_classname(class_oid);
    cubthread::entry *thread_p = &cubthread::get_entry();
    p_lsa = logtb_find_current_tran_lsa (thread_p);
    assert (p_lsa != NULL);
    
    single_row_repl_entry *repl_obj = new single_row_repl_entry(REPL_DELETE, classname, *p_lsa);
    repl_obj->set_key_value(key);
    append_repl_object(*repl_obj);

    free(classname);
  }

  void
    log_generator::add_insert_row(const DB_VALUE &key, const OID &class_oid, const RECDES &record)
  {    
    if (is_row_replication_disabled())
    {
      return;
    }

    LOG_LSA *p_lsa;
    char *classname = get_classname(class_oid);
    cubthread::entry *thread_p = &cubthread::get_entry();
    p_lsa = logtb_find_current_tran_lsa(thread_p);
    assert (p_lsa != NULL);

    rec_des_row_repl_entry *repl_obj = new rec_des_row_repl_entry(REPL_INSERT, classname, record, *p_lsa);
    repl_obj->set_key_value(key);
    append_repl_object(*repl_obj);

    free(classname);
  }

  void
    log_generator::append_repl_object(replication_object &object)
  {    
    m_stream_entry.add_packable_entry (&object);

    er_log_repl_obj(&object, "log_generator::append_repl_object");
  }

  /* in case inst_oid is not found, create a new entry and append it to pending,
   * else, add value and col_id to it
   * later, when setting key_dbvalue to it, move it to m_stream_entry
   */
  void
    log_generator::add_attribute_change(const OID &class_oid, const OID &inst_oid, ATTR_ID col_id,
      const DB_VALUE &value)
  {    
    if (is_row_replication_disabled())
    {
      return;
    }

    changed_attrs_row_repl_entry *entry = NULL;
    char *class_name = NULL;

    for (auto &repl_obj : m_pending_to_be_added)
    {
      if (repl_obj->compare_inst_oid(inst_oid))
      {
	entry = repl_obj;
	break;
      }
    }

    if (entry != NULL)
    {
      entry->copy_and_add_changed_value(col_id, value);
    }
    else
    {
      LOG_LSA *p_lsa;
      int error_code = NO_ERROR;

      char *class_name = get_classname(class_oid);

      cubthread::entry *thread_p = &cubthread::get_entry();
      p_lsa = logtb_find_current_tran_lsa(thread_p);
      assert(p_lsa != NULL);
      entry = new changed_attrs_row_repl_entry(cubreplication::repl_entry_type::REPL_UPDATE, class_name, inst_oid, *p_lsa);
      entry->copy_and_add_changed_value(col_id, value);

      m_pending_to_be_added.push_back(entry);

      free(class_name);
    }

    er_log_repl_obj(entry, "log_generator::add_attribute_change");
  }


  void log_generator::remove_attribute_change(const OID &class_oid, const OID &inst_oid)
  {
    if (is_row_replication_disabled())
    {
      return;
    }

    for (auto repl_obj = m_pending_to_be_added.begin(); repl_obj != m_pending_to_be_added.end(); ++repl_obj)
    {
      if ((*repl_obj)->compare_inst_oid(inst_oid))
      {
	(void)m_pending_to_be_added.erase(repl_obj);
	break;
      }
    }
  }

  /* first fetch the class name, then set key */
  void
    log_generator::add_update_row(const DB_VALUE &key, const OID &inst_oid, const OID &class_oid,
      const RECDES *optional_recdes)
  {
    if (is_row_replication_disabled())
    {
      return;
    }

    char *class_name = get_classname(class_oid);
    bool found = false;

    for (auto repl_obj_it = m_pending_to_be_added.begin(); repl_obj_it != m_pending_to_be_added.end(); ++repl_obj_it)
    {
      changed_attrs_row_repl_entry *repl_obj = *repl_obj_it;
      if (repl_obj->compare_inst_oid(inst_oid))
      {
	repl_obj->set_key_value(key);

	append_repl_object(*repl_obj);
	er_log_repl_obj(repl_obj, "log_generator::set_key_to_repl_object");

	// remove
	(void)m_pending_to_be_added.erase(repl_obj_it);

	found = true;

	break;
      }
    }

    if (!found)
    {
      assert(optional_recdes != NULL);

      LOG_LSA *p_lsa;
      cubthread::entry *thread_p = &cubthread::get_entry();
      p_lsa = logtb_find_current_tran_lsa(thread_p);
      assert(p_lsa != NULL);

      cubreplication::rec_des_row_repl_entry *entry =
	new cubreplication::rec_des_row_repl_entry(cubreplication::repl_entry_type::REPL_UPDATE, class_name,
	  *optional_recdes, *p_lsa);

      append_repl_object(*entry);

      er_log_repl_obj(entry, "log_generator::set_key_to_repl_object");
    }

    free(class_name);
  }

  char *
    log_generator::get_classname(const OID &class_oid)
  {
    char *classname = NULL;
    cubthread::entry *thread_p = &cubthread::get_entry();
    bool save = logtb_set_check_interrupt(thread_p, false);
    if (heap_get_class_name(thread_p, &class_oid, &classname) != NO_ERROR || classname == NULL)
    {
      assert(false);
    }
    (void)logtb_set_check_interrupt(thread_p, save);
    return classname;
  }

  /* in case of error, abort all pending replication objects */
  void
    log_generator::abort_pending_repl_objects(void)
  {
    for (changed_attrs_row_repl_entry *entry : m_pending_to_be_added)
    {
      delete entry;
    }
    m_pending_to_be_added.clear();
  }

  stream_entry *log_generator::get_stream_entry(void)
  {
    return &m_stream_entry;
  }

  void
    log_generator::pack_stream_entry(void)
  {
#if !defined(NDEBUG)
    if (prm_get_bool_value (PRM_ID_REPL_LOG_LOCAL_DEBUG))
      {
        /* Testing purpose. */
        return;
      }
#endif
    assert(m_has_stream);
    
    m_stream_entry.pack();
    m_stream_entry.reset();
    // reset state
    m_stream_entry.set_state(stream_entry_header::ACTIVE);
  }

  void
    log_generator::pack_group_commit_entry(void)
  {
    static stream_entry gc_stream_entry(s_stream, MVCCID_NULL, stream_entry_header::GROUP_COMMIT);
    gc_stream_entry.pack();
  }

  void
    log_generator::set_global_stream(cubstream::multi_thread_stream *stream)
  {
    for (int i = 0; i < log_Gl.trantable.num_total_indices; i++)
    {
      LOG_TDES *tdes = LOG_FIND_TDES(i);

      log_generator *lg = &(tdes->replication_log_generator);

      lg->set_stream(stream);
    }
    log_generator::s_stream = stream;
  }

  void
    log_generator::er_log_repl_obj(replication_object *obj, const char *message)
  {
    string_buffer strb;

    if (!enable_log_generator_logging)
    {
      return;
    }

    obj->stringify(strb);

    _er_log_debug(ARG_FILE_LINE, "%s\n%s", message, strb.get_buffer());
  }

  void
    log_generator::check_commit_end_tran(void)
  {
    /* check there are no pending replication objects */
    assert(m_pending_to_be_added.size() == 0);
  }

  void
    log_generator::on_transaction_finish(stream_entry_header::TRAN_STATE state)
  {
    if (is_replication_disabled())
    {
      return;
    }

    cubthread::entry *thread_p = &cubthread::get_entry();
    int tran_index = LOG_FIND_THREAD_TRAN_INDEX(thread_p);
    LOG_TDES *tdes = LOG_FIND_TDES(tran_index);

#if !defined(NDEBUG)
    if (prm_get_bool_value (PRM_ID_REPL_LOG_LOCAL_DEBUG))
    {
      /* Reset stream entry. */
      assert(m_sysops_stream_entry.size() == 0);      
      m_stream_entry.reset();
      
      return;
    }    
#endif

    set_tran_repl_info(tdes->mvccinfo.id, state);
    pack_stream_entry();
  }

  void
    log_generator::on_transaction_commit(void)
  {
    on_transaction_finish(stream_entry_header::TRAN_STATE::COMMITTED);
  }

  void log_generator::on_sysop_commit (int sysop_index)
  {    
    if ((is_replication_disabled() && prm_get_bool_value(PRM_ID_REPL_LOG_LOCAL_DEBUG) == false)
	|| m_sysops_stream_entry.size() == 0 || sysop_index > m_sysops_stream_entry.size())
      {
	return;
      }

    stream_entry *last_sysop_stream_entry;
    MVCCID mvccid = logtb_find_current_mvccid (&cubthread::get_entry());
    
    /* Get the stream entry of last sysop and set MVCCID. */
    assert(sysop_index == m_sysops_stream_entry.size() - 1);
    last_sysop_stream_entry = m_sysops_stream_entry.back();
    last_sysop_stream_entry->set_mvccid (mvccid);
    
    /* Write objects in stream and then destroy them. */
#if !defined(NDEBUG)
    if (prm_get_bool_value(PRM_ID_REPL_LOG_LOCAL_DEBUG) == false)
    {
      m_sysops_stream_entry.pop_back();
      return;
    }    
#endif

    last_sysop_stream_entry->pack();
    last_sysop_stream_entry->reset();
    /* Remove stream entry from vector and destroy the stream entry. */
    m_sysops_stream_entry.pop_back();
  }  

  void
    log_generator::on_transaction_abort(void)
  {
    on_transaction_finish (stream_entry_header::TRAN_STATE::ABORTED);
  }

  void log_generator::on_sysop_abort (LOG_LSA &start_lsa)
  {    
    //if ((is_replication_disabled() && prm_get_bool_value(PRM_ID_REPL_LOG_LOCAL_DEBUG) == false)
    //    /*|| m_sysops_stream_entry.size() == 0 || sysop_index > m_sysops_stream_entry.size()*/)
    //{
    //  /* Nothing to abort here. */
    //  return;
    //}

    replication_object *repl_obj;
    LOG_LSA repl_lsa;        
    cubthread::entry *thread_p = &cubthread::get_entry();
    cubreplication::stream_entry *stream_entry;
    int count_entries;
    
    /* TODO - replace it with lsa stacks */
    if (m_sysops_stream_entry.size() > 0)
    { 
      stream_entry = m_sysops_stream_entry.back();
#if !defined(NDEBUG)
      count_entries = (int) stream_entry->count_entries();
      for (int i = count_entries - 1; i >= 0; i--)
        {
          repl_obj = stream_entry->get_object_at(i);
          repl_obj->get_lsa(repl_lsa);

          if (LSA_GT (&start_lsa, &repl_lsa))
          {
            assert (false);
          }     
        }

#endif    
      
      stream_entry->reset();
      m_sysops_stream_entry.pop_back();
    }
    else
    {      
      
      cubreplication::stream_entry *stream_entry = logtb_get_tdes(thread_p)->replication_log_generator.get_stream_entry();
      int count_entries = (int) stream_entry->count_entries();

      if (count_entries == 0)
        {
          return;
        }

      stream_entry->destroy_objects_after_lsa (start_lsa);
    }           
  }

  void log_generator::on_sysop_attach_to_outer (int sysop_index)
  {
    if ((is_replication_disabled() && prm_get_bool_value(PRM_ID_REPL_LOG_LOCAL_DEBUG) == false)
        || m_sysops_stream_entry.size() == 0 || sysop_index > m_sysops_stream_entry.size())
    {
      /* Nothing to attach to outer here. */
      return;
    }

    stream_entry *last_sysop_stream_entry, *parent_sysop_stream_entry;    

    /* Get the stream entry of last sysop and move its objects into parent stream. */
    assert(sysop_index == m_sysops_stream_entry.size() - 1);
    last_sysop_stream_entry = m_sysops_stream_entry.back();

    if (m_sysops_stream_entry.size() > 1)
      {
	parent_sysop_stream_entry = m_sysops_stream_entry.back() - 1;
      }
    else
      {
	parent_sysop_stream_entry = &m_stream_entry;
      }

    for (int i = 0; i < last_sysop_stream_entry->count_entries(); i++)
      {      
	parent_sysop_stream_entry->add_packable_entry (last_sysop_stream_entry->get_object_at (i));
      }           

    /* Remove last stream entry from vector and destroy the stream entry. */
    m_sysops_stream_entry.pop_back();
  }

  void
    log_generator::clear_transaction(void)
  {
    if (is_replication_disabled())
    {
      return;
    }

    m_is_row_replication_disabled = false;
#if !defined(NDEBUG)
    m_enable_debug_repl_local = prm_get_bool_value(PRM_ID_REPL_LOG_LOCAL_DEBUG);
#endif
  }

  bool
    log_generator::is_replication_disabled()
  {
#if defined (SERVER_MODE)
    return !log_does_allow_replication();
#else
    return true;
#endif
  }

  bool
    log_generator::is_row_replication_disabled()
  {
    return is_replication_disabled() || m_is_row_replication_disabled;
  }

  void
    log_generator::set_row_replication_disabled(bool disable_if_true)
  {
    m_is_row_replication_disabled = disable_if_true;
  }

#if !defined(NDEBUG)
  void log_generator::disable_debug_repl_local()
  {
    m_enable_debug_repl_local = false;
  }

  bool log_generator::is_debug_repl_local_disabled()
  {
    return !m_enable_debug_repl_local;
  }
#endif

  /* Debug function */
  /* Abort operation and simulate it again with apply. */
  int log_generator::abort_sysop_and_simulate_apply_repl_on_master (LOG_LSA &filter_replication_lsa)
  {
    int err_code = NO_ERROR;
    replication_object *repl_obj;
    cubthread::entry *thread_p = &cubthread::get_entry();       
    LOG_TDES * tdes = logtb_get_tdes (&cubthread::get_entry());
    cubreplication::stream_entry *stream_entry = tdes->replication_log_generator.get_stream_entry ();

    assert(!tdes->replication_log_generator.is_debug_repl_local_disabled() && stream_entry->count_entries() > 0);
    
    /* Save the replication objects, before abort. */
    std::vector <cubreplication::replication_object *> repl_objects_after_lsa;
    stream_entry->move_replication_objects_after_lsa (filter_replication_lsa, repl_objects_after_lsa);

    /* First abort the operation. */
    log_sysop_abort (thread_p);          

    /* Simulate it again with apply . */
    for (unsigned int i = 0; i < repl_objects_after_lsa.size(); i++)
    {
      repl_obj = repl_objects_after_lsa[i];
      if (repl_obj != NULL)
      {
        err_code = repl_obj->apply();
        if (err_code != NO_ERROR)
        {
          break;
        }
      }
    }  

    /* Now, destroy */
    for (unsigned int i = 0; i < repl_objects_after_lsa.size(); i++)
    {
      if (repl_objects_after_lsa[i] != NULL)
      {
        delete (repl_objects_after_lsa[i]);
      }
    }
    repl_objects_after_lsa.clear();

    return err_code;
  }

  void log_generator::add_stream_entries_for_last_sysop (void)
  {
    stream_entry *p_stream_entry;
    cubstream::multi_thread_stream *p_multi_thread_stream;
    cubthread::entry *thread_p;
    int tran_index, num_new_stream_entries;
    LOG_TDES *tdes;

    thread_p = &cubthread::get_entry();
    tran_index = LOG_FIND_THREAD_TRAN_INDEX(thread_p);
    tdes = LOG_FIND_TDES(tran_index);

    assert(m_has_stream == true && tdes->topops.last > 0);
    
    /* Here we may add null streams, to optimize it. */
    p_multi_thread_stream = get_stream();
    num_new_stream_entries = tdes->topops.last - (int) m_sysops_stream_entry.size() + 1;
    for (int i = 0; i < num_new_stream_entries; i++)
      {
	p_stream_entry = new stream_entry (p_multi_thread_stream);
	p_stream_entry->set_stream (p_multi_thread_stream);
	m_sysops_stream_entry.push_back(p_stream_entry);
      }
  }

  cubstream::multi_thread_stream *log_generator::s_stream = NULL;
} /* namespace cubreplication */
