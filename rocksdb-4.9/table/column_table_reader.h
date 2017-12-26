#pragma once

#include <stdint.h>
#include <memory>
#include <utility>
#include <string>

#include "rocksdb/options.h"
#include "rocksdb/persistent_cache.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "table/table_properties_internal.h"
#include "table/table_reader.h"
#include "util/coding.h"
#include "util/file_reader_writer.h"

namespace rocksdb {

class Block;
class BlockIter;
class BlockHandle;
class Cache;
class Footer;
class InternalKeyComparator;
class Iterator;
class RandomAccessFile;
class TableCache;
class TableReader;
class WritableFile;
struct ColumnTableOptions;
struct EnvOptions;
struct ReadOptions;
class GetContext;
class InternalIterator;

using std::unique_ptr;

// A Table is a sorted map from strings to strings.  Tables are
// immutable and persistent.  A Table may be safely accessed from
// multiple threads without external synchronization.
class ColumnTable : public TableReader {
 public:
  // The longest prefix of the cache key used to identify blocks.
  // For Posix files the unique ID is three varints.
  static const size_t kMaxCacheKeyPrefixSize = kMaxVarint64Length * 3 + 1;

  // Attempt to open the table that is stored in bytes [0..file_size)
  // of "file", and read the metadata entries necessary to allow
  // retrieving data from the table.
  //
  // If successful, returns ok and sets "*table_reader" to the newly opened
  // table.  The client should delete "*table_reader" when no longer needed.
  // If there was an error while initializing the table, sets "*table_reader"
  // to nullptr and returns a non-ok status.
  //
  // @param file must remain live while this Table is in use.
  // @param prefetch_index can be used to disable prefetching of
  //    index blocks at startup
  static Status Open(const ImmutableCFOptions& ioptions,
                     const EnvOptions& env_options,
                     const ColumnTableOptions& table_options,
                     const InternalKeyComparator& internal_key_comparator,
                     unique_ptr<RandomAccessFileReader>&& file,
                     uint64_t file_size, unique_ptr<TableReader>* table_reader,
                     bool prefetch_index = true, int level = -1,
                     const std::vector<uint32_t>& cols = std::vector<uint32_t>());

  // Returns a new iterator over the table contents.
  // The result of NewIterator() is initially invalid (caller must
  // call one of the Seek methods on the iterator before using it).
  // @param skip_filters Disables loading/accessing the filter block
  InternalIterator* NewIterator(const ReadOptions&, Arena* arena = nullptr,
                                bool skip_filters = true) override;

  // @param skip_filters Disables loading/accessing the filter block
  Status Get(const ReadOptions& readOptions, const Slice& key,
             GetContext* get_context, bool skip_filters = true) override;

  // Pre-fetch the disk blocks that correspond to the key range specified by
  // (kbegin, kend). The call will return error status in the event of
  // IO or iteration error.
  Status Prefetch(const Slice* begin, const Slice* end) override;

  Status Prefetch(const Slice* begin, const Slice* end, const ReadOptions& readOptions);

  // Given a key, return an approximate byte offset in the file where
  // the data for that key begins (or would begin if the key were
  // present in the file).  The returned value is in terms of file
  // bytes, and so includes effects like compression of the underlying data.
  // E.g., the approximate offset of the last key in the table will
  // be close to the file length.
  uint64_t ApproximateOffsetOf(const Slice& key) override;

  // Set up the table for Compaction. Might change some parameters with
  // posix_fadvise
  void SetupForCompaction() override;

  std::shared_ptr<const TableProperties> GetTableProperties() const override;

  size_t ApproximateMemoryUsage() const override;

  // convert SST file to a human readable form
  Status DumpTable(WritableFile* out_file) override;

  void Close() override;

  ~ColumnTable();

  // Implementation of IndexReader will be exposed to internal cc file only.
  class IndexReader;

 private:
  template <class TValue>
  struct CachableEntry;

  struct Rep;
  Rep* rep_;
  bool compaction_optimized_;

  class BlockEntryIteratorState;

  class ColumnIterator;

  // input_iter: if it is not null, update this one and return it as Iterator
  static InternalIterator* NewDataBlockIterator(
      Rep* rep, const ReadOptions& ro, const Slice& index_value,
      bool rev = true, BlockIter* input_iter = nullptr);

  void NewDataBlockIterators(
      const ReadOptions& read_options, const std::vector<std::string>& index_values,
      ColumnIterator& input_iter);

  // Get the iterator from the index reader.
  // If input_iter is not set, return new Iterator
  // If input_iter is set, update it and return it as Iterator
  //
  // Note: ErrorIterator with Status::Incomplete shall be returned if all the
  // following conditions are met:
  //  1. We enabled table_options.cache_index_blocks.
  //  2. index is not present in block cache.
  //  3. We disallowed any io to be performed, that is, read_options ==
  //     kBlockCacheTier
  InternalIterator* NewIndexIterator(
      const ReadOptions& read_options, BlockIter* input_iter = nullptr,
      CachableEntry<IndexReader>* index_entry = nullptr);

  void NewIndexIterators(
      const ReadOptions& read_options, ColumnIterator& input_iter);

  // Read block cache from block caches (if set): block_cache.
  // On success, Status::OK with be returned and @block will be populated with
  // pointer to the block as well as its block handle.
  static Status GetDataBlockFromCache(
      const Slice& block_cache_key, Cache* block_cache,
      Statistics* statistics, ColumnTable::CachableEntry<Block>* block);

  // Put a raw block to the corresponding block caches.
  // This method will populate the block caches.
  // On success, Status::OK will be returned; also @block will be populated with
  // uncompressed block and its cache handle.
  //
  // REQUIRES: raw_block is heap-allocated. PutDataBlockToCache() will be
  // responsible for releasing its memory if error occurs.
  static Status PutDataBlockToCache(
      const Slice& block_cache_key, Cache* block_cache,
      Statistics* statistics, CachableEntry<Block>* block, Block* raw_block);

  // Calls (*handle_result)(arg, ...) repeatedly, starting with the entry found
  // after a call to Seek(key), until handle_result returns false.
  // May not make such a call if filter policy says that key is not present.
  friend class TableCache;
  friend class ColumnTableBuilder;

  // Create a index reader based on the index type stored in the table.
  Status CreateIndexReader(IndexReader** index_reader);

  // Read the meta block from sst.
  static Status ReadMetaBlock(Rep* rep, std::unique_ptr<Block>* meta_block,
                              std::unique_ptr<InternalIterator>* iter);

  static void SetupCacheKeyPrefix(Rep* rep, uint64_t file_size);

  explicit ColumnTable(Rep* rep)
      : rep_(rep), compaction_optimized_(false) {}

  // Generate a cache key prefix from the file
  static void GenerateCachePrefix(Cache* cc,
    RandomAccessFile* file, char* buffer, size_t* size);

  static Slice GetCacheKey(const char* cache_key_prefix,
                           size_t cache_key_prefix_size,
                           const BlockHandle& handle, char* cache_key);

  // Helper functions for DumpTable()
  Status DumpIndexBlock(WritableFile* out_file);
  Status DumpDataBlocks(WritableFile* out_file);

  // No copying allowed
  explicit ColumnTable(const TableReader&) = delete;
  void operator=(const TableReader&) = delete;
};

}  // namespace rocksdb
