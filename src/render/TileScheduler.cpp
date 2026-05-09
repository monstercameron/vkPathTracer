#include "render/TileScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>

namespace vkpt::render {

namespace {

// FNV-1a 64-bit hash helpers — used to fingerprint comparator-relevant tile
// state so rebuild_order() can short-circuit when the sort key set is unchanged
// across samples.
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

inline std::uint64_t FnvMix(std::uint64_t h, std::uint64_t v) noexcept {
  // Mix 8 bytes one at a time (cheap; gets folded by the compiler for small v).
  for (int i = 0; i < 8; ++i) {
    h ^= (v >> (i * 8)) & 0xffull;
    h *= kFnvPrime;
  }
  return h;
}

// Bucket variance / sample_count to keep the fingerprint stable across
// micro-fluctuations that don't actually change the sort outcome. The bucket
// granularity matches the comparator's sensitivity: variance is compared as
// a strict ordering, so we bucket to ~256 levels per tile by quantising into
// fixed bins.
inline std::uint32_t VarianceBucket(double variance) noexcept {
  if (!(variance > 0.0)) {
    return 0u;
  }
  // Quantize variance into ~256 buckets across a log range; preserves
  // ordering for adjacent-bucket tiles while letting noise within a bucket
  // skip the resort.
  const double clamped = std::clamp(variance, 1e-9, 1e9);
  const double log = std::log10(clamped);  // ~[-9, 9]
  const double scaled = (log + 9.0) * (256.0 / 18.0);
  return static_cast<std::uint32_t>(std::clamp(scaled, 0.0, 255.0));
}

inline std::uint32_t SampleCountBucket(std::uint32_t samples) noexcept {
  // 0..15 individually, then powers-of-two coarsening.
  if (samples < 16u) return samples;
  std::uint32_t bucket = 16u;
  std::uint32_t v = samples;
  while (v >= 32u) {
    v >>= 1;
    ++bucket;
  }
  return bucket;
}

// thread_local cache; the scheduler is driven from a single worker thread
// per coordinator, so per-thread keying by `this` is sufficient and avoids
// any global synchronization.
thread_local std::unordered_map<const TileScheduler*, std::uint64_t>
    tls_lastFingerprint;

}  // namespace

void TileScheduler::configure(TileSchedulerConfig config) {
  config.tile_height = std::max(1u, config.tile_height);
  config.gpu_count = std::max(1u, config.gpu_count);
  config.foveated_center_radius =
      std::clamp(config.foveated_center_radius, 0.0, 0.5);
  m_config = config;
  m_tileCount = config.height == 0u
      ? 0u
      : (config.height + config.tile_height - 1u) / config.tile_height;
  m_feedback.clear();
  m_feedback.resize(m_tileCount);
  for (std::uint32_t tileId = 0u; tileId < m_tileCount; ++tileId) {
    m_feedback[tileId].tile_id = tileId;
  }
  m_order.clear();
  m_nextTile = 0u;
  // Topology change invalidates any cached sort fingerprint for this instance.
  tls_lastFingerprint.erase(this);
}

void TileScheduler::set_feedback(std::span<const TilePriorityFeedback> feedback) {
  for (const auto& item : feedback) {
    if (item.tile_id < m_feedback.size()) {
      m_feedback[item.tile_id] = item;
    }
  }
}

void TileScheduler::begin_sample(std::uint64_t generation,
                                 std::uint32_t sample_index) {
  m_generation = generation;
  m_sampleIndex = sample_index;
  m_nextTile = 0u;
  rebuild_order();
}

bool TileScheduler::next_tile(vkpt::pathtracer::RenderTile& out) {
  if (m_config.width == 0u || m_config.height == 0u || m_nextTile >= m_order.size()) {
    return false;
  }

  const auto entry = m_order[m_nextTile];
  const std::uint32_t tileId = entry.tile_id;
  ++m_nextTile;
  const std::uint32_t y = tileId * m_config.tile_height;
  const std::uint64_t sampleStride =
      m_config.foveated_center_extra_samples == 0u
          ? 1u
          : static_cast<std::uint64_t>(m_config.foveated_center_extra_samples) + 1u;
  const std::uint64_t sampleIndex =
      static_cast<std::uint64_t>(m_sampleIndex) * sampleStride + entry.sample_offset;
  out = {};
  out.x = 0u;
  out.y = y;
  out.width = m_config.width;
  out.height = std::min(m_config.tile_height, m_config.height - y);
  out.sample_index = sampleIndex > std::numeric_limits<std::uint32_t>::max()
      ? std::numeric_limits<std::uint32_t>::max()
      : static_cast<std::uint32_t>(sampleIndex);
  out.tile_id = tileId;
  out.gpu_id = tileId % m_config.gpu_count;
  return true;
}

std::uint32_t TileScheduler::next_tile_batch(
    std::uint32_t max_tiles,
    std::vector<vkpt::pathtracer::RenderTile>& out) noexcept {
  if (max_tiles == 0u || m_config.width == 0u || m_config.height == 0u ||
      m_nextTile >= m_order.size()) {
    return 0u;
  }
  // The batch must stay on a single backend; we group by gpu_id so the
  // caller can route the entire span to one tracer instance. Once a tile
  // with a different gpu_id is encountered we stop the batch — the caller
  // can re-enter on the next iteration to drain a fresh same-gpu_id run.
  vkpt::pathtracer::RenderTile firstTile;
  if (!next_tile(firstTile)) {
    return 0u;
  }
  out.push_back(firstTile);
  std::uint32_t drained = 1u;
  const std::uint32_t batchGpuId = firstTile.gpu_id;
  while (drained < max_tiles && m_nextTile < m_order.size()) {
    // Peek at the next tile; only commit (advance m_nextTile) if it matches.
    const auto entry = m_order[m_nextTile];
    const std::uint32_t tileId = entry.tile_id;
    const std::uint32_t peekGpuId = tileId % m_config.gpu_count;
    if (peekGpuId != batchGpuId) {
      break;
    }
    vkpt::pathtracer::RenderTile next;
    if (!next_tile(next)) {
      break;
    }
    out.push_back(next);
    ++drained;
  }
  return drained;
}

TileSchedulerStats TileScheduler::stats() const {
  std::vector<std::uint32_t> gpuScheduled(
      static_cast<std::size_t>(std::max(1u, m_config.gpu_count)),
      0u);
  for (const auto& entry : m_order) {
    ++gpuScheduled[entry.tile_id % m_config.gpu_count];
  }
  return TileSchedulerStats{
      m_generation,
      m_sampleIndex,
      m_tileCount,
      m_nextTile,
      static_cast<std::uint32_t>(m_order.size()),
      std::move(gpuScheduled)};
}

void TileScheduler::rebuild_order() {
  // Fingerprint comparator-relevant inputs. If the bucketed sort keys are
  // identical to the previous call, the existing m_order is still correct
  // and we can skip the iota/sort/rebuild work entirely. The fingerprint also
  // covers config fields that affect ordering output (foveated extra samples,
  // tile_height, height, gpu_count) so any topology change forces a rebuild.
  std::uint64_t fingerprint = kFnvOffset;
  fingerprint = FnvMix(fingerprint, static_cast<std::uint64_t>(m_tileCount));
  fingerprint = FnvMix(
      fingerprint,
      static_cast<std::uint64_t>(m_config.foveated_center_extra_samples));
  fingerprint = FnvMix(fingerprint, static_cast<std::uint64_t>(m_config.tile_height));
  fingerprint = FnvMix(fingerprint, static_cast<std::uint64_t>(m_config.height));
  fingerprint = FnvMix(fingerprint, static_cast<std::uint64_t>(m_config.gpu_count));

  // Pre-compute dirty population counts in the same pass as fingerprinting so
  // we can dispatch to a specialized comparator below (constant first
  // conditional when all-dirty / all-clean).
  std::uint32_t dirtyCount = 0u;
  for (std::uint32_t tileId = 0u; tileId < m_tileCount; ++tileId) {
    const auto& fb = m_feedback[tileId];
    if (fb.dirty) {
      ++dirtyCount;
    }
    // Pack {dirty, variance_bucket, sample_count_bucket, tile_id} into a
    // single 64-bit lane so the FnvMix loop is amortised: 1 bit dirty
    // | 8 bits variance | 16 bits sample bucket | 32 bits tile_id = 57 bits.
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(fb.dirty ? 1u : 0u) << 56) |
        (static_cast<std::uint64_t>(VarianceBucket(fb.variance) & 0xffu) << 48) |
        (static_cast<std::uint64_t>(SampleCountBucket(fb.sample_count) & 0xffffu)
         << 32) |
        static_cast<std::uint64_t>(tileId);
    fingerprint = FnvMix(fingerprint, packed);
  }

