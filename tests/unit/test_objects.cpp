#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "hle/guest_types.h"
#include "runtime/objects.h"
#include "test.h"

using namespace xbem;
using rt::KernelObject;

namespace {
uint32_t wait1(const std::shared_ptr<KernelObject>& o, uint32_t ms, uint32_t tid = 4) {
  std::vector<std::shared_ptr<KernelObject>> v{o};
  return rt::wait_for_objects(v, true, ms, tid);
}
}  // namespace

TEST(handle_table) {
  rt::HandleTable t;
  auto e = std::make_shared<rt::EventObject>(true, false);
  const uint64_t h1 = t.insert(e);
  const uint64_t h2 = t.insert(e);
  CHECK(h1 != h2);
  CHECK(h1 % 4 == 0);
  CHECK(t.get(h1) == e);
  CHECK(t.get_as<rt::EventObject>(h2) == e);
  CHECK(t.get_as<rt::SemaphoreObject>(h2) == nullptr);
  CHECK(t.close(h1));
  CHECK(!t.close(h1));
  CHECK(t.get(h1) == nullptr);
  CHECK_EQ(t.size(), 1u);
}

TEST(event_auto_and_manual_reset) {
  auto autoev = std::make_shared<rt::EventObject>(false, true);
  CHECK_EQ(wait1(autoev, 0), win::WAIT_OBJECT_0);
  CHECK_EQ(wait1(autoev, 0), win::WAIT_TIMEOUT);  // consumed
  auto manual = std::make_shared<rt::EventObject>(true, false);
  CHECK_EQ(wait1(manual, 5), win::WAIT_TIMEOUT);
  manual->set();
  CHECK_EQ(wait1(manual, 0), win::WAIT_OBJECT_0);
  CHECK_EQ(wait1(manual, 0), win::WAIT_OBJECT_0);  // stays signaled
  manual->reset();
  CHECK_EQ(wait1(manual, 0), win::WAIT_TIMEOUT);
}

TEST(event_cross_thread_wakeup) {
  auto ev = std::make_shared<rt::EventObject>(false, false);
  std::thread setter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ev->set();
  });
  CHECK_EQ(wait1(ev, 5000), win::WAIT_OBJECT_0);
  setter.join();
}

TEST(semaphore_counts) {
  auto s = std::make_shared<rt::SemaphoreObject>(1, 2);
  CHECK_EQ(wait1(s, 0), win::WAIT_OBJECT_0);
  CHECK_EQ(wait1(s, 0), win::WAIT_TIMEOUT);
  int32_t prev = -1;
  CHECK(s->release(2, &prev));
  CHECK_EQ(prev, 0);
  CHECK(!s->release(1, nullptr));  // would exceed the maximum
  CHECK_EQ(wait1(s, 0), win::WAIT_OBJECT_0);
  CHECK_EQ(wait1(s, 0), win::WAIT_OBJECT_0);
  CHECK_EQ(wait1(s, 0), win::WAIT_TIMEOUT);
}

TEST(mutex_ownership_and_recursion) {
  auto m = std::make_shared<rt::MutexObject>(0);
  CHECK_EQ(wait1(m, 0, 8), win::WAIT_OBJECT_0);
  CHECK_EQ(wait1(m, 0, 8), win::WAIT_OBJECT_0);  // recursive acquire by the owner
  CHECK_EQ(wait1(m, 0, 12), win::WAIT_TIMEOUT);  // other thread id
  CHECK(!m->release(12));
  CHECK(m->release(8));
  CHECK_EQ(wait1(m, 0, 12), win::WAIT_TIMEOUT);  // still held once
  CHECK(m->release(8));
  CHECK_EQ(wait1(m, 0, 12), win::WAIT_OBJECT_0);
}

TEST(wait_any_and_all) {
  auto a = std::make_shared<rt::EventObject>(true, false);
  auto b = std::make_shared<rt::EventObject>(true, true);
  std::vector<std::shared_ptr<KernelObject>> v{a, b};
  CHECK_EQ(rt::wait_for_objects(v, false, 0, 4), win::WAIT_OBJECT_0 + 1);
  CHECK_EQ(rt::wait_for_objects(v, true, 0, 4), win::WAIT_TIMEOUT);
  a->set();
  CHECK_EQ(rt::wait_for_objects(v, true, 0, 4), win::WAIT_OBJECT_0);
  std::vector<std::shared_ptr<KernelObject>> bad{nullptr};
  CHECK_EQ(rt::wait_for_objects(bad, false, 0, 4), win::WAIT_FAILED);
}
