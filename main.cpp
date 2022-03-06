#include <cassert>
#include <chrono>
#include <thread>
#include <iostream>
#include <atomic>
#include <optional>

#include "thread_pool.hpp"

void set_true(std::atomic_bool *p) {
  *p = true;
}

void trivial_enqueue_test() {
  utils::thread_pool tp(1);
  std::atomic_bool runned = false;
  tp.enqueue(&set_true, &runned);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(runned);
}

void trivial_submit_test() {
  utils::thread_pool tp(1);
  bool runned = false;
  tp.submit([&runned]() -> bool {
    runned = true;
    return true;
  }).get();

  assert(runned);
}

void size_test() {
  utils::thread_pool tp(5);
  assert(tp.threads_count() == 5);
}


void fibonacci() {
  utils::thread_pool tp(25);

  std::function<size_t(size_t)> fib;

  fib = [&tp, &fib](size_t n) -> size_t {
    if (n <= 1) {
      return 1;
    }

    auto res1 = tp.submit(fib, n - 1);
    auto res2 = tp.submit(fib, n - 2);
    return res1.get() + res2.get();
  };

  assert(tp.submit(fib, 6).get() == 13);
}

int trivial_waiter(int i) {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return i;
}

void task_is_running() {
  utils::thread_pool tp(1);

  auto dead_task = tp.submit(&trivial_waiter, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto task = tp.submit(&trivial_waiter, 2);
  assert(!task.is_running());
  assert(!task.is_done());
  assert(!task.is_canceled());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(task.is_running());
  assert(!task.is_done());
  assert(!task.is_canceled());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(!task.is_running());
  assert(task.is_done());
  assert(!task.is_canceled());

  assert(task.get() == 2);
}

void cancel_in_destructor() {
  utils::thread_pool tp(1);

  for (int i = 0; i != 10; ++i) {
    tp.submit(&trivial_waiter, i);
  }

  auto task = tp.submit(&trivial_waiter, 42);

  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  assert(!task.is_running());
  assert(task.is_done());
  assert(!task.is_canceled());

  assert(task.get() == 42);
}

void cancel() {
  utils::thread_pool tp(1);

  auto dead_task = tp.submit(&trivial_waiter, 1);
  auto task_for_cancel = tp.submit(&trivial_waiter, 4);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));

  auto task = tp.submit(&trivial_waiter, 2);
  assert(!task.is_running());
  assert(!task.is_done());
  assert(!task.is_canceled());

  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  task_for_cancel.cancel();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(task.is_running());
  assert(!task.is_done());
  assert(!task.is_canceled());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(!task.is_running());
  assert(task.is_done());
  assert(!task.is_canceled());

  assert(task.get() == 2);
}

void cancel_too_late() {
  utils::thread_pool tp(1);

  auto task_for_cancel = tp.submit(&trivial_waiter, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  task_for_cancel.cancel();

  std::this_thread::sleep_for(std::chrono::milliseconds(40));

  auto task = tp.submit(&trivial_waiter, 2);
  assert(!task.is_running());
  assert(!task.is_done());
  assert(!task.is_canceled());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(task.is_running());
  assert(!task.is_done());
  assert(!task.is_canceled());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(!task.is_running());
  assert(task.is_done());
  assert(!task.is_canceled());

  assert(task.get() == 2);
}

int main() {
  trivial_enqueue_test();
  trivial_submit_test();
  size_test();
  fibonacci();
  task_is_running();
  cancel_in_destructor();
  cancel();
  cancel_too_late();

  std::cout << "Smoke tests passed" << std::endl;

  {
    utils::thread_pool tp(1);
    auto task = tp.submit([] { return 0; });
    int res = task.get();
    assert(res == 0);
    task.cancel();
    assert(!task.is_canceled());
  }

  std::cout << 1 << std::endl;

  {
    using int_task_t = utils::thread_pool::task_t<int>;
    using namespace std::chrono_literals;

    utils::thread_pool tp(2);

    auto A = tp.submit([] {
      std::this_thread::sleep_for(100ms);
      return 42;
    });

    auto B = tp.submit([&] {
      std::this_thread::sleep_for(50ms);
      int a = A.get();
      std::this_thread::sleep_for(50ms);
      return a;
    });

    std::atomic_bool created = false;
    std::optional<int_task_t> D;
    auto C = tp.submit([&] {
      std::this_thread::sleep_for(300ms);
      assert(created == true);
      return D.value().get();
    });

    D = tp.submit([&] {
      std::this_thread::sleep_for(500ms);
      return 24;
    });
    created = true;

    assert(B.get() == 42);
    assert(A.get() == 42);

    assert(D.value().get() == 24);
    assert(C.get() == 24);
  }

  std::cout << 2 << std::endl;

  {
    using namespace std::chrono_literals;

    utils::thread_pool tp(1);
    // load the thread pool
    tp.enqueue([] { std::this_thread::sleep_for(100ms); });

    auto task = tp.submit([] { return 42; });
    auto main_thread_id = std::this_thread::get_id();

    task.invoke_on_completion([=](int value) {
      assert(value == 42);
      assert(std::this_thread::get_id() != main_thread_id);
    });

    assert(task.get() == 42);
  }

  std::cout << 3 << std::endl;

  {
    using int_task_t = utils::thread_pool::task_t<int>;
    using namespace std::chrono_literals;

    utils::thread_pool tp(2);

    std::optional<int_task_t> C;
    std::atomic_bool stored = false;

    // A -> B
    // C -> A
    std::atomic_bool A_stored = false;
    int_task_t A = tp.submit([&] {
      int_task_t B = tp.submit([&] {
        std::this_thread::sleep_for(50ms);
        C = tp.submit([&] {
          while (!A_stored) {
          }
          return A.get();
        });
        stored.store(true);
        return 42;
      });
      std::this_thread::sleep_for(5ms);
      assert(B.get() == 42);
      std::this_thread::sleep_for(150ms);
      return 1;
    });
    A_stored = true;

    std::this_thread::sleep_for(500ms);
    assert(stored);
    assert(C.value().get() == 1);
  }

  std::cout << "All tests passed" << std::endl;
  return 0;
}
