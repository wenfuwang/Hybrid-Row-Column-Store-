#include "table/column_table_reader.h"

#include <string>
#include <utility>
#include "db/dbformat.h"
#include "db/filename.h"

#include "rocksdb/cache.h"
#include "rocksdb/comparator.h"
#include "rocksdb/env.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/table_properties.h"

#include "table/block.h"
#include "table/column_table_factory.h"
#include "table/block_prefix_index.h"
#include "table/format.h"
#include "table/get_context.h"
#include "table/internal_iterator.h"
#include "table/meta_blocks.h"
#include "table/persistent_cache_helper.h"
#include "table/two_level_iterator.h"

#include "util/coding.h"
#include "util/file_reader_writer.h"
#include "util/perf_context_imp.h"
#include "util/stop_watch.h"
#include "util/string_util.h"

namespace rocksdb {

extern const uint64_t kColumnTableMagicNumber;
using std::unique_ptr;

typedef ColumnTable::IndexReader IndexReader;

namespace {
// Read the block identified by "handle" from "file".
// The only relevant option is options.verify_checksums for now.
// On failure return non-OK.
// On success fill *result and return OK - caller owns *result
// @param compression_dict Data for presetting the compression library's
//    dictionary.
Status ReadBlockFromFile(RandomAccessFileReader* file, const Footer& footer,
                         const ReadOptions& options, const BlockHandle& handle,
                         std::unique_ptr<Block>* result, Env* env,
                         bool do_uncompress, const Slice& compression_dict,
                         const PersistentCacheOptions& cache_options,
                         Logger* info_log) {
  BlockContents contents;
  Status s = ReadBlockContents(file, footer, options, handle, &contents, env,
                               do_uncompress, compression_dict, cache_options,
                               info_log);
  if (s.ok()) {
    result->reset(new Block(std::move(contents)));
  }

  return s;
}

// Delete the resource that is held by the iterator.
template <class ResourceType>
void DeleteHeldResource(void* arg, void* ignored) {
  delete reinterpret_cast<ResourceType*>(arg);
}

// Delete the entry resided in the cache.
template <class Entry>
void DeleteCachedEntry(const Slice& key, void* value) {
  auto entry = reinterpret_cast<Entry*>(value);
  delete entry;
}

void DeleteCachedIndexEntry(const Slice& key, void* value);

// Release the cached entry and decrement its ref count.
void ReleaseCachedEntry(void* arg, void* h) {
  Cache* cache = reinterpret_cast<Cache*>(arg);
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  cache->Release(handle);
}

Slice GetCacheKeyFromOffset(const char* cache_key_prefix,
                            size_t cache_key_prefix_size, uint64_t offset,
                            char* cache_key) {
  assert(cache_key != nullptr);
  assert(cache_key_prefix_size != 0);
  assert(cache_key_prefix_size <= ColumnTable::kMaxCacheKeyPrefixSize);
  memcpy(cache_key, cache_key_prefix, cache_key_prefix_size);
  char* end = EncodeVarint64(cache_key + cache_key_prefix_size, offset);
  return Slice(cache_key, static_cast<size_t>(end - cache_key));
}

Cache::Handle* GetEntryFromCache(Cache* block_cache, const Slice& key,
                                 Tickers block_cache_miss_ticker,
                                 Tickers block_cache_hit_ticker,
                                 Statistics* statistics) {
  auto cache_handle = block_cache->Lookup(key);
  if (cache_handle != nullptr) {
    PERF_COUNTER_ADD(block_cache_hit_count, 1);
    // overall cache hit
    RecordTick(statistics, BLOCK_CACHE_HIT);
    // total bytes read from cache
    RecordTick(statistics, BLOCK_CACHE_BYTES_READ,
               block_cache->GetUsage(cache_handle));
    // block-type specific cache hit
    RecordTick(statistics, block_cache_hit_ticker);
  } else {
    // overall cache miss
    RecordTick(statistics, BLOCK_CACHE_MISS);
    // block-type specific cache miss
    RecordTick(statistics, block_cache_miss_ticker);
  }

  return cache_handle;
}

}  // namespace

// -- IndexReader and its subclasses
// IndexReader is the interface that provide the functionality for index access.
class ColumnTable::IndexReader {
 public:
  explicit IndexReader(const Comparator* comparator, Statistics* stats)
      : comparator_(comparator), statistics_(stats) {}

  virtual ~IndexReader() {}

  // Create an iterator for index access.
  // An iter is passed in, if it is not null, update this one and return it
  // If it is null, create a new Iterator
  virtual InternalIterator* NewIterator(BlockIter* iter = nullptr,
                                        bool total_order_seek = true) = 0;

  // The size of the index.
  virtual size_t size() const = 0;
  // Memory usage of the index block
  virtual size_t usable_size() const = 0;
  // return the statistics pointer
  virtual Statistics* statistics() const { return statistics_; }
  // Report an approximation of how much memory has been used other than memory
  // that was allocated in block cache.
  virtual size_t ApproximateMemoryUsage() const = 0;

 protected:
  const Comparator* comparator_;

 private:
  Statistics* statistics_;
};

// Index that allows binary search lookup for the first key of each block.
// This class can be viewed as a thin wrapper for `Block` class which already
// supports binary search.
class BinarySearchIndexReader : public IndexReader {
 public:
  // Read index from the file and create an instance for
  // `BinarySearchIndexReader`.
  // On success, index_reader will be populated; otherwise it will remain
  // unmodified.
  static Status Create(RandomAccessFileReader* file, const Footer& footer,
                       const BlockHandle& index_handle, Env* env,
                       const Comparator* comparator, IndexReader** index_reader,
                       const PersistentCacheOptions& cache_options,
                       Statistics* statistics) {
    std::unique_ptr<Block> index_block;
    auto s = ReadBlockFromFile(file, footer, ReadOptions(), index_handle,
                               &index_block, env, true /* decompress */,
                               Slice() /*compression dict*/, cache_options,
                               /*info_log*/ nullptr);

    if (s.ok()) {
      *index_reader = new BinarySearchIndexReader(
          comparator, std::move(index_block), statistics);
    }

    return s;
  }

  virtual InternalIterator* NewIterator(BlockIter* iter = nullptr,
                                        bool dont_care = true) override {
    return index_block_->NewIterator(comparator_, iter, true);
  }

  virtual size_t size() const override { return index_block_->size(); }
  virtual size_t usable_size() const override {
    return index_block_->usable_size();
  }

  virtual size_t ApproximateMemoryUsage() const override {
    assert(index_block_);
    return index_block_->ApproximateMemoryUsage();
  }

