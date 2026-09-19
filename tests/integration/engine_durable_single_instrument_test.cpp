#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

#include "order_books/engine.hpp"
#include "persistence/wal.hpp"

namespace order_books {
namespace {

class AcknowledgingSink final : public EventSink {
 public:
  Result<std::monostate> publish(ShardId, EngineSeq, std::span<const Event>,
                                 std::stop_token) override {
    return std::monostate{};
  }
};

class BlockingAfterFirstSink final : public EventSink {
 public:
  Result<std::monostate> publish(ShardId, EngineSeq, std::span<const Event>,
                                 std::stop_token stop_token) override {
    std::unique_lock lock(mutex_);
    ++calls_;
    condition_.notify_all();
    while (calls_ == 2U && !released_ && !stop_token.stop_requested()) {
      condition_.wait_for(lock, std::chrono::milliseconds(10));
    }
    if (stop_token.stop_requested()) {
      return Error{ErrorCode::engine_unavailable, "test sink stopped"};
    }
    return std::monostate{};
  }

  bool wait_until_calls(const std::size_t expected) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [&] {
      return calls_ >= expected;
    });
  }

  void release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t calls_{};
  bool released_{};
};

Command make_command(const ProducerId producer_id, const std::uint64_t order_id,
                     const Side side) {
  Command command;
  command.identity = CommandIdentity{producer_id, 1, 1, 1};
  command.instrument_id = 1;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{1, order_id};
  command.payload = NewOrderPayload{side, 100, 1};
  return command;
}

