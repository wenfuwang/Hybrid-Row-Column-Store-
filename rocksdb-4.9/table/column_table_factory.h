#pragma once
#include <stdint.h>

#include <memory>
#include <string>

#include "rocksdb/flush_block_policy.h"
#include "rocksdb/table.h"
#include "db/dbformat.h"

namespace rocksdb {

using std::unique_ptr;

class ColumnTableFactory : public TableFactory {
 public:
  explicit ColumnTableFactory(
      const ColumnTableOptions& table_options = ColumnTableOptions());

  ~ColumnTableFactory() {}

  const char* Name() const override { return "ColumnTable"; }

  Status NewTableReader(const TableReaderOptions& table_reader_options,
                        unique_ptr<RandomAccessFileReader>&& file,
                        uint64_t file_size,
                        unique_ptr<TableReader>* table_reader) const override;

  // This is a variant of virtual member function NewTableReader function with
  // added capability to disable pre-fetching of blocks on ColumnTable::Open
  Status NewTableReader(const TableReaderOptions& table_reader_options,
                        unique_ptr<RandomAccessFileReader>&& file,
                        uint64_t file_size,
                        unique_ptr<TableReader>* table_reader,
                        bool prefetch_index) const;

  TableBuilder* NewTableBuilder(
      const TableBuilderOptions& table_builder_options,
      uint32_t column_family_id, WritableFileWriter* file) const override;

  // Sanitizes the specified DB Options.
  Status SanitizeOptions(const DBOptions& db_opts,
                         const ColumnFamilyOptions& cf_opts) const override;

  std::string GetPrintableTableOptions() const override;

  const ColumnTableOptions& table_options() const;

  void* GetOptions() override { return &table_options_; }

 private:
  ColumnTableOptions table_options_;
};

}  // namespace rocksdb
