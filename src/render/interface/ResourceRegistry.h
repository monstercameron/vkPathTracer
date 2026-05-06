#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

namespace vkpt::render {

/// Snapshot entry for one live render resource lease.
struct ResourceLeaseInfo {
  vkpt::core::RuntimeHandle handle = 0u;
  std::string label;
  std::string kind;
  vkpt::core::FrameIndex acquired_frame = 0u;
  vkpt::core::FrameIndex last_access_frame = 0u;
  std::uint64_t size_bytes = 0u;
  std::uint64_t versions = 0u;
  std::uint32_t ref_count = 1u;
};

/// Thread-safe resource lifetime tracker for diagnostics and crash reporting.
///
/// Backends can register allocations here to expose leak snapshots without
/// exposing native resource objects through the public render contracts.
class ResourceLifetimeRegistry {
 public:
  /// Register or replace a resource lease with ref_count initialized to one.
  void register_resource(vkpt::core::RuntimeHandle handle,
                        std::string_view kind,
                        std::string_view label,
                        std::uint64_t size_bytes,
                        vkpt::core::FrameIndex frame = 0) {
    std::scoped_lock lock(m_mutex);
    m_leases[handle] = {
        handle,
        std::string(label),
        std::string(kind),
        frame,
        frame,
        size_bytes,
        0u,
        1u};
  }

  /// Increment the lease ref-count; returns false when the handle is unknown.
  bool retain_resource(vkpt::core::RuntimeHandle handle) {
    std::scoped_lock lock(m_mutex);
    const auto it = m_leases.find(handle);
    if (it == m_leases.end()) {
      return false;
    }
    ++it->second.ref_count;
    return true;
  }

  /// Decrement the lease ref-count and erase the record when it reaches zero.
  bool release_resource(vkpt::core::RuntimeHandle handle) {
    std::scoped_lock lock(m_mutex);
    const auto it = m_leases.find(handle);
    if (it == m_leases.end()) {
      return false;
    }
    if (it->second.ref_count > 0u) {
      --it->second.ref_count;
    }
    if (it->second.ref_count == 0u) {
      m_leases.erase(it);
    }
    return true;
  }

  /// Update the last-access frame for a live resource.
  bool touch_resource(vkpt::core::RuntimeHandle handle, vkpt::core::FrameIndex frame) {
    std::scoped_lock lock(m_mutex);
    const auto it = m_leases.find(handle);
    if (it == m_leases.end()) {
      return false;
    }
    it->second.last_access_frame = frame;
    return true;
  }

  /// Return the current number of live leases.
  std::size_t live_count() const {
    std::scoped_lock lock(m_mutex);
    return m_leases.size();
  }

  /// Return true when at least one lease is still live.
  bool has_leaks() const {
    return live_count() != 0u;
  }

  /// Return a copy of live leases for diagnostics without holding the mutex.
  std::vector<ResourceLeaseInfo> snapshot() const {
    std::scoped_lock lock(m_mutex);
    std::vector<ResourceLeaseInfo> out;
    out.reserve(m_leases.size());
    for (const auto& kvp : m_leases) {
      out.push_back(kvp.second);
    }
    return out;
  }

  /// Remove all tracked leases.
  void clear() {
    std::scoped_lock lock(m_mutex);
    m_leases.clear();
  }

 private:
  mutable std::mutex m_mutex;
  std::unordered_map<vkpt::core::RuntimeHandle, ResourceLeaseInfo> m_leases;
};

}  // namespace vkpt::render
