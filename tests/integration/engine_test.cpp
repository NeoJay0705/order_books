#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <stop_token>
#include <tuple>
#include <utility>
#include <vector>

#include "order_books/engine.hpp"
#include "persistence/snapshot_store.hpp"

namespace order_books {
namespace {

class RecordingSink final : public EventSink {
 public:
  Result<std::monostate> publish(ShardId shard_id, EngineSeq engine_seq,
                                 std::span<const Event> events,
                                 std::stop_token) override {
    std::lock_guard lock(mutex_);
    batches.emplace_back(shard_id, engine_seq, events.size());
    return std::monostate{};
  }

  std::mutex mutex_;
  std::vector<std::tuple<ShardId, EngineSeq, std::size_t>> batches;
};

class BlockingSink final : public EventSink {
 public:
  Result<std::monostate> publish(ShardId, EngineSeq, std::span<const Event>,
                                 std::stop_token stop_token) override {
    std::unique_lock lock(mutex_);
    publish_entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, stop_token, [this] { return released_; });
    return std::monostate{};
  }

  bool wait_until_publish_entered(const std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return publish_entered_; });
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
  std::condition_variable_any condition_;
  bool publish_entered_{false};
  bool released_{false};
};

TEST(EngineTest, RejectsZeroPublishLagAge) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_zero_publish_lag_age_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{7, 1, 1, 1}};
  config.runtime.max_publish_lag_age = std::chrono::hours(0);

  auto opened = Engine::open(std::move(config), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<Error>(opened));
  EXPECT_EQ(std::get<Error>(opened).code, ErrorCode::invalid_command);
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, RejectsInvalidPublisherCursorPersistencePolicy) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_invalid_publisher_policy_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  const auto open_with = [&data_directory](const RuntimeConfig& runtime) {
    RecordingSink sink;
    NullMetricsSink metrics;
    EngineConfig config;
    config.data_directory = data_directory;
    config.shard_ids = {1};
    config.instruments = {InstrumentConfig{7, 1, 1, 1}};
    config.runtime = runtime;
    return Engine::open(std::move(config), sink, metrics);
  };

  RuntimeConfig zero_commands;
  zero_commands.publisher_cursor_persist_max_commands = 0;
  auto opened = open_with(zero_commands);
  ASSERT_TRUE(std::holds_alternative<Error>(opened));
  EXPECT_EQ(std::get<Error>(opened).code, ErrorCode::invalid_command);

  RuntimeConfig zero_delay;
  zero_delay.publisher_cursor_persist_max_delay = std::chrono::microseconds(0);
  opened = open_with(zero_delay);
  ASSERT_TRUE(std::holds_alternative<Error>(opened));
  EXPECT_EQ(std::get<Error>(opened).code, ErrorCode::invalid_command);

  RuntimeConfig negative_delay;
  negative_delay.publisher_cursor_persist_max_delay = std::chrono::microseconds(-1);
  opened = open_with(negative_delay);
  ASSERT_TRUE(std::holds_alternative<Error>(opened));
  EXPECT_EQ(std::get<Error>(opened).code, ErrorCode::invalid_command);
}

