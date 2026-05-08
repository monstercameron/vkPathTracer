#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace vkpt::core::sync {

template <typename T>
class SpscRing {
 public:
  explicit SpscRing(std::size_t capacity)
      : m_capacity(round_capacity(capacity)),
        m_mask(m_capacity - 1u),
        m_slots(m_capacity) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  bool try_push(T value) {
    const auto write = m_write.load(std::memory_order_relaxed);
    const auto read = m_read.load(std::memory_order_acquire);
    if (write - read >= m_capacity) {
      m_dropped.fetch_add(1u, std::memory_order_relaxed);
      return false;
    }
    m_slots[write & m_mask].emplace(std::move(value));
    m_write.store(write + 1u, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const auto read = m_read.load(std::memory_order_relaxed);
    const auto write = m_write.load(std::memory_order_acquire);
    if (read == write) {
      return false;
    }
    auto& slot = m_slots[read & m_mask];
    out = std::move(*slot);
    slot.reset();
    m_read.store(read + 1u, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const {
    return m_capacity;
  }

  std::size_t depth() const {
    const auto write = m_write.load(std::memory_order_acquire);
    const auto read = m_read.load(std::memory_order_acquire);
    return write - read;
  }

  std::size_t dropped_total() const {
    return m_dropped.load(std::memory_order_relaxed);
  }

 private:
  static std::size_t round_capacity(std::size_t requested) {
    requested = requested < 2u ? 2u : requested;
    return std::has_single_bit(requested) ? requested : std::bit_ceil(requested);
  }

  const std::size_t m_capacity = 0u;
  const std::size_t m_mask = 0u;
  std::vector<std::optional<T>> m_slots;
  alignas(64) std::atomic<std::size_t> m_write{0u};
  alignas(64) std::atomic<std::size_t> m_read{0u};
  std::atomic<std::size_t> m_dropped{0u};
};

}  // namespace vkpt::core::sync
