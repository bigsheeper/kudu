// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Modified for kudu:
// - use boost mutexes instead of port mutexes

#include <string.h>
#include <boost/foreach.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <glog/logging.h>
#include <map>
#include <string>
#include <vector>
#include "util/env.h"
#include "util/memenv/memenv.h"
#include "util/status.h"

namespace kudu {

namespace {

using boost::mutex;
using boost::lock_guard;
using std::string;
using std::vector;

class FileState {
 public:
  // FileStates are reference counted. The initial reference count is zero
  // and the caller must call Ref() at least once.
  FileState() : refs_(0), size_(0) {}

  // Increase the reference count.
  void Ref() {
    lock_guard<mutex> lock(refs_mutex_);
    ++refs_;
  }

  // Decrease the reference count. Delete if this is the last reference.
  void Unref() {
    bool do_delete = false;

    {
      lock_guard<mutex> lock(refs_mutex_);
      --refs_;
      assert(refs_ >= 0);
      if (refs_ <= 0) {
        do_delete = true;
      }
    }

    if (do_delete) {
      delete this;
    }
  }

  uint64_t Size() const { return size_; }

  Status Read(uint64_t offset, size_t n, Slice* result, uint8_t* scratch) const {
    if (offset > size_) {
      return Status::IOError("Offset greater than file size.");
    }
    const uint64_t available = size_ - offset;
    if (n > available) {
      n = available;
    }
    if (n == 0) {
      *result = Slice();
      return Status::OK();
    }

    size_t block = offset / kBlockSize;
    size_t block_offset = offset % kBlockSize;

    if (n <= kBlockSize - block_offset) {
      // The requested bytes are all in the first block.
      *result = Slice(blocks_[block] + block_offset, n);
      return Status::OK();
    }

    size_t bytes_to_copy = n;
    uint8_t* dst = scratch;

    while (bytes_to_copy > 0) {
      size_t avail = kBlockSize - block_offset;
      if (avail > bytes_to_copy) {
        avail = bytes_to_copy;
      }
      memcpy(dst, blocks_[block] + block_offset, avail);

      bytes_to_copy -= avail;
      dst += avail;
      block++;
      block_offset = 0;
    }

    *result = Slice(scratch, n);
    return Status::OK();
  }

  Status PreAllocate(uint64_t size) {
    uint8_t *padding = new uint8_t[size];
    // TODO optimize me
    memset(&padding, 0, sizeof(uint8_t));
    Status s = AppendRaw(padding, size);
    delete [] padding;
    size_ -= size;
    return s;
  }

  Status Append(const Slice& data) {
    return AppendRaw(data.data(), data.size());
  }

  Status AppendRaw(const uint8_t *src, size_t src_len) {
    while (src_len > 0) {
      size_t avail;
      size_t offset = size_ % kBlockSize;

      if (offset != 0) {
        // There is some room in the last block.
        avail = kBlockSize - offset;
      } else {
        // No room in the last block; push new one.
        blocks_.push_back(new uint8_t[kBlockSize]);
        avail = kBlockSize;
      }

      if (avail > src_len) {
        avail = src_len;
      }
      memcpy(blocks_.back() + offset, src, avail);
      src_len -= avail;
      src += avail;
      size_ += avail;
    }

    return Status::OK();
  }

 private:
  // Private since only Unref() should be used to delete it.
  ~FileState() {
    for (vector<uint8_t*>::iterator i = blocks_.begin(); i != blocks_.end();
         ++i) {
      delete [] *i;
    }
  }

  // No copying allowed.
  FileState(const FileState&);
  void operator=(const FileState&);

  mutex refs_mutex_;
  int refs_;  // Protected by refs_mutex_;

  // The following fields are not protected by any mutex. They are only mutable
  // while the file is being written, and concurrent access is not allowed
  // to writable files.
  vector<uint8_t*> blocks_;
  uint64_t size_;

