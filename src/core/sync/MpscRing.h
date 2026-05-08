#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace vkpt::core::sync {

// Bounded multi-producer / single-consumer ring buffer based on the Vyukov
// bounded MPMC algorithm restricted to one consumer. Wait-free for the
// consumer; lock-free (CAS-loop) for producers.
//
// Capacity is rounded up to the next power of two for fast index masking.
// Drops (push to a full ring) are counted but not retried.
template <typename T>
class MpscRing {
 public:
  static_assert(std::is_nothrow_move_constructible_v<T> ||
                    std::is_nothrow_copy_constructible_v<T>,
                "MpscRing<T> requires nothrow move or copy constructible T");

  explicit MpscRing(std::size_t capacity)
      : m_capacity(round_capacity(capacity)),
        m_mask(m_capacity - 1u),
        m_storage(::operator new[](m_capacity * sizeof(Cell), std::align_val_t{alignof(Cell)})),
        m_cells(static_cast<Cell*>(m_storage)) {
    for (std::size_t i = 0; i < m_capacity; ++i) {
      ::new (&m_cells[i]) Cell();
      m_cells[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  MpscRing(const MpscRing&) = delete;
  MpscRing& operator=(const MpscRing&) = delete;

  ~MpscRing() {
    T scratch;
    while (try_pop(scratch)) {
    }
    for (std::size_t i = 0; i < m_capacity; ++i) {
      m_cells[i].~Cell();
    }
    ::operator delete[](m_storage, std::align_val_t{alignof(Cell)});
  }

  bool try_push(T value) {
    Cell* cell;
    auto pos = m_enqueue.load(std::memory_order_relaxed);
    while (true) {
      cell = &m_cells[pos & m_mask];
      const auto seq = cell->sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
      if (diff == 0) {
        if (m_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          break;
        }
      } else if (diff < 0) {
        m_dropped.fetch_add(1u, std::memory_order_relaxed);
        return false;
      } else {
        pos = m_enqueue.load(std::memory_order_relaxed);
      }
    }
    cell->emplace(std::move(value));
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    auto pos = m_dequeue.load(std::memory_order_relaxed);
    Cell* cell = &m_cells[pos & m_mask];
    const auto seq = cell->sequence.load(std::memory_order_acquire);
    const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
    if (diff != 0) {
      return false;
    }
    out = cell->consume();
    cell->sequence.store(pos + m_capacity, std::memory_order_release);
    m_dequeue.store(pos + 1, std::memory_order_relaxed);
    return true;
  }

  std::size_t capacity() const { return m_capacity; }

  std::size_t depth() const {
    const auto e = m_enqueue.load(std::memory_order_acquire);
    const auto d = m_dequeue.load(std::memory_order_acquire);
    return e - d;
  }

  std::size_t dropped_total() const {
    return m_dropped.load(std::memory_order_relaxed);
  }

 private:
  struct Cell {
    std::atomic<std::size_t> sequence{0};
    alignas(T) unsigned char buf[sizeof(T)];

    void emplace(T value) noexcept {
      ::new (static_cast<void*>(buf)) T(std::move(value));
    }
    T consume() noexcept {
      T* obj = std::launder(reinterpret_cast<T*>(buf));
      T result = std::move(*obj);
      obj->~T();
      return result;
    }
  };

  static std::size_t round_capacity(std::size_t requested) {
    requested = requested < 2u ? 2u : requested;
    return std::has_single_bit(requested) ? requested : std::bit_ceil(requested);
  }

  const std::size_t m_capacity = 0u;
  const std::size_t m_mask = 0u;
  void* m_storage;
  Cell* m_cells;
  alignas(64) std::atomic<std::size_t> m_enqueue{0u};
  alignas(64) std::atomic<std::size_t> m_dequeue{0u};
  std::atomic<std::size_t> m_dropped{0u};
};

}  // namespace vkpt::core::sync
