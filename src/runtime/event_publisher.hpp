#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "domain/state_machine.hpp"
#include "order_books/engine.hpp"
#include "persistence/snapshot_store.hpp"
#include "persistence/wal.hpp"

namespace order_books::runtime {

class EventPublisher {
 public:
  static Result<std::unique_ptr<EventPublisher>> open(
      domain::ShardState initial_state, storage::Wal& wal,
      storage::SnapshotStore replay_snapshots, EventSink& sink,
      MetricsSink& metrics, std::size_t snapshot_interval_commands,
      std::chrono::minutes snapshot_interval);

  ~EventPublisher();

  EventPublisher(const EventPublisher&) = delete;
  EventPublisher& operator=(const EventPublisher&) = delete;

  void start();
  void notify_publishable(storage::WalPosition position);
  Status stop();

  [[nodiscard]] EngineSeq confirmed_cursor() const noexcept {
    return confirmed_cursor_.load(std::memory_order_acquire);
  }
  [[nodiscard]] EngineSeq replay_snapshot_seq() const noexcept {
    return replay_snapshot_seq_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool failed() const noexcept {
    return failed_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t lag_bytes() noexcept;
  [[nodiscard]] std::optional<Timestamp> oldest_unconfirmed_received_at();

 private:
  EventPublisher(domain::ShardState state, storage::Wal& wal,
                 storage::SnapshotStore replay_snapshots, EventSink& sink,
                 MetricsSink& metrics, std::size_t snapshot_interval_commands,
                 std::chrono::minutes snapshot_interval, EngineSeq cursor);

  void run(std::stop_token stop_token);
  Status persist_cursor(EngineSeq cursor);
  Result<EngineSeq> load_cursor() const;

  domain::StateMachine state_machine_;
  storage::Wal& wal_;
  storage::SnapshotStore replay_snapshots_;
  EventSink& sink_;
  MetricsSink& metrics_;
  std::size_t snapshot_interval_commands_{};
  std::chrono::minutes snapshot_interval_{};
  EngineSeq cursor_{};
  std::atomic<EngineSeq> confirmed_cursor_{};
  std::atomic<EngineSeq> replay_snapshot_seq_{};
  EngineSeq last_snapshot_seq_{};
  std::atomic<EngineSeq> publishable_seq_{};
  std::atomic<bool> failed_{false};
  std::chrono::steady_clock::time_point last_snapshot_time_{};
  std::mutex mutex_;
  std::condition_variable_any condition_;
  std::jthread thread_;
  bool started_{false};
};

}  // namespace order_books::runtime
