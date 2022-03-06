#pragma once

#include <vector>
#include <cstdint>
#include <functional>
#include <deque>
#include <type_traits>
#include <variant>
#include <cassert>

#include <thread>
#include <condition_variable>
#include <mutex>

namespace utils {

class thread_pool;

/// Exceptions for thread pool
struct cancelation_exception final : std::exception {
  const char* what() const noexcept override {
    return "getting result from cancelled task";
  }
};

struct shutdown_exception final : std::exception {
  const char* what() const noexcept override {
    return "trying to push task into closed pool";
  }
};

///////////////////////////////////////////////////////////////////////////////////////////////////

namespace detail {
static inline thread_local thread_pool* curr_thread_pool{nullptr};

/// Unbounded multi-producers/multi-consumers queue
template <typename T>
class UnboundedMPMCQueue final {
 public:
  bool Put(T&& value) {
    {
      std::lock_guard lk(m_);
      if (is_closed) {
        return false;
      }
      data_.push_back(std::move(value));
    }
    cv_.notify_one();
    return true;
  }

  std::optional<T> Take(bool wait = true) {
    std::unique_lock lk(m_);
    if (wait) {
      cv_.wait(lk, [&] {
        return is_closed || !data_.empty();
      });
    }

    if (data_.empty()) {
      return {};
    }
    T res = std::move(data_.front());
    data_.pop_front();
    return {std::move(res)};
  }

  void Close() {
    {
      std::lock_guard lk{m_};
      is_closed = true;
    }
    cv_.notify_all();
  }

  std::size_t Size() const {
    std::lock_guard lk{m_};
    return data_.size();
  }

 private:
  bool is_closed{false};
  std::deque<T> data_;
  mutable std::mutex m_;
  std::condition_variable cv_;
};

/// States of tasks to share in share_state
enum class TaskState : std::uint32_t {
  Runnable = 0,
  Canceled,
  Running,
  Done,
};

/**
 * Class with state to promise and future
 */
template <class Result>
class State final {
 public:
  using Callback = std::function<void(Result)>;

  Result get();

  void set_result(Result res);

  bool is_running() const noexcept {
    return state_.load(std::memory_order_acquire) == TaskState::Running;
  }

  bool is_done() const noexcept {
    return state_.load(std::memory_order_acquire) == TaskState::Done;
  }

  bool is_canceled() const noexcept {
    return state_.load(std::memory_order_acquire) == TaskState::Canceled;
  }

  void set_state(TaskState state) noexcept {
    state_.store(state, std::memory_order_release);
  }

  void set_callback(const Callback& functor) {
    std::unique_lock lk{m_};
    if (state_.load(std::memory_order_acquire) == TaskState::Canceled) {
      return;
    }
    if (std::holds_alternative<Result>(content_)) {
      try {
        functor(get<Result>(content_));
      } catch (...) {
      }
    } else if (std::holds_alternative<std::monostate>(content_)) {
      callback_ = functor;
    }
  }

  void set_error(const std::exception_ptr& eptr) {
    std::unique_lock lk{m_};
    if (state_.load(std::memory_order_acquire) == TaskState::Canceled) {
      return;
    }
    content_ = eptr;
    lk.unlock();
    cv_.notify_all();
  }

  void cancel() {
    auto state = TaskState::Runnable;
    if (state_.compare_exchange_strong(state, TaskState::Canceled,
                                       std::memory_order_release,
                                       std::memory_order_relaxed)) {
      cv_.notify_all();
    }
  }

 private:
  std::atomic<TaskState> state_{TaskState::Runnable};
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::optional<Callback> callback_;
  std::variant<std::monostate, Result, std::exception_ptr> content_{
      std::monostate{}};
};

template <class Res>
using shared_state = std::shared_ptr<State<Res>>;

/**
 * Future class
 */
template <class Result>
class Future final {
  static_assert(!std::is_same_v<Result, void>);

 public:
  Future(const Future& other) = default;

  Future& operator=(const Future& other) = default;

  Future(Future&& other) noexcept = default;

  Future& operator=(Future&& other) noexcept = default;

  Future(shared_state<Result> state_) noexcept : state_(std::move(state_)) {
  }

  ~Future() {
    if (state_) {
      cancel();
    }
  }

  void invoke_on_completion(const std::function<void(Result)>& f) {
    state_->set_callback(f);
  }

  Result get() {
    return state_->get();
  }

  void cancel() noexcept {
    state_->cancel();
  }

  bool is_running() const noexcept {
    return state_->is_running();
  }

  bool is_done() const noexcept {
    return state_->is_done();
  }

  bool is_canceled() const noexcept {
    return state_->is_canceled();
  }

 private:
  shared_state<Result> state_;
};

/**
 * Promise class
 */
template <class Result>
class Promise final {
 public:
  explicit Promise(shared_state<Result> state) : state_(std::move(state)) {
  }