 private:
  BinarySearchIndexReader(const Comparator* comparator,
                          std::unique_ptr<Block>&& index_block,
                          Statistics* stats)
      : IndexReader(comparator, stats), index_block_(std::move(index_block)) {
    assert(index_block_ != nullptr);
  }
  std::unique_ptr<Block> index_block_;
};

// CachableEntry represents the entries that *may* be fetched from block cache.
//  field `value` is the item we want to get.
//  field `cache_handle` is the cache handle to the block cache. If the value
//  was not read from cache, `cache_handle` will be nullptr.
template <class TValue>
struct ColumnTable::CachableEntry {
  CachableEntry(TValue* _value, Cache::Handle* _cache_handle)
      : value(_value), cache_handle(_cache_handle) {}
  CachableEntry() : CachableEntry(nullptr, nullptr) {}
  void Release(Cache* cache) {
    if (cache_handle) {
      cache->Release(cache_handle);
      value = nullptr;
      cache_handle = nullptr;
    }
  }
  bool IsSet() const { return cache_handle != nullptr; }

  TValue* value = nullptr;
  // if the entry is from the cache, cache_handle will be populated.
  Cache::Handle* cache_handle = nullptr;
};

struct ColumnTable::Rep {
  Rep(const ImmutableCFOptions& _ioptions, const EnvOptions& _env_options,
      const ColumnTableOptions& _table_opt,
      const InternalKeyComparator& _internal_comparator)
      : ioptions(_ioptions),
        env_options(_env_options),
        table_options(_table_opt),
        internal_comparator(_internal_comparator) {}

  const ImmutableCFOptions& ioptions;
  const EnvOptions& env_options;
  const ColumnTableOptions& table_options;
  const InternalKeyComparator& internal_comparator;
  Status status;
  unique_ptr<RandomAccessFileReader> file;
  char cache_key_prefix[kMaxCacheKeyPrefixSize];
  size_t cache_key_prefix_size = 0;
  char persistent_cache_key_prefix[kMaxCacheKeyPrefixSize];
  size_t persistent_cache_key_prefix_size = 0;
  uint64_t dummy_index_reader_offset =
      0;  // ID that is unique for the block cache.
  PersistentCacheOptions persistent_cache_options;

  // Footer contains the fixed table information
  Footer footer;
  // index_reader will be populated and used only when
  // options.block_cache is nullptr; otherwise we will get the index block via
  // the block cache.
  unique_ptr<IndexReader> index_reader;

  std::shared_ptr<const TableProperties> table_properties;
  // Block containing the data for the compression dictionary. We take ownership
  // for the entire block struct, even though we only use its Slice member. This
  // is easier because the Slice member depends on the continued existence of
  // another member ("allocation").
  std::unique_ptr<const BlockContents> compression_dict_block;
  ColumnTableOptions::IndexType index_type;

  // only used in level 0 files:
  // when pin_l0_index_blocks_in_cache is true, we do use the
  // LRU cache, but we always keep the idndex block's handle checked
  // out here (=we don't call Release()), plus the parsed out objects
  // the LRU cache will never push flush them out, hence they're pinned
  CachableEntry<IndexReader> index_entry;

