#include "runtime/shard_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

#include "persistence/binary_codec.hpp"
#include "persistence/config_store.hpp"
#include "domain/invariant_checker.hpp"

namespace order_books::runtime {
namespace {

Error unavailable_error(const char* message) {
  return Error{ErrorCode::engine_unavailable, message};
}

Error invalid_config_error(const char* message) {
  return Error{ErrorCode::invalid_command, message};
}

Timestamp now_ns() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

domain::ShardState genesis_state(const ShardId shard_id, const EngineConfig& config) {
  domain::ShardState state;
  state.shard_id = shard_id;
  state.current_instrument_configuration_version =
      config.instrument_configuration_version;
  for (const auto& behavior : config.shard_behaviors) {
    state.behavior_configurations.emplace(behavior.version, behavior);
  }
  if (state.behavior_configurations.empty()) {
    ShardBehaviorConfig behavior;
    state.current_behavior_configuration_version = behavior.version;
    state.behavior_configurations.emplace(behavior.version, behavior);
  } else {
    state.current_behavior_configuration_version =
        std::max_element(config.shard_behaviors.begin(), config.shard_behaviors.end(),
                         [](const auto& lhs, const auto& rhs) {
                           return lhs.version < rhs.version;
                         })
            ->version;
  }
  for (const auto& instrument : config.instruments) {
    if (instrument.assigned_shard == shard_id) {
      state.instruments.emplace(instrument.instrument_id, instrument);
    }
  }
  state.instrument_configurations.emplace(state.current_instrument_configuration_version,
                                          state.instruments);
  return state;
}

Status merge_persisted_configurations(
    domain::ShardState& state,
    const std::unordered_map<ConfigurationVersion,
                             std::unordered_map<InstrumentId, InstrumentConfig>>& manifests,
    const std::unordered_map<ConfigurationVersion, ShardBehaviorConfig>& behaviors) {
  for (const auto& [version, manifest] : manifests) {
    for (const auto& [instrument_id, instrument] : manifest) {
      if (instrument_id == 0 || instrument.instrument_id != instrument_id ||
          instrument.assigned_shard != state.shard_id) {
        return Error{ErrorCode::corrupt_snapshot,
                     "instrument configuration shard assignment mismatch"};
      }
    }
    const auto iterator = state.instrument_configurations.find(version);
    if (iterator != state.instrument_configurations.end() && iterator->second != manifest) {
      return Error{ErrorCode::corrupt_snapshot, "instrument configuration version conflict"};
    }
    state.instrument_configurations.emplace(version, manifest);
  }
  for (const auto& [version, behavior] : behaviors) {
    const auto iterator = state.behavior_configurations.find(version);
    if (iterator != state.behavior_configurations.end() && iterator->second != behavior) {
      return Error{ErrorCode::corrupt_snapshot, "behavior configuration version conflict"};
    }
    state.behavior_configurations.emplace(version, behavior);
  }
  return std::monostate{};
}

Status require_persisted_configurations(
    const domain::ShardState& state,
    const std::unordered_map<ConfigurationVersion,
                             std::unordered_map<InstrumentId, InstrumentConfig>>& manifests,
    const std::unordered_map<ConfigurationVersion, ShardBehaviorConfig>& behaviors) {
  for (const auto& [version, manifest] : state.instrument_configurations) {
    const auto persisted = manifests.find(version);
    if (persisted == manifests.end() || persisted->second != manifest) {
      return Error{ErrorCode::corrupt_snapshot,
                   "snapshot references missing instrument configuration"};
    }
  }
  for (const auto& [version, behavior] : state.behavior_configurations) {
    const auto persisted = behaviors.find(version);
    if (persisted == behaviors.end() || persisted->second != behavior) {
      return Error{ErrorCode::corrupt_snapshot,
                   "snapshot references missing behavior configuration"};
    }
  }
  return std::monostate{};
}

Status reconcile_runtime_configuration(domain::ShardState& state, const ShardId shard_id,
                                        const EngineConfig& config) {
  std::unordered_map<InstrumentId, InstrumentConfig> desired_instruments;
  for (const auto& instrument : config.instruments) {
    if (instrument.assigned_shard == shard_id) {
      desired_instruments.emplace(instrument.instrument_id, instrument);
    }
  }
  const auto current_manifest = state.instrument_configurations.find(
      config.instrument_configuration_version);
  if (current_manifest != state.instrument_configurations.end() &&
      current_manifest->second != desired_instruments) {
    return Error{ErrorCode::corrupt_snapshot, "instrument configuration is incompatible"};
  }
  const auto has_active_orders = [&state](const InstrumentId instrument_id) {
    return std::any_of(state.order_locations.begin(), state.order_locations.end(),
                       [instrument_id](const auto& location) {
                         return location.second == instrument_id;
                       });
  };
  for (const auto& [instrument_id, previous] : state.instruments) {
    const auto desired = desired_instruments.find(instrument_id);
    if (desired == desired_instruments.end()) {
      return Error{ErrorCode::corrupt_snapshot,
                   "instrument-to-shard mapping changes require migration"};
    }
    if (desired->second != previous && has_active_orders(instrument_id)) {
      return Error{ErrorCode::corrupt_snapshot,
                   "instrument configuration changes active orders"};
    }
  }
  for (const auto& [unused_version, manifest] : state.instrument_configurations) {
    (void)unused_version;
    for (const auto& [instrument_id, previous] : manifest) {
      const auto desired = desired_instruments.find(instrument_id);
      if (previous.assigned_shard != shard_id || desired == desired_instruments.end() ||
          desired->second.assigned_shard != shard_id) {
        return Error{ErrorCode::corrupt_snapshot,
                     "instrument-to-shard mapping changes require migration"};
      }
    }
  }
  state.instrument_configurations[config.instrument_configuration_version] =
      desired_instruments;
  state.instruments = desired_instruments;
  state.current_instrument_configuration_version = config.instrument_configuration_version;

  if (config.shard_behaviors.empty()) {
    ShardBehaviorConfig default_behavior;
    state.behavior_configurations.emplace(default_behavior.version, default_behavior);
  } else {
    for (const auto& behavior : config.shard_behaviors) {
      const auto iterator = state.behavior_configurations.find(behavior.version);
      if (iterator != state.behavior_configurations.end() && iterator->second != behavior) {
        return Error{ErrorCode::corrupt_snapshot, "behavior configuration is incompatible"};
      }
      state.behavior_configurations[behavior.version] = behavior;
    }
  }
  ConfigurationVersion selected_behavior_version = 1;
  if (!config.shard_behaviors.empty()) {
    selected_behavior_version = std::max_element(
        config.shard_behaviors.begin(), config.shard_behaviors.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.version < rhs.version; })
        ->version;
  }
  if (!state.behavior_configurations.contains(selected_behavior_version)) {
    return Error{ErrorCode::corrupt_snapshot,
                 "selected behavior configuration is unavailable"};
  }
  state.current_behavior_configuration_version = selected_behavior_version;
  return std::monostate{};
}

CommandResult make_admission_result(const Command& command, const ErrorCode code) {
  CommandResult result;
  result.identity = command.identity;
  result.command_status = CommandStatus::admission_error;
  result.error_code = code;
  result.order_id = command.order_id;
  return result;
}

class ScopeGuard final {
 public:
  explicit ScopeGuard(std::function<void()> callback) : callback_(std::move(callback)) {}
  ~ScopeGuard() {
    if (callback_) {
      callback_();
    }
  }

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;

