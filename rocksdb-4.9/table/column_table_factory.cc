#include "table/column_table_factory.h"

#include <memory>
#include <string>
#include <stdint.h>

#include "port/port.h"
#include "rocksdb/flush_block_policy.h"
#include "rocksdb/cache.h"
#include "table/column_table_builder.h"
#include "table/column_table_reader.h"
#include "table/format.h"

namespace rocksdb {

ColumnTableFactory::ColumnTableFactory(
    const ColumnTableOptions& _table_options)
    : table_options_(_table_options) {
  if (table_options_.flush_block_policy_factory == nullptr) {
    table_options_.flush_block_policy_factory.reset(
        new FlushBlockBySizePolicyFactory());
  }
  if (table_options_.no_block_cache) {
    table_options_.block_cache.reset();
  } else if (table_options_.block_cache == nullptr) {
    table_options_.block_cache = NewLRUCache(8 << 20);
  }
  if (table_options_.block_size_deviation < 0 ||
      table_options_.block_size_deviation > 100) {
    table_options_.block_size_deviation = 0;
  }
  if (table_options_.block_restart_interval < 1) {
    table_options_.block_restart_interval = 1;
  }
  if (table_options_.index_block_restart_interval < 1) {
    table_options_.index_block_restart_interval = 1;
  }
}

Status ColumnTableFactory::NewTableReader(
    const TableReaderOptions& table_reader_options,
    unique_ptr<RandomAccessFileReader>&& file, uint64_t file_size,
    unique_ptr<TableReader>* table_reader) const {
  return NewTableReader(table_reader_options, std::move(file), file_size,
                        table_reader,
                        /*prefetch_index=*/true);
}

Status ColumnTableFactory::NewTableReader(
    const TableReaderOptions& table_reader_options,
    unique_ptr<RandomAccessFileReader>&& file, uint64_t file_size,
    unique_ptr<TableReader>* table_reader, bool prefetch_index) const {
  return ColumnTable::Open(
      table_reader_options.ioptions, table_reader_options.env_options,
      table_options_, table_reader_options.internal_comparator, std::move(file),
      file_size, table_reader, prefetch_index, table_reader_options.level,
      table_reader_options.cols);
}

TableBuilder* ColumnTableFactory::NewTableBuilder(
    const TableBuilderOptions& table_builder_options, uint32_t column_family_id,
    WritableFileWriter* file) const {
  auto table_builder = new ColumnTableBuilder(
      table_builder_options.ioptions, table_options_,
      table_builder_options.internal_comparator,
      table_builder_options.int_tbl_prop_collector_factories, column_family_id,
      file, table_builder_options.compression_type,
      table_builder_options.compression_opts,
      table_builder_options.compression_dict,
      table_builder_options.column_family_name,
      table_builder_options.env_options);

  return table_builder;
}

Status ColumnTableFactory::SanitizeOptions(
    const DBOptions& db_opts,
    const ColumnFamilyOptions& cf_opts) const {
  if (table_options_.cache_index_blocks &&
      table_options_.no_block_cache) {
    return Status::InvalidArgument("Enable cache_index_blocks, "
        ", but block cache is disabled");
  }
  if (table_options_.pin_l0_index_blocks_in_cache &&
      table_options_.no_block_cache) {
    return Status::InvalidArgument(
        "Enable pin_l0_index_blocks_in_cache, "
        ", but block cache is disabled");
  }
  if (!BlockBasedTableSupportedVersion(table_options_.format_version)) {
    return Status::InvalidArgument(
        "Unsupported BlockBasedTable format_version. Please check "
        "include/rocksdb/table.h for more info");
  }
  return Status::OK();
}

std::string ColumnTableFactory::GetPrintableTableOptions() const {
  std::string ret;
  ret.reserve(20000);
  const int kBufferSize = 200;
  char buffer[kBufferSize];

  snprintf(buffer, kBufferSize, "  flush_block_policy_factory: %s (%p)\n",
           table_options_.flush_block_policy_factory->Name(),
           static_cast<void*>(table_options_.flush_block_policy_factory.get()));
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  cache_index_blocks: %d\n",
           table_options_.cache_index_blocks);
  ret.append(buffer);
  snprintf(buffer, kBufferSize,
           "  pin_l0_index_blocks_in_cache: %d\n",
           table_options_.pin_l0_index_blocks_in_cache);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  index_type: %d\n",
           table_options_.index_type);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  checksum: %d\n",
           table_options_.checksum);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  no_block_cache: %d\n",
           table_options_.no_block_cache);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  block_cache: %p\n",
           static_cast<void*>(table_options_.block_cache.get()));
  ret.append(buffer);
  if (table_options_.block_cache) {
    snprintf(buffer, kBufferSize, "  block_cache_size: %" ROCKSDB_PRIszt "\n",
             table_options_.block_cache->GetCapacity());
    ret.append(buffer);
  }
  snprintf(buffer, kBufferSize, "  block_size: %" ROCKSDB_PRIszt "\n",
           table_options_.block_size);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  block_size_deviation: %d\n",
           table_options_.block_size_deviation);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  block_restart_interval: %d\n",
           table_options_.block_restart_interval);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  index_block_restart_interval: %d\n",
           table_options_.index_block_restart_interval);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  skip_table_builder_flush: %d\n",
           table_options_.skip_table_builder_flush);
  ret.append(buffer);
  snprintf(buffer, kBufferSize, "  format_version: %d\n",
           table_options_.format_version);
  ret.append(buffer);
  return ret;
}

const ColumnTableOptions& ColumnTableFactory::table_options() const {
  return table_options_;
}

TableFactory* NewColumnTableFactory(
    const ColumnTableOptions& _table_options) {
  return new ColumnTableFactory(_table_options);
}

const std::string ColumnTablePropertyNames::kIndexType =
    "rocksdb.column.table.index.type";

}  // namespace rocksdb