  bool main_column;
  std::vector<unique_ptr<ColumnTable>> tables;
};

ColumnTable::~ColumnTable() {
  if (rep_->main_column) {
    Close();
  }
  delete rep_;
}

// Helper function to setup the cache key's prefix for the Table.
void ColumnTable::SetupCacheKeyPrefix(Rep* rep, uint64_t file_size) {
  assert(kMaxCacheKeyPrefixSize >= 10);
  rep->cache_key_prefix_size = 0;
  if (rep->table_options.block_cache != nullptr) {
    GenerateCachePrefix(rep->table_options.block_cache.get(), rep->file->file(),
                        &rep->cache_key_prefix[0], &rep->cache_key_prefix_size);
    // Create dummy offset of index reader which is beyond the file size.
    rep->dummy_index_reader_offset =
        file_size + rep->table_options.block_cache->NewId();
  }
  if (rep->table_options.persistent_cache != nullptr) {
    GenerateCachePrefix(/*cache=*/nullptr, rep->file->file(),
                        &rep->persistent_cache_key_prefix[0],
                        &rep->persistent_cache_key_prefix_size);
  }
}

void ColumnTable::GenerateCachePrefix(Cache* cc,
    RandomAccessFile* file, char* buffer, size_t* size) {
  // generate an id from the file
  *size = file->GetUniqueId(buffer, kMaxCacheKeyPrefixSize);

  // If the prefix wasn't generated or was too long,
  // create one from the cache.
  if (cc && *size == 0) {
    char* end = EncodeVarint64(buffer, cc->NewId());
    *size = static_cast<size_t>(end - buffer);
  }
}

Slice ColumnTable::GetCacheKey(const char* cache_key_prefix,
                               size_t cache_key_prefix_size,
                               const BlockHandle& handle, char* cache_key) {
  assert(cache_key != nullptr);
  assert(cache_key_prefix_size != 0);
  assert(cache_key_prefix_size <= kMaxCacheKeyPrefixSize);
  memcpy(cache_key, cache_key_prefix, cache_key_prefix_size);
  char* end =
      EncodeVarint64(cache_key + cache_key_prefix_size, handle.offset());
  return Slice(cache_key, static_cast<size_t>(end - cache_key));
}

Status ColumnTable::Open(const ImmutableCFOptions& ioptions,
                         const EnvOptions& env_options,
                         const ColumnTableOptions& table_options,
                         const InternalKeyComparator& internal_comparator,
                         unique_ptr<RandomAccessFileReader>&& file,
                         uint64_t file_size,
                         unique_ptr<TableReader>* table_reader,
                         const bool prefetch_index,
                         const int level,
                         const std::vector<uint32_t>& cols) {
  table_reader->reset();

  Footer footer;
  auto s = ReadFooterFromFile(file.get(), file_size, &footer,
                              kColumnTableMagicNumber);
  if (!s.ok()) {
    return s;
  }
  if (!BlockBasedTableSupportedVersion(footer.version())) {
    return Status::Corruption(
        "Unknown Footer version. Maybe this file was created with newer "
        "version of RocksDB?");
  }

  // We've successfully read the footer and the index block: we're
  // ready to serve requests.
  Rep* rep = new ColumnTable::Rep(ioptions, env_options, table_options,
                                  internal_comparator);
  rep->file = std::move(file);
  rep->footer = footer;
  rep->index_type = table_options.index_type;
  SetupCacheKeyPrefix(rep, file_size);
  unique_ptr<ColumnTable> new_table(new ColumnTable(rep));

  // page cache options
  rep->persistent_cache_options =
      PersistentCacheOptions(rep->table_options.persistent_cache,
                             std::string(rep->persistent_cache_key_prefix,
                                         rep->persistent_cache_key_prefix_size),
                             rep->ioptions.statistics);

  // Read meta index
  std::unique_ptr<Block> meta;
  std::unique_ptr<InternalIterator> meta_iter;
  s = ReadMetaBlock(rep, &meta, &meta_iter);
  if (!s.ok()) {
    return s;
  }

  // Read column block
  bool found_column_block = true;
  s = SeekToColumnBlock(meta_iter.get(), &found_column_block);
  if (!s.ok()) {
    Log(InfoLogLevel::WARN_LEVEL, rep->ioptions.info_log,
	    "Cannot seek to column block from file: %s", s.ToString().c_str());
  } else if (found_column_block) {
	s = meta_iter->status();
	if (!s.ok()) {
	  return s;
	}

	uint32_t column_num;
	std::vector<uint64_t> file_sizes;
    s = ReadColumnBlock(meta_iter->value(), rep->file.get(), rep->footer,
		                ioptions.env, ioptions.info_log, &column_num, file_sizes);
    if (!s.ok()) {
      return s;
    }

    rep->tables.resize(column_num);
    rep->main_column = rep->tables.empty()? false: true;
    if (rep->main_column && column_num != rep->table_options.column_num) {
      return Status::InvalidArgument("table_options.column_num");
    }

    size_t readahead = rep->file->file()->ReadaheadSize();
    std::string fname = rep->file->file()->GetFileName();
    for (auto i = 0u; i < column_num; i++) {
      if (!cols.empty() && std::find(cols.begin(), cols.end(), i+1) == cols.end()) {
        continue;
      }
      std::string col_fname = TableSubFileName(fname, i+1);
      unique_ptr<RandomAccessFile> col_file;
      s = rep->ioptions.env->NewRandomAccessFile(col_fname, &col_file, env_options);
      if (!s.ok()) {
    	return s;
      }
      if (readahead > 0) {
        col_file = NewReadaheadRandomAccessFile(std::move(col_file), readahead);
      }
      unique_ptr<RandomAccessFileReader> file_reader(
          new RandomAccessFileReader(std::move(col_file), ioptions.env));
      unique_ptr<TableReader> table;
      s = Open(ioptions, env_options, table_options, internal_comparator,
    	       std::move(file_reader), file_sizes[i], &table,
    	       prefetch_index, level);
      if (!s.ok()) {
        return s;
      }
      rep->tables[i].reset(dynamic_cast<ColumnTable*>(table.release()));
    }
  }

  // Read the properties
  bool found_properties_block = true;
  s = SeekToPropertiesBlock(meta_iter.get(), &found_properties_block);
  if (!s.ok()) {
    Log(InfoLogLevel::WARN_LEVEL, rep->ioptions.info_log,
        "Cannot seek to properties block from file: %s",
        s.ToString().c_str());
  } else if (found_properties_block) {
    s = meta_iter->status();
    TableProperties* table_properties = nullptr;
    if (s.ok()) {
      s = ReadProperties(meta_iter->value(), rep->file.get(), rep->footer,
                         rep->ioptions.env, rep->ioptions.info_log,
                         &table_properties);
    }

    if (!s.ok()) {
      Log(InfoLogLevel::WARN_LEVEL, rep->ioptions.info_log,
        "Encountered error while reading data from properties "
        "block %s", s.ToString().c_str());
    } else {
      rep->table_properties.reset(table_properties);
    }
  } else {
    Log(InfoLogLevel::ERROR_LEVEL, rep->ioptions.info_log,
        "Cannot find Properties block from file.");
  }

  // Read the compression dictionary meta block
  bool found_compression_dict;
  s = SeekToCompressionDictBlock(meta_iter.get(), &found_compression_dict);
  if (!s.ok()) {
    Log(InfoLogLevel::WARN_LEVEL, rep->ioptions.info_log,
        "Cannot seek to compression dictionary block from file: %s",
        s.ToString().c_str());
  } else if (found_compression_dict) {
    // TODO(andrewkr): Add to block cache if cache_index_blocks is true.
    unique_ptr<BlockContents> compression_dict_block{new BlockContents()};
    s = rocksdb::ReadMetaBlock(rep->file.get(), file_size,
                               kColumnTableMagicNumber, rep->ioptions.env,
                               rocksdb::kCompressionDictBlock,
                               compression_dict_block.get());
    if (!s.ok()) {
      Log(InfoLogLevel::WARN_LEVEL, rep->ioptions.info_log,
          "Encountered error while reading data from compression dictionary "
          "block %s",
          s.ToString().c_str());
    } else {
      rep->compression_dict_block = std::move(compression_dict_block);
    }
  }

  if (prefetch_index) {
    // pre-fetching of blocks is turned on
    // Will use block cache for index blocks access?
    if (table_options.cache_index_blocks) {
      assert(table_options.block_cache != nullptr);
      // Hack: Call NewIndexIterator() to implicitly add index to the
      // block_cache

      // if pin_l0_index_blocks_in_cache is true and this is
      // a level0 file, then we will pass in this pointer to rep->index
      // to NewIndexIterator(), which will save the index block in there
      // else it's a nullptr and nothing special happens
      CachableEntry<IndexReader>* index_entry = nullptr;
      if (rep->table_options.pin_l0_index_blocks_in_cache && level == 0) {
        index_entry = &rep->index_entry;
      }
      unique_ptr<InternalIterator> iter(
          new_table->NewIndexIterator(ReadOptions(), nullptr, index_entry));
      s = iter->status();
    } else {
      // If we don't use block cache for index blocks access, we'll
      // pre-load these blocks, which will kept in member variables in Rep
      // and with a same life-time as this table object.
      IndexReader* index_reader = nullptr;
      s = new_table->CreateIndexReader(&index_reader);

      if (s.ok()) {
        rep->index_reader.reset(index_reader);
      } else {
        delete index_reader;
      }
    }
  }

  if (s.ok()) {
    *table_reader = std::move(new_table);
  }

  return s;
}

void ColumnTable::SetupForCompaction() {
  switch (rep_->ioptions.access_hint_on_compaction_start) {
    case Options::NONE:
    case Options::NORMAL:
    case Options::WILLNEED:
    case Options::SEQUENTIAL:
      rep_->file->file()->Hint(RandomAccessFile::SEQUENTIAL);
      break;
    default:
      assert(false);
  }
  compaction_optimized_ = true;
  for (const auto& it : rep_->tables) {
    if (it) {
      it->SetupForCompaction();
    }
  }
}

std::shared_ptr<const TableProperties> ColumnTable::GetTableProperties()
    const {
  return rep_->table_properties;
}

size_t ColumnTable::ApproximateMemoryUsage() const {
  size_t usage = 0;
  if (rep_->index_reader) {
    usage += rep_->index_reader->ApproximateMemoryUsage();
  }
  return usage;
}

// Load the meta-block from the file. On success, return the loaded meta block
// and its iterator.
Status ColumnTable::ReadMetaBlock(Rep* rep,
                                  std::unique_ptr<Block>* meta_block,
                                  std::unique_ptr<InternalIterator>* iter) {
  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  //  TODO: we never really verify check sum for meta index block
  std::unique_ptr<Block> meta;
  Status s = ReadBlockFromFile(
      rep->file.get(), rep->footer, ReadOptions(),
      rep->footer.metaindex_handle(), &meta, rep->ioptions.env,
      true /* decompress */, Slice() /*compression dict*/,
      rep->persistent_cache_options, rep->ioptions.info_log);

  if (!s.ok()) {
    Log(InfoLogLevel::ERROR_LEVEL, rep->ioptions.info_log,
        "Encountered error while reading data from properties"
        " block %s", s.ToString().c_str());
    return s;
  }

  *meta_block = std::move(meta);
  // meta block uses bytewise comparator.
  iter->reset(meta_block->get()->NewIterator(BytewiseComparator()));
  return Status::OK();
}

Status ColumnTable::GetDataBlockFromCache(
    const Slice& block_cache_key, Cache* block_cache,
    Statistics* statistics, ColumnTable::CachableEntry<Block>* block) {
  Status s;

  // Lookup uncompressed cache first
  if (block_cache != nullptr) {
    block->cache_handle =
        GetEntryFromCache(block_cache, block_cache_key, BLOCK_CACHE_DATA_MISS,
                          BLOCK_CACHE_DATA_HIT, statistics);
    if (block->cache_handle != nullptr) {
      block->value =
          reinterpret_cast<Block*>(block_cache->Value(block->cache_handle));
      return s;
    }
  }

  // If not found, search from the compressed block cache.
  assert(block->cache_handle == nullptr && block->value == nullptr);

  return s;
}

Status ColumnTable::PutDataBlockToCache(
    const Slice& block_cache_key, Cache* block_cache,
    Statistics* statistics, CachableEntry<Block>* block,
    Block* raw_block) {
  assert(raw_block->compression_type() == kNoCompression);
  Status s;
  block->value = raw_block;
  raw_block = nullptr;
  delete raw_block;

  // insert into uncompressed block cache
  assert((block->value->compression_type() == kNoCompression));
  if (block_cache != nullptr && block->value->cachable()) {
    s = block_cache->Insert(block_cache_key, block->value,
                            block->value->usable_size(),
                            &DeleteCachedEntry<Block>, &(block->cache_handle));
    if (s.ok()) {
      assert(block->cache_handle != nullptr);
      RecordTick(statistics, BLOCK_CACHE_ADD);
      RecordTick(statistics, BLOCK_CACHE_BYTES_WRITE,
                 block->value->usable_size());
      assert(reinterpret_cast<Block*>(
                 block_cache->Value(block->cache_handle)) == block->value);
    } else {
      RecordTick(statistics, BLOCK_CACHE_ADD_FAILURES);
      delete block->value;
      block->value = nullptr;
    }
  }

  return s;
}

InternalIterator* ColumnTable::NewIndexIterator(
    const ReadOptions& read_options, BlockIter* input_iter,
    CachableEntry<IndexReader>* index_entry) {
  // index reader has already been pre-populated.
  if (rep_->index_reader) {
    return rep_->index_reader->NewIterator(
        input_iter, read_options.total_order_seek);
  }
  // we have a pinned index block
  if (rep_->index_entry.IsSet()) {
    return rep_->index_entry.value->NewIterator(input_iter,
                                                read_options.total_order_seek);
  }

  PERF_TIMER_GUARD(read_index_block_nanos);

  bool no_io = read_options.read_tier == kBlockCacheTier;
  Cache* block_cache = rep_->table_options.block_cache.get();
  char cache_key[kMaxCacheKeyPrefixSize + kMaxVarint64Length];
  auto key =
      GetCacheKeyFromOffset(rep_->cache_key_prefix, rep_->cache_key_prefix_size,
                            rep_->dummy_index_reader_offset, cache_key);
  Statistics* statistics = rep_->ioptions.statistics;
  auto cache_handle =
      GetEntryFromCache(block_cache, key, BLOCK_CACHE_INDEX_MISS,
                        BLOCK_CACHE_INDEX_HIT, statistics);

  if (cache_handle == nullptr && no_io) {
    if (input_iter != nullptr) {
      input_iter->SetStatus(Status::Incomplete("no blocking io"));
      return input_iter;
    } else {
      return NewErrorInternalIterator(Status::Incomplete("no blocking io"));
    }
  }

  IndexReader* index_reader = nullptr;
  if (cache_handle != nullptr) {
    index_reader =
        reinterpret_cast<IndexReader*>(block_cache->Value(cache_handle));
  } else {
    // Create index reader and put it in the cache.
    Status s;
    s = CreateIndexReader(&index_reader);
    if (s.ok()) {
      s = block_cache->Insert(key, index_reader, index_reader->usable_size(),
                              &DeleteCachedIndexEntry, &cache_handle);
    }

    if (s.ok()) {
      size_t usable_size = index_reader->usable_size();
      RecordTick(statistics, BLOCK_CACHE_ADD);
      RecordTick(statistics, BLOCK_CACHE_BYTES_WRITE, usable_size);
      RecordTick(statistics, BLOCK_CACHE_INDEX_BYTES_INSERT, usable_size);
    } else {
      RecordTick(statistics, BLOCK_CACHE_ADD_FAILURES);
      // make sure if something goes wrong, index_reader shall remain intact.
      if (input_iter != nullptr) {
        input_iter->SetStatus(s);
        return input_iter;
      } else {
        return NewErrorInternalIterator(s);
      }
    }

  }

  assert(cache_handle);
  auto* iter = index_reader->NewIterator(
      input_iter, read_options.total_order_seek);

  // the caller would like to take ownership of the index block
  // don't call RegisterCleanup() in this case, the caller will take care of it
  if (index_entry != nullptr) {
    *index_entry = {index_reader, cache_handle};
  } else {
    iter->RegisterCleanup(&ReleaseCachedEntry, block_cache, cache_handle);
  }

  return iter;
}

// Convert an index iterator value (i.e., an encoded BlockHandle)
// into an iterator over the contents of the corresponding block.
// If input_iter is null, new a iterator
// If input_iter is not null, update this iter and return it
InternalIterator* ColumnTable::NewDataBlockIterator(
    Rep* rep, const ReadOptions& ro, const Slice& index_value,
    bool rev, BlockIter* input_iter) {
  PERF_TIMER_GUARD(new_table_block_iter_nanos);

  const bool no_io = (ro.read_tier == kBlockCacheTier);
  Cache* block_cache = rep->table_options.block_cache.get();
  CachableEntry<Block> block;

  BlockHandle handle;
  Slice input = index_value;
  // We intentionally allow extra stuff in index_value so that we
  // can add more features in the future.
  Status s = handle.DecodeFrom(&input);

  if (!s.ok()) {
    if (input_iter != nullptr) {
      input_iter->SetStatus(s);
      return input_iter;
    } else {
      return NewErrorInternalIterator(s);
    }
  }

  Slice compression_dict;
  if (rep->compression_dict_block) {
    compression_dict = rep->compression_dict_block->data;
  }
  // If block cache is enabled, we'll try to read from it.
  if (block_cache != nullptr) {
    Statistics* statistics = rep->ioptions.statistics;
    char cache_key[kMaxCacheKeyPrefixSize + kMaxVarint64Length];
    // create key for block cache
    Slice key = GetCacheKey(rep->cache_key_prefix, rep->cache_key_prefix_size,
                            handle, cache_key);

    s = GetDataBlockFromCache(key, block_cache, statistics, &block);

    if (block.value == nullptr && !no_io && ro.fill_cache) {
      std::unique_ptr<Block> raw_block;
      {
        StopWatch sw(rep->ioptions.env, statistics, READ_BLOCK_GET_MICROS);
        s = ReadBlockFromFile(rep->file.get(), rep->footer, ro, handle,
                              &raw_block, rep->ioptions.env, true,
                              compression_dict, rep->persistent_cache_options,
                              rep->ioptions.info_log);
      }

      if (s.ok()) {
        s = PutDataBlockToCache(key, block_cache, statistics, &block,
        		                raw_block.release());
      }
    }
  }

  // Didn't get any data from block caches.
  if (s.ok() && block.value == nullptr) {
    if (no_io) {
      // Could not read from block_cache and can't do IO
      if (input_iter != nullptr) {
        input_iter->SetStatus(Status::Incomplete("no blocking io"));
        return input_iter;
      } else {
        return NewErrorInternalIterator(Status::Incomplete("no blocking io"));
      }
    }
    std::unique_ptr<Block> block_value;
    s = ReadBlockFromFile(rep->file.get(), rep->footer, ro, handle,
                          &block_value, rep->ioptions.env, true /* compress */,
                          compression_dict, rep->persistent_cache_options,
                          rep->ioptions.info_log);
    if (s.ok()) {
      block.value = block_value.release();
    }
  }

  InternalIterator* iter;
  if (s.ok() && block.value != nullptr) {
    iter = block.value->NewIterator(&rep->internal_comparator, input_iter);
    dynamic_cast<BlockIter*>(iter)->SetReverse(true);
    if (block.cache_handle != nullptr) {
      iter->RegisterCleanup(&ReleaseCachedEntry, block_cache,
                            block.cache_handle);
    } else {
      iter->RegisterCleanup(&DeleteHeldResource<Block>, block.value, nullptr);
    }
  } else {
    if (input_iter != nullptr) {
      input_iter->SetStatus(s);
      iter = input_iter;
    } else {
      iter = NewErrorInternalIterator(s);
    }
  }
  return iter;
}

class ColumnTable::BlockEntryIteratorState : public TwoLevelIteratorState {
 public:
  BlockEntryIteratorState(ColumnTable* table,
                          const ReadOptions& read_options)
      : TwoLevelIteratorState(table->rep_->ioptions.prefix_extractor !=
                              nullptr),
        table_(table),
        read_options_(read_options) {}

