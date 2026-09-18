#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "domain/state_machine.hpp"
#include "order_books/event_sink.hpp"
#include "persistence/binary_codec.hpp"
#include "persistence/snapshot_store.hpp"
#include "persistence/wal.hpp"
#include "runtime/event_publisher.hpp"

namespace order_books::runtime {

struct EventPublisherTestPeer {
  static void add_invalid_inactive_configuration(EventPublisher& publisher) {
    auto& state = publisher.state_machine_.state();
    state.instrument_configurations.emplace(
        99, std::unordered_map<InstrumentId, InstrumentConfig>{
                {99, InstrumentConfig{99, 0, 1, publisher.state_machine_.state().shard_id}}});
  }

  static Result<std::optional<domain::ShardState>> load_snapshot(
      const EventPublisher& publisher) {
    return publisher.replay_snapshots_.load_latest();
  }
};

namespace {

using namespace std::chrono_literals;

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("order_books_event_publisher_" + std::to_string(stamp));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

class TestSink final : public EventSink {
 public:
  Result<std::monostate> publish(const ShardId, const EngineSeq engine_seq,
                                 const std::span<const Event> events,
                                 const std::stop_token stop_token) override {
    std::unique_lock lock(mutex_);
    sequences_.push_back(engine_seq);
    const auto call_number = sequences_.size();
    if (blocked_call_ == call_number) {
      blocked_entered_ = true;
      condition_.notify_all();
      condition_.wait(lock, stop_token, [this] { return released_; });
      if (stop_token.stop_requested()) {
        return Error{ErrorCode::engine_unavailable, "test sink stopped"};
      }
    }
    if (failures_remaining_ != 0) {
      --failures_remaining_;
      condition_.notify_all();
      return Error{ErrorCode::engine_unavailable, "test sink failure"};
    }
    for (const auto& event : events) {
      acknowledged_event_ids_.push_back(event.id);
    }
    condition_.notify_all();
    return std::monostate{};
  }

  void block_on_call(const std::size_t call_number) {
    std::lock_guard lock(mutex_);
    blocked_call_ = call_number;
    released_ = false;
  }

  void fail_next(const std::size_t count = 1) {
    std::lock_guard lock(mutex_);
    failures_remaining_ = count;
  }

  bool wait_until_call(const std::size_t call_number,
                      const std::chrono::milliseconds timeout = 5s) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this, call_number] {
      return sequences_.size() >= call_number &&
             (blocked_call_ != call_number || blocked_entered_);
    });
  }

  void release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

  [[nodiscard]] std::size_t calls() const {
    std::lock_guard lock(mutex_);
    return sequences_.size();
  }

  [[nodiscard]] std::vector<EngineSeq> sequences() const {
    std::lock_guard lock(mutex_);
    return sequences_;
  }

  [[nodiscard]] std::vector<EventId> acknowledged_event_ids() const {
    std::lock_guard lock(mutex_);
    return acknowledged_event_ids_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::vector<EngineSeq> sequences_;
  std::vector<EventId> acknowledged_event_ids_;
  std::size_t failures_remaining_{0};
  std::size_t blocked_call_{0};
  bool blocked_entered_{false};
  bool released_{false};
};

domain::ShardState genesis_state() {
  domain::ShardState state;
  state.shard_id = 1;
  state.current_instrument_configuration_version = 1;
  state.current_behavior_configuration_version = 1;
  state.instruments.emplace(7, InstrumentConfig{7, 1, 1, 1});
  state.instrument_configurations.emplace(1, state.instruments);
  state.behavior_configurations.emplace(
      1, ShardBehaviorConfig{1, 1'000'000, 3'600'000'000'000LL, 1'000'000});
  return state;
}

domain::CommittedCommand new_order(const EngineSeq sequence) {
  Command command;
  command.identity = CommandIdentity{17, 1, 1, sequence};
  command.instrument_id = 7;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{9, sequence};
  command.payload = NewOrderPayload{Side::buy, 100, 1};
  return domain::CommittedCommand{std::move(command), sequence,
                                  static_cast<Timestamp>(sequence), 1, 1};
}

domain::CommittedCommand no_change_amend(const EngineSeq sequence) {
  Command command;
  command.identity = CommandIdentity{17, 1, 1, sequence};
  command.instrument_id = 7;
  command.command_type = CommandType::amend_quantity;
  command.order_id = OrderId{9, 1};
  command.payload = AmendQuantityPayload{1};
  return domain::CommittedCommand{std::move(command), sequence,
                                  static_cast<Timestamp>(sequence), 1, 1};
}

struct WalFixture {
  std::unique_ptr<storage::Wal> wal;
  domain::ShardState live_state;
};

