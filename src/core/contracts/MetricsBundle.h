#pragma once

#include <string>
#include <string_view>

#include "core/metrics/Metrics.h"

namespace vkpt::core::contracts {

template <typename Tag>
class MetricsBundle {
 private:
  std::string m_prefix;

 public:
  explicit MetricsBundle(std::string_view prefix)
      : m_prefix(prefix),
        tick(metrics::MetricsRegistry::instance().counter(metric_name("tick_total"))),
        error(metrics::MetricsRegistry::instance().counter(metric_name("errors_total"))),
        duration_us(
            metrics::MetricsRegistry::instance().histogram(metric_name("duration_us"))),
        depth(metrics::MetricsRegistry::instance().gauge(metric_name("depth"))) {}

  MetricsBundle(const MetricsBundle&) = delete;
  MetricsBundle& operator=(const MetricsBundle&) = delete;

  metrics::Counter& tick;
  metrics::Counter& error;
  metrics::Histogram& duration_us;
  metrics::Gauge& depth;

 private:
  std::string metric_name(std::string_view suffix) const {
    std::string out = m_prefix;
    if (!out.empty() && out.back() != '.') {
      out.push_back('.');
    }
    out.append(suffix);
    return out;
  }
};

}  // namespace vkpt::core::contracts