 private:
  std::function<void()> callback_;
};

}  // namespace

Result<std::unique_ptr<ShardRuntime>> ShardRuntime::open(
    const ShardId shard_id, const EngineConfig& config, EventSink& event_sink,
    MetricsSink& metrics_sink) {
  if (config.data_directory.empty()) {
    return invalid_config_error("data directory is empty");
  }
  if (config.runtime.ingress_queue_capacity == 0 ||
      config.runtime.group_commit_max_commands == 0 ||
      config.runtime.wal_segment_size == 0) {
    return invalid_config_error("runtime capacity is zero");
  }

  auto state = genesis_state(shard_id, config);
  if (state.behavior_configurations.find(state.current_behavior_configuration_version) ==
      state.behavior_configurations.end()) {
    return invalid_config_error("behavior configuration is missing");
  }

  const auto shard_directory = config.data_directory / ("shard-" + std::to_string(shard_id));
  auto config_store_result =
      storage::ConfigStore::open(shard_directory / "config", shard_id);
  if (std::holds_alternative<Error>(config_store_result)) {
    return std::get<Error>(config_store_result);
  }
  auto config_store = std::get<storage::ConfigStore>(std::move(config_store_result));
  auto persisted_instruments = config_store.load_instrument_manifests();
  if (std::holds_alternative<Error>(persisted_instruments)) {
    return std::get<Error>(persisted_instruments);
  }
  auto persisted_behaviors = config_store.load_behavior_configs();
  if (std::holds_alternative<Error>(persisted_behaviors)) {
    return std::get<Error>(persisted_behaviors);
  }
  if (const auto status = merge_persisted_configurations(
          state,
          std::get<std::unordered_map<ConfigurationVersion,
                                     std::unordered_map<InstrumentId, InstrumentConfig>>>(
              persisted_instruments),
          std::get<std::unordered_map<ConfigurationVersion, ShardBehaviorConfig>>(
              persisted_behaviors));
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  auto wal_result = storage::Wal::open(shard_directory / "wal", shard_id,
                                       config.runtime.wal_segment_size);
  if (std::holds_alternative<Error>(wal_result)) {
    return std::get<Error>(wal_result);
  }
  auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(wal_result));

  auto snapshots_result = storage::SnapshotStore::open(shard_directory / "snapshots", shard_id);
  if (std::holds_alternative<Error>(snapshots_result)) {
    return std::get<Error>(snapshots_result);
  }
  auto snapshots = std::get<storage::SnapshotStore>(std::move(snapshots_result));
  auto loaded_snapshot = snapshots.load_latest();
  if (std::holds_alternative<Error>(loaded_snapshot)) {
    return std::get<Error>(loaded_snapshot);
  }
  auto snapshot_state = std::get<std::optional<domain::ShardState>>(
      std::move(loaded_snapshot));
  if (snapshot_state.has_value()) {
    if (const auto status = require_persisted_configurations(
            *snapshot_state,
            std::get<std::unordered_map<ConfigurationVersion,
                                       std::unordered_map<InstrumentId, InstrumentConfig>>>(
                persisted_instruments),
            std::get<std::unordered_map<ConfigurationVersion, ShardBehaviorConfig>>(
                persisted_behaviors));
        std::holds_alternative<Error>(status)) {
      return std::get<Error>(status);
    }
    state = std::move(*snapshot_state);
  }
  if (const auto status = merge_persisted_configurations(
          state,
          std::get<std::unordered_map<ConfigurationVersion,
                                     std::unordered_map<InstrumentId, InstrumentConfig>>>(
              persisted_instruments),
          std::get<std::unordered_map<ConfigurationVersion, ShardBehaviorConfig>>(
              persisted_behaviors));
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  if (const auto status = reconcile_runtime_configuration(state, shard_id, config);
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  for (const auto& [version, manifest] : state.instrument_configurations) {
    if (const auto status = config_store.persist_instrument_manifest(version, manifest);
        std::holds_alternative<Error>(status)) {
      return std::get<Error>(status);
    }
  }
  for (const auto& [version, behavior] : state.behavior_configurations) {
    if (const auto status = config_store.persist_behavior_config(version, behavior);
        std::holds_alternative<Error>(status)) {
      return std::get<Error>(status);
    }
  }

  domain::StateMachine state_machine(std::move(state));
  auto records = wal->replay();
  if (std::holds_alternative<Error>(records)) {
    return std::get<Error>(records);
  }
  const auto& persisted_instrument_manifests =
      std::get<std::unordered_map<ConfigurationVersion,
                                  std::unordered_map<InstrumentId, InstrumentConfig>>>(
          persisted_instruments);
  const auto& persisted_behavior_configs =
      std::get<std::unordered_map<ConfigurationVersion, ShardBehaviorConfig>>(
          persisted_behaviors);
  for (const auto& record : std::get<std::vector<domain::CommittedCommand>>(records)) {
    if (!persisted_instrument_manifests.contains(record.instrument_configuration_version) ||
        !persisted_behavior_configs.contains(record.behavior_configuration_version)) {
      return Error{ErrorCode::corrupt_snapshot,
                   "WAL references missing configuration manifest"};
    }
    if (record.engine_seq <= state_machine.state().last_committed_engine_seq) {
      continue;
    }
    auto execution = state_machine.apply(record);
    if (std::holds_alternative<Error>(execution)) {
      return std::get<Error>(execution);
    }
  }
  if (const auto status = reconcile_runtime_configuration(state_machine.state(), shard_id,
                                                          config);
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  if (const auto status = domain::validate_state(state_machine.state());
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }

  auto replay_snapshots_result = storage::SnapshotStore::open(
      shard_directory / "event-replay", shard_id);
  if (std::holds_alternative<Error>(replay_snapshots_result)) {
    return std::get<Error>(replay_snapshots_result);
  }
  auto replay_snapshots = std::get<storage::SnapshotStore>(
      std::move(replay_snapshots_result));
  auto metrics_registry = std::make_unique<MetricsRegistry>(metrics_sink);
  metrics_registry->observe("active_instruments", state_machine.state().instruments.size());
  metrics_registry->observe("active_orders", state_machine.state().active_order_count);
  metrics_registry->observe("wal_size_bytes", wal->size_bytes());
  auto publisher_state_result =
      storage::decode_state(storage::encode_state(state_machine.state()));
  if (std::holds_alternative<Error>(publisher_state_result)) {
    return std::get<Error>(publisher_state_result);
  }
  auto publisher_result = EventPublisher::open(
      std::get<domain::ShardState>(std::move(publisher_state_result)), *wal,
      std::move(replay_snapshots), event_sink, *metrics_registry,
      config.runtime.event_replay_snapshot_interval_commands,
      config.runtime.event_replay_snapshot_interval);
  if (std::holds_alternative<Error>(publisher_result)) {
    return std::get<Error>(publisher_result);
  }
  auto publisher = std::get<std::unique_ptr<EventPublisher>>(std::move(publisher_result));
  // Recovery has applied and validated the complete WAL before the publisher is
  // started; make that durable head publishable so restart replays unconfirmed
  // events from the persisted cursor.
  publisher->notify_publishable(wal->durable_position());

  return std::unique_ptr<ShardRuntime>(new ShardRuntime(
      shard_id, config, std::move(metrics_registry), std::move(wal), std::move(snapshots),
      std::move(state_machine), std::move(publisher)));
}

ShardRuntime::ShardRuntime(ShardId shard_id, EngineConfig config,
                           std::unique_ptr<MetricsRegistry> metrics_registry,
                           std::unique_ptr<storage::Wal> wal,
                           storage::SnapshotStore snapshots,
                           domain::StateMachine state_machine,
                           std::unique_ptr<EventPublisher> publisher)
    : shard_id_(shard_id),
      config_(std::move(config)),
      metrics_registry_(std::move(metrics_registry)),
      wal_(std::move(wal)),
      snapshots_(std::move(snapshots)),
      state_machine_(std::move(state_machine)),
      publisher_(std::move(publisher)),
      last_snapshot_time_(std::chrono::steady_clock::now()) {}

ShardRuntime::~ShardRuntime() { (void)stop(); }

void ShardRuntime::start() {
  std::lock_guard lock(queue_mutex_);
  if (started_ || failed_) {
    return;
  }
  started_ = true;
  publisher_->start();
  completion_worker_ = std::jthread(
      [this](std::stop_token token) { completion_run(token); });
  worker_ = std::jthread([this](std::stop_token token) { run(token); });
}

SubmitResult ShardRuntime::submit(Command command, CompletionHandler completion) {
  if (command.identity.producer_stream_id != shard_id_) {
    return SubmitResult{false,
                        Error{ErrorCode::wrong_producer_stream,
                              "producer stream does not select this shard"}};
  }
  if (config_.runtime.max_envelope_bytes != 0 &&
      storage::encode_command(command).size() > config_.runtime.max_envelope_bytes) {
    return SubmitResult{false,
                        Error{ErrorCode::invalid_envelope, "command envelope is too large"}};
  }
  if (!enqueue(CommandWork{std::move(command), std::move(completion),
                           std::chrono::steady_clock::now()})) {
    return SubmitResult{false, unavailable_error("shard is not accepting commands")};
  }
  return SubmitResult{true, std::nullopt};
}

std::future<Result<OrderView>> ShardRuntime::get_order(const InstrumentId instrument_id,
                                                       const OrderId order_id) {
  auto promise = std::make_shared<std::promise<Result<OrderView>>>();
  auto future = promise->get_future();
  QueryWork work;
  work.execute = [promise, instrument_id, order_id](const domain::StateMachine& machine) {
    if (machine.state().instruments.find(instrument_id) == machine.state().instruments.end()) {
      promise->set_value(Error{ErrorCode::unknown_instrument, "unknown instrument"});
      return;
    }
    const auto instrument = machine.state().books.find(instrument_id);
    if (instrument == machine.state().books.end()) {
      promise->set_value(Error{ErrorCode::order_not_found, "order not found"});
      return;
    }
    const auto order = instrument->second.find(order_id);
    if (!order.has_value()) {
      promise->set_value(Error{ErrorCode::order_not_found, "order not found"});
      return;
    }
    promise->set_value(*order);
  };
  work.reject = [promise](Error error) { promise->set_value(std::move(error)); };
  if (!enqueue(std::move(work))) {
    promise->set_value(unavailable_error("shard is not accepting queries"));
  }
  return future;
}

std::future<Result<BookDepth>> ShardRuntime::depth(const InstrumentId instrument_id,
                                                   const Side side,
                                                   const std::size_t limit) {
  auto promise = std::make_shared<std::promise<Result<BookDepth>>>();
  auto future = promise->get_future();
  const auto max_top_n = config_.runtime.max_top_n;
  QueryWork work;
  work.execute = [promise, instrument_id, side, limit,
                  max_top_n](const domain::StateMachine& machine) {
    if (side != Side::buy && side != Side::sell) {
      promise->set_value(Error{ErrorCode::invalid_side, "invalid book side"});
      return;
    }
    if (limit > max_top_n) {
      promise->set_value(Error{ErrorCode::invalid_command, "depth limit is too large"});
      return;
    }
    if (machine.state().instruments.find(instrument_id) == machine.state().instruments.end()) {
      promise->set_value(Error{ErrorCode::unknown_instrument, "unknown instrument"});
      return;
    }
    const auto instrument = machine.state().books.find(instrument_id);
    if (instrument == machine.state().books.end()) {
      promise->set_value(BookDepth{});
      return;
    }
    promise->set_value(instrument->second.depth(side, limit));
  };
  work.reject = [promise](Error error) { promise->set_value(std::move(error)); };
  if (!enqueue(std::move(work))) {
    promise->set_value(unavailable_error("shard is not accepting queries"));
  }
  return future;
}

std::future<Result<std::optional<BookLevel>>> ShardRuntime::best(
    const InstrumentId instrument_id, const Side side) {
  auto promise = std::make_shared<std::promise<Result<std::optional<BookLevel>>>>();
  auto future = promise->get_future();
  QueryWork work;
  work.execute = [promise, instrument_id, side](const domain::StateMachine& machine) {
    if (side != Side::buy && side != Side::sell) {
      promise->set_value(Error{ErrorCode::invalid_side, "invalid book side"});
      return;
    }
    if (machine.state().instruments.find(instrument_id) == machine.state().instruments.end()) {
      promise->set_value(Error{ErrorCode::unknown_instrument, "unknown instrument"});
      return;
    }
    const auto instrument = machine.state().books.find(instrument_id);
    if (instrument == machine.state().books.end()) {
      promise->set_value(std::optional<BookLevel>{});
      return;
    }
    promise->set_value(instrument->second.best(side));
  };
  work.reject = [promise](Error error) { promise->set_value(std::move(error)); };
  if (!enqueue(std::move(work))) {
    promise->set_value(unavailable_error("shard is not accepting queries"));
  }
  return future;
}

MetricsSnapshot ShardRuntime::metrics() const { return metrics_registry_->snapshot(); }

Status ShardRuntime::stop() {
  {
    std::lock_guard lock(queue_mutex_);
    if (!started_) {
      if (publisher_ != nullptr) {
        return publisher_->stop();
      }
      return std::monostate{};
    }
    stopping_ = true;
    worker_.request_stop();
  }
  queue_condition_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  if (publisher_ != nullptr) {
    const auto publisher_status = publisher_->stop();
    if (std::holds_alternative<Error>(publisher_status)) {
      fail(std::get<Error>(publisher_status));
    }
  }
  {
    std::lock_guard lock(completion_mutex_);
    completion_worker_.request_stop();
  }
  completion_condition_.notify_all();
  if (completion_worker_.joinable()) {
    completion_worker_.join();
  }
  {
    std::lock_guard lock(queue_mutex_);
    started_ = false;
  }
  return failure_.has_value() ? Status{*failure_} : Status{std::monostate{}};
}

void ShardRuntime::run(const std::stop_token stop_token) {
  for (;;) {
    Work first;
    {
      std::unique_lock lock(queue_mutex_);
      queue_condition_.wait(lock, stop_token, [this] {
        return stopping_ || failed_ || !queue_.empty();
      });
      if (failed_ || (stop_token.stop_requested() && queue_.empty())) {
        break;
      }
      if (queue_.empty()) {
        continue;
      }
      first = std::move(queue_.front());
      queue_.pop_front();
      metrics_registry_->observe("queue_depth", queue_.size());
    }

    if (std::holds_alternative<QueryWork>(first)) {
      process_query(std::get<QueryWork>(first));
      continue;
    }

    std::vector<CommandWork> batch;
    batch.push_back(std::move(std::get<CommandWork>(first)));
    const auto deadline = std::chrono::steady_clock::now() +
                          config_.runtime.group_commit_max_delay;
    while (batch.size() < config_.runtime.group_commit_max_commands) {
      std::unique_lock lock(queue_mutex_);
      if (!queue_.empty() && std::holds_alternative<CommandWork>(queue_.front())) {
        batch.push_back(std::move(std::get<CommandWork>(queue_.front())));
        queue_.pop_front();
        metrics_registry_->observe("queue_depth", queue_.size());
        continue;
      }
      if (stop_token.stop_requested() || stopping_ || config_.runtime.group_commit_max_delay.count() == 0) {
        break;
      }
      if (queue_condition_.wait_until(lock, stop_token, deadline, [this] {
            return stopping_ || failed_ || !queue_.empty();
          }) &&
          !queue_.empty() && std::holds_alternative<CommandWork>(queue_.front())) {
        batch.push_back(std::move(std::get<CommandWork>(queue_.front())));
        queue_.pop_front();
        metrics_registry_->observe("queue_depth", queue_.size());
        continue;
      }
      break;
    }
    try {
      process_command_batch(std::move(batch));
    } catch (...) {
      fail(Error{ErrorCode::engine_unavailable, "unexpected shard exception"});
    }
  }

  std::deque<Work> rejected;
  {
    std::lock_guard lock(queue_mutex_);
    rejected.swap(queue_);
  }
  for (auto& work : rejected) {
    if (std::holds_alternative<CommandWork>(work)) {
      auto command_work = std::get<CommandWork>(std::move(work));
      auto result = admission_error(command_work.command, ErrorCode::engine_unavailable);
      dispatch(std::move(result), command_work.completion);
    } else {
      auto query = std::get<QueryWork>(std::move(work));
      query.reject(unavailable_error("shard stopped"));
    }
  }
}

void ShardRuntime::completion_run(const std::stop_token stop_token) {
  for (;;) {
    std::pair<CommandResult, CompletionHandler> completion;
    {
      std::unique_lock lock(completion_mutex_);
      completion_condition_.wait(lock, stop_token, [this] {
        return !completions_.empty();
      });
      if (completions_.empty()) {
        if (stop_token.stop_requested()) {
          break;
        }
        continue;
      }
      completion = std::move(completions_.front());
      completions_.pop_front();
    }
    completion_condition_.notify_all();
    if (!completion.second) {
      continue;
    }
    try {
      completion.second(std::move(completion.first));
    } catch (...) {
      metrics_registry_->observe("completion_callback_error", 1);
    }
  }
}

void ShardRuntime::process_command_batch(std::vector<CommandWork> batch) {
  if (publisher_->failed()) {
    for (auto& work : batch) {
      auto result = admission_error(work.command, ErrorCode::engine_unavailable);
      dispatch(std::move(result), work.completion);
    }
    fail(Error{ErrorCode::engine_unavailable, "event publisher failed"});
    return;
  }
  std::vector<CompletionHandler> batch_completions;
  std::vector<domain::CommittedCommand> accepted;
  std::vector<std::chrono::steady_clock::time_point> enqueued_times;
  std::vector<std::size_t> accepted_slots;
  std::vector<std::optional<CommandResult>> results(batch.size());
  std::unordered_set<ProducerKey, ProducerKeyHash> batch_producers;
  bool batch_finished = false;
  const auto reject_all_pending = [&](const ErrorCode code) {
    for (std::size_t index = 0; index < batch_completions.size(); ++index) {
      if (batch_completions[index]) {
        auto result = results[index].has_value()
                          ? std::move(*results[index])
                          : admission_error(batch[index].command, code);
        dispatch(std::move(result), batch_completions[index]);
      }
    }
  };
  const auto dispatch_results = [&] {
    for (std::size_t index = 0; index < batch_completions.size(); ++index) {
      if (batch_completions[index] && results[index].has_value()) {
        dispatch(std::move(*results[index]), batch_completions[index]);
      }
    }
  };
  ScopeGuard pending_guard([&] {
    if (!batch_finished) {
      reject_all_pending(ErrorCode::engine_unavailable);
      fail(Error{ErrorCode::engine_unavailable, "unexpected shard exception"});
    }
  });
  batch_completions.reserve(batch.size());
  for (auto& work : batch) {
    batch_completions.push_back(std::move(work.completion));
  }
  accepted.reserve(batch.size());
  enqueued_times.reserve(batch.size());
  accepted_slots.reserve(batch.size());
  const auto reject_batch_pending = [&](const ErrorCode code) {
    batch_finished = true;
    reject_all_pending(code);
  };
  struct PublisherPressureSample {
    std::uint64_t lag_bytes{};
    std::uint64_t lag_age_ns{};
    bool warning{};
    bool critical{};
    bool pressure{};
  };
  std::optional<PublisherPressureSample> publisher_pressure;
  auto next_sequence = state_machine_.state().last_committed_engine_seq;
  for (std::size_t batch_index = 0; batch_index < batch.size(); ++batch_index) {
    auto& work = batch[batch_index];
    const auto& command = work.command;
    const ProducerKey key{command.identity.producer_id,
                          command.identity.producer_stream_id};
    if (batch_producers.find(key) != batch_producers.end()) {
      results[batch_index] = admission_error(command, ErrorCode::producer_sequence_gap);
      metrics_registry_->observe("sequence_gaps", 1);
      continue;
    }
    const auto producer = state_machine_.state().producer_states.find(key);
    if (producer != state_machine_.state().producer_states.end()) {
      const auto& previous = producer->second;
      if (command.identity.producer_epoch < previous.current_epoch) {
        results[batch_index] = admission_error(command, ErrorCode::stale_producer_epoch);
        metrics_registry_->observe("stale_epochs", 1);
        continue;
      }
      if (command.identity.producer_epoch == previous.current_epoch &&
          command.identity.producer_seq == previous.last_processed_seq) {
        if (is_duplicate(command, previous)) {
          results[batch_index] = previous.last_result;
          metrics_registry_->observe("duplicate_commands", 1);
        } else {
          results[batch_index] = admission_error(command, ErrorCode::command_identity_conflict);
          metrics_registry_->observe("identity_conflicts", 1);
        }
        continue;
      }
      if (command.identity.producer_epoch == previous.current_epoch &&
          command.identity.producer_seq < previous.last_processed_seq) {
        results[batch_index] = admission_error(command, ErrorCode::duplicate_too_old);
        continue;
      }
      if (command.identity.producer_epoch > previous.current_epoch &&
          command.identity.producer_seq != 1) {
        results[batch_index] = admission_error(command, ErrorCode::producer_sequence_gap);
        metrics_registry_->observe("sequence_gaps", 1);
        continue;
      }
      if (command.identity.producer_epoch == previous.current_epoch &&
          (previous.last_processed_seq == std::numeric_limits<ProducerSeq>::max() ||
           command.identity.producer_seq != previous.last_processed_seq + 1U)) {
        results[batch_index] = admission_error(command, ErrorCode::producer_sequence_gap);
        metrics_registry_->observe("sequence_gaps", 1);
        continue;
      }
    } else if (command.identity.producer_seq != 1) {
      results[batch_index] = admission_error(command, ErrorCode::producer_sequence_gap);
      metrics_registry_->observe("sequence_gaps", 1);
      continue;
    }

    if (config_.runtime.wal_soft_limit_bytes != 0 &&
        wal_->size_bytes() >= config_.runtime.wal_soft_limit_bytes) {
      results[batch_index] = admission_error(command, ErrorCode::engine_storage_pressure);
      continue;
    }
    if (!publisher_pressure.has_value()) {
      const auto publish_lag_age = publisher_->oldest_unconfirmed_received_at();
      const auto current_time = now_ns();
      const auto max_lag_age = std::chrono::duration_cast<std::chrono::nanoseconds>(
          config_.runtime.max_publish_lag_age);
      const auto lag_bytes = publisher_->lag_bytes();
      if (publisher_->failed()) {
        reject_batch_pending(ErrorCode::engine_unavailable);
        fail(Error{ErrorCode::engine_unavailable, "event publisher failed"});
        return;
      }
      const auto lag_age = publish_lag_age.has_value() && *publish_lag_age >= 0 &&
                                   current_time >= *publish_lag_age
                               ? static_cast<std::uint64_t>(current_time - *publish_lag_age)
                               : 0U;
      const auto max_lag_age_count = max_lag_age.count();
      const bool bytes_warning = config_.runtime.max_publish_lag_bytes != 0 &&
                                 lag_bytes >= config_.runtime.max_publish_lag_bytes / 2U;
      const bool bytes_critical = config_.runtime.max_publish_lag_bytes != 0 &&
                                  lag_bytes >= config_.runtime.max_publish_lag_bytes -
                                                   config_.runtime.max_publish_lag_bytes / 5U;
      const bool age_warning = max_lag_age_count > 0 &&
                               lag_age >= static_cast<std::uint64_t>(max_lag_age_count / 2);
      const bool age_critical = max_lag_age_count > 0 &&
                                lag_age >= static_cast<std::uint64_t>(
                                    max_lag_age_count - max_lag_age_count / 5);
      const bool age_pressure = publish_lag_age.has_value() && max_lag_age_count >= 0 &&
                                lag_age >= static_cast<std::uint64_t>(max_lag_age_count);
      const bool pressure =
          (config_.runtime.max_publish_lag_bytes != 0 &&
           lag_bytes >= config_.runtime.max_publish_lag_bytes) ||
          age_pressure;
      metrics_registry_->observe("event_publish_lag_bytes", lag_bytes);
      metrics_registry_->observe("event_publish_lag_age_ns", lag_age);
      if (bytes_warning || age_warning) {
        metrics_registry_->observe("publisher_lag_warning", 1);
      }
      if (bytes_critical || age_critical) {
        metrics_registry_->observe("publisher_lag_critical", 1);
      }
      publisher_pressure = PublisherPressureSample{
          lag_bytes, lag_age, bytes_warning || age_warning, bytes_critical || age_critical,
          pressure};
    }
    if (publisher_pressure->pressure) {
      results[batch_index] = admission_error(command, ErrorCode::engine_storage_pressure);
      continue;
    }
    if (next_sequence == std::numeric_limits<EngineSeq>::max()) {
      reject_batch_pending(ErrorCode::engine_unavailable);
      fail(Error{ErrorCode::engine_unavailable, "engine sequence exhausted"});
      return;
    }
    const auto candidate_sequence = next_sequence + 1U;
    auto prepared = prepare(command, candidate_sequence);
    if (std::holds_alternative<Error>(prepared)) {
      const auto error = std::get<Error>(prepared);
      if (error.code == ErrorCode::corrupt_snapshot ||
          error.code == ErrorCode::corrupt_wal) {
        reject_batch_pending(ErrorCode::engine_unavailable);
        fail(error);
        return;
      }
      results[batch_index] = admission_error(command, error.code);
      continue;
    }
    accepted.push_back(std::get<domain::CommittedCommand>(std::move(prepared)));
    accepted_slots.push_back(batch_index);
    enqueued_times.push_back(work.enqueued_at);
    next_sequence = candidate_sequence;
    batch_producers.emplace(key);
  }

  if (accepted.empty()) {
    dispatch_results();
    batch_finished = true;
    return;
  }
  const auto dequeued_at = std::chrono::steady_clock::now();
  for (const auto enqueued_at : enqueued_times) {
    const auto queue_latency = std::chrono::duration_cast<std::chrono::microseconds>(
        dequeued_at - enqueued_at);
    metrics_registry_->observe("queue_latency_us",
                               static_cast<std::uint64_t>(queue_latency.count()));
  }
  const auto wal_start = std::chrono::steady_clock::now();
  for (const auto& command : accepted) {
    auto appended = wal_->append(command);
    if (std::holds_alternative<Error>(appended)) {
      reject_batch_pending(ErrorCode::engine_unavailable);
      fail(std::get<Error>(appended));
      return;
    }
  }
  if (const auto status = wal_->sync(); std::holds_alternative<Error>(status)) {
    reject_batch_pending(ErrorCode::engine_unavailable);
    fail(std::get<Error>(status));
    return;
  }
  const auto wal_latency = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - wal_start);
  for (std::size_t index = 0; index < accepted.size(); ++index) {
    metrics_registry_->observe("wal_commit_latency_us",
                               static_cast<std::uint64_t>(wal_latency.count()));
  }

  const auto execution_start = std::chrono::steady_clock::now();
  std::vector<domain::ExecutionOutput> outputs;
  outputs.reserve(accepted.size());
  for (std::size_t index = 0; index < accepted.size(); ++index) {
    auto execution = state_machine_.apply(accepted[index]);
    if (std::holds_alternative<Error>(execution)) {
      reject_batch_pending(ErrorCode::engine_unavailable);
      fail(std::get<Error>(execution));
      return;
    }
    outputs.push_back(std::get<domain::ExecutionOutput>(std::move(execution)));
  }

  // Validate the complete transition before exposing any committed result to
  // callers.  A core/invariant failure is fail-stop and must not be reported
  // as a successful completion for only part of the batch.
  if (const auto status = domain::validate_state(state_machine_.state());
      std::holds_alternative<Error>(status)) {
    reject_batch_pending(ErrorCode::engine_unavailable);
    fail(std::get<Error>(status));
    return;
  }
  if (publisher_->failed()) {
    reject_batch_pending(ErrorCode::engine_unavailable);
    fail(Error{ErrorCode::engine_unavailable, "event publisher failed"});
    return;
  }
  const auto execution_latency = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - execution_start);
  publisher_->notify_publishable(wal_->durable_position());

  for (std::size_t index = 0; index < outputs.size(); ++index) {
    auto& output = outputs[index];
    results[accepted_slots[index]] = std::move(output.result);
    metrics_registry_->observe(
        "execution_latency_us", static_cast<std::uint64_t>(execution_latency.count()));
    const auto end_to_end = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - enqueued_times[index]);
    metrics_registry_->observe("end_to_end_latency_us",
                               static_cast<std::uint64_t>(end_to_end.count()));
    metrics_registry_->observe("commands", 1);
    const auto trade_count = static_cast<std::uint64_t>(std::count_if(
        output.events.begin(), output.events.end(), [](const auto& event) {
          return event.event_type == EventType::trade;
        }));
    metrics_registry_->observe("trades", trade_count);
    metrics_registry_->observe("wal_size_bytes", wal_->size_bytes());
    metrics_registry_->observe("active_orders", state_machine_.state().active_order_count);
    metrics_registry_->observe("active_instruments", state_machine_.state().instruments.size());
    std::size_t active_levels = 0;
    for (const auto& [unused_instrument, book] : state_machine_.state().books) {
      (void)unused_instrument;
      active_levels += book.active_price_level_count();
    }
    metrics_registry_->observe("active_price_levels", active_levels);
    ++commands_since_snapshot_;
  }

  dispatch_results();
  batch_finished = true;

  // The publisher is an independent replay replica.  A replay, sink cursor,
  // or replay-snapshot failure is fatal to the shard: continuing to accept
  // durable commands would make event delivery and retention unverifiable.
  if (publisher_->failed()) {
    fail(Error{ErrorCode::engine_unavailable, "event publisher failed"});
    return;
  }

  const auto elapsed = std::chrono::steady_clock::now() - last_snapshot_time_;
  if (commands_since_snapshot_ >= config_.runtime.snapshot_interval_commands ||
      elapsed >= config_.runtime.snapshot_interval) {
    if (const auto status = snapshots_.write(state_machine_.state());
        std::holds_alternative<Error>(status)) {
      fail(std::get<Error>(status));
      return;
    }
    commands_since_snapshot_ = 0;
    last_snapshot_time_ = std::chrono::steady_clock::now();
    const auto replay_snapshot_seq = publisher_->replay_snapshot_seq();
    const auto retention_watermark = std::min(
        state_machine_.state().last_committed_engine_seq, replay_snapshot_seq);
    if (const auto status = wal_->retain_through(retention_watermark);
        std::holds_alternative<Error>(status)) {
      fail(std::get<Error>(status));
      return;
    }
  }
}