  InternalIterator* NewSecondaryIterator(const Slice& index_value) override {
    return NewDataBlockIterator(table_->rep_, read_options_, index_value);
  }

  bool PrefixMayMatch(const Slice& internal_key) override {
    return true;
  }

 private:
  // Don't own table_
  ColumnTable* table_;
  const ReadOptions read_options_;
};

class ColumnTable::ColumnIterator : public InternalIterator {
 public:
  ColumnIterator(const std::vector<InternalIterator*>& columns,
                 char delim,
                 const InternalKeyComparator& internal_comparator,
                 uint64_t num_entries = 0)
  : columns_(columns),
    delim_(delim),
    internal_comparator_(internal_comparator),
    num_entries_(num_entries) {}

  virtual ~ColumnIterator() {
    for (const auto& it : columns_) {
      it->~InternalIterator();
    }
  }

  virtual bool Valid() const {
    for (const auto& it : columns_) {
      if (!it->Valid()) {
        return false;
      }
    }
    return true;
  }

  virtual void SeekToFirst() {
    for (const auto& it : columns_) {
      it->SeekToFirst();
    }
    ParseCurrentValue();
  }

  virtual void SeekToLast() {
    for (const auto& it : columns_) {
      it->SeekToLast();
    }
    ParseCurrentValue();
  }

