// Copyright 2025 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/page-pool.h"

#include <algorithm>

#include "src/base/platform/mutex.h"
#include "src/execution/isolate.h"
#include "src/heap/large-page-metadata.h"
#include "src/heap/memory-allocator.h"
#include "src/heap/mutable-page-metadata.h"
#include "src/tasks/cancelable-task.h"

namespace v8::internal {

template <typename PoolEntry>
void PagePool::PoolImpl<PoolEntry>::TearDown() {
  DCHECK(local_pools_.empty());
  shared_pool_.clear();
}

template <typename PoolEntry>
void PagePool::PoolImpl<PoolEntry>::PutLocal(Isolate* isolate,
                                             PoolEntry entry) {
  base::MutexGuard guard(&mutex_);
  local_pools_[isolate].emplace_back(std::move(entry));
}

template <typename PoolEntry>
std::optional<PoolEntry> PagePool::PoolImpl<PoolEntry>::Get(Isolate* isolate) {
  DCHECK_NOT_NULL(isolate);
  base::MutexGuard guard(&mutex_);

  // Try to get a page from the page pool for the given isolate first.
  auto it = local_pools_.find(isolate);

  if (it == local_pools_.end()) {
    // Pages in this pool will be flushed soon, reuse them.
    if (!shared_pool_.empty()) {
      auto& shared_entries = shared_pool_.back().second;
      if (!shared_entries.empty()) {
        PoolEntry shared_entry = std::move(shared_entries.back());
        shared_entries.pop_back();
        if (shared_entries.empty()) {
          shared_pool_.pop_back();
        }
        return {std::move(shared_entry)};
      }
    }
  }

  if (it != local_pools_.end()) {
    DCHECK(!it->second.empty());
    std::vector<PoolEntry>& entries = it->second;
    if (!entries.empty()) {
      PoolEntry local_entry = std::move(entries.back());
      entries.pop_back();
      if (entries.empty()) {
        local_pools_.erase(it);
      }
      return {std::move(local_entry)};
    }
  }

  return std::nullopt;
}

template <typename PoolEntry>
bool PagePool::PoolImpl<PoolEntry>::MoveLocalToShared(
    Isolate* isolate, InternalTime release_time) {
  base::MutexGuard guard(&mutex_);
  auto it = local_pools_.find(isolate);
  if (it != local_pools_.end()) {
    DCHECK(!it->second.empty());
    shared_pool_.push_back(std::make_pair(release_time, std::move(it->second)));
    local_pools_.erase(it);
  }
  return !shared_pool_.empty();
}

template <typename PoolEntry>
void PagePool::PoolImpl<PoolEntry>::ReleaseShared() {
  std::vector<std::pair<InternalTime, std::vector<PoolEntry>>> entries_to_free;
  {
    base::MutexGuard guard(&mutex_);
    entries_to_free = std::move(shared_pool_);
    shared_pool_.clear();
  }
  // Entries will be freed automatically here.
}

template <typename PoolEntry>
void PagePool::PoolImpl<PoolEntry>::ReleaseLocal() {
  std::vector<std::vector<PoolEntry>> entries_to_free;
  {
    base::MutexGuard page_guard(&mutex_);
    for (auto& entry : local_pools_) {
      entries_to_free.push_back(std::move(entry.second));
    }
    local_pools_.clear();
  }
  // Entries will be freed automatically here.
}

template <typename PoolEntry>
void PagePool::PoolImpl<PoolEntry>::ReleaseLocal(Isolate* isolate) {
  std::vector<PoolEntry> entries_to_free;
  {
    base::MutexGuard page_guard(&mutex_);
    const auto it = local_pools_.find(isolate);
    if (it != local_pools_.end()) {
      DCHECK(!it->second.empty());
      entries_to_free = std::move(it->second);
      local_pools_.erase(it);
    }
  }
  // Entries will be freed automatically here.
}

template <typename PoolEntry>
size_t PagePool::PoolImpl<PoolEntry>::Size() const {
  base::MutexGuard guard(&mutex_);
  size_t count = 0;
  for (const auto& entry : local_pools_) {
    count += entry.second.size();
  }
  for (const auto& entry : shared_pool_) {
    count += entry.second.size();
  }
  return count;
}

template <typename PoolEntry>
size_t PagePool::PoolImpl<PoolEntry>::LocalSize(Isolate* isolate) const {
  base::MutexGuard guard(&mutex_);
  const auto it = local_pools_.find(isolate);
  return (it != local_pools_.end()) ? it->second.size() : 0;
}

template <typename PoolEntry>
size_t PagePool::PoolImpl<PoolEntry>::SharedSize() const {
  base::MutexGuard guard(&mutex_);
  size_t count = 0;
  for (const auto& entry : shared_pool_) {
    count += entry.second.size();
  }
  return count;
}

template <typename PoolEntry>
size_t PagePool::PoolImpl<PoolEntry>::ReleaseUpTo(InternalTime release_time) {
  std::vector<std::vector<PoolEntry>> entries_to_free;
  size_t freed = 0;
  {
    base::MutexGuard guard(&mutex_);
    std::erase_if(shared_pool_,
                  [&entries_to_free, &freed, release_time](auto& entry) {
                    if (entry.first <= release_time) {
                      freed += entry.second.size();
                      entries_to_free.push_back(std::move(entry.second));
                      return true;
                    }
                    return false;
                  });
  }
  // Entries will be freed automatically here.
  return freed;
}

bool PagePool::LargePagePoolImpl::Add(std::vector<LargePageMetadata*>& pages,
                                      InternalTime time) {
  bool added_to_pool = false;
  base::MutexGuard guard(&mutex_);
  DCHECK_EQ(total_size_, ComputeTotalSize());

  const size_t max_total_size = v8_flags.max_large_page_pool_size * MB;

  std::erase_if(pages, [this, &added_to_pool, time,
                        max_total_size](LargePageMetadata* page) {
    if (total_size_ + page->size() > max_total_size) {
      return false;
    }

    total_size_ += page->size();
    pages_.emplace_back(time,
                        LargePageMemory(page, [](LargePageMetadata* metadata) {
                          MemoryAllocator::DeleteMemoryChunk(metadata);
                        }));
    added_to_pool = true;
    return true;
  });

  DCHECK_EQ(total_size_, ComputeTotalSize());
  return added_to_pool;
}

LargePageMetadata* PagePool::LargePagePoolImpl::Remove(size_t chunk_size) {
  base::MutexGuard guard(&mutex_);
  auto selected = pages_.end();
  DCHECK_EQ(total_size_, ComputeTotalSize());

  // Best-fit: Select smallest large page large with a size of at least
  // |chunk_size|. In case the page is larger than necessary, the next full GC
  // will trim down its size.
  for (auto it = pages_.begin(); it != pages_.end(); it++) {
    const size_t page_size = it->second->size();
    if (page_size < chunk_size) continue;
    if (selected == pages_.end() || page_size < selected->second->size()) {
      selected = it;
    }
  }

  if (selected == pages_.end()) {
    return nullptr;
  }

  LargePageMetadata* result = selected->second.release();
  total_size_ -= result->size();
  pages_.erase(selected);
  DCHECK_EQ(total_size_, ComputeTotalSize());
  return result;
}

void PagePool::LargePagePoolImpl::ReleaseAll() {
  std::vector<std::pair<InternalTime, LargePageMemory>> pages_to_free;

  {
    base::MutexGuard guard(&mutex_);
    pages_to_free = std::move(pages_);
    total_size_ = 0;
  }

  // Entries will be freed automatically here.
}

size_t PagePool::LargePagePoolImpl::ReleaseUpTo(InternalTime release_time) {
  std::vector<LargePageMemory> entries_to_free;
  size_t freed = 0;
  {
    base::MutexGuard guard(&mutex_);
    std::erase_if(pages_, [this, &entries_to_free, release_time](auto& entry) {
      if (entry.first <= release_time) {
        total_size_ -= entry.second->size();
        entries_to_free.push_back(std::move(entry.second));
        return true;
      }
      return false;
    });

    DCHECK_EQ(total_size_, ComputeTotalSize());
  }
  // Entries will be freed automatically here.
  return freed;
}

size_t PagePool::LargePagePoolImpl::ComputeTotalSize() const {
  size_t result = 0;

  for (auto& entry : pages_) {
    result += entry.second->size();
  }

  return result;
}

void PagePool::LargePagePoolImpl::TearDown() { CHECK(pages_.empty()); }

PagePool::~PagePool() = default;

class PagePool::ReleasePooledChunksTask final : public CancelableTask {
 public:
  ReleasePooledChunksTask(Isolate* isolate, PagePool* pool,
                          InternalTime release_time)
      : CancelableTask(isolate),
        isolate_(isolate),
        pool_(pool),
        release_time_(release_time) {}