TEST(EngineTest, PublisherPressureRejectsNewMutationButPreservesDuplicate) {
  const auto data_directory = std::filesystem::temp_directory_path() /
                              "order_books_publisher_pressure_admission_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  BlockingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{7, 1, 1, 1}};
  config.runtime.group_commit_max_commands = 1;
  config.runtime.group_commit_max_delay = std::chrono::microseconds(0);
  config.runtime.max_publish_lag_bytes = 1;

  auto opened = Engine::open(config, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(opened));
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));

  Command first;
  first.identity = CommandIdentity{101, 1, 1, 1};
  first.instrument_id = 7;
  first.command_type = CommandType::new_order;
  first.order_id = {101, 1};
  first.payload = NewOrderPayload{Side::buy, 100, 1};
  const auto first_copy = first;

  std::promise<CommandResult> first_completion;
  auto first_result = first_completion.get_future();
  ASSERT_TRUE(
      engine->submit(first, [&first_completion](CommandResult result) {
        first_completion.set_value(std::move(result));
      }).queued);
  ASSERT_EQ(first_result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  const auto committed = first_result.get();
  ASSERT_EQ(committed.command_status, CommandStatus::committed);
  EXPECT_EQ(committed.error_code, ErrorCode::none);
  ASSERT_TRUE(sink.wait_until_publish_entered(std::chrono::seconds(5)));

  Command pressured;
  pressured.identity = CommandIdentity{102, 1, 1, 1};
  pressured.instrument_id = 7;
  pressured.command_type = CommandType::new_order;
  pressured.order_id = {102, 1};
  pressured.payload = NewOrderPayload{Side::buy, 99, 1};
  std::promise<CommandResult> pressured_completion;
  auto pressured_result = pressured_completion.get_future();
  ASSERT_TRUE(
      engine->submit(std::move(pressured), [&pressured_completion](CommandResult result) {
        pressured_completion.set_value(std::move(result));
      }).queued);
  ASSERT_EQ(pressured_result.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  const auto rejected = pressured_result.get();
  EXPECT_EQ(rejected.command_status, CommandStatus::admission_error);
  EXPECT_EQ(rejected.error_code, ErrorCode::engine_storage_pressure);

  std::promise<CommandResult> duplicate_completion;
  auto duplicate_result = duplicate_completion.get_future();
  ASSERT_TRUE(
      engine->submit(first_copy, [&duplicate_completion](CommandResult result) {
        duplicate_completion.set_value(std::move(result));
      }).queued);
  ASSERT_EQ(duplicate_result.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  const auto duplicate = duplicate_result.get();
  EXPECT_EQ(duplicate.command_status, CommandStatus::committed);
  EXPECT_EQ(duplicate.error_code, ErrorCode::none);
  EXPECT_EQ(duplicate.engine_seq, committed.engine_seq);

  const auto snapshot = engine->metrics(1);
  ASSERT_TRUE(std::holds_alternative<MetricsSnapshot>(snapshot));
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).commands, 1U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).duplicate_commands, 1U);

  sink.release();
  EXPECT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, CompletionOrderingIsPreservedWithinProducerStream) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_completion_order_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{7, 1, 1, 1}};
  config.runtime.group_commit_max_commands = 2;
  config.runtime.group_commit_max_delay = std::chrono::milliseconds(100);

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

  Command first;
  first.identity = CommandIdentity{12, 1, 1, 1};
  first.instrument_id = 7;
  first.command_type = CommandType::new_order;
  first.order_id = {12, 1};
  first.payload = NewOrderPayload{Side::buy, 100, 1};
  ASSERT_TRUE(engine->submit(std::move(first), completion).queued);

  Command second;
  second.identity = CommandIdentity{12, 1, 1, 2};
  second.instrument_id = 7;
  second.command_type = CommandType::new_order;
  second.order_id = {12, 2};
  second.payload = NewOrderPayload{Side::buy, 99, 1};
  ASSERT_TRUE(engine->submit(std::move(second), completion).queued);

  {
    std::unique_lock lock(completion_mutex);
    ASSERT_TRUE(completion_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return completions.size() == 2;
    }));
    ASSERT_EQ(completions.size(), 2U);
    EXPECT_EQ(completions[0].identity.producer_seq, 1U);
    EXPECT_EQ(completions[0].command_status, CommandStatus::committed);
    EXPECT_EQ(completions[1].identity.producer_seq, 2U);
    EXPECT_EQ(completions[1].command_status, CommandStatus::admission_error);
    EXPECT_EQ(completions[1].error_code, ErrorCode::producer_sequence_gap);
  }
  EXPECT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, OpensConfiguredShardAndAcceptsACommand) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_engine_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{7, 1, 1, 1}};

  auto opened = Engine::open(std::move(config), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(opened));
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));

  Command command;
  command.identity = CommandIdentity{9, 1, 1, 1};
  command.instrument_id = 7;
  command.command_type = CommandType::new_order;
  command.order_id = {1, 1};
  command.payload = NewOrderPayload{Side::buy, 100, 2};
  EXPECT_TRUE(engine->submit(std::move(command), {}).queued);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));
  const auto metrics_snapshot = engine->metrics(1);
  ASSERT_TRUE(std::holds_alternative<MetricsSnapshot>(metrics_snapshot));
  EXPECT_EQ(std::get<MetricsSnapshot>(metrics_snapshot).commands, 1U);
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, InvalidIdentityDoesNotConsumeEngineSequence) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_engine_sequence_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{7, 1, 1, 1}};
  config.runtime.group_commit_max_delay = std::chrono::microseconds(0);

  auto opened = Engine::open(config, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(opened));
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));

  Command invalid;
  invalid.identity = CommandIdentity{10, 0, 1, 1};
  invalid.instrument_id = 7;
  invalid.command_type = CommandType::new_order;
  invalid.order_id = {10, 1};
  invalid.payload = NewOrderPayload{Side::buy, 100, 1};
  std::promise<CommandResult> invalid_completion;
  auto invalid_result = invalid_completion.get_future();
  ASSERT_TRUE(engine->submit(std::move(invalid),
                             [&invalid_completion](CommandResult result) {
                               invalid_completion.set_value(std::move(result));
                             })
                  .queued);

  Command valid;
  valid.identity = CommandIdentity{10, 1, 1, 1};
  valid.instrument_id = 7;
  valid.command_type = CommandType::new_order;
  valid.order_id = {10, 2};
  valid.payload = NewOrderPayload{Side::buy, 100, 1};
  std::promise<CommandResult> valid_completion;
  auto valid_result = valid_completion.get_future();
  ASSERT_TRUE(engine->submit(std::move(valid),
                             [&valid_completion](CommandResult result) {
                               valid_completion.set_value(std::move(result));
                             })
                  .queued);

  const auto rejected = invalid_result.get();
  EXPECT_EQ(rejected.command_status, CommandStatus::admission_error);
  EXPECT_EQ(rejected.error_code, ErrorCode::invalid_command);
  const auto committed = valid_result.get();
  EXPECT_EQ(committed.command_status, CommandStatus::committed);
  ASSERT_TRUE(committed.engine_seq.has_value());
  EXPECT_EQ(*committed.engine_seq, 1U);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, RestartRejectsChangedImmutableInstrumentVersion) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_engine_config_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig initial;
  initial.data_directory = data_directory;
  initial.shard_ids = {1};
  initial.instruments = {InstrumentConfig{7, 1, 1, 1}};
  auto first_open = Engine::open(initial, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(first_open));
  auto first_engine = std::get<std::unique_ptr<Engine>>(std::move(first_open));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(first_engine->stop()));

  EngineConfig incompatible = initial;
  incompatible.instruments = {InstrumentConfig{7, 5, 1, 1}};
  auto second_open = Engine::open(std::move(incompatible), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<Error>(second_open));
  EXPECT_EQ(std::get<Error>(second_open).code, ErrorCode::corrupt_snapshot);
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, RestartRejectsActiveInstrumentChangeInNewVersion) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_active_instrument_change_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig initial;
  initial.data_directory = data_directory;
  initial.shard_ids = {1};
  initial.instruments = {InstrumentConfig{7, 1, 1, 1}};
  initial.runtime.group_commit_max_commands = 1;
  initial.runtime.group_commit_max_delay = std::chrono::microseconds(0);
  auto first_open = Engine::open(initial, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(first_open));
  auto first_engine = std::get<std::unique_ptr<Engine>>(std::move(first_open));

  Command command;
  command.identity = CommandIdentity{16, 1, 1, 1};
  command.instrument_id = 7;
  command.command_type = CommandType::new_order;
  command.order_id = {16, 1};
  command.payload = NewOrderPayload{Side::buy, 100, 1};
  std::promise<CommandResult> completion;
  auto result = completion.get_future();
  ASSERT_TRUE(first_engine->submit(std::move(command), [&completion](CommandResult value) {
                                     completion.set_value(std::move(value));
                                   })
                  .queued);
  EXPECT_EQ(result.get().command_status, CommandStatus::committed);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(first_engine->stop()));

  EngineConfig incompatible = initial;
  incompatible.instrument_configuration_version = 2;
  incompatible.instruments = {InstrumentConfig{7, 5, 1, 1}};
  auto second_open = Engine::open(std::move(incompatible), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<Error>(second_open));
  EXPECT_EQ(std::get<Error>(second_open).code, ErrorCode::corrupt_snapshot);
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, RestartRejectsInstrumentMappingChange) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_instrument_mapping_change_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig initial;
  initial.data_directory = data_directory;
  initial.shard_ids = {1, 2};
  initial.instruments = {InstrumentConfig{7, 1, 1, 1}};
  auto first_open = Engine::open(initial, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(first_open));
  auto first_engine = std::get<std::unique_ptr<Engine>>(std::move(first_open));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(first_engine->stop()));

  EngineConfig incompatible = initial;
  incompatible.instrument_configuration_version = 2;
  incompatible.instruments = {InstrumentConfig{7, 1, 1, 2}};
  auto second_open = Engine::open(std::move(incompatible), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<Error>(second_open));
  EXPECT_EQ(std::get<Error>(second_open).code, ErrorCode::corrupt_snapshot);
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, AllowsAddingInstrumentWhenExistingInstrumentHasActiveOrder) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_add_instrument_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig initial;
  initial.data_directory = data_directory;
  initial.shard_ids = {1};
  initial.instruments = {InstrumentConfig{7, 1, 1, 1}};
  initial.runtime.group_commit_max_commands = 1;
  initial.runtime.group_commit_max_delay = std::chrono::microseconds(0);
  auto first_open = Engine::open(initial, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(first_open));
  auto first_engine = std::get<std::unique_ptr<Engine>>(std::move(first_open));

  Command command;
  command.identity = CommandIdentity{13, 1, 1, 1};
  command.instrument_id = 7;
  command.command_type = CommandType::new_order;
  command.order_id = {13, 1};
  command.payload = NewOrderPayload{Side::buy, 100, 1};
  std::promise<CommandResult> completion;
  auto result = completion.get_future();
  ASSERT_TRUE(first_engine->submit(std::move(command), [&completion](CommandResult value) {
                                     completion.set_value(std::move(value));
                                   })
                  .queued);
  EXPECT_EQ(result.get().command_status, CommandStatus::committed);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(first_engine->stop()));

  EngineConfig updated = initial;
  updated.instrument_configuration_version = 2;
  updated.instruments = {InstrumentConfig{7, 1, 1, 1}, InstrumentConfig{8, 5, 1, 1}};
  auto second_open = Engine::open(std::move(updated), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(second_open));
  auto second_engine = std::get<std::unique_ptr<Engine>>(std::move(second_open));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(second_engine->stop()));
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, AllowsChangingInactiveInstrumentConfiguration) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_inactive_instrument_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig initial;
  initial.data_directory = data_directory;
  initial.shard_ids = {1};
  initial.instruments = {InstrumentConfig{7, 1, 1, 1}, InstrumentConfig{8, 5, 1, 1}};
  initial.runtime.group_commit_max_commands = 1;
  initial.runtime.group_commit_max_delay = std::chrono::microseconds(0);
  auto first_open = Engine::open(initial, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(first_open));
  auto first_engine = std::get<std::unique_ptr<Engine>>(std::move(first_open));

  Command command;
  command.identity = CommandIdentity{15, 1, 1, 1};
  command.instrument_id = 7;
  command.command_type = CommandType::new_order;
  command.order_id = {15, 1};
  command.payload = NewOrderPayload{Side::buy, 100, 1};
  std::promise<CommandResult> completion;
  auto result = completion.get_future();
  ASSERT_TRUE(first_engine->submit(std::move(command), [&completion](CommandResult value) {
                                     completion.set_value(std::move(value));
                                   })
                  .queued);
  EXPECT_EQ(result.get().command_status, CommandStatus::committed);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(first_engine->stop()));

  EngineConfig updated = initial;
  updated.instrument_configuration_version = 2;
  updated.instruments = {InstrumentConfig{7, 1, 1, 1}, InstrumentConfig{8, 7, 1, 1}};
  auto second_open = Engine::open(std::move(updated), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(second_open));
  auto second_engine = std::get<std::unique_ptr<Engine>>(std::move(second_open));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(second_engine->stop()));
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, UsesSuppliedBehaviorVersionInsteadOfHistoricalMaximum) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_behavior_selection_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  const ShardBehaviorConfig version_one{1, 1, 3'600'000'000'000LL, 1'000'000};
  const ShardBehaviorConfig version_two{2, 2, 3'600'000'000'000LL, 1'000'000};
  EngineConfig initial;
  initial.data_directory = data_directory;
  initial.shard_ids = {1};
  initial.instruments = {InstrumentConfig{7, 1, 1, 1}};
  initial.shard_behaviors = {version_one, version_two};
  initial.runtime.group_commit_max_commands = 1;
  initial.runtime.group_commit_max_delay = std::chrono::microseconds(0);
  auto first_open = Engine::open(initial, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(first_open));
  auto first_engine = std::get<std::unique_ptr<Engine>>(std::move(first_open));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(first_engine->stop()));

  EngineConfig selected = initial;
  selected.shard_behaviors = {version_one};
  auto second_open = Engine::open(std::move(selected), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(second_open));
  auto second_engine = std::get<std::unique_ptr<Engine>>(std::move(second_open));

  Command first;
  first.identity = CommandIdentity{14, 1, 1, 1};
  first.instrument_id = 7;
  first.command_type = CommandType::new_order;
  first.order_id = {14, 1};
  first.payload = NewOrderPayload{Side::buy, 100, 1};
  std::promise<CommandResult> first_completion;
  auto first_result = first_completion.get_future();
  ASSERT_TRUE(second_engine->submit(std::move(first), [&first_completion](CommandResult value) {
                                      first_completion.set_value(std::move(value));
                                    })
                  .queued);
  EXPECT_EQ(first_result.get().command_status, CommandStatus::committed);

  Command second;
  second.identity = CommandIdentity{14, 1, 1, 2};
  second.instrument_id = 7;
  second.command_type = CommandType::new_order;
  second.order_id = {14, 2};
  second.payload = NewOrderPayload{Side::buy, 99, 1};
  std::promise<CommandResult> second_completion;
  auto second_result = second_completion.get_future();
  ASSERT_TRUE(second_engine->submit(std::move(second), [&second_completion](CommandResult value) {
                                      second_completion.set_value(std::move(value));
                                    })
                  .queued);
  const auto rejected = second_result.get();
  EXPECT_EQ(rejected.command_status, CommandStatus::rejected);
  EXPECT_EQ(rejected.error_code, ErrorCode::shard_capacity_exceeded);

  EXPECT_TRUE(std::holds_alternative<std::monostate>(second_engine->stop()));
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, MissingSnapshotConfigurationManifestFailsRecovery) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_missing_manifest_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{7, 1, 1, 1}};
  config.runtime.group_commit_max_delay = std::chrono::microseconds(0);
  config.runtime.snapshot_interval_commands = 1;

  auto first_open = Engine::open(config, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(first_open));
  auto first_engine = std::get<std::unique_ptr<Engine>>(std::move(first_open));
  Command command;
  command.identity = CommandIdentity{11, 1, 1, 1};
  command.instrument_id = 7;
  command.command_type = CommandType::new_order;
  command.order_id = {11, 1};
  command.payload = NewOrderPayload{Side::buy, 100, 1};
  std::promise<CommandResult> completion;
  auto result = completion.get_future();
  ASSERT_TRUE(first_engine->submit(std::move(command), [&completion](CommandResult value) {
                                     completion.set_value(std::move(value));
                                   })
                  .queued);
  EXPECT_EQ(result.get().command_status, CommandStatus::committed);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(first_engine->stop()));

  std::filesystem::remove(data_directory / "shard-1" / "config" / "instruments-1.bin",
                          ignored);
  auto reopened = Engine::open(std::move(config), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<Error>(reopened));
  EXPECT_EQ(std::get<Error>(reopened).code, ErrorCode::corrupt_snapshot);
  std::filesystem::remove_all(data_directory, ignored);
}

