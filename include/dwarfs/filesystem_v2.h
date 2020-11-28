/* vim:set ts=2 sw=2 sts=2 et: */
/**
 * \author     Marcus Holland-Moritz (github@mhxnet.de)
 * \copyright  Copyright (c) Marcus Holland-Moritz
 *
 * This file is part of dwarfs.
 *
 * dwarfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dwarfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dwarfs.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <ostream>
#include <string>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include "dwarfs/error.h"
#include "dwarfs/logger.h"
#include "dwarfs/metadata_v2.h"
#include "dwarfs/mmif.h"

namespace dwarfs {

struct iovec_read_buf;
struct block_cache_options;

class filesystem_writer;
class progress;

class filesystem_v2 {
 public:
  filesystem_v2(logger& lgr, std::shared_ptr<mmif> mm,
                const block_cache_options& bc_options,
                const struct ::stat* stat_defaults = nullptr,
                int inode_offset = 0);

  static void rewrite(logger& lgr, progress& prog, std::shared_ptr<mmif> mm,
                      filesystem_writer& writer);

  static void identify(logger& lgr, std::shared_ptr<mmif> mm, std::ostream& os,
                       int detail_level = 0);

  void dump(std::ostream& os, int detail_level) const {
    impl_->dump(os, detail_level);
  }

  void walk(std::function<void(entry_view)> const& func) const {
    impl_->walk(func);
  }

  std::optional<entry_view> find(const char* path) const {
    return impl_->find(path);
  }

  std::optional<entry_view> find(int inode) const { return impl_->find(inode); }

  std::optional<entry_view> find(int inode, const char* name) const {
    return impl_->find(inode, name);
  }

  int getattr(entry_view entry, struct ::stat* stbuf) const {
    return impl_->getattr(entry, stbuf);
  }

  int access(entry_view entry, int mode, uid_t uid, gid_t gid) const {
    return impl_->access(entry, mode, uid, gid);
  }

  std::optional<directory_view> opendir(entry_view entry) const {
    return impl_->opendir(entry);
  }

  std::optional<std::pair<entry_view, std::string_view>>
  readdir(directory_view dir, size_t offset) const {
    return impl_->readdir(dir, offset);
  }

  size_t dirsize(directory_view dir) const { return impl_->dirsize(dir); }

  int readlink(entry_view entry, std::string* buf) const {
    return impl_->readlink(entry, buf);
  }

  folly::Expected<std::string_view, int> readlink(entry_view entry) const {
    return impl_->readlink(entry);
  }

  int statvfs(struct ::statvfs* stbuf) const { return impl_->statvfs(stbuf); }

  int open(entry_view entry) const { return impl_->open(entry); }

  ssize_t read(uint32_t inode, char* buf, size_t size, off_t offset = 0) const {
    return impl_->read(inode, buf, size, offset);
  }

  ssize_t
  readv(uint32_t inode, iovec_read_buf& buf, size_t size, off_t offset) const {
    return impl_->readv(inode, buf, size, offset);
  }

  class impl {
   public:
    virtual ~impl() = default;

    virtual void dump(std::ostream& os, int detail_level) const = 0;
    virtual void walk(std::function<void(entry_view)> const& func) const = 0;
    virtual std::optional<entry_view> find(const char* path) const = 0;
    virtual std::optional<entry_view> find(int inode) const = 0;
    virtual std::optional<entry_view>
    find(int inode, const char* name) const = 0;
    virtual int getattr(entry_view entry, struct ::stat* stbuf) const = 0;
    virtual int
    access(entry_view entry, int mode, uid_t uid, gid_t gid) const = 0;
    virtual std::optional<directory_view> opendir(entry_view entry) const = 0;
    virtual std::optional<std::pair<entry_view, std::string_view>>
    readdir(directory_view dir, size_t offset) const = 0;
    virtual size_t dirsize(directory_view dir) const = 0;
    virtual int readlink(entry_view entry, std::string* buf) const = 0;
    virtual folly::Expected<std::string_view, int>
    readlink(entry_view entry) const = 0;
    virtual int statvfs(struct ::statvfs* stbuf) const = 0;
    virtual int open(entry_view entry) const = 0;
    virtual ssize_t
    read(uint32_t inode, char* buf, size_t size, off_t offset) const = 0;
    virtual ssize_t readv(uint32_t inode, iovec_read_buf& buf, size_t size,
                          off_t offset) const = 0;
  };

 private:
  std::unique_ptr<impl> impl_;
};
} // namespace dwarfs
