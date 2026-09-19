#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "order_books/event_sink.hpp"
#include "order_books/metrics.hpp"
#include "order_books/model.hpp"

namespace order_books {

struct RuntimeConfig {
  std::size_t max_instruments_per_shard{1024};
  std::size_t max_envelope_bytes{1024U * 1024U};
  std::size_t ingress_queue_capacity{65'536};
  std::chrono::microseconds group_commit_max_delay{200};
  std::size_t group_commit_max_commands{256};
  std::size_t wal_segment_size{256U * 1024U * 1024U};
  std::uint64_t wal_soft_limit_bytes{64ULL * 1024ULL * 1024ULL * 1024ULL};
  std::chrono::minutes snapshot_interval{5};
  std::size_t snapshot_interval_commands{1'000'000};
  std::chrono::minutes event_replay_snapshot_interval{5};
  std::size_t event_replay_snapshot_interval_commands{1'000'000};
  std::size_t publisher_cursor_persist_max_commands{256};
  std::chrono::microseconds publisher_cursor_persist_max_delay{1000};
  std::chrono::hours max_publish_lag_age{24};
  std::uint64_t max_publish_lag_bytes{32ULL * 1024ULL * 1024ULL * 1024ULL};
  std::size_t max_top_n{10'000};
  // Bounded WAL preparation lanes include the shard writer caller.  The
  // default keeps the production path single-threaded; values >1 are an
  // explicit deployment opt-in.
  std::size_t wal_prepare_lanes{1};
  std::size_t wal_parallel_prepare_min_commands{4096};
};

struct EngineConfig {
  std::filesystem::path data_directory;
  std::vector<ShardId> shard_ids;
  std::vector<InstrumentConfig> instruments;
  std::vector<ShardBehaviorConfig> shard_behaviors;
  ConfigurationVersion instrument_configuration_version{1};
  RuntimeConfig runtime;
};

using CompletionHandler = std::function<void(CommandResult)>;

struct SubmitResult {
  bool queued{false};
  std::optional<Error> error;
};

class Engine {
 public:
  static Result<std::unique_ptr<Engine>> open(EngineConfig config,
                                               EventSink& event_sink,
                                               MetricsSink& metrics_sink);

  ~Engine();

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  SubmitResult submit(Command command, CompletionHandler completion);
  Result<OrderView> get_order(InstrumentId instrument_id, OrderId order_id);
  Result<BookDepth> depth(InstrumentId instrument_id, Side side,
                          std::size_t limit);
  Result<std::optional<BookLevel>> best(InstrumentId instrument_id, Side side);
  Result<MetricsSnapshot> metrics(ShardId shard_id) const;
  Status stop();

 private:
  struct Impl;
  explicit Engine(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace order_books
