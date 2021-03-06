/** -*- c++ -*-
 * Copyright (C) 2007-2012 Hypertable, Inc.
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 3 of the
 * License, or any later version.
 *
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifndef HYPERTABLE_INDEXRESULTCALLBACK_H
#define HYPERTABLE_INDEXRESULTCALLBACK_H

#include <vector>
#include <deque>
#include <map>
#include <boost/thread/condition.hpp>

#include "Common/Filesystem.h"
#include "HyperAppHelper/Unique.h"
#include "ResultCallback.h"
#include "TableScannerAsync.h"
#include "ScanSpec.h"
#include "Namespace.h"
#include "Client.h"

// this macro enables the "ScanSpecBuilder queue" test code; it fills the queue
// till it exceeds the limit, and makes sure that the queue is blocking
// till it gets empty again
#undef TEST_SSB_QUEUE

namespace Hypertable {

static String last;
  static const char *tmp_schema_outer=
        "<Schema>"
          "<AccessGroup name=\"default\">"
            "%s"
          "</AccessGroup>"
        "</Schema>";

  static const char *tmp_schema_inner=
        "<ColumnFamily>"
          "<Name>%s</Name>"
          "<Counter>false</Counter>"
          "<MaxVersions>1</MaxVersions> "
          "<deleted>false</deleted>"
        "</ColumnFamily>";

  /** ResultCallback for secondary indices; used by TableScannerAsync
   */
  class IndexScannerCallback : public ResultCallback {

#if defined (TEST_SSB_QUEUE)
    static const size_t SSB_QUEUE_LIMIT = 4;
#else
    static const size_t SSB_QUEUE_LIMIT = 40;
#endif

    /** if more than TMP_CUTOFF bytes are received from the index then
     * store all results in a temporary table */
#if defined (TEST_SSB_QUEUE)
    static const size_t TMP_CUTOFF = 1;
#else
    static const size_t TMP_CUTOFF = 1024*1024;
#endif

  public:

    IndexScannerCallback(TablePtr primary_table, const ScanSpec &primary_spec, 
            ResultCallback *original_cb, uint32_t timeout_ms, 
            bool qualifier_scan)
      : ResultCallback(), m_primary_table(primary_table), 
        m_primary_spec(primary_spec), m_original_cb(original_cb), 
        m_timeout_ms(timeout_ms), m_mutator(0), 
        m_row_limit(0), m_cell_limit(0), m_cell_count(0), m_row_offset(0), 
        m_cell_offset(0), m_row_count(0), m_cell_limit_per_family(0), 
        m_eos(false), m_limits_reached(false), m_readahead_count(0), 
        m_qualifier_scan(qualifier_scan), m_tmp_cutoff(0), 
        m_final_decrement(false) {
      atomic_set(&m_outstanding_scanners, 0);
      m_original_cb->increment_outstanding();

      if (m_primary_spec.row_limit != 0 ||
          m_primary_spec.cell_limit != 0 ||
          m_primary_spec.row_offset != 0 ||
          m_primary_spec.cell_offset != 0 ||
          m_primary_spec.cell_limit_per_family != 0) {
        // keep track of offset and limit
        m_track_limits = true;
        m_row_limit = m_primary_spec.row_limit;
        m_cell_limit = m_primary_spec.cell_limit;
        m_row_offset = m_primary_spec.row_offset;
        m_cell_offset = m_primary_spec.cell_offset;
        m_cell_limit_per_family = m_primary_spec.cell_limit_per_family;
      }
      else
        m_track_limits = false;

      Schema::ColumnFamilies &families =
                primary_table->schema()->get_column_families();
      foreach (Schema::ColumnFamily *cf, families) {
        if (!cf->has_index && !cf->has_qualifier_index)
          continue;
        m_column_map[cf->id] = cf->name;
      }
    }

    virtual ~IndexScannerCallback() {
      ScopedLock lock(m_mutex);
      if (m_mutator)
        delete m_mutator;
      foreach (TableScannerAsync *s, m_scanners)
        delete s;
      m_scanners.clear();
      sspecs_clear();
      if (m_tmp_table) {
        Client *client = m_primary_table->get_namespace()->get_client();
        NamespacePtr nstmp = client->open_namespace("/tmp");
        nstmp->drop_table(Filesystem::basename(m_tmp_table->get_name()), true);
      }
    }

    void sspecs_clear() {
      foreach (ScanSpecBuilder *ssb, m_sspecs)
        delete ssb;
      m_sspecs.clear();
      m_sspecs_cond.notify_one();
    }

    /**
     * Callback method for successful scan
     *
     * @param scanner
     * @param cells returned cells
     */
    virtual void scan_ok(TableScannerAsync *scanner, ScanCellsPtr &scancells) {
      bool is_eos = scancells->get_eos();
      String table_name = scanner->get_table_name();

      ScopedLock lock(m_mutex);

      // ignore empty packets
      if (scancells->get_eos() == false && scancells->empty())
        return;

      // reached end of this scanner?
      if (is_eos) {
        HT_ASSERT(atomic_read(&m_outstanding_scanners) > 0);
        atomic_dec(&m_outstanding_scanners);
      }

      // we've reached eos (i.e. because CELL_LIMITs/ROW_LIMITs were reached)
      // just collect the outstanding scanners and ignore the cells
      if (m_eos == true) {
        if (atomic_read(&m_outstanding_scanners) == 0)
          final_decrement(scanner, is_eos);
        return;
      }

      // If the cells are from the index table then collect and store them
      // in memory (or in a temporary table)
      if (Filesystem::basename(table_name)[0] == '^')
        collect_indices(scanner, scancells);
      // If the cells are from the temporary table then they need to be 
      // verified against the primary table
      else if (table_name != m_primary_table->get_name())
        verify_results(lock, scanner, scancells);
      // Otherwise cells are returned from the primary table: check 
      // LIMIT/OFFSET and send them to the original callback
      else {
        scancells->set_eos(false);

        if (m_track_limits)
          track_predicates(scanner, scancells);
        else
          m_original_cb->scan_ok(scanner, scancells);

        // fetch data from the next scanner when we have reached the end of
        // the current one
        if (!m_limits_reached && is_eos)
          readahead();
      }

      final_decrement(scanner, is_eos);
    }

    virtual void register_scanner(TableScannerAsync *scanner) { 
      atomic_inc(&m_outstanding_scanners);
    }

    /**
     * Callback method for scan errors
     *
     * @param scanner
     * @param error
     * @param error_msg
     * @param eos end of scan
     */
    virtual void scan_error(TableScannerAsync *scanner, int error, 
                            const String &error_msg, bool eos) {
      m_original_cb->scan_error(scanner, error, error_msg, eos);
      if (eos)
        m_original_cb->decrement_outstanding();
    }

    virtual void update_ok(TableMutatorAsync *mutator) {
    }

    virtual void update_error(TableMutatorAsync *mutator, int error, 
            FailedMutations &failedMutations) {
      m_original_cb->update_error(mutator, error, failedMutations);
    }

   private:
    void final_decrement(TableScannerAsync *scanner, bool is_eos) {
      // If the last outstanding scanner just finished; send an "eos" 
      // packet to the original callback and decrement the outstanding scanners
      // once more (this is the equivalent operation to the increment in
      // the constructor)
      bool final_eos = false;
      if (m_track_limits) {
        if (((m_limits_reached && is_eos && m_eos) && 
                    atomic_read(&m_outstanding_scanners) == 0)
            || (!m_limits_reached && (is_eos || m_eos) && 
                atomic_read(&m_outstanding_scanners) == 0))
          final_eos = true;
      }
      else {
        if ((is_eos || m_eos) && atomic_read(&m_outstanding_scanners) == 0)
          final_eos = true;
      }
      if (final_eos) {
        m_eos = true;
        HT_ASSERT(m_final_decrement == false);
        if (!m_final_decrement) {
          // send empty eos package to caller
          ScanCellsPtr empty = new ScanCells;
          empty->set_eos();
          m_original_cb->scan_ok(scanner, empty);
          m_original_cb->decrement_outstanding();
          m_final_decrement = true;
        }
      }
    }

    void collect_indices(TableScannerAsync *scanner, ScanCellsPtr &scancells) {
      // split the index row into column id, cell value and cell row key
      size_t old_inserted_keys = m_tmp_keys.size();
      Cells cells;
      scancells->get(cells);
      foreach (Cell &cell, cells) {
        char *r = (char *)cell.row_key;
        char *p = r + strlen(r);
        while (*p != '\t' && p > (char *)cell.row_key)
          p--;
        if (*p != '\t') {
          HT_WARNF("Invalid index entry '%s' in index table '^%s'",
                  r, m_primary_table->get_name().c_str());
          continue;
        }
        // cut off the "%d," part at the beginning to get the column id
        // The max. column id is 255, therefore there must be a ',' after 3
        // positions
        char *id = r;
        while (*r != ',' && (r - id <= 4))
          r++;
        if (*r != ',') {
          HT_WARNF("Invalid index entry '%s' in index table '^%s'",
                  id, m_primary_table->get_name().c_str());
          continue;
        }
        *r++ = 0;
        uint32_t cfid = (uint32_t)atoi(id);
        if (!cfid || m_column_map.find(cfid) == m_column_map.end()) {
          HT_WARNF("Invalid index entry '%s' in index table '^%s'",
                  r, m_primary_table->get_name().c_str());
          continue;
        }

        // split strings; after the next line p will point to the row key
        // and r will point to the value. id points to the column id
        *p++ = 0;

        // if the original query specified row intervals then these have
        // to be filtered in the client
        if (m_primary_spec.row_intervals.size()) {
          if (!row_intervals_match(m_primary_spec.row_intervals, p))
            continue;
        }

        // same about cell intervals
        if (m_primary_spec.cell_intervals.size()) {
          if (!cell_intervals_match(m_primary_spec.cell_intervals, p, 
                                m_column_map[cfid].c_str()))
            continue;
        }

        // if a temporary table was already created then store it in the 
        // temporary table. otherwise buffer it in memory but make sure
        // that no duplicate rows are inserted
        KeySpec key;
        key.row = p;
        key.row_len = strlen(p);
        key.column_family = m_column_map[cfid].c_str();
        key.timestamp = cell.timestamp;
        if (m_mutator) {
          m_mutator->set(key, 0);
        }
        else if (m_tmp_keys.find(key) == m_tmp_keys.end()) {
          m_tmp_keys.insert(CkeyMap::value_type(key, true));
        }
        m_tmp_cutoff += key.row_len + sizeof(KeySpec);
      }

      // if the temporary table was not yet created: make sure that the keys
      // don't point to invalid memory
      if (!m_mutator && m_tmp_keys.size() > old_inserted_keys)
        m_scancells_buffer.push_back(scancells);

      // reached EOS? then flush the mutator
      if (scancells->get_eos()) {
        if (m_mutator) {
          delete m_mutator;
          m_mutator = 0;
        }
        if (!m_tmp_table && m_tmp_keys.empty()) {
          m_eos = true;
          return;
        }
      }
      // not EOS? then more keys will follow
      else {
        // store all buffered keys in a temp. table if we have too many
        // results from the index
        if (!m_tmp_table && m_tmp_cutoff > TMP_CUTOFF) {
          create_temp_table();
          for (CkeyMap::iterator it = m_tmp_keys.begin(); 
                  it != m_tmp_keys.end(); ++it) 
            m_mutator->set(it->first, 0);
        }
        // if a temp table existed (or was just created): clear the buffered
        // keys. they're no longer required
        if (m_tmp_table) {
          m_tmp_keys.clear();
          m_scancells_buffer.clear();
        }

        return;
      }

      // we've reached EOS. If there's a temporary table then create a 
      // scanner for this table. Otherwise immediately send the temporary
      // results to the primary table for verification
      ScanSpecBuilder ssb;
      ssb.set_max_versions(m_primary_spec.max_versions);
      ssb.set_return_deletes(m_primary_spec.return_deletes);
      ssb.set_keys_only(m_primary_spec.keys_only);
      ssb.set_row_regexp(m_primary_spec.row_regexp);
      foreach (const String &s, m_primary_spec.columns)
        ssb.add_column(s.c_str());
      ssb.set_time_interval(m_primary_spec.time_interval.first, 
                            m_primary_spec.time_interval.second);

      TableScannerAsync *s;
      if (m_tmp_table) {
        s = m_tmp_table->create_scanner_async(this, ssb.get(), 
                m_timeout_ms, Table::SCANNER_FLAG_IGNORE_INDEX);
      }
      else {
        for (CkeyMap::iterator it = m_tmp_keys.begin(); 
                it != m_tmp_keys.end(); ++it) 
          ssb.add_row((const char *)it->first.row);
        foreach (const ColumnPredicate &cp, m_primary_spec.column_predicates)
          ssb.add_column_predicate(cp.column_family, cp.operation,
                  cp.value, cp.value_len);

        s = m_primary_table->create_scanner_async(this, ssb.get(), 
                m_timeout_ms, Table::SCANNER_FLAG_IGNORE_INDEX);

        // clean up
        m_tmp_keys.clear();
        m_scancells_buffer.clear();
      }

      m_scanners.push_back(s);
    }

    /*
     * the temporary table mimicks the primary table: all column families
     * with an index are also created for the temporary table
     */
    void create_temp_table() {
      HT_ASSERT(m_tmp_table == NULL);
      HT_ASSERT(m_mutator == NULL);

      String inner;
      foreach (Schema::ColumnFamily *cf, 
              m_primary_table->schema()->get_column_families()) {
        if (m_qualifier_scan && !cf->has_qualifier_index)
          continue;
        if (!m_qualifier_scan && !cf->has_index)
          continue;
        inner += format(tmp_schema_inner, cf->name.c_str());
      }

      Client *client = m_primary_table->get_namespace()->get_client();
      NamespacePtr nstmp = client->open_namespace("/tmp");
      String guid = HyperAppHelper::generate_guid();
      nstmp->create_table(guid, format(tmp_schema_outer, inner.c_str()));
      m_tmp_table = nstmp->open_table(guid);

      m_mutator = m_tmp_table->create_mutator_async(this);
    }

    void verify_results(ScopedLock &lock, TableScannerAsync *scanner, 
                        ScanCellsPtr &scancells) {
      // no results from the primary table, or LIMIT/CELL_LIMIT exceeded? 
      // then return immediately
      if ((scancells->get_eos() && scancells->empty()) || m_limits_reached) {
        sspecs_clear();
        m_eos = true;
        return;
      }

      Cells cells;
      scancells->get(cells);
      const char *last = m_last_rowkey_verify.size() 
                       ? m_last_rowkey_verify.c_str() 
                       : "";

      // this test code creates one ScanSpec for each single row that is 
      // received from the temporary table. As soon as the scan spec queue
      // overflows it will block till the primary table scanners clear the
      // queue.
      //
      // see below for more comments
#if defined (TEST_SSB_QUEUE)
      foreach (Cell &cell, cells) {
        if (!strcmp(last, (const char *)cell.row_key))
          continue;
        last = (const char *)cell.row_key;

        ScanSpecBuilder *ssb = new ScanSpecBuilder;
        foreach (const String &s, m_primary_spec.columns)
          ssb->add_column(s.c_str());
        ssb->set_max_versions(m_primary_spec.max_versions);
        ssb->set_return_deletes(m_primary_spec.return_deletes);
        foreach (const ColumnPredicate &cp, m_primary_spec.column_predicates)
          ssb->add_column_predicate(cp.column_family, cp.operation,
                  cp.value, cp.value_len);
        if (m_primary_spec.value_regexp)
          ssb->set_value_regexp(m_primary_spec.value_regexp);

        ssb->add_row(cell.row_key);

        m_last_rowkey_verify = last;

        while (m_sspecs.size()>SSB_QUEUE_LIMIT && !m_limits_reached)
          m_sspecs_cond.wait(lock);

        if (m_limits_reached) { 
          delete ssb;
          return;
        }

        m_sspecs.push_back(ssb);
        if (atomic_read(&m_outstanding_scanners) <= 1)
          readahead();
      }
#else
      // This is the "production-ready" code, using a single ScanSpec for all 
      // rows that are returned from the intermediate table
      //
      // Create a new ScanSpec
      ScanSpecBuilder *ssb = new ScanSpecBuilder;
      foreach (const String &s, m_primary_spec.columns)
        ssb->add_column(s.c_str());
      ssb->set_max_versions(m_primary_spec.max_versions);
      ssb->set_return_deletes(m_primary_spec.return_deletes);
      foreach (const ColumnPredicate &cp, m_primary_spec.column_predicates)
        ssb->add_column_predicate(cp.column_family, cp.operation,
                cp.value, cp.value_len);
      if (m_primary_spec.value_regexp)
        ssb->set_value_regexp(m_primary_spec.value_regexp);

      // foreach cell from the secondary index: verify that it exists in
      // the primary table, but make sure that each rowkey is only inserted
      // ONCE
      foreach (Cell &cell, cells) {
        if (!strcmp(last, (const char *)cell.row_key))
          continue;
        last = (const char *)cell.row_key;

        // then add the key to the ScanSpec
        ssb->add_row(cell.row_key);
      } 
 
      // store the "last" pointer before it goes out of scope
      m_last_rowkey_verify = last;

      // add the ScanSpec to the queue
      while (m_sspecs.size()>SSB_QUEUE_LIMIT && !m_limits_reached)
        m_sspecs_cond.wait(lock);

      // if, in the meantime, we reached any CELL_LIMIT/ROW_LIMIT then return
      if (m_limits_reached) { 
        delete ssb;
        return;
      }

      // store ScanSpec in the queue
      m_sspecs.push_back(ssb);

      // there should always at least be two scanners outstanding: this scanner
      // from the intermediate table and one scanner from the primary table.
      // If not then make sure to start another readahead scanner on the
      // primary table.
      if (atomic_read(&m_outstanding_scanners) <= 0)
        readahead();
#endif
    }

    void readahead() {
      HT_ASSERT(m_limits_reached == false);
      HT_ASSERT(m_eos == false);

      if (m_sspecs.empty())
        return;

      ScanSpecBuilder *ssb = m_sspecs[0];
      m_sspecs.pop_front();
      TableScannerAsync *s = 
            m_primary_table->create_scanner_async(this, ssb->get(), 
                        m_timeout_ms, Table::SCANNER_FLAG_IGNORE_INDEX);
      m_scanners.push_back(s);
      m_readahead_count++;
      delete ssb;
      m_sspecs_cond.notify_one();
    }

    void track_predicates(TableScannerAsync *scanner, ScanCellsPtr &scancells) {
      // no results from the primary table, or LIMIT/CELL_LIMIT exceeded? 
      // then return immediately
      if ((scancells->get_eos() && scancells->empty()) || m_limits_reached) {
        sspecs_clear();
        m_eos = true;
        return;
      }

      // count cells and rows; skip CELL_OFFSET/OFFSET cells/rows and reduce
      // the results to CELL_LIMIT/LIMIT cells/rows
      ScanCellsPtr scp = new ScanCells();
      Cells cells;
      scancells->get(cells);
      const char *last = m_last_rowkey_tracking.size() 
                       ? m_last_rowkey_tracking.c_str() 
                       : "";
      bool skip_row = false;
      foreach (Cell &cell, cells) {
        bool new_row = false;
        if (strcmp(last, cell.row_key)) {
          new_row = true;
          skip_row = false;
          last = cell.row_key;
          if (m_cell_limit_per_family)
            m_cell_count = 0;
          // adjust row offset
          if (m_row_offset) {
            m_row_offset--;
            skip_row = true;
            continue;
          }
        }
        else if (skip_row)
          continue;

        // check cell offset
        if (m_cell_offset) {
          m_cell_offset--;
          continue;
        }
        // check row offset
        if (m_row_offset)
          continue;
        // check cell limit
        if (m_cell_limit && m_cell_count >= m_cell_limit) {
          m_limits_reached = true;
          break;
        }
        // check row limit
        if (m_row_limit && new_row && m_row_count >= m_row_limit) {
          m_limits_reached = true;
          break;
        }
        // check cell limit per family
        if (!m_cell_limit_per_family || m_cell_count < m_cell_limit_per_family){
          // cell pointers will go out of scope, therefore "own" is true
          scp->add(cell, true);
        }

        m_cell_count++;
        if (new_row)
          m_row_count++;
      }

      // store the contents of "last" before it goes out of scope
      m_last_rowkey_tracking = last;

      // send the results to the original callback
      if (scp->size())
        m_original_cb->scan_ok(scanner, scp);
    }

    bool row_intervals_match(const RowIntervals &rivec, const char *row) {
      foreach (const RowInterval &ri, rivec) {
        if (ri.start && ri.start[0]) {
          if (ri.start_inclusive) {
            if (strcmp(row, ri.start)<0)
              continue;
          }
          else {
            if (strcmp(row, ri.start)<=0)
              continue;
          }
        }
        if (ri.end && ri.end[0]) {
          if (ri.end_inclusive) {
            if (strcmp(row, ri.end)>0)
              continue;
          }
          else {
            if (strcmp(row, ri.end)>=0)
              continue;
          }
        }
        return true;
      } 
      return false;
    }

    bool cell_intervals_match(const CellIntervals &civec, const char *row,
            const char *column) {
      foreach (const CellInterval &ci, civec) {
        if (ci.start_row && ci.start_row[0]) {
          int s=strcmp(row, ci.start_row);
          if (s>0)
            return true;
          if (s<0)
            continue;
        }
        if (ci.start_column && ci.start_column[0]) {
          if (ci.start_inclusive) {
            if (strcmp(column, ci.start_column)<0)
              continue;
          }
          else {
            if (strcmp(column, ci.start_column)<=0)
              continue;
          }
        }
        if (ci.end_row && ci.end_row[0]) {
          int s=strcmp(row, ci.end_row);
          if (s<0)
            return true;
          if (s>0)
            continue;
        }
        if (ci.end_column && ci.end_column[0]) {
          if (ci.end_inclusive) {
            if (strcmp(column, ci.end_column)>0)
              continue;
          }
          else {
            if (strcmp(column, ci.end_column)>=0)
              continue;
          }
        }
        return true;
      } 
      return false;
    }

    typedef std::map<KeySpec, bool> CkeyMap;
    typedef std::map<String, String> CstrMap;

    // a pointer to the primary table
    TablePtr m_primary_table;

    // the original scan spec for the primary table
    ScanSpec m_primary_spec;

    // the original callback object specified by the user
    ResultCallback *m_original_cb;

    // the original timeout value specified by the user
    uint32_t m_timeout_ms;

    // a list of all scanners that are created in this object
    std::vector<TableScannerAsync *> m_scanners;

    // a deque of ScanSpecs, needed for readahead in the primary table
    std::deque<ScanSpecBuilder *> m_sspecs;

    // a condition to wait if the sspecs-queue is too full
    boost::condition m_sspecs_cond;

    // a mapping from column id to column name
    std::map<uint32_t, String> m_column_map;

    // the temporary table; can be NULL
    TablePtr m_tmp_table;

    // a mutator for the temporary table
    TableMutatorAsync *m_mutator;

    // limit and offset values from the original ScanSpec
    int m_row_limit;
    int m_cell_limit;
    int m_cell_count;
    int m_row_offset;
    int m_cell_offset;
    int m_row_count;
    int m_cell_limit_per_family;

    // we reached eos - no need to continue scanning
    bool m_eos;

    // track limits and offsets
    bool m_track_limits;

    // limits were reached, all following keys are discarded
    bool m_limits_reached;

    // a mutex 
    Mutex m_mutex;

    // counting the read-ahead scans
    int m_readahead_count;

    // temporary storage to persist pointer data before it goes out of scope
    String m_last_rowkey_verify;

    // temporary storage to persist pointer data before it goes out of scope
    String m_last_rowkey_tracking;

    // true if this index is a qualifier index
    bool m_qualifier_scan;

    // buffer for accumulating keys from the index
    CkeyMap m_tmp_keys;

    // accumulator; if > TMP_CUTOFF then store all index results in a
    // temporary table
    size_t m_tmp_cutoff;

    // stores the ScanCells; otherwise keys in m_tmp_keys can point to 
    // invalid memory
    std::vector<ScanCellsPtr> m_scancells_buffer;

    // keep track whether we called final_decrement() 
    bool m_final_decrement;

    // number of outstanding scanners (this is more precise than m_outstanding)
    atomic_t m_outstanding_scanners;
  };

  typedef intrusive_ptr<IndexScannerCallback> IndexScannerCallbackPtr;

  inline bool operator<(const KeySpec &lhs, const KeySpec &rhs) {
    size_t len1 = strlen((const char *)lhs.row); 
    size_t len2 = strlen((const char *)rhs.row); 
    int cmp = memcmp(lhs.row, rhs.row, std::min(len1, len2));
    if (cmp > 0) 
      return false;
    if (cmp < 0)
      return true;
    if (len1 < len2)
      return true;
    return false;
  }
} // namespace Hypertable

#endif // HYPERTABLE_INDEXRESULTCALLBACK_H