TEST(EngineDurableSingleInstrumentTest, CompletesCommittedCrossingPair) {
  const auto data_directory = std::filesystem::temp_directory_path() /
                              "order_books_engine_durable_single_instrument_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  AcknowledgingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.group_commit_max_commands = 256;
  config.runtime.group_commit_max_delay = std::chrono::microseconds(200);

  auto opened = Engine::open(config, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(opened));
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));

  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  std::vector<CommandResult> completions;
  const auto completion = [&](CommandResult result) {
    {
      std::lock_guard lock(completion_mutex);
      completions.push_back(std::move(result));
    }
    completion_condition.notify_all();
  };

  for (std::uint64_t index = 0; index < 4; ++index) {
    ASSERT_TRUE(engine
                    ->submit(make_command(index * 2U + 1U, index * 2U + 1U, Side::sell),
                             completion)
                    .queued);
    ASSERT_TRUE(engine
                    ->submit(make_command(index * 2U + 2U, index * 2U + 2U, Side::buy),
                             completion)
                    .queued);
  }

  {
    std::unique_lock lock(completion_mutex);
    ASSERT_TRUE(completion_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return completions.size() == 8;
    }));
  }
  for (const auto& result : completions) {
    EXPECT_EQ(result.command_status, CommandStatus::committed);
    EXPECT_EQ(result.error_code, ErrorCode::none);
  }

  const auto snapshot = engine->metrics(1);
  ASSERT_TRUE(std::holds_alternative<MetricsSnapshot>(snapshot));
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).commands, 8U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).trades, 4U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).active_orders, 0U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).active_price_levels, 0U);
  EXPECT_GT(std::get<MetricsSnapshot>(snapshot).wal_group_commits, 0U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).wal_group_commands, 8U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).wal_prepare_lanes, 1U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).wal_parallel_prepare_min_commands, 4096U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).wal_parallel_prepare_groups, 0U);
  EXPECT_GT(std::get<MetricsSnapshot>(snapshot).wal_prepare_tasks, 0U);
  EXPECT_GT(std::get<MetricsSnapshot>(snapshot).wal_sync_latency.count, 0U);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));

  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineDurableSingleInstrumentTest, AcceptsExplicitParallelPrepareConfiguration) {
  const auto data_directory = std::filesystem::temp_directory_path() /
                              "order_books_engine_parallel_prepare_configuration_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  AcknowledgingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.group_commit_max_commands = 4;
  config.runtime.group_commit_max_delay = std::chrono::milliseconds(100);
  config.runtime.wal_prepare_lanes = 2;
  config.runtime.wal_parallel_prepare_min_commands = 4;

  auto opened = Engine::open(config, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(opened));
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));

  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  std::vector<CommandResult> completions;
  for (std::uint64_t index = 0; index < 4; ++index) {
    ASSERT_TRUE(engine
                    ->submit(make_command(index + 1U, index + 1U,
                                           index % 2U == 0U ? Side::sell : Side::buy),
                             [&completion_mutex, &completion_condition, &completions](
                                 CommandResult result) {
                               {
                                 std::lock_guard lock(completion_mutex);
                                 completions.push_back(std::move(result));
                               }
                               completion_condition.notify_all();
                             })
                    .queued);
  }
  {
    std::unique_lock lock(completion_mutex);
    ASSERT_TRUE(completion_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return completions.size() == 4U;
    }));
  }

  for (const auto& result : completions) {
    EXPECT_EQ(result.command_status, CommandStatus::committed);
    EXPECT_EQ(result.error_code, ErrorCode::none);
  }

  const auto snapshot = engine->metrics(1);
  ASSERT_TRUE(std::holds_alternative<MetricsSnapshot>(snapshot));
  const auto& metrics_snapshot = std::get<MetricsSnapshot>(snapshot);
  EXPECT_EQ(metrics_snapshot.wal_prepare_lanes, 2U);
  EXPECT_EQ(metrics_snapshot.wal_parallel_prepare_min_commands, 4U);
  EXPECT_GT(metrics_snapshot.wal_parallel_prepare_groups, 0U);
  EXPECT_GE(metrics_snapshot.wal_prepare_tasks, 2U);

  ASSERT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));
  engine.reset();

  auto reopened = storage::Wal::open(
      data_directory / "shard-1" / "wal", 1, config.runtime.wal_segment_size,
      storage::WalPrepareOptions{config.runtime.wal_prepare_lanes,
                                 config.runtime.wal_parallel_prepare_min_commands});
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<storage::Wal>>(reopened));
  auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(reopened));
  auto replayed = wal->replay();
  ASSERT_TRUE(std::holds_alternative<std::vector<domain::CommittedCommand>>(replayed));
  EXPECT_EQ(std::get<std::vector<domain::CommittedCommand>>(replayed).size(), 4U);
  EXPECT_EQ(wal->last_engine_seq(), 4U);
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineDurableSingleInstrumentTest, FallsBackBeforeParallelPrepareThreshold) {
  const auto data_directory = std::filesystem::temp_directory_path() /
                              "order_books_engine_parallel_prepare_fallback_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  AcknowledgingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.group_commit_max_commands = 2;
  config.runtime.group_commit_max_delay = std::chrono::milliseconds(100);
  config.runtime.wal_prepare_lanes = 2;
  config.runtime.wal_parallel_prepare_min_commands = 4;

  auto opened = Engine::open(config, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(opened));
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));

  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  std::size_t completed = 0;
  for (std::uint64_t index = 0; index < 2; ++index) {
    ASSERT_TRUE(engine
                    ->submit(make_command(index + 1U, index + 1U,
                                           index % 2U == 0U ? Side::sell : Side::buy),
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
  }
  {
    std::unique_lock lock(completion_mutex);
    ASSERT_TRUE(completion_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return completed == 2U;
    }));
  }

  const auto snapshot = engine->metrics(1);
  ASSERT_TRUE(std::holds_alternative<MetricsSnapshot>(snapshot));
  const auto& metrics_snapshot = std::get<MetricsSnapshot>(snapshot);
  EXPECT_EQ(metrics_snapshot.wal_parallel_prepare_groups, 0U);
  EXPECT_GT(metrics_snapshot.wal_prepare_tasks, 0U);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineDurableSingleInstrumentTest, MergesPublisherMetricsAndDrainsLag) {
  const auto data_directory = std::filesystem::temp_directory_path() /
                              "order_books_engine_metrics_ownership_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  BlockingAfterFirstSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.group_commit_max_commands = 4;
  config.runtime.group_commit_max_delay = std::chrono::microseconds(200);

  auto opened = Engine::open(config, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(opened));
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));

  std::mutex completion_mutex;
  std::condition_variable completion_condition;
  std::size_t completions{};
  const auto completion = [&](CommandResult result) {
    (void)result;
    {
      std::lock_guard lock(completion_mutex);
      ++completions;
    }
    completion_condition.notify_all();
  };
  for (std::uint64_t index = 0; index < 4; ++index) {
    const auto side = index % 2U == 0U ? Side::sell : Side::buy;
    ASSERT_TRUE(engine->submit(make_command(index + 1U, index + 1U, side), completion).queued);
  }
  {
    std::unique_lock lock(completion_mutex);
    ASSERT_TRUE(completion_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return completions == 4U;
    }));
  }
  ASSERT_TRUE(sink.wait_until_calls(2));

  auto blocked_snapshot = engine->metrics(1);
  ASSERT_TRUE(std::holds_alternative<MetricsSnapshot>(blocked_snapshot));
  const auto& blocked = std::get<MetricsSnapshot>(blocked_snapshot);
  EXPECT_EQ(blocked.commands, 4U);
  EXPECT_EQ(blocked.trades, 2U);
  EXPECT_GT(blocked.wal_group_commits, 0U);
  EXPECT_EQ(blocked.wal_group_commands, 4U);
  EXPECT_GT(blocked.replayed_records, 0U);
  EXPECT_GT(blocked.publish_latency.count, 0U);
  EXPECT_GT(blocked.event_publish_lag_events, 0U);
  EXPECT_GT(blocked.event_publish_lag_bytes, 0U);
  EXPECT_GT(blocked.event_publish_lag_age_ns, 0U);

  sink.release();
  ASSERT_TRUE(sink.wait_until_calls(4));
  MetricsSnapshot drained;
  bool publisher_drained = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    auto snapshot = engine->metrics(1);
    ASSERT_TRUE(std::holds_alternative<MetricsSnapshot>(snapshot));
    drained = std::get<MetricsSnapshot>(std::move(snapshot));
    if (drained.event_publish_lag_events == 0U &&
        drained.event_publish_lag_bytes == 0U &&
        drained.event_publish_lag_age_ns == 0U) {
      publisher_drained = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(publisher_drained);
  EXPECT_EQ(drained.commands, 4U);
  EXPECT_EQ(drained.trades, 2U);
  EXPECT_GT(drained.wal_group_commits, 0U);
  EXPECT_EQ(drained.wal_group_commands, 4U);
  EXPECT_EQ(drained.event_publish_lag_events, 0U);
  EXPECT_EQ(drained.event_publish_lag_bytes, 0U);
  EXPECT_EQ(drained.event_publish_lag_age_ns, 0U);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));

  std::filesystem::remove_all(data_directory, ignored);
}

}  // namespace
}  // namespace order_books
