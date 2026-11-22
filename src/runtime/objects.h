#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

namespace xbem::rt {

// All waitable kernel objects share one mutex/condition variable pair. This is
// not fast, but it makes WaitForMultipleObjects (any/all) trivially correct.
// Performance work can replace it later without changing the HLE layer.
std::mutex& sync_mutex();
std::condition_variable& sync_cv();

class KernelObject {
 public:
  virtual ~KernelObject() = default;
  virtual const char* type_name() const = 0;
  virtual bool waitable() const { return false; }
  // Both called with sync_mutex() held.
  virtual bool is_signaled(uint32_t thread_id) const { return false; }
  virtual void acquire(uint32_t thread_id) {}
};

class EventObject final : public KernelObject {
 public:
  EventObject(bool manual_reset, bool initial) : manual_reset_(manual_reset), signaled_(initial) {}
  const char* type_name() const override { return "Event"; }
  bool waitable() const override { return true; }
  bool is_signaled(uint32_t) const override { return signaled_; }
  void acquire(uint32_t) override {
    if (!manual_reset_) signaled_ = false;
  }
  void set();
  void reset();

 private:
  bool manual_reset_;
  bool signaled_;
};

class SemaphoreObject final : public KernelObject {
 public:
  SemaphoreObject(int32_t initial, int32_t maximum) : count_(initial), max_(maximum) {}
  const char* type_name() const override { return "Semaphore"; }
  bool waitable() const override { return true; }
  bool is_signaled(uint32_t) const override { return count_ > 0; }
  void acquire(uint32_t) override { --count_; }
  // Returns false if the release would exceed the maximum count.
  bool release(int32_t count, int32_t* previous);

 private:
  int32_t count_;
  int32_t max_;
};

class MutexObject final : public KernelObject {
 public:
  explicit MutexObject(uint32_t initial_owner) : owner_(initial_owner), recursion_(initial_owner ? 1 : 0) {}
  const char* type_name() const override { return "Mutex"; }
  bool waitable() const override { return true; }
  bool is_signaled(uint32_t tid) const override { return owner_ == 0 || owner_ == tid; }
  void acquire(uint32_t tid) override {
    owner_ = tid;
    ++recursion_;
  }
  bool release(uint32_t tid);

 private:
  uint32_t owner_;
  uint32_t recursion_;
};

class HandleTable {
 public:
  uint64_t insert(std::shared_ptr<KernelObject> object);
  std::shared_ptr<KernelObject> get(uint64_t handle) const;
  template <class T>
  std::shared_ptr<T> get_as(uint64_t handle) const {
    return std::dynamic_pointer_cast<T>(get(handle));
  }
  bool close(uint64_t handle);
  size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<KernelObject>> objects_;
  uint64_t next_ = 0x100;
};

// Implements WaitForSingleObject / WaitForMultipleObjects semantics.
// Returns WAIT_OBJECT_0 + index, WAIT_TIMEOUT or WAIT_FAILED.
uint32_t wait_for_objects(std::span<const std::shared_ptr<KernelObject>> objects, bool wait_all,
                          uint32_t timeout_ms, uint32_t thread_id);

}  // namespace xbem::rt
