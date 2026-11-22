#include "runtime/objects.h"

#include <chrono>

#include "hle/guest_types.h"

namespace xbem::rt {

std::mutex& sync_mutex() {
  static std::mutex m;
  return m;
}

std::condition_variable& sync_cv() {
  static std::condition_variable cv;
  return cv;
}

void EventObject::set() {
  {
    std::lock_guard lock(sync_mutex());
    signaled_ = true;
  }
  sync_cv().notify_all();
}

void EventObject::reset() {
  std::lock_guard lock(sync_mutex());
  signaled_ = false;
}

bool SemaphoreObject::release(int32_t count, int32_t* previous) {
  {
    std::lock_guard lock(sync_mutex());
    if (count <= 0 || count_ > max_ - count) return false;
    if (previous) *previous = count_;
    count_ += count;
  }
  sync_cv().notify_all();
  return true;
}

bool MutexObject::release(uint32_t tid) {
  {
    std::lock_guard lock(sync_mutex());
    if (owner_ != tid || recursion_ == 0) return false;
    if (--recursion_ == 0) owner_ = 0;
  }
  sync_cv().notify_all();
  return true;
}

uint64_t HandleTable::insert(std::shared_ptr<KernelObject> object) {
  std::lock_guard lock(mutex_);
  const uint64_t h = next_;
  next_ += 4;  // Windows handle values are multiples of 4
  objects_.emplace(h, std::move(object));
  return h;
}

std::shared_ptr<KernelObject> HandleTable::get(uint64_t handle) const {
  std::lock_guard lock(mutex_);
  auto it = objects_.find(handle);
  return it == objects_.end() ? nullptr : it->second;
}

bool HandleTable::close(uint64_t handle) {
  std::lock_guard lock(mutex_);
  return objects_.erase(handle) != 0;
}

size_t HandleTable::size() const {
  std::lock_guard lock(mutex_);
  return objects_.size();
}

uint32_t wait_for_objects(std::span<const std::shared_ptr<KernelObject>> objects, bool wait_all,
                          uint32_t timeout_ms, uint32_t thread_id) {
  if (objects.empty()) return win::WAIT_FAILED;
  for (const auto& o : objects)
    if (!o || !o->waitable()) return win::WAIT_FAILED;

  std::unique_lock lock(sync_mutex());
  auto ready_index = [&]() -> int {
    if (wait_all) {
      for (const auto& o : objects)
        if (!o->is_signaled(thread_id)) return -1;
      return 0;
    }
    for (size_t i = 0; i < objects.size(); ++i)
      if (objects[i]->is_signaled(thread_id)) return static_cast<int>(i);
    return -1;
  };

  int idx = ready_index();
  if (idx < 0 && timeout_ms != 0) {
    if (timeout_ms == win::INFINITE) {
      sync_cv().wait(lock, [&] { return (idx = ready_index()) >= 0; });
    } else {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
      sync_cv().wait_until(lock, deadline, [&] { return (idx = ready_index()) >= 0; });
    }
  }
  if (idx < 0) return win::WAIT_TIMEOUT;
  if (wait_all) {
    for (const auto& o : objects) o->acquire(thread_id);
  } else {
    objects[static_cast<size_t>(idx)]->acquire(thread_id);
  }
  return win::WAIT_OBJECT_0 + static_cast<uint32_t>(idx);
}

}  // namespace xbem::rt
