#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <utility>
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
  EXPECT_TRUE(std::holds_alternative<std::monostate>(engine->stop()));

  std::filesystem::remove_all(data_directory, ignored);
}

}  // namespace
}  // namespace order_books