  bool is_canceled() const noexcept {
    return state_->is_canceled();
  }

  void running() noexcept {
    state_->set_state(TaskState::Running);
  }

  void done() noexcept {
    state_->set_state(TaskState::Done);
  }

  void set_error(const std::exception_ptr& eptr) noexcept {
    state_->set_error(eptr);
  }

  void set_result(Result res) noexcept {
    state_->set_result(std::move(res));
  }

 private:
  shared_state<Result> state_;
};

/// creates channel with promise and future
template <class ReturnVal>
auto make_contract() {
  detail::shared_state<ReturnVal> state =
      std::make_shared<detail::State<ReturnVal>>();
  return std::pair{detail::Future(state), detail::Promise(state)};
}

}  // namespace detail

///////////////////////////////////////////////////////////////////////////////////////////////////

class thread_pool final {
 public:
  template <class Res>
  using task_t = detail::Future<Res>;

  thread_pool(thread_pool&&) = delete;
  thread_pool(const thread_pool&) = delete;
  thread_pool& operator=(thread_pool&&) = delete;
  thread_pool& operator=(const thread_pool&) = delete;

  explicit thread_pool(
      std::size_t threads_count = std::thread::hardware_concurrency()) {
    workers_.reserve(threads_count);
    for (std::size_t i = 0; i < threads_count; ++i) {
      workers_.emplace_back([this]() {
        detail::curr_thread_pool = this;
        for (auto task = tasks_.Take(); bool(task); task = tasks_.Take()) {
          try {
            (*task)();
          } catch (...) {
          }
        }
      });
    }
  }

  bool run_without_wait() {
    auto task = tasks_.Take(/*wait=*/false);
    if (task) {
      (task.value())();
    }
    return (bool)task;
  }

  template <class Functor, class... Args>
  void enqueue(Functor&& f, Args&&... args) {
    bool success = tasks_.Put(
        [func = std::move(f), ... args = std::forward<Args>(args)]() mutable {
          try {
            std::forward<Functor>(func)(std::forward<Args>(args)...);
          } catch (...) {
          }
        });
    if (!success) {
      throw shutdown_exception{};
    }
  }

  template <class Functor, class... Args>
  auto submit(Functor&& f, Args&&... args) -> task_t<
      decltype(std::forward<Functor>(f)(std::forward<Args>(args)...))> {
    auto [ret, p] =
        detail::make_contract<std::decay_t<decltype(std::forward<Functor>(f)(
            std::forward<Args>(args)...))>>();

    enqueue([promise = std::move(p), func = std::forward<Functor>(f),
             ... args = std::forward<Args>(args)]() mutable {
      if (promise.is_canceled()) {
        return;
      }
      promise.running();
      try {
        promise.set_result(std::forward<Functor>(func)(std::move(args)...));
      } catch (...) {
        promise.set_error(std::current_exception());
      }
      promise.done();
    });
    return std::move(ret);
  }

  std::size_t threads_count() const {
    return workers_.size();
  }

  std::size_t remaining_tasks() const {
    return tasks_.Size();
  }

  ~thread_pool() {
    tasks_.Close();
    for (auto& el : workers_) {
      if (el.joinable()) {
        el.join();
      }
    }
  }

 private:
  std::vector<std::thread> workers_;
  detail::UnboundedMPMCQueue<std::function<void()>> tasks_;
};

///////////////////////////////////////////////////////////////////////////////////////////////////

template <class Result>
void detail::State<Result>::set_result(Result res) {
  std::unique_lock lk{m_};
  TaskState check = state_.load(std::memory_order_acquire);
  assert(check == TaskState::Running || check == TaskState::Canceled);

  if (callback_) {
    curr_thread_pool->enqueue(*callback_, res);
  }
  content_ = std::move(res);
  lk.unlock();
  cv_.notify_all();
}

template <class Result>
Result detail::State<Result>::get() {
  if (state_.load(std::memory_order_acquire) == TaskState::Canceled) {
    throw cancelation_exception{};
  }
  bool is_wait{false};
  std::unique_lock lk{m_};
  while (!std::holds_alternative<Result>(content_) &&
         !std::holds_alternative<std::exception_ptr>(content_) &&
         state_.load(std::memory_order_acquire) != TaskState::Canceled) {
    if (is_wait) {
      cv_.wait(lk);
      is_wait = false;
    } else if (curr_thread_pool) {
      lk.unlock();
      is_wait = !curr_thread_pool->run_without_wait();
      lk.lock();
    } else {
      is_wait = true;
    }
  }
  if (state_.load(std::memory_order_acquire) == TaskState::Canceled) {
    throw cancelation_exception{};
  }
  if (std::holds_alternative<std::exception_ptr>(content_)) {
    std::rethrow_exception(std::get<std::exception_ptr>(content_));
  }
  return std::get<Result>(content_);
}

}  // namespace utils
