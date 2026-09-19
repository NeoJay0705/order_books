#include <gtest/gtest.h>

#include <limits>

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

TEST(MetricsRegistryTest, ExposesWalGroupTotalsAsCounters) {
  NullMetricsSink downstream;
  MetricsRegistry registry(downstream);

  registry.observe("wal_group_commits", 2);
  registry.observe("wal_group_commands", 256);
  registry.observe("wal_group_commands", 128);

  const auto snapshot = registry.snapshot();
  EXPECT_EQ(snapshot.wal_group_commits, 2U);
  EXPECT_EQ(snapshot.wal_group_commands, 384U);
}

TEST(MetricsRegistryTest, ExposesWalPrepareConfigurationAndSyncLatency) {
  NullMetricsSink downstream;
  MetricsRegistry registry(downstream);

  registry.observe("wal_prepare_lanes", 2);
  registry.observe("wal_parallel_prepare_min_commands", 4096);
  registry.observe("wal_parallel_prepare_groups", 3);
  registry.observe("wal_prepare_tasks", 6);
  registry.observe("wal_sync_latency_us", 10);
  registry.observe("wal_sync_latency_us", 1000);

  const auto snapshot = registry.snapshot();
  EXPECT_EQ(snapshot.wal_prepare_lanes, 2U);
  EXPECT_EQ(snapshot.wal_parallel_prepare_min_commands, 4096U);
  EXPECT_EQ(snapshot.wal_parallel_prepare_groups, 3U);
  EXPECT_EQ(snapshot.wal_prepare_tasks, 6U);
  EXPECT_EQ(snapshot.wal_sync_latency.count, 2U);
  EXPECT_GE(snapshot.wal_sync_latency.p99_microseconds, 1000U);
  EXPECT_EQ(snapshot.wal_sync_latency.max_microseconds, 1000U);
}

TEST(MetricsRegistryTest, SaturatesPrepareCountersAndIgnoresUnknownMetrics) {
  NullMetricsSink downstream;
  MetricsRegistry registry(downstream);

  registry.observe("wal_parallel_prepare_groups",
                   std::numeric_limits<std::uint64_t>::max() - 1U);
  registry.observe("wal_parallel_prepare_groups", 10U);
  const auto before_unknown = registry.snapshot();

  registry.observe("unknown_metric", 123U);
  const auto after_unknown = registry.snapshot();

  EXPECT_EQ(before_unknown.wal_parallel_prepare_groups,
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(after_unknown.wal_parallel_prepare_groups,
            before_unknown.wal_parallel_prepare_groups);
}

}  // namespace
}  // namespace order_books::runtime
