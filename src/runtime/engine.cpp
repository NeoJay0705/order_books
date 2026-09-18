#include "order_books/engine.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "runtime/shard_runtime.hpp"

namespace order_books {
namespace {

Error config_error(const char* message) {
  return Error{ErrorCode::invalid_command, message};
}

Error unavailable(const char* message) {
  return Error{ErrorCode::engine_unavailable, message};
}

}  // namespace

struct Engine::Impl {
  EngineConfig config;
  EventSink& event_sink;
  MetricsSink& metrics_sink;
  std::unordered_map<ShardId, std::unique_ptr<runtime::ShardRuntime>> shards;
  std::unordered_map<InstrumentId, ShardId> instrument_routes;
};

Engine::Engine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Result<std::unique_ptr<Engine>> Engine::open(EngineConfig config,
                                             EventSink& event_sink,
                                             MetricsSink& metrics_sink) {
  if (config.data_directory.empty() || config.shard_ids.empty() ||
      config.instrument_configuration_version == 0) {
    return config_error("engine configuration is incomplete");
  }
  if (config.runtime.max_instruments_per_shard == 0 ||
      config.runtime.max_top_n == 0 || config.runtime.group_commit_max_delay.count() < 0 ||
      config.runtime.snapshot_interval.count() < 0 ||
      config.runtime.event_replay_snapshot_interval.count() < 0 ||
      config.runtime.publisher_cursor_persist_max_commands == 0 ||
      config.runtime.publisher_cursor_persist_max_delay.count() <= 0 ||
      config.runtime.max_publish_lag_age.count() <= 0) {
    return config_error("runtime limits must be positive");
  }

  std::unordered_set<ShardId> shard_set;
  for (const auto shard_id : config.shard_ids) {
    if (!shard_set.emplace(shard_id).second) {
      return config_error("duplicate shard id");
    }
  }
  std::unordered_map<ConfigurationVersion, ShardBehaviorConfig> behavior_versions;
  for (const auto& behavior : config.shard_behaviors) {
    if (behavior.version == 0 || behavior.max_active_orders == 0 ||
        behavior.terminal_tombstone_max_age_ns <= 0 ||
        behavior.terminal_tombstone_max_count == 0 ||
        !behavior_versions.emplace(behavior.version, behavior).second) {
      return config_error("duplicate behavior configuration version");
    }
  }
  if (config.shard_behaviors.empty()) {
    config.shard_behaviors.push_back(ShardBehaviorConfig{});
  }

  std::unordered_map<InstrumentId, ShardId> routes;
  std::unordered_map<ShardId, std::size_t> instrument_counts;
  for (const auto& instrument : config.instruments) {
    if (instrument.instrument_id == 0 || instrument.tick_size <= 0 ||
        instrument.lot_size <= 0 || shard_set.find(instrument.assigned_shard) == shard_set.end()) {
      return config_error("invalid instrument configuration");
    }
    if (!routes.emplace(instrument.instrument_id, instrument.assigned_shard).second) {
      return config_error("duplicate instrument id");
    }
    if (++instrument_counts[instrument.assigned_shard] >
        config.runtime.max_instruments_per_shard) {
      return config_error("instrument capacity exceeded");
    }
  }

  auto impl = std::make_unique<Impl>(Impl{std::move(config), event_sink, metrics_sink, {},
                                          std::move(routes)});
  for (const auto shard_id : impl->config.shard_ids) {
    auto runtime = runtime::ShardRuntime::open(shard_id, impl->config, event_sink,
                                               metrics_sink);
    if (std::holds_alternative<Error>(runtime)) {
      for (auto& [unused_id, existing] : impl->shards) {
        (void)unused_id;
        (void)existing->stop();
      }
      return std::get<Error>(runtime);
    }
    impl->shards.emplace(shard_id,
                         std::get<std::unique_ptr<runtime::ShardRuntime>>(std::move(runtime)));
  }

  auto engine = std::unique_ptr<Engine>(new Engine(std::move(impl)));
  for (auto& [unused_id, shard] : engine->impl_->shards) {
    (void)unused_id;
    shard->start();
  }
  return engine;
}

Engine::~Engine() {
  if (impl_ != nullptr) {
    (void)stop();
  }
}

SubmitResult Engine::submit(Command command, CompletionHandler completion) {
  if (impl_ == nullptr) {
    return SubmitResult{false, unavailable("engine is closed")};
  }
  const auto route = impl_->shards.find(command.identity.producer_stream_id);
  if (route == impl_->shards.end()) {
    return SubmitResult{false, unavailable("producer stream selects unknown shard")};
  }
  const auto instrument = impl_->instrument_routes.find(command.instrument_id);
  if (instrument != impl_->instrument_routes.end() &&
      instrument->second != command.identity.producer_stream_id) {
    return SubmitResult{false,
                        Error{ErrorCode::wrong_producer_stream,
                              "instrument is assigned to another shard"}};
  }
  return route->second->submit(std::move(command), std::move(completion));
}

Result<OrderView> Engine::get_order(const InstrumentId instrument_id,
                                    const OrderId order_id) {
  if (impl_ == nullptr) {
    return unavailable("engine is closed");
  }
  const auto route = impl_->instrument_routes.find(instrument_id);
  if (route == impl_->instrument_routes.end()) {
    return Error{ErrorCode::unknown_instrument, "unknown instrument"};
  }
  return impl_->shards.at(route->second)->get_order(instrument_id, order_id).get();
}

Result<BookDepth> Engine::depth(const InstrumentId instrument_id, const Side side,
                                const std::size_t limit) {
  if (impl_ == nullptr) {
    return unavailable("engine is closed");
  }
  const auto route = impl_->instrument_routes.find(instrument_id);
  if (route == impl_->instrument_routes.end()) {
    return Error{ErrorCode::unknown_instrument, "unknown instrument"};
  }
  return impl_->shards.at(route->second)->depth(instrument_id, side, limit).get();
}

Result<std::optional<BookLevel>> Engine::best(const InstrumentId instrument_id,
                                              const Side side) {
  if (impl_ == nullptr) {
    return unavailable("engine is closed");
  }
  const auto route = impl_->instrument_routes.find(instrument_id);
  if (route == impl_->instrument_routes.end()) {
    return Error{ErrorCode::unknown_instrument, "unknown instrument"};
  }
  return impl_->shards.at(route->second)->best(instrument_id, side).get();
}

Result<MetricsSnapshot> Engine::metrics(const ShardId shard_id) const {
  if (impl_ == nullptr) {
    return unavailable("engine is closed");
  }
  const auto shard = impl_->shards.find(shard_id);
  if (shard == impl_->shards.end()) {
    return unavailable("unknown shard");
  }
  return shard->second->metrics();
}

Status Engine::stop() {
  if (impl_ == nullptr) {
    return std::monostate{};
  }
  std::optional<Error> first_error;
  for (auto& [unused_id, shard] : impl_->shards) {
    (void)unused_id;
    auto status = shard->stop();
    if (!first_error.has_value() && std::holds_alternative<Error>(status)) {
      first_error = std::get<Error>(std::move(status));
    }
  }
  return first_error.has_value() ? Status{*first_error} : Status{std::monostate{}};
}

}  // namespace order_books