WalFixture make_fixture(const std::filesystem::path& directory,
                        const std::vector<domain::CommittedCommand>& commands) {
  auto opened = storage::Wal::open(directory / "wal", 1, 1U * 1024U * 1024U);
  if (std::holds_alternative<Error>(opened)) {
    throw std::runtime_error(std::get<Error>(opened).message);
  }
  auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(opened));
  if (std::holds_alternative<Error>(wal->append_batch(commands))) {
    throw std::runtime_error("failed to append test WAL");
  }
  if (std::holds_alternative<Error>(wal->sync())) {
    throw std::runtime_error("failed to sync test WAL");
  }
  auto replayed = wal->replay();
  if (std::holds_alternative<Error>(replayed) ||
      std::get<std::vector<domain::CommittedCommand>>(replayed).size() != commands.size()) {
    throw std::runtime_error("failed to initialize test WAL reader");
  }
  domain::StateMachine live_machine(genesis_state());
  for (const auto& command : commands) {
    auto execution = live_machine.apply(command);
    if (std::holds_alternative<Error>(execution)) {
      throw std::runtime_error(std::get<Error>(execution).message);
    }
  }
  return WalFixture{std::move(wal), std::move(live_machine.state())};
}

domain::ShardState clone_state(const domain::ShardState& state) {
  auto decoded = storage::decode_state(storage::encode_state(state));
  if (std::holds_alternative<Error>(decoded)) {
    throw std::runtime_error(std::get<Error>(decoded).message);
  }
  return std::get<domain::ShardState>(std::move(decoded));
}

Result<std::unique_ptr<EventPublisher>> open_publisher_with_metrics(
    const std::filesystem::path& directory, storage::Wal& wal,
    const domain::ShardState& state, EventSink& sink, MetricsSink& metrics,
    const std::size_t max_commands, const std::chrono::microseconds max_delay,
    const std::size_t snapshot_interval_commands = std::numeric_limits<std::size_t>::max()) {
  auto snapshots_result = storage::SnapshotStore::open(directory / "event-replay", 1);
  if (std::holds_alternative<Error>(snapshots_result)) {
    return std::get<Error>(snapshots_result);
  }
  return EventPublisher::open(
      clone_state(state), wal,
      std::get<storage::SnapshotStore>(std::move(snapshots_result)), sink, metrics,
      snapshot_interval_commands, 24h, max_commands, max_delay);
}

template <typename Predicate>
bool wait_for(Predicate predicate, const std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

TEST(EventPublisherTest, CountTriggerFlushesBeforeNextWalRecord) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1), new_order(2),
                                                                new_order(3)};
  auto fixture = make_fixture(temporary.path(), commands);
  TestSink sink;
  sink.block_on_call(3);
  NullMetricsSink metrics;
  auto opened = open_publisher_with_metrics(temporary.path(), *fixture.wal,
                                             fixture.live_state, sink, metrics, 2, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(opened));
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(opened));

  publisher->start();
  publisher->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(sink.wait_until_call(3));
  EXPECT_EQ(publisher->confirmed_cursor(), 2U);
  EXPECT_EQ(publisher->durable_cursor(), 2U);
  sink.release();
  ASSERT_TRUE(wait_for([&] { return publisher->confirmed_cursor() == 3U; }));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher->stop()));
  EXPECT_EQ(publisher->durable_cursor(), 3U);
  EXPECT_EQ(publisher->successful_cursor_persists(), 2U);
}

TEST(EventPublisherTest, CountTriggerDoesNotFlushBeforeThreshold) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1)};
  auto fixture = make_fixture(temporary.path(), commands);
  TestSink sink;
  NullMetricsSink metrics;
  auto opened = open_publisher_with_metrics(temporary.path(), *fixture.wal,
                                             fixture.live_state, sink, metrics, 2, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(opened));
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(opened));

  publisher->start();
  publisher->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(wait_for([&] { return publisher->confirmed_cursor() == 1U; }));
  EXPECT_EQ(publisher->durable_cursor(), 0U);
  EXPECT_EQ(publisher->successful_cursor_persists(), 0U);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher->stop()));
  EXPECT_EQ(publisher->durable_cursor(), 1U);
  EXPECT_EQ(publisher->successful_cursor_persists(), 1U);
}

TEST(EventPublisherTest, TimeTriggerFlushesWithoutAnotherNotification) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1)};
  auto fixture = make_fixture(temporary.path(), commands);
  TestSink sink;
  NullMetricsSink metrics;
  auto opened = open_publisher_with_metrics(temporary.path(), *fixture.wal,
                                             fixture.live_state, sink, metrics, 100, 1ms);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(opened));
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(opened));

  publisher->start();
  publisher->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(sink.wait_until_call(1));
  ASSERT_TRUE(wait_for([&] { return publisher->durable_cursor() == 1U; }));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher->stop()));
  EXPECT_EQ(publisher->successful_cursor_persists(), 1U);
}

