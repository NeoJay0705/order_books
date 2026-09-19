#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

#include "order_books/engine.hpp"
#include "runtime/shard_runtime.hpp"
#include "runtime/writer_profile.hpp"

namespace order_books::runtime {
namespace {

class AcknowledgingSink final : public EventSink {
 public:
  Result<std::monostate> publish(const ShardId, const EngineSeq,
                                 const std::span<const Event>,
                                 const std::stop_token) override {
    return std::monostate{};
  }
};

class Collector final : public WriterProfileCollector {
 public:
  void observe_profile_group(const bool sampled) noexcept override {
    ++eligible_groups;
    if (sampled) {
      ++sampled_groups;
    }
  }

  void observe_writer_group(const WriterGroupProfile& sample) noexcept override {
    writer_groups.push_back(sample);
  }

  void observe_rejected_group(const std::uint64_t commands) noexcept override {
    rejected_commands += commands;
  }

  void observe_completion(const CompletionProfile& sample) noexcept override {
    completions.push_back(sample);
  }

  std::vector<WriterGroupProfile> writer_groups;
  std::vector<CompletionProfile> completions;
  std::uint64_t eligible_groups{};
  std::uint64_t sampled_groups{};
  std::uint64_t rejected_commands{};
};

Command make_command(const ProducerId producer_id, const ProducerSeq sequence,
                     const Side side, const std::uint64_t order_id) {
  Command command;
  command.identity = CommandIdentity{producer_id, 1, 1, sequence};
  command.instrument_id = 1;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{1, order_id};
  command.payload = NewOrderPayload{side, 100, 1};
  return command;
}

TEST(EngineWriterProfileTest, ShardRuntimeReportsWriterAndCompletionSamples) {
  const auto data_directory = std::filesystem::temp_directory_path() /
                              "order_books_engine_writer_profile_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  AcknowledgingSink sink;
  NullMetricsSink metrics;
  Collector collector;
  collector.writer_groups.reserve(4);
  collector.completions.reserve(2);
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.ingress_queue_capacity = 64;
  config.runtime.group_commit_max_commands = 2;
  config.runtime.group_commit_max_delay = std::chrono::microseconds(0);
  config.runtime.snapshot_interval_commands = std::numeric_limits<std::size_t>::max();
  config.runtime.event_replay_snapshot_interval_commands =
      std::numeric_limits<std::size_t>::max();

  auto opened = ShardRuntime::open(1, config, sink, metrics, &collector);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<ShardRuntime>>(opened));
  auto runtime = std::get<std::unique_ptr<ShardRuntime>>(std::move(opened));
  runtime->start();

  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  std::vector<CommandResult> results;
  const auto completion = [&](CommandResult result) {
    const auto should_throw = result.identity.producer_id == 2U;
    {
      std::lock_guard lock(completion_mutex);
      results.push_back(std::move(result));
    }
    completion_condition.notify_all();
    if (should_throw) {
      throw std::runtime_error("diagnostic callback failure");
    }
  };
  ASSERT_TRUE(runtime
                  ->submit(make_command(1, 1, Side::sell, 1), completion)
                  .queued);
  ASSERT_TRUE(runtime
                  ->submit(make_command(2, 1, Side::buy, 2), completion)
                  .queued);

  {
    std::unique_lock lock(completion_mutex);
    ASSERT_TRUE(completion_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return results.size() == 2U;
    }));
  }
  ASSERT_TRUE(std::holds_alternative<std::monostate>(runtime->stop()));
  ASSERT_EQ(results.size(), 2U);
  EXPECT_EQ(collector.rejected_commands, 0U);
  ASSERT_FALSE(collector.writer_groups.empty());
  ASSERT_EQ(collector.completions.size(), results.size());

  std::uint64_t accepted = 0;
  for (const auto& sample : collector.writer_groups) {
    accepted += sample.accepted_commands;
    EXPECT_GE(sample.input_commands, sample.accepted_commands);
    EXPECT_LE(sample.wal.payload_bytes, sample.wal.frame_bytes);
    const auto writer_children = sample.admission_ns + sample.wal_append_ns + sample.wal_sync_ns +
                                 sample.apply_ns + sample.publisher_notify_ns + sample.post_apply_ns +
                                 sample.completion_enqueue_ns;
    EXPECT_GE(sample.writer_service_ns, writer_children);
    EXPECT_GE(sample.writer_cycle_ns, sample.group_collect_ns + sample.writer_service_ns);
    EXPECT_GE(sample.wal.prepare_ns,
              sample.wal.payload_encode_ns + sample.wal.crc_ns + sample.wal.frame_assembly_ns);
    EXPECT_GE(sample.wal.plan_copy_ns, sample.wal.chunk_copy_ns);
  }
  EXPECT_EQ(accepted, 2U);
  for (const auto& sample : collector.completions) {
    EXPECT_EQ(sample.completions, 1U);
  }

  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineWriterProfileTest, SamplingKeepsUnprofiledGroupsOnNormalPath) {
  const auto data_directory = std::filesystem::temp_directory_path() /
                              ("order_books_engine_writer_sampling_test-" +
                               std::to_string(std::chrono::steady_clock::now()
                                                  .time_since_epoch()
                                                  .count()));
  std::error_code ignored;

  AcknowledgingSink sink;
  NullMetricsSink metrics;
  Collector collector;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.ingress_queue_capacity = 64;
  config.runtime.group_commit_max_commands = 1;
  config.runtime.group_commit_max_delay = std::chrono::microseconds(0);
  config.runtime.snapshot_interval_commands = std::numeric_limits<std::size_t>::max();
  config.runtime.event_replay_snapshot_interval_commands =
      std::numeric_limits<std::size_t>::max();

  auto opened = ShardRuntime::open(1, config, sink, metrics, &collector,
                                   WriterProfileOptions{2});
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<ShardRuntime>>(opened));
  auto runtime = std::get<std::unique_ptr<ShardRuntime>>(std::move(opened));
  runtime->start();

  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  std::size_t completed = 0;
  for (std::uint64_t index = 0; index < 4; ++index) {
    ASSERT_TRUE(runtime
                    ->submit(make_command(static_cast<ProducerId>(index + 1U), 1,
                                           index % 2U == 0U ? Side::sell : Side::buy,
                                           index + 1U),
                             [&completion_mutex, &completion_condition, &completed](
                                 CommandResult result) {
                               (void)result;
                               {
                                 std::lock_guard lock(completion_mutex);
                                 ++completed;
                               }
                               completion_condition.notify_all();
                             })
                    .queued);
    std::unique_lock lock(completion_mutex);
    ASSERT_TRUE(completion_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return completed == index + 1U;
    }));
  }

  ASSERT_TRUE(std::holds_alternative<std::monostate>(runtime->stop()));
  EXPECT_EQ(collector.eligible_groups, 4U);
  EXPECT_EQ(collector.sampled_groups, 2U);
  EXPECT_EQ(collector.writer_groups.size(), 2U);
  EXPECT_EQ(collector.completions.size(), 2U);
  for (const auto& sample : collector.writer_groups) {
    EXPECT_EQ(sample.input_commands, 1U);
    EXPECT_EQ(sample.accepted_commands, 1U);
  }
  for (const auto& sample : collector.completions) {
    EXPECT_EQ(sample.completions, 1U);
  }

  std::filesystem::remove_all(data_directory, ignored);
}

}  // namespace
}  // namespace order_books::runtime