  enum { kBlockSize = 8 * 1024 };
};

class SequentialFileImpl : public SequentialFile {
 public:
  explicit SequentialFileImpl(FileState* file) : file_(file), pos_(0) {
    file_->Ref();
  }

  ~SequentialFileImpl() {
    file_->Unref();
  }

  virtual Status Read(size_t n, Slice* result, uint8_t* scratch) OVERRIDE {
    Status s = file_->Read(pos_, n, result, scratch);
    if (s.ok()) {
      pos_ += result->size();
    }
    return s;
  }

  virtual Status Skip(uint64_t n) OVERRIDE {
    if (pos_ > file_->Size()) {
      return Status::IOError("pos_ > file_->Size()");
    }
    const size_t available = file_->Size() - pos_;
    if (n > available) {
      n = available;
    }
    pos_ += n;
    return Status::OK();
  }

 private:
  FileState* file_;
  size_t pos_;
};

class RandomAccessFileImpl : public RandomAccessFile {
 public:
  explicit RandomAccessFileImpl(FileState* file) : file_(file) {
    file_->Ref();
  }

  ~RandomAccessFileImpl() {
    file_->Unref();
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      uint8_t* scratch) const OVERRIDE {
    return file_->Read(offset, n, result, scratch);
  }

  virtual Status Size(uint64_t *size) const OVERRIDE {
    *size = file_->Size();
    return Status::OK();
  }

 private:
  FileState* file_;
};

class WritableFileImpl : public WritableFile {
 public:
  explicit WritableFileImpl(FileState* file) : file_(file) {
    file_->Ref();
  }

  ~WritableFileImpl() {
    file_->Unref();
  }

  virtual Status PreAllocate(uint64_t size) OVERRIDE {
    return file_->PreAllocate(size);
  }

  virtual Status Append(const Slice& data) OVERRIDE {
    return file_->Append(data);
  }

  // This is a dummy implementation that simply serially appends all
  // slices using regular I/O.
  virtual Status AppendVector(const vector<Slice>& data_vector) OVERRIDE {
    BOOST_FOREACH(const Slice& data, data_vector) {
      RETURN_NOT_OK(file_->Append(data));
    }
    return Status::OK();
  }

  virtual Status Close() OVERRIDE { return Status::OK(); }
  virtual Status Flush() OVERRIDE { return Status::OK(); }
  virtual Status Sync() OVERRIDE { return Status::OK(); }
  virtual uint64_t Size() const OVERRIDE { return file_->Size(); }

 private:
  FileState* file_;
};

class InMemoryEnv : public EnvWrapper {
 public:
  explicit InMemoryEnv(Env* base_env) : EnvWrapper(base_env) { }

  virtual ~InMemoryEnv() {
    for (FileSystem::iterator i = file_map_.begin(); i != file_map_.end(); ++i) {
      i->second->Unref();
    }
  }

  // Partial implementation of the Env interface.
  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) OVERRIDE {
    lock_guard<mutex> lock(mutex_);
    if (file_map_.find(fname) == file_map_.end()) {
      *result = NULL;
      return Status::IOError(fname, "File not found");
    }

    *result = new SequentialFileImpl(file_map_[fname]);
    return Status::OK();
  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                                     RandomAccessFile** result) OVERRIDE {
    lock_guard<mutex> lock(mutex_);
    if (file_map_.find(fname) == file_map_.end()) {
      *result = NULL;
      return Status::IOError(fname, "File not found");
    }

    *result = new RandomAccessFileImpl(file_map_[fname]);
    return Status::OK();
  }

  virtual Status NewWritableFile(const WritableFileOptions& /* unused */,
                                 const std::string& fname,
                                 WritableFile** result) OVERRIDE {
    return NewWritableFile(fname, result);
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 WritableFile** result) OVERRIDE {
    lock_guard<mutex> lock(mutex_);
    if (file_map_.find(fname) != file_map_.end()) {
      DeleteFileInternal(fname);
    }

    FileState* file = new FileState();
    file->Ref();
    file_map_[fname] = file;

    *result = new WritableFileImpl(file);
    return Status::OK();
  }