void ShardRuntime::process_query(const QueryWork& query) {
  try {
    query.execute(state_machine_);
  } catch (...) {
    query.reject(unavailable_error("query execution failed"));
  }
}

void ShardRuntime::dispatch(CommandResult result, CompletionHandler& completion) {
  if (!completion) {
    return;
  }
  std::unique_lock lock(completion_mutex_);
  completion_condition_.wait(lock, [this] {
    return completions_.size() < config_.runtime.ingress_queue_capacity;
  });
  completions_.emplace_back(std::move(result), std::move(completion));
  lock.unlock();
  completion_condition_.notify_all();
}

void ShardRuntime::fail(Error error) {
  std::deque<Work> rejected;
  {
    std::lock_guard lock(queue_mutex_);
    if (failed_) {
      return;
    }
    failed_ = true;
    failure_ = error;
    rejected.swap(queue_);
  }
  for (auto& work : rejected) {
    if (std::holds_alternative<CommandWork>(work)) {
      auto command_work = std::get<CommandWork>(std::move(work));
      auto result = admission_error(command_work.command, ErrorCode::engine_unavailable);
      dispatch(std::move(result), command_work.completion);
    } else {
      std::get<QueryWork>(std::move(work)).reject(error);
    }
  }
  queue_condition_.notify_all();
}

