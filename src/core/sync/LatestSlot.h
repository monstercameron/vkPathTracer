#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

namespace vkpt::core::sync {

// Single-slot, latest-wins exchange. Producer publishes a value (replacing any
// unread one). Consumer takes the value (acquiring ownership). When no value
// is available, take() returns nullopt. Counts publishes that overwrote an
// unread value as drops.
template <typename T>
class LatestSlot {
 public:
  LatestSlot() = default;
  ~LatestSlot() {
    delete m_slot.exchange(nullptr, std::memory_order_acq_rel);
  }

  LatestSlot(const LatestSlot&) = delete;
  LatestSlot& operator=(const LatestSlot&) = delete;

  bool publish(T value) {
    auto* node = new T(std::move(value));
    auto* prev = m_slot.exchange(node, std::memory_order_acq_rel);
    if (prev != nullptr) {
      delete prev;
      m_dropped.fetch_add(1u, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  std::optional<T> take() {
    auto* node = m_slot.exchange(nullptr, std::memory_order_acq_rel);
    if (node == nullptr) {
      return std::nullopt;
    }
    T result = std::move(*node);
    delete node;
    return result;
  }

  std::optional<T> peek() const {
    auto* node = m_slot.load(std::memory_order_acquire);
    if (node == nullptr) {
      return std::nullopt;
    }
    return *node;
  }

  bool has_value() const noexcept {
    return m_slot.load(std::memory_order_acquire) != nullptr;
  }

  std::uint64_t dropped_total() const noexcept {
    return m_dropped.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<T*> m_slot{nullptr};
  std::atomic<std::uint64_t> m_dropped{0};
};

}  // namespace vkpt::core::sync