  virtual bool FileExists(const std::string& fname) OVERRIDE {
    lock_guard<mutex> lock(mutex_);
    return file_map_.find(fname) != file_map_.end();
  }

  virtual Status GetChildren(const std::string& dir,
                             vector<std::string>* result) OVERRIDE {
    lock_guard<mutex> lock(mutex_);
    result->clear();

    for (FileSystem::iterator i = file_map_.begin(); i != file_map_.end(); ++i) {
      const std::string& filename = i->first;

      if (filename.size() >= dir.size() + 1 && filename[dir.size()] == '/' &&
          Slice(filename).starts_with(Slice(dir))) {
        result->push_back(filename.substr(dir.size() + 1));
      }
    }

    return Status::OK();
  }

  void DeleteFileInternal(const std::string& fname) {
    if (file_map_.find(fname) == file_map_.end()) {
      return;
    }

    file_map_[fname]->Unref();
    file_map_.erase(fname);
  }

  virtual Status DeleteFile(const std::string& fname) OVERRIDE {
    lock_guard<mutex> lock(mutex_);
    if (file_map_.find(fname) == file_map_.end()) {
      return Status::IOError(fname, "File not found");
    }

    DeleteFileInternal(fname);
    return Status::OK();
  }

  virtual Status CreateDir(const std::string& dirname) OVERRIDE {
    return Status::OK();
  }

  virtual Status DeleteDir(const std::string& dirname) OVERRIDE {
    return Status::OK();
  }

  virtual Status SyncDir(const std::string& dirname) OVERRIDE {
    return Status::OK();
  }

  virtual Status DeleteRecursively(const std::string& dirname) OVERRIDE {
    CHECK(!dirname.empty());
    string dir(dirname);
    if (dir[dir.size() - 1] != '/') {
      dir.push_back('/');
    }

    lock_guard<mutex> lock(mutex_);

    for (FileSystem::iterator i = file_map_.begin(); i != file_map_.end(); ) {
      const std::string& filename = i->first;

      if (filename.size() >= dir.size() && Slice(filename).starts_with(Slice(dir))) {
        file_map_.erase(i++);
      } else {
        ++i;
      }
    }

    return Status::OK();
  }

  virtual Status GetFileSize(const std::string& fname, uint64_t* file_size) OVERRIDE {
    lock_guard<mutex> lock(mutex_);
    if (file_map_.find(fname) == file_map_.end()) {
      return Status::IOError(fname, "File not found");
    }

    *file_size = file_map_[fname]->Size();
    return Status::OK();
  }

  virtual Status RenameFile(const std::string& src,
                            const std::string& target) OVERRIDE {
    lock_guard<mutex> lock(mutex_);
    if (file_map_.find(src) == file_map_.end()) {
      return Status::IOError(src, "File not found");
    }

    DeleteFileInternal(target);
    file_map_[target] = file_map_[src];
    file_map_.erase(src);
    return Status::OK();
  }

  virtual Status LockFile(const std::string& fname, FileLock** lock) OVERRIDE {
    *lock = new FileLock;
    return Status::OK();
  }

  virtual Status UnlockFile(FileLock* lock) OVERRIDE {
    delete lock;
    return Status::OK();
  }

  virtual Status GetTestDirectory(std::string* path) OVERRIDE {
    *path = "/test";
    return Status::OK();
  }

 private:
  // Map from filenames to FileState objects, representing a simple file system.
  typedef std::map<std::string, FileState*> FileSystem;
  mutex mutex_;
  FileSystem file_map_;  // Protected by mutex_.
};

}  // namespace

Env* NewMemEnv(Env* base_env) {
  return new InMemoryEnv(base_env);
}

} // namespace kudu