Result<domain::CommittedCommand> ShardRuntime::prepare(const Command& command,
                                                        const EngineSeq engine_seq) {
  if (command.identity.producer_epoch == 0 || command.identity.producer_seq == 0) {
    return Error{ErrorCode::invalid_command, "producer epoch and sequence are required"};
  }
  if (command.identity.producer_stream_id != shard_id_) {
    return Error{ErrorCode::wrong_producer_stream, "producer stream does not select shard"};
  }
  const auto& state = state_machine_.state();
  if (state.current_behavior_configuration_version == 0 ||
      state.behavior_configurations.find(state.current_behavior_configuration_version) ==
          state.behavior_configurations.end()) {
    return Error{ErrorCode::corrupt_snapshot, "active behavior configuration is missing"};
  }
  if (state.current_instrument_configuration_version == 0 ||
      state.instrument_configurations.find(state.current_instrument_configuration_version) ==
          state.instrument_configurations.end()) {
    return Error{ErrorCode::corrupt_snapshot, "active instrument configuration is missing"};
  }
  return domain::CommittedCommand{command,
                                  engine_seq,
                                  now_ns(),
                                  state.current_behavior_configuration_version,
                                  state.current_instrument_configuration_version};
}

CommandResult ShardRuntime::admission_error(const Command& command, const ErrorCode code) const {
  return make_admission_result(command, code);
}

bool ShardRuntime::is_duplicate(const Command& command,
                                const domain::ProducerState& producer) const {
  return producer.last_canonical_command == domain::canonical_command_bytes(command);
}

bool ShardRuntime::enqueue(Work work) {
  std::lock_guard lock(queue_mutex_);
  if (!started_ || stopping_ || failed_ ||
      queue_.size() >= config_.runtime.ingress_queue_capacity) {
    return false;
  }
  queue_.push_back(std::move(work));
  metrics_registry_->observe("queue_depth", queue_.size());
  queue_condition_.notify_one();
  return true;
}

}  // namespace order_books::runtime
