#include "pathtracer/ScalarCpuPathTracerJobs.h"

#include <algorithm>
#include <thread>

namespace vkpt::pathtracer {
namespace {

std::size_t ScalarRenderWorkerCount() {
  const auto hardware = std::max<std::size_t>(1u, std::thread::hardware_concurrency());
  if (hardware <= 2u) {
    return 1u;
  }
  if (hardware <= 4u) {
    return 2u;
  }
  const auto reserved = std::clamp<std::size_t>((hardware + 1u) / 2u, 3u, hardware - 1u);
  return std::max<std::size_t>(1u, hardware - reserved);
}

}  // namespace

vkpt::jobs::JobSystem& ScalarRenderJobs() {
  static vkpt::jobs::JobSystem jobs(vkpt::jobs::JobSystemConfig{
      ScalarRenderWorkerCount(),
      vkpt::jobs::WorkerThreadPriority::Background,
      false});
  return jobs;
}

}  // namespace vkpt::pathtracer
