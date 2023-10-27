#pragma once

#include <concepts>
#include <future>
#include <thread>

#include <llvm/Support/ThreadPool.h>

namespace cheri {

/**
 * Thread pool interface.
 *
 * An simple thread pool that supports graceful shutdown.
 */
class ThreadPool {
public:
  explicit ThreadPool(unsigned long workers)
      : pool_(llvm::hardware_concurrency(workers)) {}

  template <typename F>
  auto Async(F &&fn)
    requires std::invocable<F, std::stop_token>
  {
    return pool_.async(std::bind(std::forward<F>(fn), stop_state_.get_token()));
  }

  template <typename F, typename... Args>
  auto Async(F &&fn, Args &&...args)
    requires std::invocable<F, std::stop_token, Args...>
  {
    return pool_.async(std::bind(std::forward<F>(fn), stop_state_.get_token(),
                                 std::forward<Args>(args)...));
  }

  void Join() { pool_.wait(); }

  void Cancel() { stop_state_.request_stop(); }

private:
  std::stop_source stop_state_;
  llvm::ThreadPool pool_;
};

} /* namespace cheri */
