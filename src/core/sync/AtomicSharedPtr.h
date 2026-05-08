#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace vkpt::core::sync {

// Portable atomic shared-ptr swap. Used by Track B's SnapshotRing as the
// publish primitive. Wraps std::atomic<std::shared_ptr<T>> when available
// (clang 16+/MSVC 19.30+/libstdc++ 12+); falls back to a mutex-protected
// pointer otherwise. Consumers should not rely on lock-free progress in the
// fallback path, but correctness is preserved.

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L

template <typename T>
class AtomicSharedPtr {
 public:
  AtomicSharedPtr() = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> ptr) : m_ptr(std::move(ptr)) {}

  std::shared_ptr<T> load(std::memory_order order = std::memory_order_acquire) const {
    return m_ptr.load(order);
  }

  void store(std::shared_ptr<T> ptr, std::memory_order order = std::memory_order_release) {
    m_ptr.store(std::move(ptr), order);
  }

  std::shared_ptr<T> exchange(std::shared_ptr<T> ptr,
                              std::memory_order order = std::memory_order_acq_rel) {
    return m_ptr.exchange(std::move(ptr), order);
  }

 private:
  std::atomic<std::shared_ptr<T>> m_ptr;
};

#else

template <typename T>
class AtomicSharedPtr {
 public:
  AtomicSharedPtr() = default;
  explicit AtomicSharedPtr(std::shared_ptr<T> ptr) : m_ptr(std::move(ptr)) {}

  std::shared_ptr<T> load(std::memory_order = std::memory_order_acquire) const {
    std::scoped_lock lock(m_mutex);
    return m_ptr;
  }

  void store(std::shared_ptr<T> ptr, std::memory_order = std::memory_order_release) {
    std::scoped_lock lock(m_mutex);
    m_ptr = std::move(ptr);
  }

  std::shared_ptr<T> exchange(std::shared_ptr<T> ptr,
                              std::memory_order = std::memory_order_acq_rel) {
    std::scoped_lock lock(m_mutex);
    std::shared_ptr<T> prev = std::move(m_ptr);
    m_ptr = std::move(ptr);
    return prev;
  }

 private:
  mutable std::mutex m_mutex;
  std::shared_ptr<T> m_ptr;
};

#endif

}  // namespace vkpt::core::sync
