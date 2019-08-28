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
 * replication_stream_entry.hpp
 */

#ident "$Id$"

#ifndef _REPLICATION_STREAM_ENTRY_HPP_
#define _REPLICATION_STREAM_ENTRY_HPP_

#include "log_lsa.hpp"
#include "replication_object.hpp"
#include "stream_entry.hpp"
#include "storage_common.h"

#include <vector>

namespace cubreplication
{

  /* a replication stream entry is generated by a transaction or a group commit action */
  struct stream_entry_header
  {
    typedef enum
    {
      UNDEFINED = 0,
      ACTIVE,
      COMMITTED,
      ABORTED,
      GROUP_COMMIT,
      NEW_MASTER,
      SUBTRAN_COMMIT,
      START_OF_EXTRACT_HEAP,
      END_OF_EXTRACT_HEAP,
      END_OF_REPLICATION_COPY
    } TRAN_STATE;

    cubstream::stream_position prev_record;
    MVCCID mvccid;
    unsigned int count_replication_entries;
    int data_size;

    TRAN_STATE tran_state;

    stream_entry_header ()
      : prev_record (0),
	mvccid (MVCCID_NULL),
	count_replication_entries (0),
	data_size (0),
	tran_state (UNDEFINED)
    {
    };

    static size_t get_size (cubpacking::packer &serializator)
    {
      size_t header_size = 0;

      header_size += serializator.get_packed_bigint_size (header_size);
      header_size += serializator.get_packed_bigint_size (header_size);
      header_size += serializator.get_packed_int_size (header_size);
      header_size += serializator.get_packed_int_size (header_size);
      header_size += serializator.get_packed_int_size (header_size);

      return header_size;
    }

    static const char *tran_state_string (TRAN_STATE state);

    bool needs_mvccid () const;
  };

  class stream_entry : public cubstream::entry<replication_object>
  {
    public:
      enum string_dump_mode
      {
	short_dump = 0,
	detailed_dump = 1
      };

    private:
      stream_entry_header m_header;
      cubpacking::packer m_serializator;
      cubpacking::unpacker m_deserializator;

      static cubstream::entry<replication_object>::packable_factory *s_replication_factory_po;

      static cubstream::entry<replication_object>::packable_factory *create_builder ();

    public:
      stream_entry (cubstream::multi_thread_stream *stream_p)
	: entry (stream_p)
	, m_serializator ()
	, m_deserializator ()
      {
      };

      stream_entry (cubstream::multi_thread_stream *stream_p,
		    MVCCID arg_mvccid,
		    stream_entry_header::TRAN_STATE state)
	: entry (stream_p)
	, m_serializator ()
	, m_deserializator ()
      {
	m_header.mvccid = arg_mvccid;
	m_header.tran_state = state;
      };

      size_t get_packed_header_size () override
      {
	return s_header_size;
      }

      size_t get_data_packed_size (void) override;
      void set_header_data_size (const size_t &data_size) override;

      cubstream::entry<replication_object>::packable_factory *get_builder () override;

      cubpacking::packer *get_packer () override
      {
	return &m_serializator;
      }

      cubpacking::unpacker *get_unpacker () override
      {
	return &m_deserializator;
      }

      void set_mvccid (MVCCID mvccid)
      {
	m_header.mvccid = mvccid;
      }

      MVCCID get_mvccid ()
      {
	return m_header.mvccid;
      }

      void set_state (stream_entry_header::TRAN_STATE state)
      {
	m_header.tran_state = state;
      }

      bool is_group_commit (void) const
      {
	return m_header.tran_state == stream_entry_header::GROUP_COMMIT;
      }

      bool is_new_master () const
      {
	return m_header.tran_state == stream_entry_header::NEW_MASTER;
      }

      bool is_tran_commit (void) const
      {
	return m_header.tran_state == stream_entry_header::COMMITTED;
      }

      bool is_subtran_commit (void) const
      {
	return m_header.tran_state == stream_entry_header::SUBTRAN_COMMIT;
      }

      bool is_tran_abort (void) const
      {
	return m_header.tran_state == stream_entry_header::ABORTED;
      }

      bool is_tran_state_undefined (void) const
      {
	return m_header.tran_state < stream_entry_header::ACTIVE
	       || m_header.tran_state > stream_entry_header::SUBTRAN_COMMIT;
      }

      bool check_mvccid_is_valid () const;

      int pack_stream_entry_header () override;
      int unpack_stream_entry_header () override;
      int get_packable_entry_count_from_header (void) override;

      void stringify (string_buffer &sb, const string_dump_mode mode = short_dump);

      bool is_equal (const cubstream::entry<replication_object> *other) override;
      static size_t compute_header_size (void);
      void move_replication_objects_after_lsa_to_stream (LOG_LSA &lsa, stream_entry &entry);
      void destroy_objects_after_lsa (LOG_LSA &start_lsa);

      static size_t s_header_size;
  };

} /* namespace cubreplication */

#endif /* _REPLICATION_STREAM_ENTRY_HPP_ */
