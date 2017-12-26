#include "table/column_table_builder.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include <map>
#include <memory>
#include <string>
#include <utility>

#include "db/dbformat.h"
#include "db/filename.h"

#include "rocksdb/cache.h"
#include "rocksdb/comparator.h"
#include "rocksdb/env.h"
#include "rocksdb/flush_block_policy.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/table.h"

#include "table/block.h"
#include "table/column_table_reader.h"
#include "table/block_builder.h"
#include "table/column_table_factory.h"
#include "table/format.h"
#include "table/meta_blocks.h"
#include "table/table_builder.h"

#include "util/string_util.h"
#include "util/coding.h"
#include "util/compression.h"
#include "util/crc32c.h"
#include "util/stop_watch.h"
#include "util/xxhash.h"
#include "util/string_util.h"
#include "rocksdb/global.h"
namespace rocksdb {

typedef ColumnTableOptions::IndexType IndexType;
typedef ColumnTableBuilder::IndexBuilder IndexBuilder;

// The interface for building index.
// Instruction for adding a new concrete IndexBuilder:
//  1. Create a subclass instantiated from IndexBuilder.
//  2. Add a new entry associated with that subclass in TableOptions::IndexType.
//  3. Add a create function for the new subclass in CreateIndexBuilder.
// Note: we can devise more advanced design to simplify the process for adding
// new subclass, which will, on the other hand, increase the code complexity and
// catch unwanted attention from readers. Given that we won't add/change
// indexes frequently, it makes sense to just embrace a more straightforward
// design that just works.
class ColumnTableBuilder::IndexBuilder {
 public:
  // Index builder will construct a set of blocks which contain:
  //  1. One primary index block.
  struct IndexBlocks {
    Slice index_block_contents;
  };
  explicit IndexBuilder(const Comparator* comparator)
      : comparator_(comparator) {}

  virtual ~IndexBuilder() {}

  // Add a new index entry to index block.
  // To allow further optimization, we provide `last_key_in_current_block` and
  // `first_key_in_next_block`, based on which the specific implementation can
  // determine the best index key to be used for the index block.
  // @last_key_in_current_block: this parameter maybe overridden with the value
  //                             "substitute key".
  // @first_key_in_next_block: it will be nullptr if the entry being added is
  //                           the last one in the table
  //
  // REQUIRES: Finish() has not yet been called.
  virtual void AddIndexEntry(std::string* last_key_in_current_block,
                             const Slice* first_key_in_next_block,
                             const BlockHandle& block_handle) = 0;

  // This method will be called whenever a key is added. The subclasses may
  // override OnKeyAdded() if they need to collect additional information.
  virtual void OnKeyAdded(const Slice& key) {}

  // Inform the index builder that all entries has been written. Block builder
  // may therefore perform any operation required for block finalization.
  //
  // REQUIRES: Finish() has not yet been called.
  virtual Status Finish(IndexBlocks* index_blocks) = 0;

  // Get the estimated size for index block.
  virtual size_t EstimatedSize() const = 0;

 protected:
  const Comparator* comparator_;
};

// This index builder builds space-efficient index block.
//
// Optimizations:
//  1. Made block's `block_restart_interval` to be 1, which will avoid linear
//     search when doing index lookup (can be disabled by setting
//     index_block_restart_interval).
//  2. Shorten the key length for index block. Other than honestly using the
//     last key in the data block as the index key, we instead find a shortest
//     substitute key that serves the same function.
class ShortenedIndexBuilder : public IndexBuilder {
 public:
  explicit ShortenedIndexBuilder(const Comparator* comparator,
                                 int index_block_restart_interval)
      : IndexBuilder(comparator),
        index_block_builder_(index_block_restart_interval) {}

  virtual void AddIndexEntry(std::string* last_key_in_current_block,
                             const Slice* first_key_in_next_block,
                             const BlockHandle& block_handle) override {
    if (first_key_in_next_block != nullptr) {
      comparator_->FindShortestSeparator(last_key_in_current_block,
                                         *first_key_in_next_block);
    } else {
      comparator_->FindShortSuccessor(last_key_in_current_block);
    }

    std::string handle_encoding;
    block_handle.EncodeTo(&handle_encoding);
    index_block_builder_.Add(*last_key_in_current_block, handle_encoding);
  }

