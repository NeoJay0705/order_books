#pragma once

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <variant>
#include <vector>

#include "domain/state_machine.hpp"
#include "order_books/engine.hpp"
#include "persistence/snapshot_store.hpp"
#include "persistence/wal.hpp"
#include "runtime/event_publisher.hpp"
#include "runtime/metrics_registry.hpp"
#include "runtime/writer_profile.hpp"

namespace order_books::runtime {

struct ShardRuntimeTestPeer;

class ShardRuntime {
 public:
  static Result<std::unique_ptr<ShardRuntime>> open(
      ShardId shard_id, const EngineConfig& config, EventSink& event_sink,
      MetricsSink& metrics_sink, WriterProfileCollector* profile = nullptr,
      WriterProfileOptions profile_options = {});

  ~ShardRuntime();

  ShardRuntime(const ShardRuntime&) = delete;
  ShardRuntime& operator=(const ShardRuntime&) = delete;

  void start();
  // Internal benchmark control: profiling is disabled during warmup and
  // re-enabled with a fresh sampling sequence for the measured phase.
  void set_writer_profile_phase_active(bool active) noexcept;
  void reset_writer_profile_phase() noexcept;
  SubmitResult submit(Command command, CompletionHandler completion);
  std::future<Result<OrderView>> get_order(InstrumentId instrument_id, OrderId order_id);
  std::future<Result<BookDepth>> depth(InstrumentId instrument_id, Side side,
                                       std::size_t limit);
  std::future<Result<std::optional<BookLevel>>> best(InstrumentId instrument_id,
                                                     Side side);
  [[nodiscard]] MetricsSnapshot metrics() const;
  Status stop();

  [[nodiscard]] ShardId shard_id() const noexcept { return shard_id_; }

 private:
  friend struct ShardRuntimeTestPeer;

  struct CommandWork {
    Command command;
    CompletionHandler completion;
    std::chrono::steady_clock::time_point enqueued_at;
  };
  struct QueryWork {
    std::function<void(const domain::StateMachine&)> execute;
    std::function<void(Error)> reject;
  };
  using Work = std::variant<CommandWork, QueryWork>;

  ShardRuntime(ShardId shard_id, EngineConfig config,
               std::unique_ptr<MetricsRegistry> metrics_registry,
               std::unique_ptr<MetricsRegistry> publisher_metrics,
               std::unique_ptr<storage::Wal> wal,
               storage::SnapshotStore snapshots,
               domain::StateMachine state_machine,
               std::unique_ptr<EventPublisher> publisher,
               WriterProfileCollector* profile,
               WriterProfileOptions profile_options);

  void run(std::stop_token stop_token);
  void completion_run(std::stop_token stop_token);
  void process_command_batch(std::vector<CommandWork> batch,
                             std::chrono::steady_clock::time_point group_start,
                             std::uint64_t group_collect_ns,
                             std::uint64_t group_wait_ns,
                             bool profile_sampled);
  void process_query(const QueryWork& query);
  void dispatch(CommandResult result, CompletionHandler& completion,
                bool profile_completion = false);
  void fail(Error error);

  [[nodiscard]] Result<domain::CommittedCommand> prepare(
      const Command& command, EngineSeq engine_seq);
  [[nodiscard]] CommandResult admission_error(const Command& command,
                                               ErrorCode code) const;
  [[nodiscard]] bool is_duplicate(const Command& command,
                                  const domain::ProducerState& producer) const;
  [[nodiscard]] bool enqueue(Work work);

  ShardId shard_id_{};
  EngineConfig config_;
  std::unique_ptr<MetricsRegistry> metrics_registry_;
  std::unique_ptr<MetricsRegistry> publisher_metrics_;
  std::unique_ptr<storage::Wal> wal_;
  storage::SnapshotStore snapshots_;
  domain::StateMachine state_machine_;
  std::unique_ptr<EventPublisher> publisher_;
  WriterProfileCollector* profile_{};
  WriterProfileOptions profile_options_{};
  std::atomic<std::uint64_t> profile_groups_seen_{};
  std::atomic<bool> profile_phase_active_{true};

  std::mutex queue_mutex_;
  std::condition_variable_any queue_condition_;
  std::deque<Work> queue_;
  std::mutex completion_mutex_;
  std::condition_variable_any completion_condition_;
  std::deque<std::pair<CommandResult, CompletionHandler>> completions_;
  std::jthread worker_;
  std::jthread completion_worker_;
  bool started_{false};
  bool stopping_{false};
  bool failed_{false};
  std::optional<Error> failure_;
  std::chrono::steady_clock::time_point last_snapshot_time_;
  std::size_t commands_since_snapshot_{};
};

}  // namespace order_books::runtime
