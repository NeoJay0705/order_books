#include <gtest/gtest.h>

#include "runtime/metrics_registry.hpp"

namespace order_books::runtime {
namespace {

TEST(MetricsRegistryTest, ExposesFixedBucketLatencyQuantiles) {
  NullMetricsSink downstream;
  MetricsRegistry registry(downstream);
  registry.observe("end_to_end_latency_us", 10);
  registry.observe("end_to_end_latency_us", 1000);
  registry.observe("end_to_end_latency_us", 100'000);
  registry.observe("queue_latency_us", 25);

  const auto snapshot = registry.snapshot();
  EXPECT_EQ(snapshot.end_to_end_latency.count, 3U);
  EXPECT_GE(snapshot.end_to_end_latency.p50_microseconds, 10U);
  EXPECT_GE(snapshot.end_to_end_latency.p99_microseconds, 1000U);
  EXPECT_EQ(snapshot.end_to_end_latency.max_microseconds, 100'000U);
  EXPECT_EQ(snapshot.queue_latency.count, 1U);
}

}  // namespace
}  // namespace order_books::runtime
