// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#ifndef ROCKSDB_LITE

#include <string>
#include "rocksdb/options.h"
#include "rocksdb/table.h"

namespace rocksdb {

struct EnvOptions;

using std::unique_ptr;
class Status;
class RandomAccessFile;
class WritableFile;
class Table;
class TableBuilder;
class InstrumentedMutex;  // Shichao

class AdaptiveTableFactory : public TableFactory {
 public:
  ~AdaptiveTableFactory();

  explicit AdaptiveTableFactory(
      std::shared_ptr<TableFactory> table_factory_to_write,
      std::shared_ptr<TableFactory> block_based_table_factory,
      std::shared_ptr<TableFactory> plain_table_factory,
      std::shared_ptr<TableFactory> cuckoo_table_factory,
      std::shared_ptr<TableFactory> column_table_factory,  // Shichao
      int knob);  // Shichao

  const char* Name() const override { return "AdaptiveTableFactory"; }

  Status NewTableReader(const TableReaderOptions& table_reader_options,
                        unique_ptr<RandomAccessFileReader>&& file,
                        uint64_t file_size,
                        unique_ptr<TableReader>* table) const override;

  TableBuilder* NewTableBuilder(
      const TableBuilderOptions& table_builder_options,
      uint32_t column_family_id, WritableFileWriter* file) const override;

  // Sanitizes the specified DB Options.
  Status SanitizeOptions(const DBOptions& db_opts,
                         const ColumnFamilyOptions& cf_opts) const override {
    return Status::OK();
  }

  std::string GetPrintableTableOptions() const override;

  /********************** Shichao **********************/
  // not thread-safe
  void SetWriteTableFactory(
      std::shared_ptr<TableFactory> table_factory_to_write);

  // thread-safe
  void SetOutputLevel(
      const std::string& file_name, int output_level);

  int GetKnob() const { return knob_; }
  /********************** Shichao **********************/

 private:
  std::shared_ptr<TableFactory> table_factory_to_write_;
  std::shared_ptr<TableFactory> block_based_table_factory_;
  std::shared_ptr<TableFactory> plain_table_factory_;
  std::shared_ptr<TableFactory> cuckoo_table_factory_;
  std::shared_ptr<TableFactory> column_table_factory_;  // Shichao
  std::map<std::string, int> output_levels_;            // Shichao
  int knob_;                                            // Shichao
  std::unique_ptr<InstrumentedMutex> mutex_;            // Shichao
};

}  // namespace rocksdb
#endif  // ROCKSDB_LITE