TEST(EventPublisherTest, CleanStopFlushesConfirmedCursor) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1)};
  auto fixture = make_fixture(temporary.path(), commands);
  TestSink sink;
  NullMetricsSink metrics;
  auto opened = open_publisher_with_metrics(temporary.path(), *fixture.wal,
                                             fixture.live_state, sink, metrics, 100, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(opened));
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(opened));
  publisher->start();
  publisher->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(sink.wait_until_call(1));
  ASSERT_EQ(publisher->confirmed_cursor(), 1U);
  EXPECT_EQ(publisher->durable_cursor(), 0U);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher->stop()));
  EXPECT_EQ(publisher->durable_cursor(), 1U);

  auto reopened_result = open_publisher_with_metrics(
      temporary.path(), *fixture.wal, fixture.live_state, sink, metrics, 100, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(reopened_result));
  auto reopened = std::get<std::unique_ptr<EventPublisher>>(std::move(reopened_result));
  EXPECT_EQ(reopened->confirmed_cursor(), 1U);
  EXPECT_EQ(reopened->durable_cursor(), 1U);
  EXPECT_EQ(sink.calls(), 1U);
}

TEST(EventPublisherTest, CursorFlushFailureRemainsVisibleOnRepeatedStop) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1)};
  auto fixture = make_fixture(temporary.path(), commands);
  TestSink sink;
  NullMetricsSink metrics;
  auto opened = open_publisher_with_metrics(temporary.path(), *fixture.wal,
                                             fixture.live_state, sink, metrics, 100, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(opened));
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(opened));
  publisher->start();
  publisher->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(wait_for([&] { return publisher->confirmed_cursor() == 1U; }));
  EXPECT_EQ(publisher->durable_cursor(), 0U);

  std::error_code filesystem_error;
  std::filesystem::remove_all(temporary.path() / "event-replay", filesystem_error);
  ASSERT_FALSE(filesystem_error);

  const auto first_stop = publisher->stop();
  ASSERT_TRUE(std::holds_alternative<Error>(first_stop));
  EXPECT_EQ(std::get<Error>(first_stop).code, ErrorCode::engine_unavailable);
  const auto second_stop = publisher->stop();
  ASSERT_TRUE(std::holds_alternative<Error>(second_stop));
  EXPECT_EQ(std::get<Error>(second_stop).code, ErrorCode::engine_unavailable);
  EXPECT_EQ(publisher->durable_cursor(), 0U);
  EXPECT_EQ(publisher->successful_cursor_persists(), 0U);
}

TEST(EventPublisherTest, SinkRetryDoesNotAdvanceCursorBeforeAck) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1)};
  auto fixture = make_fixture(temporary.path(), commands);
  TestSink sink;
  sink.fail_next();
  sink.block_on_call(2);
  NullMetricsSink metrics;
  auto opened = open_publisher_with_metrics(temporary.path(), *fixture.wal,
                                             fixture.live_state, sink, metrics, 1, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(opened));
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(opened));
  publisher->start();
  publisher->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(sink.wait_until_call(2));
  EXPECT_EQ(publisher->confirmed_cursor(), 0U);
  EXPECT_EQ(publisher->durable_cursor(), 0U);
  sink.release();
  ASSERT_TRUE(wait_for([&] { return publisher->confirmed_cursor() == 1U; }));
  ASSERT_TRUE(wait_for([&] { return publisher->durable_cursor() == 1U; }));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher->stop()));
  EXPECT_EQ(publisher->durable_cursor(), 1U);
  EXPECT_EQ(sink.sequences(), (std::vector<EngineSeq>{1, 1}));
}

TEST(EventPublisherTest, ZeroEventCommandCountsTowardCursorGroup) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1),
                                                                no_change_amend(2)};
  auto fixture = make_fixture(temporary.path(), commands);
  TestSink sink;
  NullMetricsSink metrics;
  auto opened = open_publisher_with_metrics(temporary.path(), *fixture.wal,
                                             fixture.live_state, sink, metrics, 2, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(opened));
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(opened));
  publisher->start();
  publisher->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(wait_for([&] { return publisher->confirmed_cursor() == 2U; }));
  ASSERT_TRUE(wait_for([&] { return publisher->durable_cursor() == 2U; }));
  EXPECT_EQ(publisher->successful_cursor_persists(), 1U);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher->stop()));
  EXPECT_EQ(sink.sequences(), (std::vector<EngineSeq>{1}));
  EXPECT_EQ(publisher->durable_cursor(), 2U);
}