  if (!m_order.empty()) {
    auto cached = tls_lastFingerprint.find(this);
    if (cached != tls_lastFingerprint.end() && cached->second == fingerprint) {
      // Sort-relevant state matches the previous rebuild; m_order is stale-free.
      return;
    }
  }
  tls_lastFingerprint[this] = fingerprint;

  std::vector<std::uint32_t> rankedTileIds(m_tileCount);
  std::iota(rankedTileIds.begin(), rankedTileIds.end(), 0u);

  // Specialize the comparator on the dirty distribution. When every tile is
  // dirty (snapshot-change case, common) or every tile is clean, the first
  // conditional is constant and we elide it from the inner loop.
  const bool allDirty = (dirtyCount == m_tileCount);
  const bool allClean = (dirtyCount == 0u);
  if (allDirty || allClean) {
    // Fast path: same priority class across all tiles, so dirty bit is moot.
    std::stable_sort(rankedTileIds.begin(),
                     rankedTileIds.end(),
                     [&](std::uint32_t lhs, std::uint32_t rhs) {
                       const auto& left = m_feedback[lhs];
                       const auto& right = m_feedback[rhs];
                       if (left.variance != right.variance) {
                         return left.variance > right.variance;
                       }
                       if (left.sample_count != right.sample_count) {
                         return left.sample_count < right.sample_count;
                       }
                       return lhs < rhs;
                     });
  } else {
    // General path: tiles span both dirty/clean classes.
    std::stable_sort(rankedTileIds.begin(),
                     rankedTileIds.end(),
                     [&](std::uint32_t lhs, std::uint32_t rhs) {
                       const auto& left = m_feedback[lhs];
                       const auto& right = m_feedback[rhs];
                       if (left.dirty != right.dirty) {
                         return left.dirty && !right.dirty;
                       }
                       if (left.variance != right.variance) {
                         return left.variance > right.variance;
                       }
                       if (left.sample_count != right.sample_count) {
                         return left.sample_count < right.sample_count;
                       }
                       return lhs < rhs;
                     });
  }

