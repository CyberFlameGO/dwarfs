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

#include <condition_variable>
#include <mutex>

#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "dwarfs/filesystem_extractor.h"
#include "dwarfs/filesystem_v2.h"
#include "dwarfs/fstypes.h"
#include "dwarfs/logger.h"
#include "dwarfs/options.h"
#include "dwarfs/worker_group.h"

namespace dwarfs {

namespace {

class cache_semaphore {
 public:
  void post(int64_t n) {
    {
      std::lock_guard lock(mx_);
      size_ += n;
      ++count_;
    }
    condition_.notify_one();
  }

  void wait(int64_t n) {
    std::unique_lock lock(mx_);
    while (size_ < n && count_ <= 0) {
      condition_.wait(lock);
    }
    size_ -= n;
    --count_;
  }

 private:
  std::mutex mx_;
  std::condition_variable condition_;
  int64_t count_{0};
  int64_t size_{0};
};

} // namespace

template <typename LoggerPolicy>
class filesystem_extractor_ final : public filesystem_extractor::impl {
 public:
  explicit filesystem_extractor_(logger& lgr)
      : log_{lgr} {}

  ~filesystem_extractor_() override {
    try {
      close();
    } catch (std::exception const& e) {
      LOG_ERROR << "close() failed in destructor: " << e.what();
    } catch (...) {
      LOG_ERROR << "close() failed in destructor";
    }
  }

  void
  open_archive(std::string const& output, std::string const& format) override {
    a_ = ::archive_write_new();

    check_result(::archive_write_set_format_by_name(a_, format.c_str()));
    check_result(::archive_write_open_filename(
        a_, output.empty() ? nullptr : output.c_str()));
  }

  void open_disk(std::string const& output) override {
    if (!output.empty()) {
      if (::chdir(output.c_str()) != 0) {
        DWARFS_THROW(runtime_error,
                     output + ": " + std::string(strerror(errno)));
      }
    }

    a_ = ::archive_write_disk_new();

    check_result(::archive_write_disk_set_options(
        a_,
        ARCHIVE_EXTRACT_OWNER | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_TIME |
            ARCHIVE_EXTRACT_UNLINK | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS |
            ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_SYMLINKS));
  }

  void close() override {
    if (a_) {
      check_result(::archive_write_free(a_));
      a_ = nullptr;
    }
  }

  void extract(filesystem_v2& fs, size_t max_queued_bytes) override;

 private:
  void check_result(int res) {
    switch (res) {
    case ARCHIVE_OK:
      break;
    case ARCHIVE_WARN:
      LOG_WARN << std::string(archive_error_string(a_));
      break;
    case ARCHIVE_RETRY:
    case ARCHIVE_FATAL:
      DWARFS_THROW(runtime_error, std::string(archive_error_string(a_)));
    }
  }

  log_proxy<debug_logger_policy> log_;
  struct ::archive* a_{nullptr};
};

template <typename LoggerPolicy>
void filesystem_extractor_<LoggerPolicy>::extract(filesystem_v2& fs,
                                                  size_t max_queued_bytes) {
  DWARFS_CHECK(a_, "filesystem not opened");

  auto lr = ::archive_entry_linkresolver_new();

  if (auto fmt = ::archive_format(a_)) {
    ::archive_entry_linkresolver_set_strategy(lr, fmt);
  }

  ::archive_entry* spare = nullptr;

  worker_group archiver("archiver", 1);
  cache_semaphore sem;

  sem.post(max_queued_bytes);

  auto do_archive = [&](::archive_entry* ae,
                        inode_view entry) { // TODO: inode vs. entry
    if (auto size = ::archive_entry_size(ae);
        S_ISREG(entry.mode()) && size > 0) {
      auto fd = fs.open(entry);

      sem.wait(size);

      if (auto ranges = fs.readv(fd, size, 0)) {
        archiver.add_job(
            [this, &sem, ranges = std::move(*ranges), ae, size]() mutable {
              LOG_TRACE << "archiving " << ::archive_entry_pathname(ae);
              check_result(::archive_write_header(a_, ae));
              for (auto& r : ranges) {
                auto br = r.get();
                LOG_TRACE << "writing " << br.size() << " bytes";
                check_result(::archive_write_data(a_, br.data(), br.size()));
              }
              sem.post(size);
              ::archive_entry_free(ae);
            });
      } else {
        LOG_ERROR << "error reading inode [" << fd
                  << "]: " << ::strerror(-ranges.error());
      }
    } else {
      archiver.add_job([this, ae] {
        check_result(::archive_write_header(a_, ae));
        ::archive_entry_free(ae);
      });
    }
  };

  fs.walk_inode_order([&](auto entry) {
    if (entry.is_root()) {
      return;
    }

    auto inode = entry.inode();

    auto ae = ::archive_entry_new();
    struct ::stat stbuf;

    if (fs.getattr(inode, &stbuf) != 0) {
      DWARFS_THROW(runtime_error, "getattr() failed");
    }

    ::archive_entry_set_pathname(ae, entry.path().c_str());
    ::archive_entry_copy_stat(ae, &stbuf);

    if (S_ISLNK(inode.mode())) {
      std::string link;
      if (fs.readlink(inode, &link) != 0) {
        LOG_ERROR << "readlink() failed";
      }
      ::archive_entry_set_symlink(ae, link.c_str());
    }

    ::archive_entry_linkify(lr, &ae, &spare);

    if (ae) {
      do_archive(ae, inode);
    }

    if (spare) {
      auto ev = fs.find(::archive_entry_ino(spare));
      if (!ev) {
        LOG_ERROR << "find() failed";
      }
      LOG_INFO << "archiving spare " << ::archive_entry_pathname(spare);
      do_archive(spare, *ev);
    }
  });

  archiver.wait();

  // As we're visiting *all* hardlinks, we should never see any deferred
  // entries.
  ::archive_entry* ae = nullptr;
  ::archive_entry_linkify(lr, &ae, &spare);
  if (ae) {
    DWARFS_THROW(runtime_error, "unexpected deferred entry");
  }

  ::archive_entry_linkresolver_free(lr);
}

filesystem_extractor::filesystem_extractor(logger& lgr)
    : impl_(make_unique_logging_object<filesystem_extractor::impl,
                                       filesystem_extractor_, logger_policies>(
          lgr)) {}

} // namespace dwarfs