  virtual void Seek(const Slice& target) {
    for (const auto& it : columns_) {
      it->Seek(target);
    }
    ParseCurrentValue();
  }

  virtual void Next() {
    assert(Valid());
    for (const auto& it : columns_) {
      it->Next();
    }
    ParseCurrentValue();
  }

  virtual void Prev() {
    assert(Valid());
    for (const auto& it : columns_) {
      it->Prev();
    }
    ParseCurrentValue();
  }

  virtual Slice key() const {
    assert(Valid());
    return columns_[0]->key();
  }

  virtual Slice value() const {
    assert(Valid());
    return value_;
  }

  virtual Status status() const {
    if (!status_.ok()) {
      return status_;
    }
    Status s;
    for (const auto& it : columns_) {
      s = it->status();
      if (!s.ok()) {
        break;
      }
    }
    return s;
  }

  virtual Status RangeQuery(const LookupRange& range,
                            std::map<std::string, SeqTypeVal>& res,
                            filterFun filter,
                            groupFun group,
                            void* arg,
                            bool unique_key) const {
    SequenceNumber sequence_num = range.SequenceNum();
    std::vector<std::string> tmp_keys;
    std::vector<SeqTypeVal> tmp_STVs;
    std::vector<bool> out_bs, inn_bs;
    if (num_entries_ > 0) {
      tmp_keys.reserve(num_entries_);
      tmp_STVs.reserve(num_entries_);
      out_bs.reserve(num_entries_);
      inn_bs.reserve(num_entries_);
    }

    for (auto i = 0u; i < columns_.size(); i++) {
      InternalIterator* it = columns_[i];
      std::string val, user_key;
      std::vector<const std::string*> v;
      if (i == 0) {
        for (it->Seek(range.start_->internal_key()); it->Valid(); it->Next()) {
          if (internal_comparator_.Compare(it->key(), range.limit_->internal_key()) >= 0) {
            break;
          }
          ParsedInternalKey parsed_key;
          if (!ParseInternalKey(it->key(), &parsed_key)) {
            return Status::Corruption("corrupted internal key in Table::Iter");
          }
          if (parsed_key.sequence <= sequence_num && parsed_key.type != kTypeMerge) {
            out_bs.push_back(true);
            val.assign(it->value().data(), it->value().size());
            user_key.assign(it->key().data(), it->key().size() - 8);
            v.clear();
            v.push_back(&user_key);
            v.push_back(&val);
            if (filter && !filter(v, i + 1)) {
              out_bs.back() = false;
              continue;
            }
            inn_bs.push_back(true);
            tmp_keys.push_back(std::move(user_key));
            tmp_STVs.push_back(SeqTypeVal(parsed_key.sequence, parsed_key.type, std::move(val)));
          } else {
            out_bs.push_back(false);
          }
        }
      } else {
        size_t out_cnt = 0;
        size_t inn_cnt = 0;
        for (it->Seek(range.start_->internal_key()); it->Valid()
             && out_cnt < out_bs.size(); it->Next()) {
          if (!out_bs[out_cnt++]) {
            continue;
          }
          if (!inn_bs[inn_cnt]) {
            inn_cnt++;
            continue;
          }
          val.assign(it->value().data(), it->value().size());
          v.clear();
          v.push_back(&val);
          if (filter && !filter(v, i + 1)) {
            inn_bs[inn_cnt++] = false;
            continue;
          }
          tmp_STVs[inn_cnt].val_ += delim_;
          tmp_STVs[inn_cnt++].val_ += val;
        }
      }
    }

    assert(tmp_keys.size() == tmp_STVs.size());
    assert(tmp_keys.size() == inn_bs.size());
    auto hint = res.end();
    for (auto i = 0u; i < tmp_keys.size(); i++) {
      if (!inn_bs[i]) {
        continue;
      }
      std::vector<const std::string*> v{&tmp_keys[i], &tmp_STVs[i].val_};
      if (filter && !filter(v, columns_.size() + 1)) {
        continue;
      }

      if (!unique_key) {
        while (hint != res.end() && hint->first < tmp_keys[i]) {
          hint++;
        }
        if (hint == res.end() || hint->first > tmp_keys[i]) {
          hint = res.emplace_hint(hint, tmp_keys[i], tmp_STVs[i]);
          if (hint->second.seq_ < tmp_STVs[i].seq_) {
            hint->second.seq_ = tmp_STVs[i].seq_;
            hint->second.type_ = tmp_STVs[i].type_;
            hint->second.val_ = tmp_STVs[i].val_;
          }
        } else if (hint->second.seq_ < tmp_STVs[i].seq_) {
          assert(hint->first == tmp_keys[i]);
          hint->second.seq_ = tmp_STVs[i].seq_;
          hint->second.type_ = tmp_STVs[i].type_;
          hint->second.val_ = tmp_STVs[i].val_;
        }
      }
      if (group && (unique_key || hint->second.seq_ == tmp_STVs[i].seq_)) {
        if (group(v, columns_.size() + 1, arg) && !unique_key) {
          hint = res.erase(hint);
          continue;
        }
      }

      if (!unique_key) {
        hint++;
      }
    }

    return Status();
  }

