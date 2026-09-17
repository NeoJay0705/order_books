#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <stop_token>
#include <utility>
#include <variant>
#include <vector>

#include "order_books/engine.hpp"

namespace order_books {
namespace {

class AcknowledgingSink final : public EventSink {
 public:
  Result<std::monostate> publish(ShardId, EngineSeq, std::span<const Event>,
                                 std::stop_token) override {
    return std::monostate{};
  }
};

Command make_admission_error_command(const ProducerId producer_id,
                                     const std::uint64_t order_id) {
  Command command;
  command.identity = CommandIdentity{producer_id, 0, 1, 1};
  command.instrument_id = 1;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{1, order_id};
  command.payload = NewOrderPayload{Side::buy, 100, 1};
  return command;
}

TEST(EnginePipelineCeilingTest, AdmissionHandoffCompletesExactlyOnce) {
  const auto data_directory = std::filesystem::temp_directory_path() /
                              "order_books_engine_pipeline_ceiling_test";
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);

  AcknowledgingSink sink;
  NullMetricsSink metrics;
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.group_commit_max_commands = 4;
  config.runtime.group_commit_max_delay = std::chrono::microseconds(0);

  auto opened = Engine::open(config, sink, metrics);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Engine>>(opened));
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));

  constexpr std::size_t kCommands = 8;
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

  for (std::size_t index = 0; index < kCommands; ++index) {
    ASSERT_TRUE(engine
                    ->submit(make_admission_error_command(index + 1U, index + 1U),
                             completion)
                    .queued);
  }

  {
    std::unique_lock lock(completion_mutex);
    ASSERT_TRUE(completion_condition.wait_for(lock, std::chrono::seconds(5), [&] {
      return completions.size() == kCommands;
    }));
  }
  ASSERT_EQ(completions.size(), kCommands);
  std::vector<bool> seen(kCommands + 1U);
  for (const auto& result : completions) {
    ASSERT_GT(result.identity.producer_id, 0U);
    ASSERT_LE(result.identity.producer_id, kCommands);
    EXPECT_FALSE(seen[result.identity.producer_id]);
    seen[result.identity.producer_id] = true;
    EXPECT_EQ(result.identity.producer_epoch, 0U);
    EXPECT_EQ(result.command_status, CommandStatus::admission_error);
    EXPECT_EQ(result.error_code, ErrorCode::invalid_command);
    EXPECT_FALSE(result.engine_seq.has_value());
  }

  const auto snapshot = engine->metrics(1);
  ASSERT_TRUE(std::holds_alternative<MetricsSnapshot>(snapshot));
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).commands, 0U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).active_orders, 0U);
  EXPECT_EQ(std::get<MetricsSnapshot>(snapshot).active_price_levels, 0U);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));
  std::filesystem::remove_all(data_directory, ignored);
}

}  // namespace
}  // namespace order_books
