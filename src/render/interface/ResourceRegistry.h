#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Types.h"

namespace vkpt::render {

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

class ResourceLifetimeRegistry {
 public:
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

  bool retain_resource(vkpt::core::RuntimeHandle handle) {
    std::scoped_lock lock(m_mutex);
    const auto it = m_leases.find(handle);
    if (it == m_leases.end()) {
      return false;
    }
    ++it->second.ref_count;
    return true;
  }

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

  bool touch_resource(vkpt::core::RuntimeHandle handle, vkpt::core::FrameIndex frame) {
    std::scoped_lock lock(m_mutex);
    const auto it = m_leases.find(handle);
    if (it == m_leases.end()) {
      return false;
    }
    it->second.last_access_frame = frame;
    return true;
  }

  std::size_t live_count() const {
    std::scoped_lock lock(m_mutex);
    return m_leases.size();
  }

  bool has_leaks() const {
    return live_count() != 0u;
  }

  std::vector<ResourceLeaseInfo> snapshot() const {
    std::scoped_lock lock(m_mutex);
    std::vector<ResourceLeaseInfo> out;
    out.reserve(m_leases.size());
    for (const auto& kvp : m_leases) {
      out.push_back(kvp.second);
    }
    return out;
  }

  void clear() {
    std::scoped_lock lock(m_mutex);
    m_leases.clear();
  }

 private:
  mutable std::mutex m_mutex;
  std::unordered_map<vkpt::core::RuntimeHandle, ResourceLeaseInfo> m_leases;
};

}  // namespace vkpt::render