  virtual Status Finish(IndexBlocks* index_blocks) override {
    index_blocks->index_block_contents = index_block_builder_.Finish();
    return Status::OK();
  }

  virtual size_t EstimatedSize() const override {
    return index_block_builder_.CurrentSizeEstimate();
  }

 private:
  BlockBuilder index_block_builder_;
};

// Without anonymous namespace here, we fail the warning -Wmissing-prototypes
namespace {

// Create a index builder based on its type.
IndexBuilder* CreateIndexBuilder(IndexType type, const Comparator* comparator,
                                 int index_block_restart_interval) {
  switch (type) {
    case ColumnTableOptions::kBinarySearch: {
      return new ShortenedIndexBuilder(comparator,
                                       index_block_restart_interval);
    }
    default: {
      assert(!"Do not recognize the index type ");
      return nullptr;
    }
  }
  // impossible.
  assert(false);
  return nullptr;
}

bool GoodCompressionRatio(size_t compressed_size, size_t raw_size) {
  // Check to see if compressed less than 12.5%
  return compressed_size < raw_size - (raw_size / 8u);
}

// format_version is the block format as defined in include/rocksdb/table.h
Slice CompressBlock(const Slice& raw,
                    const CompressionOptions& compression_options,
                    CompressionType* type, uint32_t format_version,
                    const Slice& compression_dict,
                    std::string* compressed_output) {
  if (*type == kNoCompression) {
    return raw;
  }

  // Will return compressed block contents if (1) the compression method is
  // supported in this platform and (2) the compression rate is "good enough".
  switch (*type) {
    case kSnappyCompression:
      if (Snappy_Compress(compression_options, raw.data(), raw.size(),
                          compressed_output) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;  // fall back to no compression.
    case kZlibCompression:
      if (Zlib_Compress(
              compression_options,
              GetCompressFormatForVersion(kZlibCompression, format_version),
              raw.data(), raw.size(), compressed_output, compression_dict) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;  // fall back to no compression.
    case kBZip2Compression:
      if (BZip2_Compress(
              compression_options,
              GetCompressFormatForVersion(kBZip2Compression, format_version),
              raw.data(), raw.size(), compressed_output) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;  // fall back to no compression.
    case kLZ4Compression:
      if (LZ4_Compress(
              compression_options,
              GetCompressFormatForVersion(kLZ4Compression, format_version),
              raw.data(), raw.size(), compressed_output, compression_dict) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;  // fall back to no compression.
    case kLZ4HCCompression:
      if (LZ4HC_Compress(
              compression_options,
              GetCompressFormatForVersion(kLZ4HCCompression, format_version),
              raw.data(), raw.size(), compressed_output, compression_dict) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;     // fall back to no compression.
    case kXpressCompression:
      if (XPRESS_Compress(raw.data(), raw.size(),
          compressed_output) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;
    case kZSTDNotFinalCompression:
      if (ZSTD_Compress(compression_options, raw.data(), raw.size(),
                        compressed_output, compression_dict) &&
          GoodCompressionRatio(compressed_output->size(), raw.size())) {
        return *compressed_output;
      }
      break;     // fall back to no compression.
    default: {}  // Do not recognize this compression type
  }

  // Compression method is not supported, or not good compression ratio, so just
  // fall back to uncompressed form.
  *type = kNoCompression;
  return raw;
}

}  // namespace

// Slight change from kBlockBasedTableMagicNumber.
// Please note that kColumnTableMagicNumber may also be accessed by other
// .cc files
// for that reason we declare it extern in the header but to get the space
// allocated
// it must be not extern in one place.
const uint64_t kColumnTableMagicNumber = 0x88e241b785f4cfffull;

// A collector that collects properties of interest to column table.
// For now this class looks heavy-weight since we only write one additional
// property.
// But in the foreseeable future, we will add more and more properties that are
// specific to column table.
class ColumnTableBuilder::ColumnTablePropertiesCollector
    : public IntTblPropCollector {
 public:
  explicit ColumnTablePropertiesCollector(
	  ColumnTableOptions::IndexType index_type)
      : index_type_(index_type) {}

  virtual Status InternalAdd(const Slice& key, const Slice& value,
                             uint64_t file_size) override {
    // Intentionally left blank. Have no interest in collecting stats for
    // individual key/value pairs.
    return Status::OK();
  }

  virtual Status Finish(UserCollectedProperties* properties) override {
    std::string val;
    PutFixed32(&val, static_cast<uint32_t>(index_type_));
    properties->insert({ColumnTablePropertyNames::kIndexType, val});
    return Status::OK();
  }

  // The name of the properties collector can be used for debugging purpose.
  virtual const char* Name() const override {
    return "ColumnTablePropertiesCollector";
  }

  virtual UserCollectedProperties GetReadableProperties() const override {
    // Intentionally left blank.
    return UserCollectedProperties();
  }

 private:
  ColumnTableOptions::IndexType index_type_;
};

struct ColumnTableBuilder::Rep {
  const ImmutableCFOptions ioptions;
  const ColumnTableOptions table_options;
  const InternalKeyComparator& internal_comparator;
  WritableFileWriter* file;
  uint64_t offset = 0;
  Status status;
  BlockBuilder data_block;

  std::unique_ptr<IndexBuilder> index_builder;

  std::string last_key;
  const CompressionType compression_type;
  const CompressionOptions compression_opts;
  // Data for presetting the compression library's dictionary, or nullptr.
  const std::string* compression_dict;
  TableProperties props;

  bool closed = false;  // Either Finish() or Abandon() has been called.

  BlockHandle pending_handle;  // Handle to add to index block

  std::string compressed_output;
  std::unique_ptr<FlushBlockPolicy> flush_block_policy;
  uint32_t column_family_id;
  const std::string& column_family_name;

  std::vector<std::unique_ptr<IntTblPropCollector>> table_properties_collectors;

  const EnvOptions& env_options;
  bool main_column;
  std::vector<std::unique_ptr<ColumnTableBuilder>> builders;

  Rep(const ImmutableCFOptions& _ioptions,
      const ColumnTableOptions& table_opt,
      const InternalKeyComparator& icomparator,
      const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*
          int_tbl_prop_collector_factories,
      uint32_t _column_family_id, WritableFileWriter* f,
      const CompressionType _compression_type,
      const CompressionOptions& _compression_opts,
      const std::string* _compression_dict,
      const std::string& _column_family_name,
      const EnvOptions& _env_options,
      bool _main_column)
      : ioptions(_ioptions),
        table_options(table_opt),
        internal_comparator(icomparator),
        file(f),
        data_block(table_options.block_restart_interval,
                   table_options.use_delta_encoding, true),
        index_builder(
            CreateIndexBuilder(table_options.index_type, &internal_comparator,
                               table_options.index_block_restart_interval)),
        compression_type(_compression_type),
        compression_opts(_compression_opts),
        compression_dict(_compression_dict),
        flush_block_policy(
            table_options.flush_block_policy_factory->NewFlushBlockPolicy(
                table_options, data_block)),
        column_family_id(_column_family_id),
        column_family_name(_column_family_name),
        env_options(_env_options),
        main_column(_main_column) {
	if (main_column && int_tbl_prop_collector_factories) {
      for (auto& collector_factories : *int_tbl_prop_collector_factories) {
        table_properties_collectors.emplace_back(
            collector_factories->CreateIntTblPropCollector(column_family_id));
      }
	}
    table_properties_collectors.emplace_back(
        new ColumnTablePropertiesCollector(table_options.index_type));
  }

  ~Rep() {
    for (const auto& it : builders) {
      if (it && !it->rep_->closed) {
        it->rep_->file->Sync(ioptions.use_fsync);
        it->rep_->file->Close();
      }
    }
  }
};

ColumnTableBuilder::ColumnTableBuilder(
    const ImmutableCFOptions& ioptions,
    const ColumnTableOptions& table_options,
    const InternalKeyComparator& internal_comparator,
    const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*
        int_tbl_prop_collector_factories,
    uint32_t column_family_id, WritableFileWriter* file,
    const CompressionType compression_type,
    const CompressionOptions& compression_opts,
    const std::string* compression_dict,
    const std::string& column_family_name,
    const EnvOptions& env_options,
    bool main_column) {
  ColumnTableOptions sanitized_table_options(table_options);
  if (sanitized_table_options.format_version == 0 &&
      sanitized_table_options.checksum != kCRC32c) {
    Log(InfoLogLevel::WARN_LEVEL, ioptions.info_log,
        "Silently converting format_version to 1 because checksum is "
        "non-default");
    // silently convert format_version to 1 to keep consistent with current
    // behavior
    sanitized_table_options.format_version = 1;
  }

  rep_ = new Rep(ioptions, sanitized_table_options, internal_comparator,
                 int_tbl_prop_collector_factories, column_family_id, file,
                 compression_type, compression_opts, compression_dict,
                 column_family_name, env_options, main_column);
}

ColumnTableBuilder::~ColumnTableBuilder() {
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  delete rep_;
}

//void ColumnTableBuilder::Add(const Slice& key, const Slice& value) {
//  Rep* r = rep_;
//  assert(!r->closed);
//  if (!ok()) return;
//  if (r->props.num_entries > 0) {
//    assert(r->internal_comparator.Compare(key, Slice(r->last_key)) > 0);
//  }
//
//  t11 = steady_clock::now();
//  std::vector<std::string> vals(StringSplit(value.ToString(), r->table_options.delim));
//  if (!vals.empty() && vals.size() != r->table_options.column_num) {
//    r->status = Status::InvalidArgument("table_options.column_num");
//    return;
//  }
//  t12 = steady_clock::now();
//  diff1 += t12 - t11;
//
//  if (r->builders.empty()) {
//    r->builders.resize(r->table_options.column_num);
//    std::string fname = r->file->writable_file()->GetFileName();
//    Env::IOPriority pri = r->file->writable_file()->GetIOPriority();
//    for (auto i = 0u; i < r->table_options.column_num; i++) {
//      unique_ptr<WritableFile> file;
//      std::string col_fname(TableSubFileName(fname, i+1));
//      r->status = NewWritableFile(r->ioptions.env, col_fname, &file, r->env_options);
//      assert(r->status.ok());
//      file->SetIOPriority(pri);
//      r->builders[i].reset(new ColumnTableBuilder(r->ioptions, r->table_options,
//          r->internal_comparator, nullptr, r->column_family_id,
//          new WritableFileWriter(std::move(file), r->env_options),
//          r->compression_type, r->compression_opts, r->compression_dict,
//          r->column_family_name, r->env_options, false));
//    }
//  }
//
//  std::string val;
//  PutVarint64(&val, r->props.num_entries++);
//  auto should_flush = r->flush_block_policy->Update(key, val);
//  if (should_flush) {
//    assert(!r->data_block.empty());
//    Flush();
//
//    // Add item to index block.
//    // We do not emit the index entry for a block until we have seen the
//    // first key for the next data block.  This allows us to use shorter
//    // keys in the index block.  For example, consider a block boundary
//    // between the keys "the quick brown fox" and "the who".  We can use
//    // "the r" as the key for the index block entry since it is >= all
//    // entries in the first block and < all entries in subsequent
//    // blocks.
//    if (ok()) {
//      r->index_builder->AddIndexEntry(&r->last_key, &key, r->pending_handle);
//    }
//  }
//
//  t13 = steady_clock::now();
//  for (auto i = 0u; i < r->table_options.column_num; i++) {
//    const auto& rep = r->builders[i]->rep_;
//    if (rep->flush_block_policy->Update(vals.empty()? Slice(): vals[i], key)) {
//      assert(!rep->data_block.empty());
//      r->builders[i]->Flush();
//      if (r->builders[i]->ok()) {
//        rep->index_builder->AddIndexEntry(&rep->last_key, &key, rep->pending_handle);
//      }
//    }
//  }
//  t14 = steady_clock::now();
//  diff2 += t14 - t13;
//
//  r->last_key.assign(key.data(), key.size());
//  r->data_block.Add(key, val);
//  r->props.raw_key_size += key.size();
//  r->props.raw_value_size += value.size();
//  r->index_builder->OnKeyAdded(key);
//  NotifyCollectTableCollectorsOnAdd(key, value, r->offset,
//                                    r->table_properties_collectors,
//                                    r->ioptions.info_log);
//
//  t15 = steady_clock::now();
//  for (auto i = 0u; i < r->table_options.column_num; i++) {
//	const auto& rep = r->builders[i]->rep_;
//    rep->last_key.assign(key.data(), key.size());
//    rep->data_block.Add(vals.empty()? Slice(): vals[i], key);
//    rep->props.num_entries++;
//    rep->props.raw_key_size += vals.empty()? 0: vals[i].size();
//    rep->props.raw_value_size += key.size();
//    rep->index_builder->OnKeyAdded(key);
//  }
//  t16 = steady_clock::now();
//  diff3 += t16 - t15;
//}

void ColumnTableBuilder::Add(const Slice& key, const Slice& value) {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->props.num_entries > 0) {
    assert(r->internal_comparator.Compare(key, Slice(r->last_key)) > 0);
  }

  std::vector<std::string> vals(StringSplit(value.ToString(), r->table_options.delim));
  if (!vals.empty() && vals.size() != r->table_options.column_num) {
    r->status = Status::InvalidArgument("table_options.column_num");
    return;
  }

  if (r->builders.empty()) {
    r->builders.resize(r->table_options.column_num);
    std::string fname = r->file->writable_file()->GetFileName();
    Env::IOPriority pri = r->file->writable_file()->GetIOPriority();
    for (auto i = 0u; i < r->table_options.column_num; i++) {
      unique_ptr<WritableFile> file;
      std::string col_fname(TableSubFileName(fname, i+1));
      r->status = NewWritableFile(r->ioptions.env, col_fname, &file, r->env_options);
      assert(r->status.ok());
      file->SetIOPriority(pri);
      r->builders[i].reset(new ColumnTableBuilder(r->ioptions, r->table_options,
          r->internal_comparator, nullptr, r->column_family_id,
          new WritableFileWriter(std::move(file), r->env_options),
          r->compression_type, r->compression_opts, r->compression_dict,
          r->column_family_name, r->env_options, false));
    }
  }

  std::string val;
  PutVarint64(&val, r->props.num_entries++);

  int column_num = r->table_options.column_num;

  t15 = steady_clock::now();
  #pragma omp parallel for
  for (int i=-1; i<column_num; ++i) {
      if (i == -1) {
        ParallelAdd(this, key, val, false);
      } else {
        ParallelAdd(r->builders[i].get(), key, vals.empty()? "": vals[i], true);
      }
  }
  t16 = steady_clock::now();
  diff3 += t16 - t15;
}

void ColumnTableBuilder::ParallelAdd(ColumnTableBuilder* builders,
    const Slice& key, const std::string& val, bool reverse) {
  auto rep = builders->rep_;
  if (reverse) {
    if (rep->flush_block_policy->Update(val.empty()? Slice(): val, key)) {
      assert(!rep->data_block.empty());
      builders->Flush();
      if (builders->ok()) {
        rep->index_builder->AddIndexEntry(&rep->last_key, &key, rep->pending_handle);
      }
    }

    rep->last_key.assign(key.data(), key.size());
    rep->data_block.Add(val.empty()? Slice(): val, key);
    rep->props.num_entries++;
    rep->props.raw_key_size += val.empty()? 0: val.size();
    rep->props.raw_value_size += key.size();
    rep->index_builder->OnKeyAdded(key);
  } else {
    if (rep->flush_block_policy->Update(key, val)) {
      assert(!rep->data_block.empty());
      builders->Flush();
      if (builders->ok()) {
        rep->index_builder->AddIndexEntry(&rep->last_key, &key, rep->pending_handle);
      }
    }

    rep->last_key.assign(key.data(), key.size());
    rep->data_block.Add(key, val);
    rep->props.raw_key_size += key.size();
    rep->props.raw_value_size += val.size();
    rep->index_builder->OnKeyAdded(key);
    NotifyCollectTableCollectorsOnAdd(key, val, rep->offset,
                                      rep->table_properties_collectors,
                                      rep->ioptions.info_log);
  }
}

void ColumnTableBuilder::Flush() {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->data_block.empty()) return;
  WriteBlock(&r->data_block, &r->pending_handle, true /* is_data_block */);
  if (ok() && !r->table_options.skip_table_builder_flush) {
    r->status = r->file->Flush();
  }
  r->props.data_size = r->offset;
  ++r->props.num_data_blocks;
}

void ColumnTableBuilder::WriteBlock(BlockBuilder* block,
                                    BlockHandle* handle,
                                    bool is_data_block) {
  WriteBlock(block->Finish(), handle, is_data_block);
  block->Reset();
}

void ColumnTableBuilder::WriteBlock(const Slice& raw_block_contents,
                                    BlockHandle* handle,
                                    bool is_data_block) {
  // File format contains a sequence of blocks where each block has:
  //    block_data: uint8[n]
  //    type: uint8
  //    crc: uint32
  assert(ok());
  Rep* r = rep_;

  auto type = r->compression_type;
  Slice block_contents;
  if (raw_block_contents.size() < kCompressionSizeLimit) {
    Slice compression_dict;
    if (is_data_block && r->compression_dict && r->compression_dict->size()) {
      compression_dict = *r->compression_dict;
    }
    block_contents = CompressBlock(raw_block_contents, r->compression_opts,
                                   &type, r->table_options.format_version,
                                   compression_dict, &r->compressed_output);
  } else {
    RecordTick(r->ioptions.statistics, NUMBER_BLOCK_NOT_COMPRESSED);
    type = kNoCompression;
    block_contents = raw_block_contents;
  }
  WriteRawBlock(block_contents, type, handle);
  r->compressed_output.clear();
}

void ColumnTableBuilder::WriteRawBlock(const Slice& block_contents,
                                       CompressionType type,
                                       BlockHandle* handle) {
  Rep* r = rep_;
  StopWatch sw(r->ioptions.env, r->ioptions.statistics, WRITE_RAW_BLOCK_MICROS);
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());
  r->status = r->file->Append(block_contents);
  if (r->status.ok()) {
    char trailer[kBlockTrailerSize];
    trailer[0] = type;
    char* trailer_without_type = trailer + 1;
    switch (r->table_options.checksum) {
      case kNoChecksum:
        // we don't support no checksum yet
        assert(false);
        // intentional fallthrough in release binary
      case kCRC32c: {
        auto crc = crc32c::Value(block_contents.data(), block_contents.size());
        crc = crc32c::Extend(crc, trailer, 1);  // Extend to cover block type
        EncodeFixed32(trailer_without_type, crc32c::Mask(crc));
        break;
      }
      case kxxHash: {
        void* xxh = XXH32_init(0);
        XXH32_update(xxh, block_contents.data(),
                     static_cast<uint32_t>(block_contents.size()));
        XXH32_update(xxh, trailer, 1);  // Extend  to cover block type
        EncodeFixed32(trailer_without_type, XXH32_digest(xxh));
        break;
      }
    }

    r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));
    if (r->status.ok()) {
      r->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
}

Status ColumnTableBuilder::status() const {
  for (const auto& it : rep_->builders) {
    if (it && !it->rep_->status.ok()) {
	  return it->rep_->status;
    }
  }
  return rep_->status;
}

Status ColumnTableBuilder::Finish() {
  Rep* r = rep_;
  if (r->main_column) {
    for (const auto& it : r->builders) {
      if (it) {
        it->rep_->status = it->Finish();
        if (!it->rep_->status.ok()) {
          return it->rep_->status;
        }
      }
    }
  }

  bool empty_data_block = r->data_block.empty();
  Flush();
  assert(!r->closed);
  r->closed = true;

  BlockHandle metaindex_block_handle, index_block_handle,
      compression_dict_block_handle;

  // To make sure properties block is able to keep the accurate size of index
  // block, we will finish writing all index entries here and flush them
  // to storage after metaindex block is written.
  if (ok() && !empty_data_block) {
    r->index_builder->AddIndexEntry(
        &r->last_key, nullptr /* no next data block */, r->pending_handle);
  }

  IndexBuilder::IndexBlocks index_blocks;
  auto s = r->index_builder->Finish(&index_blocks);
  if (!s.ok()) {
    return s;
  }

  // Write meta blocks and metaindex block with the following order.
  //    1. [meta block: format, col_num; col_file_size...]
  //    1. [meta block: properties]
  //    2. [metaindex block]
  // write meta blocks
  MetaIndexBuilder meta_index_builder;

  if (ok()) {
	// Write column block.
	{
      ColumnBlockBuilder column_block_builder;
      uint32_t column_num = r->builders.size();
      column_block_builder.Add(r->main_column, column_num);
	  for (auto i = 0u; i < column_num; i++) {
	    column_block_builder.Add(i+1, r->builders[i]->rep_->offset);
	  }

	  BlockHandle column_block_handle;
	  WriteRawBlock(column_block_builder.Finish(), kNoCompression,
			        &column_block_handle);
	  meta_index_builder.Add(kColumnBlock, column_block_handle);
	}

    // Write properties and compression dictionary blocks.
    {
      PropertyBlockBuilder property_block_builder;
      r->props.column_family_id = r->column_family_id;
      r->props.column_family_name = r->column_family_name;
      r->props.index_size =
          r->index_builder->EstimatedSize() + kBlockTrailerSize;
      r->props.comparator_name = r->ioptions.comparator != nullptr
                                     ? r->ioptions.comparator->Name()
                                     : "nullptr";
      r->props.merge_operator_name = r->ioptions.merge_operator != nullptr
                                         ? r->ioptions.merge_operator->Name()
                                         : "nullptr";
      r->props.compression_name = CompressionTypeToString(r->compression_type);

      if (r->main_column) {
        std::string property_collectors_names = "[";
        for (size_t i = 0;
             i < r->ioptions.table_properties_collector_factories.size(); ++i) {
          if (i != 0) {
            property_collectors_names += ",";
          }
          property_collectors_names +=
              r->ioptions.table_properties_collector_factories[i]->Name();
        }
        property_collectors_names += "]";
        r->props.property_collectors_names = property_collectors_names;
      }

      // Add basic properties
      property_block_builder.AddTableProperty(r->props);

      // Add use collected properties
      NotifyCollectTableCollectorsOnFinish(r->table_properties_collectors,
                                           r->ioptions.info_log,
                                           &property_block_builder);

      BlockHandle properties_block_handle;
      WriteRawBlock(property_block_builder.Finish(), kNoCompression,
                    &properties_block_handle);
      meta_index_builder.Add(kPropertiesBlock, properties_block_handle);

      // Write compression dictionary block
      if (r->compression_dict && r->compression_dict->size()) {
        WriteRawBlock(*r->compression_dict, kNoCompression,
                      &compression_dict_block_handle);
        meta_index_builder.Add(kCompressionDictBlock,
                               compression_dict_block_handle);
      }
    }  // end of properties/compression dictionary block writing
  }    // meta blocks

  // Write index block
  if (ok()) {
    // flush the meta index block
    WriteRawBlock(meta_index_builder.Finish(), kNoCompression,
                  &metaindex_block_handle);
    WriteBlock(index_blocks.index_block_contents, &index_block_handle,
               false /* is_data_block */);
  }

  // Write footer
  if (ok()) {
    // No need to write out new footer if we're using default checksum.
    assert(r->table_options.checksum == kCRC32c ||
           r->table_options.format_version != 0);
    Footer footer(kColumnTableMagicNumber,
                  r->table_options.format_version);
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);
    footer.set_checksum(r->table_options.checksum);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    r->status = r->file->Append(footer_encoding);
    if (r->status.ok()) {
      r->offset += footer_encoding.size();
    }
  }

  if (r->main_column) {
    for (const auto& it : r->builders) {
      if (it && it->rep_->status.ok()) {
        it->rep_->file->Sync(r->ioptions.use_fsync);
        it->rep_->file->Close();
      }
    }
  }

  return r->status;
}

void ColumnTableBuilder::Abandon() {
  Rep* r = rep_;
  for (const auto& it : r->builders) {
    if (it) {
      assert(!it->rep_->closed);
      it->rep_->closed = true;
    }
  }
  assert(!r->closed);
  r->closed = true;
}

uint64_t ColumnTableBuilder::NumEntries() const {
  return rep_->props.num_entries;
}

uint64_t ColumnTableBuilder::FileSize() const {
  return rep_->offset;
}

uint64_t ColumnTableBuilder::FileSizeTotal() const {
  uint64_t res = rep_->offset;
  for (const auto& it : rep_->builders) {
    if (it) {
      res += it->rep_->offset;
    }
  }
  return res / rep_->table_options.denominator;
}

bool ColumnTableBuilder::NeedCompact() const {
  for (const auto& collector : rep_->table_properties_collectors) {
    if (collector->NeedCompact()) {
      return true;
    }
  }
  return false;
}

TableProperties ColumnTableBuilder::GetTableProperties() const {
  TableProperties ret = rep_->props;
  for (const auto& collector : rep_->table_properties_collectors) {
    for (const auto& prop : collector->GetReadableProperties()) {
      ret.readable_properties.insert(prop);
    }
    collector->Finish(&ret.user_collected_properties);
  }
  return ret;
}

}  // namespace rocksdb
