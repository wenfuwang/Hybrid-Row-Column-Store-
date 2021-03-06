//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// BlockBuilder generates blocks where keys are prefix-compressed:
//
// When we store a key, we drop the prefix shared with the previous
// string.  This helps reduce the space requirement significantly.
// Furthermore, once every K keys, we do not apply the prefix
// compression and store the entire key.  We call this a "restart
// point".  The tail end of the block stores the offsets of all of the
// restart points, and can be used to do a binary search when looking
// for a particular key.  Values are stored as-is (without compression)
// immediately following the corresponding key.
//
// An entry for a particular key-value pair has the form:
//     shared_bytes: varint32
//     unshared_bytes: varint32
//     value_length: varint32
//     key_delta: char[unshared_bytes]
//     value: char[value_length]
// shared_bytes == 0 for restart points.
//
// The trailer of the block has the form:
//     restarts: uint32[num_restarts]
//     num_restarts: uint32
// restarts[i] contains the offset within the block of the ith restart point.

#include "table/block_builder.h"

#include <algorithm>
#include <assert.h>
#include "rocksdb/comparator.h"
#include "db/dbformat.h"
#include "util/coding.h"

namespace rocksdb {

BlockBuilder::BlockBuilder(int block_restart_interval,
                           bool use_delta_encoding,
                           bool column)  // Shichao
    : block_restart_interval_(block_restart_interval),
      use_delta_encoding_(use_delta_encoding),
      restarts_(),
      counter_(0),
      finished_(false),
      column_(column) {  // Shichao
  assert(block_restart_interval_ >= 1);
  restarts_.push_back(0);       // First restart point is at offset 0
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);       // First restart point is at offset 0
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
  last_value_.clear();  // Shichao
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return (buffer_.size() +                        // Raw data buffer
          restarts_.size() * sizeof(uint32_t) +   // Restart array
          sizeof(uint32_t));                      // Restart array length
}

size_t BlockBuilder::EstimateSizeAfterKV(const Slice& key, const Slice& value)
  const {
  size_t estimate = CurrentSizeEstimate();
  estimate += key.size() + value.size();
  if (counter_ >= block_restart_interval_) {
    estimate += sizeof(uint32_t); // a new restart entry.
  }

  estimate += sizeof(int32_t); // varint for shared prefix length.
  estimate += VarintLength(key.size()); // varint for key length.

  /******************************* Shichao **********************************/
  if (column_) {
    estimate += sizeof(int32_t);
    estimate += VarintLength(value.size());
  } else {
    estimate += VarintLength(value.size()); // varint for value length.
  }
  /******************************* Shichao **********************************/

  return estimate;
}

Slice BlockBuilder::Finish() {
  // Append restart array
  for (size_t i = 0; i < restarts_.size(); i++) {
    PutFixed32(&buffer_, restarts_[i]);
  }
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return Slice(buffer_);
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  Slice last_key_piece(last_key_);
  assert(!finished_);
  assert(counter_ <= block_restart_interval_);
  size_t shared = 0;  // number of bytes shared with prev key
  if (counter_ >= block_restart_interval_) {
    // Restart compression
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
//    counter_ = 0;  // Shichao
  } else if (use_delta_encoding_) {
    // See how much sharing to do with previous string
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
      shared++;
    }
  }
  const size_t non_shared = key.size() - shared;

  /***************************** Shichao *****************************/
  size_t shared_value = 0;
  if (column_ && counter_ < block_restart_interval_) {
    const size_t min_length = std::min(last_value_.size(), value.size());
    while ((shared_value < min_length) &&
        (last_value_[shared_value] == value[shared_value])) {
      shared_value++;
    }
  }
  const size_t non_shared_value = value.size() - shared_value;

  counter_ = counter_ >= block_restart_interval_? 0: counter_;
  /***************************** Shichao *****************************/

  // Add "<shared><non_shared><value_size>" to buffer_
  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));

  /***************************** Shichao *****************************/
  if (column_) {
    PutVarint32(&buffer_, static_cast<uint32_t>(shared_value));
    PutVarint32(&buffer_, static_cast<uint32_t>(non_shared_value));
  } else {
    PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  }
  /***************************** Shichao *****************************/

  // Add string delta to buffer_ followed by value
  buffer_.append(key.data() + shared, non_shared);
  /***************************** Shichao *****************************/
  column_ ?
      buffer_.append(value.data() + shared_value, non_shared_value):
      buffer_.append(value.data(), value.size());
  /***************************** Shichao *****************************/

  // Update state
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(Slice(last_key_) == key);
  counter_++;

  /***************************** Shichao *****************************/
  if (column_) {
    last_value_.resize(shared_value);
    last_value_.append(value.data() + shared_value, non_shared_value);
    assert(Slice(last_value_) == value);
  }
  /***************************** Shichao *****************************/
}

}  // namespace rocksdb