  ~ReleasePooledChunksTask() override = default;
  ReleasePooledChunksTask(const ReleasePooledChunksTask&) = delete;
  ReleasePooledChunksTask& operator=(const ReleasePooledChunksTask&) = delete;

 private:
  void RunInternal() override { pool_->ReleaseUpTo(isolate_, release_time_); }

  Isolate* const isolate_;
  PagePool* const pool_;
  const InternalTime release_time_;
};

void PagePool::ReleaseOnTearDown(Isolate* isolate) {
  if (!v8_flags.memory_pool_share_memory_on_teardown) {
    ReleaseImmediately(isolate);
    return;
  }

  const InternalTime time = next_time_.fetch_add(1, std::memory_order_relaxed);

  const bool shared_page_pool_populate =
      page_pool_.MoveLocalToShared(isolate, time);
  const bool shared_zone_pool_populated =
      zone_pool_.MoveLocalToShared(isolate, time);

  // Always post task when there are pages in the shared pool.
  if (shared_page_pool_populate || shared_zone_pool_populated) {
    auto schedule_task = [this, isolate, time](Isolate* target_isolate) {
      DCHECK_NE(isolate, target_isolate);
      USE(isolate);
      static constexpr base::TimeDelta kReleaseTaskDelayInSeconds =
          base::TimeDelta::FromSeconds(8);
      target_isolate->task_runner()->PostDelayedTask(
          std::make_unique<ReleasePooledChunksTask>(target_isolate, this, time),
          kReleaseTaskDelayInSeconds.InSecondsF());
    };

    if (!isolate->isolate_group()->FindAnotherIsolateLocked(isolate,
                                                            schedule_task)) {
      // No other isolate could be found. Release pooled pages right away.
      page_pool_.ReleaseShared();
      zone_pool_.ReleaseShared();
    }
  }

  large_pool_.ReleaseAll();
}

void PagePool::ReleaseImmediately(Isolate* isolate) {
  page_pool_.ReleaseLocal(isolate);
  zone_pool_.ReleaseLocal(isolate);
  large_pool_.ReleaseAll();
}

void PagePool::ReleaseLargeImmediately() { large_pool_.ReleaseAll(); }

void PagePool::TearDown() {
  page_pool_.TearDown();
  zone_pool_.TearDown();
  large_pool_.TearDown();
}

void PagePool::ReleaseUpTo(Isolate* isolate_for_printing,
                           InternalTime release_time) {
  const auto pages_removed = page_pool_.ReleaseUpTo(release_time);
  const auto zone_reservations_removed = zone_pool_.ReleaseUpTo(release_time);
  if (v8_flags.trace_gc_nvp) {
    isolate_for_printing->PrintWithTimestamp(
        "Shared pool: Removed pages: %zu removed zone reservations: %zu\n",
        pages_removed, zone_reservations_removed);
  }
}

size_t PagePool::GetCount(Isolate* isolate) const {
  return page_pool_.LocalSize(isolate);
}

size_t PagePool::GetSharedCount() const { return page_pool_.SharedSize(); }

size_t PagePool::GetTotalCount() const { return page_pool_.Size(); }

void PagePool::Add(Isolate* isolate, MutablePageMetadata* chunk) {
  DCHECK_NOT_NULL(isolate);
  // This method is called only on the main thread and only during the
  // atomic pause so a lock is not needed.
  DCHECK_NOT_NULL(chunk);
  DCHECK_EQ(chunk->size(), PageMetadata::kPageSize);
  DCHECK(!chunk->Chunk()->IsLargePage());
  DCHECK(!chunk->Chunk()->IsTrusted());
  DCHECK_NE(chunk->Chunk()->executable(), EXECUTABLE);
  // Ensure that ReleaseAllAllocatedMemory() was called on the page.
  DCHECK(!chunk->ContainsAnySlots());
  page_pool_.PutLocal(isolate,
                      PageMemory(chunk, [](MutablePageMetadata* metadata) {
                        MemoryAllocator::DeleteMemoryChunk(metadata);
                      }));
}

MutablePageMetadata* PagePool::Remove(Isolate* isolate) {
  DCHECK_NOT_NULL(isolate);
  auto result = page_pool_.Get(isolate);
  if (result) {
    return result.value().release();
  }
  return nullptr;
}

class PagePool::ReleasePooledLargeChunksTask final : public CancelableTask {
 public:
  ReleasePooledLargeChunksTask(Isolate* isolate, PagePool* pool,
                               InternalTime time)
      : CancelableTask(isolate), pool_(pool), time_(time) {}