  virtual void SetPinnedItersMgr(PinnedIteratorsManager* pinned_iters_mgr) {
    for (const auto& it : columns_) {
      it->SetPinnedItersMgr(pinned_iters_mgr);
    }
  }

  virtual bool IsKeyPinned() const {
    for (const auto& it : columns_) {
      if (!it->IsKeyPinned()) {
        return false;
      }
    }
    return true;
  }

  bool SetIter(uint32_t i, InternalIterator* const it) {
    if (i < columns_.size()) {
      columns_[i] = it;
      return true;
    } else {
      return false;
    }
  }

  const std::vector<std::string>& GetVals() const { return vals_; }

 private:
  inline bool ParseCurrentValue() {
    value_.clear();
    vals_.clear();
    for (auto i = 0u; i < columns_.size(); i++) {
      if (!columns_[i]->Valid()) {
        return false;
      }
      vals_.emplace_back(columns_[i]->value().ToString());
      value_ += vals_.back();
      if (i < columns_.size() - 1) {
        value_ += delim_;
      }
    }
    return true;
  }

  std::vector<InternalIterator*> columns_;
  std::string value_;
  char delim_;
  Status status_;
  const InternalKeyComparator& internal_comparator_;
  uint64_t num_entries_;
  std::vector<std::string> vals_;
};

void ColumnTable::NewDataBlockIterators(const ReadOptions& read_options,
                                        const std::vector<std::string>& index_values,
                                        ColumnIterator& input_iter) {
  assert(rep_->main_column);

  for (auto i = 0u; i < read_options.columns.size(); i++) {
    input_iter.SetIter(i,
        NewDataBlockIterator(rep_->tables[read_options.columns[i]]->rep_,
            read_options, index_values[i]));
  }
}

void ColumnTable::NewIndexIterators(const ReadOptions& read_options,
                                    ColumnIterator& input_iter) {
  assert(rep_->main_column);

  for (auto i = 0u; i < read_options.columns.size(); i++) {
    input_iter.SetIter(i, rep_->tables[read_options.columns[i]]
                          ->NewIndexIterator(read_options));
  }
}

inline static ReadOptions SanitizeReadOptionsColumns(uint32_t column_num,
                                                     const ReadOptions& read_options) {
  ReadOptions ro = read_options;
  if (ro.columns.empty()) {
    ro.columns.resize(column_num);
    for (uint32_t i = 0; i < column_num; i++) {
      ro.columns[i] = i;
    }
    return ro;
  }

  for (uint32_t i = 0; i < ro.columns.size(); i++) {
    --ro.columns[i];
  }
  return ro;
}

InternalIterator* ColumnTable::NewIterator(const ReadOptions& ro,
                                           Arena* arena,
                                           bool skip_filters) {
  ReadOptions read_options = SanitizeReadOptionsColumns(
      rep_->table_options.column_num, ro);

  std::vector<InternalIterator*> iters;
  for (const auto& it : read_options.columns) {
    iters.push_back(NewTwoLevelIterator(
        new BlockEntryIteratorState(rep_->tables[it].get(), read_options),
        rep_->tables[it]->NewIndexIterator(read_options), arena));
  }
  return new ColumnIterator(iters, rep_->table_options.delim,
                                   rep_->internal_comparator,
                                   rep_->table_properties->num_entries);
}

Status ColumnTable::Get(const ReadOptions& ro, const Slice& key,
                        GetContext* get_context, bool skip_filters) {
  ReadOptions read_options = SanitizeReadOptionsColumns(
      rep_->table_options.column_num, ro);
  std::vector<InternalIterator*> v(read_options.columns.size());
  ColumnIterator iiter(v, rep_->table_options.delim, rep_->internal_comparator);
  NewIndexIterators(read_options, iiter);

  Status s;

  bool done = false;
  for (iiter.Seek(key); iiter.Valid() && !done; iiter.Next()) {
    ColumnIterator biter(v, rep_->table_options.delim, rep_->internal_comparator);
    NewDataBlockIterators(read_options, iiter.GetVals(), biter);

    if (read_options.read_tier == kBlockCacheTier &&
        biter.status().IsIncomplete()) {
      // couldn't get block from block_cache
      // Update Saver.state to Found because we are only looking for whether
      // we can guarantee the key is not there when "no_io" is set
      get_context->MarkKeyMayExist();
      break;
    }
    if (!biter.status().ok()) {
      s = biter.status();
      break;
    }

    // Call the *saver function on each entry/block until it returns false
    for (biter.Seek(key); biter.Valid(); biter.Next()) {
      ParsedInternalKey parsed_key;
      if (!ParseInternalKey(biter.key(), &parsed_key)) {
        s = Status::Corruption(Slice());
      }

      if (!get_context->SaveValue(parsed_key, biter.value())) {
        done = true;
        break;
      }
    }
    s = biter.status();
  }
  if (s.ok()) {
    s = iiter.status();
  }

  return s;
}

Status ColumnTable::Prefetch(const Slice* const begin,
                             const Slice* const end) {
  return Prefetch(begin, end, ReadOptions());
}

Status ColumnTable::Prefetch(const Slice* const begin,
                             const Slice* const end,
                             const ReadOptions& ro) {
  ReadOptions read_options = SanitizeReadOptionsColumns(
      rep_->table_options.column_num, ro);
  auto& comparator = rep_->internal_comparator;
  // pre-condition
  if (begin && end && comparator.Compare(*begin, *end) > 0) {
    return Status::InvalidArgument(*begin, *end);
  }

  std::vector<InternalIterator*> v(rep_->table_options.column_num);
  ColumnIterator iiter(v, rep_->table_options.delim, rep_->internal_comparator);
  NewIndexIterators(read_options, iiter);

  if (!iiter.status().ok()) {
    // error opening index iterator
    return iiter.status();
  }

  // indicates if we are on the last page that need to be pre-fetched
  bool prefetching_boundary_page = false;

  for (begin ? iiter.Seek(*begin) : iiter.SeekToFirst(); iiter.Valid();
       iiter.Next()) {

    if (end && comparator.Compare(iiter.key(), *end) >= 0) {
      if (prefetching_boundary_page) {
        break;
      }

      // The index entry represents the last key in the data block.
      // We should load this page into memory as well, but no more
      prefetching_boundary_page = true;
    }

    // Load the block specified by the block_handle into the block cache
    ColumnIterator biter(v, rep_->table_options.delim, rep_->internal_comparator);
    NewDataBlockIterators(read_options, iiter.GetVals(), biter);

    if (!biter.status().ok()) {
      // there was an unexpected error while pre-fetching
      return biter.status();
    }
  }

  return Status::OK();
}

// REQUIRES: The following fields of rep_ should have already been populated:
//  1. file
//  2. index_handle,
//  3. options
//  4. internal_comparator
//  5. index_type
Status ColumnTable::CreateIndexReader(IndexReader** index_reader) {
  // Some old version of block-based tables don't have index type present in
  // table properties. If that's the case we can safely use the kBinarySearch.
  auto index_type_on_file = ColumnTableOptions::kBinarySearch;
  if (rep_->table_properties) {
    auto& props = rep_->table_properties->user_collected_properties;
    auto pos = props.find(ColumnTablePropertyNames::kIndexType);
    if (pos != props.end()) {
      index_type_on_file = static_cast<ColumnTableOptions::IndexType>(
          DecodeFixed32(pos->second.c_str()));
    }
  }

  auto file = rep_->file.get();
  auto env = rep_->ioptions.env;
  auto comparator = &rep_->internal_comparator;
  const Footer& footer = rep_->footer;
  Statistics* stats = rep_->ioptions.statistics;

  switch (index_type_on_file) {
    case ColumnTableOptions::kBinarySearch: {
      return BinarySearchIndexReader::Create(
          file, footer, footer.index_handle(), env, comparator, index_reader,
          rep_->persistent_cache_options, stats);
    }
    default: {
      std::string error_message =
          "Unrecognized index type: " + ToString(rep_->index_type);
      return Status::InvalidArgument(error_message.c_str());
    }
  }
}

uint64_t ColumnTable::ApproximateOffsetOf(const Slice& key) {
  unique_ptr<InternalIterator> index_iter(NewIndexIterator(ReadOptions()));

  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    BlockHandle handle;
    Slice input = index_iter->value();
    Status s = handle.DecodeFrom(&input);
    if (s.ok()) {
      result = handle.offset();
    } else {
      // Strange: we can't decode the block handle in the index block.
      // We'll just return the offset of the metaindex block, which is
      // close to the whole file size for this case.
      result = rep_->footer.metaindex_handle().offset();
    }
  } else {
    // key is past the last key in the file. If table_properties is not
    // available, approximate the offset by returning the offset of the
    // metaindex block (which is right near the end of the file).
    result = 0;
    if (rep_->table_properties) {
      result = rep_->table_properties->data_size;
    }
    // table_properties is not present in the table.
    if (result == 0) {
      result = rep_->footer.metaindex_handle().offset();
    }
  }
  return result;
}

