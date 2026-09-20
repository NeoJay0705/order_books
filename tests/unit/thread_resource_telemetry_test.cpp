#include "thread_resource_telemetry.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace order_books::benchmark {
namespace {

TEST(ThreadResourceTelemetryTest, ParsesRequiredProcFieldsAndOptionalMigration) {
  const auto parsed = parse_thread_resource_sample(
      42, "ob-wr-1",
      "Name:\tob-wr-1\nvoluntary_ctxt_switches: 10\n"
      "nonvoluntary_ctxt_switches: 2\n",
      "1000 2000 30\n", "se.nr_migrations : 4\n");

  ASSERT_TRUE(std::holds_alternative<ThreadResourceSnapshot>(parsed));
  const auto& sample = std::get<ThreadResourceSnapshot>(parsed).samples.front();
  EXPECT_EQ(sample.tid, 42U);
  EXPECT_EQ(sample.role, "ob-wr-1");
  EXPECT_EQ(sample.cpu_runtime_ns, 1000U);
  EXPECT_EQ(sample.runqueue_wait_ns, 2000U);
  EXPECT_EQ(sample.sched_timeslices, 30U);
  EXPECT_EQ(sample.voluntary_context_switches, 10U);
  EXPECT_EQ(sample.involuntary_context_switches, 2U);
  ASSERT_TRUE(sample.cpu_migrations.has_value());
  EXPECT_EQ(*sample.cpu_migrations, 4U);
}

TEST(ThreadResourceTelemetryTest, MissingOptionalMigrationIsNotZero) {
  const auto parsed = parse_thread_resource_sample(
      42, "ob-wr-1", "voluntary_ctxt_switches: 1\nnonvoluntary_ctxt_switches: 2\n",
      "1000 2000 30\n", "scheduler : value\n");

  ASSERT_TRUE(std::holds_alternative<ThreadResourceSnapshot>(parsed));
  EXPECT_FALSE(std::get<ThreadResourceSnapshot>(parsed).samples.front().cpu_migrations.has_value());
}

TEST(ThreadResourceTelemetryTest, RejectsMissingRequiredField) {
  const auto parsed = parse_thread_resource_sample(
      42, "ob-wr-1", "voluntary_ctxt_switches: 1\n", "1000 2000 30\n", "");

  ASSERT_TRUE(std::holds_alternative<ThreadResourceError>(parsed));
  EXPECT_EQ(std::get<ThreadResourceError>(parsed).code, "status_field_missing");
}

TEST(ThreadResourceTelemetryTest, ComputesRoleDeltasAndNormalization) {
  const ThreadResourceSnapshot before{{ThreadResourceSample{
      42, "ob-wr-1", 100, 200, 3, 4, 5, 6}}};
  const ThreadResourceSnapshot after{{ThreadResourceSample{
      42, "ob-wr-1", 1100, 1200, 13, 14, 25, 16}}};

  const auto result = subtract_thread_resources(before, after);
  ASSERT_TRUE(std::holds_alternative<std::vector<ThreadResourceDelta>>(result));
  const auto& delta = std::get<std::vector<ThreadResourceDelta>>(result).front().sample;
  EXPECT_EQ(delta.cpu_runtime_ns, 1000U);
  EXPECT_EQ(delta.runqueue_wait_ns, 1000U);
  EXPECT_EQ(delta.sched_timeslices, 10U);
  EXPECT_EQ(delta.voluntary_context_switches, 10U);
  EXPECT_EQ(delta.involuntary_context_switches, 20U);
  ASSERT_TRUE(delta.cpu_migrations.has_value());
  EXPECT_EQ(*delta.cpu_migrations, 10U);
  ASSERT_TRUE(per_million(10, 2).has_value());
  EXPECT_EQ(*per_million(10, 2), 5'000'000U);
  EXPECT_FALSE(per_million(1, 0).has_value());
}

TEST(ThreadResourceTelemetryTest, RejectsRoleOrCounterMismatch) {
  const ThreadResourceSnapshot before{{ThreadResourceSample{
      42, "ob-wr-1", 100, 200, 3, 4, 5, std::nullopt}}};
  const ThreadResourceSnapshot after{{ThreadResourceSample{
      43, "ob-wr-1", 1100, 1200, 13, 14, 25, std::nullopt}}};

  const auto result = subtract_thread_resources(before, after);
  ASSERT_TRUE(std::holds_alternative<ThreadResourceError>(result));
  EXPECT_EQ(std::get<ThreadResourceError>(result).code, "snapshot_role_mismatch");
}

}  // namespace
}  // namespace order_books::benchmark
