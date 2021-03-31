/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/experimental/symbolizer/ElfCache.h>

#include <link.h>
#include <signal.h>

#include <folly/ScopeGuard.h>
#include <folly/portability/SysMman.h>

/*
 * This is declared in `link.h' on Linux platforms, but apparently not on the
 * Mac version of the file.  It's harmless to declare again, in any case.
 *
 * Note that declaring it with `extern "C"` results in linkage conflicts.
 */
extern struct r_debug _r_debug;

namespace folly {
namespace symbolizer {

size_t countLoadedElfFiles() {
  // _r_debug synchronization is... lacking to say the least. It's
  // meant as an aid for debuggers and synchronization is done by
  // calling dl_debug_state() which debuggers are supposed to
  // intercept by setting a breakpoint on.

  // Can't really do that here, so we apply the hope-and-pray strategy.
  if (_r_debug.r_version != 1 || _r_debug.r_state != r_debug::RT_CONSISTENT) {
    // computo ergo sum
    return 1;
  }

  //     r_map       -> head of a linked list of 'link_map_t' entries,
  //                    one per ELF 'binary' in the process address space.
  size_t count = 0;
  for (auto lmap = _r_debug.r_map; lmap != nullptr; lmap = lmap->l_next) {
    ++count;
  }
  return count;
}

std::shared_ptr<ElfFile> SignalSafeElfCache::getFile(StringPiece p) {
  struct cmp {
    bool operator()(Entry const& a, StringPiece b) const noexcept {
      return a.path < b;
    }
    bool operator()(StringPiece a, Entry const& b) const noexcept {
      return a < b.path;
    }
  };

  sigset_t newsigs;
  sigfillset(&newsigs);
  sigset_t oldsigs;
  sigemptyset(&oldsigs);
  sigprocmask(SIG_SETMASK, &newsigs, &oldsigs);
  SCOPE_EXIT {
    sigprocmask(SIG_SETMASK, &oldsigs, nullptr);
  };

  if (!state_) {
    state_.emplace();
  }

  auto pos = state_->map.find(p, cmp{});
  if (pos == state_->map.end()) {
    state_->list.emplace_front(p, state_->alloc);
    pos = state_->map.insert(state_->list.front()).first;
  }

  if (!pos->init) {
    int r = pos->file->openAndFollow(pos->path.c_str());
    pos->init = r == ElfFile::kSuccess;
  }
  if (!pos->init) {
    return nullptr;
  }

  return pos->file;
}

ElfCache::ElfCache(size_t capacity) : capacity_(capacity) {}

std::shared_ptr<ElfFile> ElfCache::getFile(StringPiece p) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto pos = files_.find(p);
  if (pos != files_.end()) {
    // Found, move to back (MRU)
    auto& entry = pos->second;
    lruList_.erase(lruList_.iterator_to(*entry));
    lruList_.push_back(*entry);
    return filePtr(entry);
  }

  auto entry = std::make_shared<Entry>();
  entry->path = p.str();
  auto& path = entry->path;

  // No negative caching
  int r = entry->file.openAndFollow(path.c_str());
  if (r != ElfFile::kSuccess) {
    return nullptr;
  }

  if (files_.size() == capacity_) {
    auto& e = lruList_.front();
    lruList_.pop_front();
    files_.erase(e.path);
  }

  files_.emplace(entry->path, entry);
  lruList_.push_back(*entry);

  return filePtr(entry);
}

std::shared_ptr<ElfFile> ElfCache::filePtr(const std::shared_ptr<Entry>& e) {
  // share ownership
  return std::shared_ptr<ElfFile>(e, &e->file);
}
} // namespace symbolizer
} // namespace folly