  ~ReleasePooledLargeChunksTask() override = default;
  ReleasePooledLargeChunksTask(const ReleasePooledLargeChunksTask&) = delete;
  ReleasePooledLargeChunksTask& operator=(const ReleasePooledLargeChunksTask&) =
      delete;

 private:
  // v8::internal::CancelableTask overrides.
  void RunInternal() override { pool_->large_pool_.ReleaseUpTo(time_); }

  PagePool* pool_;
  InternalTime time_;
};

void PagePool::AddLarge(Isolate* isolate,
                        std::vector<LargePageMetadata*>& pages) {
  const InternalTime time = next_time_.fetch_add(1, std::memory_order_relaxed);
  const bool added_to_pool = large_pool_.Add(pages, time);
  const int timeout = v8_flags.large_page_pool_timeout;
  if (added_to_pool && timeout > 0) {
    const base::TimeDelta large_page_release_task_delay =
        base::TimeDelta::FromSeconds(timeout);
    auto task =
        std::make_unique<ReleasePooledLargeChunksTask>(isolate, this, time);
    if (v8_flags.single_threaded) {
      isolate->task_runner()->PostDelayedTask(
          std::move(task), large_page_release_task_delay.InSecondsF());
    } else {
      V8::GetCurrentPlatform()->PostDelayedTaskOnWorkerThread(
          TaskPriority::kBestEffort, std::move(task),
          large_page_release_task_delay.InSecondsF());
    }
  }
}

LargePageMetadata* PagePool::RemoveLarge(Isolate* isolate, size_t chunk_size) {
  return large_pool_.Remove(chunk_size);
}

void PagePool::AddZoneReservation(Isolate* isolate,
                                  VirtualMemory zone_reservation) {
  DCHECK_NOT_NULL(isolate);
  zone_pool_.PutLocal(isolate, std::move(zone_reservation));
}

std::optional<VirtualMemory> PagePool::RemoveZoneReservation(Isolate* isolate) {
  DCHECK_NOT_NULL(isolate);
  return zone_pool_.Get(isolate);
}

}  // namespace v8::internal