  // Indexed writes into a pre-sized vector — no per-tile push_back / reallocation.
  // First pass counts foveated-center tiles to size the buffer exactly; second
  // pass fills it. Single allocation in resize(); both loops are then index-only.
  const std::uint32_t extraSamples = m_config.foveated_center_extra_samples;
  std::size_t foveatedCenterCount = 0u;
  if (extraSamples > 0u) {
    for (const std::uint32_t tileId : rankedTileIds) {
      if (is_foveated_center_tile(tileId)) {
        ++foveatedCenterCount;
      }
    }
  }
  const std::size_t totalSize = static_cast<std::size_t>(m_tileCount) +
                                foveatedCenterCount *
                                    static_cast<std::size_t>(extraSamples);
  m_order.clear();
  m_order.resize(totalSize);
  std::size_t writeIndex = 0u;
  for (const std::uint32_t tileId : rankedTileIds) {
    m_order[writeIndex++] = ScheduledTile{tileId, 0u};
  }
  if (extraSamples == 0u) {
    return;
  }
  for (std::uint32_t sampleOffset = 1u;
       sampleOffset <= extraSamples;
       ++sampleOffset) {
    for (const std::uint32_t tileId : rankedTileIds) {
      if (is_foveated_center_tile(tileId)) {
        m_order[writeIndex++] = ScheduledTile{tileId, sampleOffset};
      }
    }
  }
}

bool TileScheduler::is_foveated_center_tile(std::uint32_t tile_id) const {
  if (m_tileCount == 0u || m_config.height == 0u) {
    return false;
  }
  const std::uint32_t y = tile_id * m_config.tile_height;
  if (y >= m_config.height) {
    return false;
  }
  const std::uint32_t tileHeight =
      std::min(m_config.tile_height, m_config.height - y);
  const double tileCenter =
      (static_cast<double>(y) + static_cast<double>(tileHeight) * 0.5) /
      static_cast<double>(m_config.height);
  return std::abs(tileCenter - 0.5) <= m_config.foveated_center_radius;
}

}  // namespace vkpt::render