Status ColumnTable::DumpTable(WritableFile* out_file) {
  // Output Footer
  out_file->Append(
      "Footer Details:\n"
      "--------------------------------------\n"
      "  ");
  out_file->Append(rep_->footer.ToString().c_str());
  out_file->Append("\n");

  // Output MetaIndex
  out_file->Append(
      "Metaindex Details:\n"
      "--------------------------------------\n");
  std::unique_ptr<Block> meta;
  std::unique_ptr<InternalIterator> meta_iter;
  Status s = ReadMetaBlock(rep_, &meta, &meta_iter);
  if (s.ok()) {
    for (meta_iter->SeekToFirst(); meta_iter->Valid(); meta_iter->Next()) {
      s = meta_iter->status();
      if (!s.ok()) {
        return s;
      }
      if (meta_iter->key() == rocksdb::kPropertiesBlock) {
        out_file->Append("  Properties block handle: ");
        out_file->Append(meta_iter->value().ToString(true).c_str());
        out_file->Append("\n");
      } else if (meta_iter->key() == rocksdb::kCompressionDictBlock) {
        out_file->Append("  Compression dictionary block handle: ");
        out_file->Append(meta_iter->value().ToString(true).c_str());
        out_file->Append("\n");
      }
    }
    out_file->Append("\n");
  } else {
    return s;
  }

  // Output TableProperties
  const rocksdb::TableProperties* table_properties;
  table_properties = rep_->table_properties.get();

  if (table_properties != nullptr) {
    out_file->Append(
        "Table Properties:\n"
        "--------------------------------------\n"
        "  ");
    out_file->Append(table_properties->ToString("\n  ", ": ").c_str());
    out_file->Append("\n");
  }

  // Output Index block
  s = DumpIndexBlock(out_file);
  if (!s.ok()) {
    return s;
  }
  // Output Data blocks
  s = DumpDataBlocks(out_file);

  return s;
}

