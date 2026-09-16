#pragma once

#include <cstdint>
#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "order_books/metrics.hpp"

namespace order_books::runtime {

class MetricsRegistry final : public MetricsSink {
 public:
  explicit MetricsRegistry(MetricsSink& downstream) : downstream_(downstream) {}

  void observe(std::string_view name, std::uint64_t value) override;
  [[nodiscard]] MetricsSnapshot snapshot() const;

 private:
  static constexpr std::size_t kHistogramBuckets = 22U;
  struct Histogram {
    std::array<std::uint64_t, kHistogramBuckets> buckets{};
    std::uint64_t count{};
    std::uint64_t max{};

    void observe(std::uint64_t value) noexcept;
    [[nodiscard]] HistogramSnapshot snapshot() const noexcept;
  };

  MetricsSink& downstream_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::uint64_t> counters_;
  std::unordered_map<std::string, Histogram> histograms_;
};

}  // namespace order_books::runtime