TEST(EventPublisherTest, SnapshotSequenceNeverExceedsDurableCursor) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1), new_order(2)};
  auto fixture = make_fixture(temporary.path(), commands);
  TestSink sink;
  NullMetricsSink metrics;
  auto opened = open_publisher_with_metrics(temporary.path(), *fixture.wal,
                                             fixture.live_state, sink, metrics, 100, 10s, 1);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(opened));
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(opened));
  publisher->start();
  publisher->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(wait_for([&] { return publisher->replay_snapshot_seq() == 2U; }));
  EXPECT_EQ(publisher->durable_cursor(), 2U);
  EXPECT_LE(publisher->replay_snapshot_seq(), publisher->durable_cursor());
  ASSERT_TRUE(std::holds_alternative<std::monostate>(publisher->stop()));
  EXPECT_LE(publisher->replay_snapshot_seq(), publisher->durable_cursor());
  EXPECT_EQ(publisher->replay_snapshot_seq(), 2U);
}

TEST(EventPublisherTest, SnapshotValidationFailureDoesNotWriteSnapshot) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1)};
  auto fixture = make_fixture(temporary.path(), commands);
  TestSink sink;
  NullMetricsSink metrics;
  auto opened = open_publisher_with_metrics(temporary.path(), *fixture.wal,
                                             fixture.live_state, sink, metrics, 100, 10s, 1);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(opened));
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(opened));
  EventPublisherTestPeer::add_invalid_inactive_configuration(*publisher);

  publisher->start();
  publisher->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(wait_for([&] { return publisher->failed(); }));
  EXPECT_EQ(publisher->replay_snapshot_seq(), 0U);
  EXPECT_EQ(sink.calls(), 1U);

  auto snapshot = EventPublisherTestPeer::load_snapshot(*publisher);
  ASSERT_TRUE(std::holds_alternative<std::optional<domain::ShardState>>(snapshot));
  EXPECT_FALSE(std::get<std::optional<domain::ShardState>>(snapshot).has_value());
  (void)publisher->stop();
}

TEST(EventPublisherTest, CrashImageReplaysOnlyTheNonDurableWindow) {
  TemporaryDirectory temporary;
  const auto commands = std::vector<domain::CommittedCommand>{new_order(1), new_order(2),
                                                                new_order(3)};
  auto fixture = make_fixture(temporary.path(), commands);
  NullMetricsSink metrics;
  TestSink first_sink;
  auto first_opened = open_publisher_with_metrics(
      temporary.path(), *fixture.wal, fixture.live_state, first_sink, metrics, 1, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(first_opened));
  auto first = std::get<std::unique_ptr<EventPublisher>>(std::move(first_opened));
  first->start();
  first->notify_publishable(storage::WalPosition{1, {}, 0});
  ASSERT_TRUE(wait_for([&] { return first->confirmed_cursor() == 1U; }));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(first->stop()));
  EXPECT_EQ(first->durable_cursor(), 1U);

  TestSink second_sink;
  auto second_opened = open_publisher_with_metrics(
      temporary.path(), *fixture.wal, fixture.live_state, second_sink, metrics, 100, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(second_opened));
  auto second = std::get<std::unique_ptr<EventPublisher>>(std::move(second_opened));
  EXPECT_EQ(second->confirmed_cursor(), 1U);
  second->start();
  second->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(wait_for([&] { return second->confirmed_cursor() == 3U; }));
  EXPECT_EQ(second->durable_cursor(), 1U);
  const auto expected_event_ids = second_sink.acknowledged_event_ids();
  ASSERT_FALSE(expected_event_ids.empty());

  const auto crash_image = temporary.path() / "crash-image";
  std::error_code filesystem_error;
  ASSERT_TRUE(std::filesystem::create_directories(crash_image, filesystem_error) ||
              !filesystem_error);
  std::filesystem::copy(temporary.path() / "event-replay", crash_image / "event-replay",
                        std::filesystem::copy_options::recursive, filesystem_error);
  ASSERT_FALSE(filesystem_error);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(second->stop()));

  TestSink replay_sink;
  auto replayed_opened = open_publisher_with_metrics(
      crash_image, *fixture.wal, fixture.live_state, replay_sink, metrics, 100, 10s);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<EventPublisher>>(replayed_opened));
  auto replayed = std::get<std::unique_ptr<EventPublisher>>(std::move(replayed_opened));
  EXPECT_EQ(replayed->confirmed_cursor(), 1U);
  EXPECT_EQ(replayed->durable_cursor(), 1U);
  replayed->start();
  replayed->notify_publishable(fixture.wal->durable_position());
  ASSERT_TRUE(wait_for([&] { return replayed->confirmed_cursor() == 3U; }));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(replayed->stop()));
  EXPECT_EQ(replay_sink.sequences(), (std::vector<EngineSeq>{2, 3}));
  EXPECT_EQ(replay_sink.acknowledged_event_ids(), expected_event_ids);
  EXPECT_EQ(replayed->durable_cursor(), 3U);
}

}  // namespace
}  // namespace order_books::runtime