void ColumnTable::Close() {
  rep_->index_entry.Release(rep_->table_options.block_cache.get());
  // cleanup index blocks to avoid accessing dangling pointer
  if (!rep_->table_options.no_block_cache) {
    char cache_key[kMaxCacheKeyPrefixSize + kMaxVarint64Length];
    // Get the index block key
    auto key = GetCacheKeyFromOffset(rep_->cache_key_prefix,
                                rep_->cache_key_prefix_size,
                                rep_->dummy_index_reader_offset, cache_key);
    rep_->table_options.block_cache.get()->Erase(key);
  }
  for (const auto& it : rep_->tables) {
    if (it) {
      it->Close();
    }
  }
}

Status ColumnTable::DumpIndexBlock(WritableFile* out_file) {
  out_file->Append(
      "Index Details:\n"
      "--------------------------------------\n");

  std::unique_ptr<InternalIterator> blockhandles_iter(
      NewIndexIterator(ReadOptions()));
  Status s = blockhandles_iter->status();
  if (!s.ok()) {
    out_file->Append("Can not read Index Block \n\n");
    return s;
  }

  out_file->Append("  Block key hex dump: Data block handle\n");
  out_file->Append("  Block key ascii\n\n");
  for (blockhandles_iter->SeekToFirst(); blockhandles_iter->Valid();
       blockhandles_iter->Next()) {
    s = blockhandles_iter->status();
    if (!s.ok()) {
      break;
    }
    Slice key = blockhandles_iter->key();
    InternalKey ikey;
    ikey.DecodeFrom(key);

    out_file->Append("  HEX    ");
    out_file->Append(ikey.user_key().ToString(true).c_str());
    out_file->Append(": ");
    out_file->Append(blockhandles_iter->value().ToString(true).c_str());
    out_file->Append("\n");

    std::string str_key = ikey.user_key().ToString();
    std::string res_key("");
    char cspace = ' ';
    for (size_t i = 0; i < str_key.size(); i++) {
      res_key.append(&str_key[i], 1);
      res_key.append(1, cspace);
    }
    out_file->Append("  ASCII  ");
    out_file->Append(res_key.c_str());
    out_file->Append("\n  ------\n");
  }
  out_file->Append("\n");
  return Status::OK();
}

Status ColumnTable::DumpDataBlocks(WritableFile* out_file) {
  std::unique_ptr<InternalIterator> blockhandles_iter(
      NewIndexIterator(ReadOptions()));
  Status s = blockhandles_iter->status();
  if (!s.ok()) {
    out_file->Append("Can not read Index Block \n\n");
    return s;
  }

  size_t block_id = 1;
  for (blockhandles_iter->SeekToFirst(); blockhandles_iter->Valid();
       block_id++, blockhandles_iter->Next()) {
    s = blockhandles_iter->status();
    if (!s.ok()) {
      break;
    }

    out_file->Append("Data Block # ");
    out_file->Append(rocksdb::ToString(block_id));
    out_file->Append(" @ ");
    out_file->Append(blockhandles_iter->value().ToString(true).c_str());
    out_file->Append("\n");
    out_file->Append("--------------------------------------\n");

    std::unique_ptr<InternalIterator> datablock_iter;
    datablock_iter.reset(
        NewDataBlockIterator(rep_, ReadOptions(), blockhandles_iter->value(), false));
    s = datablock_iter->status();

    if (!s.ok()) {
      out_file->Append("Error reading the block - Skipped \n\n");
      continue;
    }

    for (datablock_iter->SeekToFirst(); datablock_iter->Valid();
         datablock_iter->Next()) {
      s = datablock_iter->status();
      if (!s.ok()) {
        out_file->Append("Error reading the block - Skipped \n");
        break;
      }
      Slice key = datablock_iter->key();
      Slice value = datablock_iter->value();
      InternalKey ikey, iValue;
      ikey.DecodeFrom(key);
      iValue.DecodeFrom(value);

      out_file->Append("  HEX    ");
      out_file->Append(ikey.user_key().ToString(true).c_str());
      out_file->Append(": ");
      out_file->Append(iValue.user_key().ToString(true).c_str());
      out_file->Append("\n");

      std::string str_key = ikey.user_key().ToString();
      std::string str_value = iValue.user_key().ToString();
      std::string res_key(""), res_value("");
      char cspace = ' ';
      for (size_t i = 0; i < str_key.size(); i++) {
        res_key.append(&str_key[i], 1);
        res_key.append(1, cspace);
      }
      for (size_t i = 0; i < str_value.size(); i++) {
        res_value.append(&str_value[i], 1);
        res_value.append(1, cspace);
      }

      out_file->Append("  ASCII  ");
      out_file->Append(res_key.c_str());
      out_file->Append(": ");
      out_file->Append(res_value.c_str());
      out_file->Append("\n  ------\n");
    }
    out_file->Append("\n");
  }
  return Status::OK();
}

namespace {

void DeleteCachedIndexEntry(const Slice& key, void* value) {
  IndexReader* index_reader = reinterpret_cast<IndexReader*>(value);
  if (index_reader->statistics() != nullptr) {
    RecordTick(index_reader->statistics(), BLOCK_CACHE_INDEX_BYTES_EVICT,
               index_reader->usable_size());
  }
  delete index_reader;
}

}  // anonymous namespace

}  // namespace rocksdb