TEST(EngineTest, InvalidPublisherSnapshotIsRejectedBeforeStart) {
  const auto data_directory =
      std::filesystem::temp_directory_path() / "order_books_publisher_snapshot_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  RecordingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{7, 1, 1, 1}};
  auto initial_open = Engine::open(config, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(initial_open));
  auto initial_engine = std::get<std::unique_ptr<Engine>>(std::move(initial_open));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(initial_engine->stop()));

  auto snapshots = storage::SnapshotStore::open(
      data_directory / "shard-1" / "event-replay", 1);
  ASSERT_TRUE(std::holds_alternative<storage::SnapshotStore>(snapshots));
  domain::ShardState invalid;
  invalid.shard_id = 1;
  invalid.current_instrument_configuration_version = 1;
  invalid.current_behavior_configuration_version = 1;
  invalid.instruments.emplace(7, InstrumentConfig{7, 1, 1, 1});
  invalid.instrument_configurations.emplace(1, invalid.instruments);
  invalid.behavior_configurations.emplace(1, ShardBehaviorConfig{});
  invalid.active_order_count = 1;
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      std::get<storage::SnapshotStore>(snapshots).write(invalid)));

  auto reopened = Engine::open(std::move(config), sink, metrics);
  ASSERT_TRUE(std::holds_alternative<Error>(reopened));
  EXPECT_EQ(std::get<Error>(reopened).code, ErrorCode::corrupt_snapshot);
  std::filesystem::remove_all(data_directory, ignored);
}

}  // namespace
}  // namespace order_books
