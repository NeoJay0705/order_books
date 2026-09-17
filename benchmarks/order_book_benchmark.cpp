#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

#include "domain/order_book.hpp"
#include "domain/invariant_checker.hpp"
#include "domain/state_machine.hpp"
#include "order_books/engine.hpp"
#include "persistence/snapshot_store.hpp"
#include "persistence/wal.hpp"

namespace {

using namespace order_books;

constexpr std::uint64_t kIterations = 2'000;
constexpr std::uint64_t kWarmup = 100;
constexpr std::size_t kDurableProducerLanes = 1'024;
constexpr auto kDurablePhaseTimeout = std::chrono::seconds(60);

enum class WorkloadSelection { all, engine_durable_single_instrument };

struct BenchmarkOptions {
  std::uint64_t iterations{kIterations};
  std::uint64_t warmup{kWarmup};
  WorkloadSelection workload{WorkloadSelection::all};
  std::optional<std::filesystem::path> data_directory;
};

struct WorkloadDelta {
  std::uint64_t commands{1};
  std::uint64_t trades{};
};

using WorkloadStats = std::function<std::pair<std::uint64_t, std::uint64_t>()>;

OrderView make_order(const std::uint64_t id, const Side side, const Price price,
                    const Quantity quantity, const EngineSeq priority) {
  return OrderView{OrderId{0, id}, 1, side, price, quantity, quantity, 0,
                   OrderStatus::active, 1, priority};
}

template <typename Work>
bool run_workload(const std::string_view name, const BenchmarkOptions& options, Work work,
                  WorkloadStats stats = {}, const std::string_view metadata = {}) {
  const auto invoke = [&work, name](const std::uint64_t index,
                                    WorkloadDelta& delta) {
    try {
      delta = work(index);
      return true;
    } catch (const std::exception& error) {
      std::cerr << name << " failed: " << error.what() << '\n';
    } catch (...) {
      std::cerr << name << " failed: unknown error\n";
    }
    return false;
  };
  for (std::uint64_t index = 0; index < options.warmup; ++index) {
    WorkloadDelta delta;
    if (!invoke(index, delta)) {
      return false;
    }
  }
  std::vector<std::uint64_t> samples;
  samples.reserve(options.iterations);
  std::uint64_t command_count = 0;
  std::uint64_t trade_count = 0;
  for (std::uint64_t index = 0; index < options.iterations; ++index) {
    const auto start = std::chrono::steady_clock::now();
    WorkloadDelta delta;
    if (!invoke(index + options.warmup, delta)) {
      return false;
    }
    command_count = command_count > std::numeric_limits<std::uint64_t>::max() - delta.commands
                        ? std::numeric_limits<std::uint64_t>::max()
                        : command_count + delta.commands;
    trade_count = trade_count > std::numeric_limits<std::uint64_t>::max() - delta.trades
                      ? std::numeric_limits<std::uint64_t>::max()
                      : trade_count + delta.trades;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start);
    samples.push_back(static_cast<std::uint64_t>(elapsed.count()));
  }
  if (samples.empty()) {
    return true;
  }
  std::sort(samples.begin(), samples.end());
  const auto percentile = [&samples](const std::size_t numerator,
                                     const std::size_t denominator) {
    const auto index = std::min(
        samples.size() - 1U,
        (samples.size() * numerator + denominator - 1U) / denominator - 1U);
    return samples[index];
  };
  const auto elapsed_total =
      std::accumulate(samples.begin(), samples.end(), std::uint64_t{0});
  const auto commands_per_second = elapsed_total == 0
                                       ? 0.0
                                       : static_cast<double>(command_count) * 1'000'000'000.0 /
                                             static_cast<double>(elapsed_total);
  const auto trades_per_second = elapsed_total == 0
                                     ? 0.0
                                     : static_cast<double>(trade_count) * 1'000'000'000.0 /
                                           static_cast<double>(elapsed_total);
  std::cout << name << " iterations=" << options.iterations << " commands=" << command_count
            << " trades=" << trade_count << " commands_per_second=" << commands_per_second
            << " trades_per_second=" << trades_per_second << " p50_us="
            << percentile(50, 100) / 1'000.0
            << " p99_us=" << percentile(99, 100) / 1'000.0
            << " p99.9_us=" << percentile(999, 1000) / 1'000.0
            << " max_us=" << samples.back() / 1'000.0
            << " elapsed_ms=" << elapsed_total / 1'000'000.0;
  if (stats) {
    const auto [active_orders, active_levels] = stats();
    std::cout << " active_orders=" << active_orders << " active_levels=" << active_levels;
  }
  if (!metadata.empty()) {
    std::cout << ' ' << metadata;
  }
  std::cout << '\n';
  return true;
}

std::optional<std::uint64_t> parse_positive_option(const std::string_view argument,
                                                   const std::string_view prefix) {
  if (!argument.starts_with(prefix)) {
    return std::nullopt;
  }
  const auto value = argument.substr(prefix.size());
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t result = 0;
  for (const auto character : value) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return std::nullopt;
    }
    result = result * 10U + digit;
  }
  return result;
}

std::optional<WorkloadSelection> parse_workload(const std::string_view value) {
  if (value == "all") {
    return WorkloadSelection::all;
  }
  if (value == "engine_durable_single_instrument") {
    return WorkloadSelection::engine_durable_single_instrument;
  }
  return std::nullopt;
}

std::optional<BenchmarkOptions> parse_options(const int argc, char** argv) {
  BenchmarkOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (const auto iterations = parse_positive_option(argument, "--iterations=")) {
      options.iterations = *iterations;
    } else if (const auto warmup = parse_positive_option(argument, "--warmup=")) {
      options.warmup = *warmup;
    } else if (argument.starts_with("--workload=")) {
      const auto workload = parse_workload(argument.substr(std::string_view("--workload=").size()));
      if (!workload.has_value()) {
        return std::nullopt;
      }
      options.workload = *workload;
    } else if (argument.starts_with("--data-dir=")) {
      const auto path = argument.substr(std::string_view("--data-dir=").size());
      if (path.empty()) {
        return std::nullopt;
      }
      options.data_directory = std::filesystem::path(path);
    } else {
      return std::nullopt;
    }
  }
  return options;
}

domain::CommittedCommand make_committed(const EngineSeq sequence) {
  Command command;
  command.identity = CommandIdentity{1, 1, 1, sequence};
  command.instrument_id = 1;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{1, sequence};
  command.payload = NewOrderPayload{Side::buy, 100, 1};
  return domain::CommittedCommand{command, sequence, static_cast<Timestamp>(sequence), 1, 1};
}

void require_order_book_success(const domain::OrderBookApplyResult& result) {
  if (result.error != ErrorCode::none) {
    throw std::runtime_error("order-book operation failed");
  }
}

class AcknowledgingSink final : public EventSink {
 public:
  Result<std::monostate> publish(const ShardId, const EngineSeq,
                                 const std::span<const Event>,
                                 const std::stop_token) override {
    return std::monostate{};
  }
};

struct ProducerLane {
  ProducerId producer_id{};
  ProducerSeq next_sequence{1};
};

enum class DurablePhase { warmup, measured };

struct DurableFailure {
  std::string code;
  std::string detail;
};

struct DurableRunState {
  explicit DurableRunState(const std::size_t lane_count)
      : lanes(lane_count), started_at(lane_count), expected(lane_count), in_flight(lane_count) {
    for (std::size_t index = 0; index < lane_count; ++index) {
      lanes[index].producer_id = static_cast<ProducerId>(index + 1U);
      available_lanes.push_back(index);
    }
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::vector<ProducerLane> lanes;
  std::deque<std::size_t> available_lanes;
  std::vector<std::chrono::steady_clock::time_point> started_at;
  std::vector<CommandIdentity> expected;
  std::vector<bool> in_flight;
  std::vector<std::uint64_t> latency_samples;
  DurablePhase phase{DurablePhase::warmup};
  std::size_t outstanding{};
  std::uint64_t phase_completed{};
  std::optional<DurableFailure> failure;
};

struct DurablePhaseResult {
  std::uint64_t submitted{};
  std::uint64_t completed{};
  std::uint64_t elapsed_ns{};
  std::vector<std::uint64_t> latency_samples;
};

void set_durable_failure(DurableRunState& state, std::string code, std::string detail) {
  if (!state.failure.has_value()) {
    state.failure = DurableFailure{std::move(code), std::move(detail)};
  }
  state.condition.notify_all();
}

std::string_view durable_phase_name(const DurablePhase phase) {
  return phase == DurablePhase::warmup ? "warmup" : "measured";
}

void report_durable_error(const std::string_view phase, const std::string_view code,
                         const std::string_view detail = {}) {
  std::cerr << "workload=engine_durable_single_instrument phase=" << phase
            << " error_code=" << code;
  if (!detail.empty()) {
    std::cerr << " detail=" << detail;
  }
  std::cerr << '\n';
}

std::string error_code_message(const ErrorCode code) {
  return "engine_error_code=" + std::to_string(static_cast<unsigned int>(code));
}

std::string command_identity_message(const CommandIdentity& identity) {
  return "producer_id=" + std::to_string(identity.producer_id) +
         " producer_epoch=" + std::to_string(identity.producer_epoch) +
         " producer_stream_id=" + std::to_string(identity.producer_stream_id) +
         " producer_seq=" + std::to_string(identity.producer_seq);
}

Command make_durable_command(const ProducerLane& lane, const Side side,
                             const std::uint64_t order_id) {
  Command command;
  command.identity = CommandIdentity{lane.producer_id, 1, 1, lane.next_sequence};
  command.instrument_id = 1;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{1, order_id};
  command.payload = NewOrderPayload{side, 100, 1};
  return command;
}

std::optional<std::uint64_t> double_count(const std::uint64_t count) {
  if (count > std::numeric_limits<std::uint64_t>::max() / 2U) {
    return std::nullopt;
  }
  return count * 2U;
}

std::optional<std::uint64_t> counter_delta(const std::uint64_t after,
                                           const std::uint64_t before) {
  if (after < before) {
    return std::nullopt;
  }
  return after - before;
}

bool wait_for_durable_phase(DurableRunState& state, const std::uint64_t submitted,
                            const std::chrono::steady_clock::time_point deadline) {
  std::unique_lock lock(state.mutex);
  if (!state.condition.wait_until(lock, deadline, [&state, submitted] {
        return state.failure.has_value() ||
               (state.phase_completed == submitted && state.outstanding == 0);
      })) {
    set_durable_failure(state, "phase_timeout", "phase completion timeout");
    return false;
  }
  return !state.failure.has_value() && state.phase_completed == submitted &&
         state.outstanding == 0;
}

std::optional<DurablePhaseResult> run_durable_phase(
    Engine& engine, DurableRunState& state, const std::uint64_t command_count,
    const DurablePhase phase, std::uint64_t& next_order_id) {
  {
    std::lock_guard lock(state.mutex);
    state.phase = phase;
    state.phase_completed = 0;
    state.latency_samples.clear();
    state.failure.reset();
  }

  const auto phase_start = std::chrono::steady_clock::now();
  std::uint64_t submitted = 0;
  while (submitted < command_count) {
    std::size_t lane_index = 0;
    Command command;
    std::chrono::steady_clock::time_point command_start;
    {
      std::unique_lock lock(state.mutex);
      if (!state.condition.wait_for(lock, kDurablePhaseTimeout, [&state] {
            return state.failure.has_value() || !state.available_lanes.empty();
          })) {
        set_durable_failure(state, "producer_lane_timeout",
                            "producer lane availability timeout");
        break;
      }
      if (state.failure.has_value()) {
        break;
      }
      lane_index = state.available_lanes.front();
      state.available_lanes.pop_front();
      auto& lane = state.lanes[lane_index];
      command_start = std::chrono::steady_clock::now();
      command = make_durable_command(
          lane, submitted % 2U == 0U ? Side::sell : Side::buy, next_order_id++);
      state.started_at[lane_index] = command_start;
      state.expected[lane_index] = command.identity;
      state.in_flight[lane_index] = true;
      ++state.outstanding;
    }

    const auto submit_result = engine.submit(
        std::move(command), [&state, lane_index](CommandResult result) {
          const auto completed_at = std::chrono::steady_clock::now();
          std::lock_guard lock(state.mutex);
          if (lane_index >= state.in_flight.size() || !state.in_flight[lane_index]) {
            set_durable_failure(state, "duplicate_completion",
                                "duplicate or unknown completion callback");
            return;
          }
          if (state.expected[lane_index] != result.identity) {
            set_durable_failure(
                state, "completion_identity_mismatch",
                "expected " + command_identity_message(state.expected[lane_index]) +
                    ", actual " + command_identity_message(result.identity));
          } else if (result.command_status != CommandStatus::committed) {
            set_durable_failure(state, "command_not_committed",
                                error_code_message(result.error_code));
          }
          if (state.phase == DurablePhase::measured) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                completed_at - state.started_at[lane_index]);
            state.latency_samples.push_back(static_cast<std::uint64_t>(elapsed.count()));
          }
          state.in_flight[lane_index] = false;
          state.available_lanes.push_back(lane_index);
          if (state.outstanding > 0) {
            --state.outstanding;
          }
          ++state.phase_completed;
          state.condition.notify_all();
        });
    if (!submit_result.queued) {
      std::lock_guard lock(state.mutex);
      state.in_flight[lane_index] = false;
      state.available_lanes.push_back(lane_index);
      if (state.outstanding > 0) {
        --state.outstanding;
      }
      if (submit_result.error.has_value()) {
        set_durable_failure(state, "submit_rejected",
                            error_code_message(submit_result.error->code) + " " +
                                submit_result.error->message);
      } else {
        set_durable_failure(state, "submit_rejected_without_error", {});
      }
      break;
    }
    {
      std::lock_guard lock(state.mutex);
      ++state.lanes[lane_index].next_sequence;
    }
    ++submitted;
  }

  const auto deadline = std::chrono::steady_clock::now() + kDurablePhaseTimeout;
  if (!wait_for_durable_phase(state, submitted, deadline)) {
    return std::nullopt;
  }
  const auto phase_end = std::chrono::steady_clock::now();
  DurablePhaseResult result;
  result.submitted = submitted;
  result.completed = state.phase_completed;
  result.elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(phase_end - phase_start).count());
  result.latency_samples = std::move(state.latency_samples);
  return result;
}

std::uint64_t percentile_ns(const std::vector<std::uint64_t>& samples,
                            const std::size_t numerator, const std::size_t denominator) {
  const auto index = std::min(
      samples.size() - 1U,
      (samples.size() * numerator + denominator - 1U) / denominator - 1U);
  return samples[index];
}

bool prepare_data_directory(const std::filesystem::path& path) {
  std::error_code error;
  const auto exists = std::filesystem::exists(path, error);
  if (error) {
    report_durable_error("setup", "data_directory_inspection_failed", path.string());
    return false;
  }
  if (exists) {
    if (error || !std::filesystem::is_directory(path, error)) {
      report_durable_error("setup", "data_directory_not_directory", path.string());
      return false;
    }
    if (!std::filesystem::is_empty(path, error) || error) {
      report_durable_error("setup", "data_directory_not_empty", path.string());
      return false;
    }
    return true;
  }
  if (!std::filesystem::create_directories(path, error) && error) {
    report_durable_error("setup", "data_directory_create_failed", path.string());
    return false;
  }
  return true;
}

bool run_engine_durable_single_instrument(const BenchmarkOptions& options) {
  const auto run_id = std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(static_cast<unsigned long long>(::getpid()));
  const bool owned_data_directory = !options.data_directory.has_value();
  const auto data_directory = options.data_directory.value_or(
      std::filesystem::temp_directory_path() / ("order_books_benchmark_engine-" + run_id));
  if (!prepare_data_directory(data_directory)) {
    return false;
  }
  const auto cleanup = [&] {
    if (owned_data_directory) {
      std::error_code ignored;
      std::filesystem::remove_all(data_directory, ignored);
    }
  };

  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.ingress_queue_capacity = 65'536;
  config.runtime.group_commit_max_commands = 256;
  config.runtime.group_commit_max_delay = std::chrono::microseconds(200);
  config.runtime.snapshot_interval_commands = std::numeric_limits<std::size_t>::max();
  config.runtime.snapshot_interval = std::chrono::hours(24);
  config.runtime.event_replay_snapshot_interval_commands =
      std::numeric_limits<std::size_t>::max();
  config.runtime.event_replay_snapshot_interval = std::chrono::hours(24);

  AcknowledgingSink event_sink;
  NullMetricsSink metrics_sink;
  auto opened = Engine::open(config, event_sink, metrics_sink);
  if (std::holds_alternative<Error>(opened)) {
    const auto& error = std::get<Error>(opened);
    report_durable_error("open", "engine_open_failed",
                         error_code_message(error.code) + " message=" + error.message);
    cleanup();
    return false;
  }
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));
  DurableRunState state(kDurableProducerLanes);
  std::uint64_t next_order_id = 1;
  const auto command_count = double_count(options.iterations);
  const auto warmup_command_count = double_count(options.warmup);
  if (!command_count.has_value() || !warmup_command_count.has_value()) {
    report_durable_error("setup", "iteration_count_overflow");
    (void)engine->stop();
    cleanup();
    return false;
  }

  const auto initial_metrics = engine->metrics(1);
  if (std::holds_alternative<Error>(initial_metrics)) {
    const auto& error = std::get<Error>(initial_metrics);
    report_durable_error("initial_metrics", "metrics_failed",
                         error_code_message(error.code) + " message=" + error.message);
    (void)engine->stop();
    cleanup();
    return false;
  }
  const auto warmup = run_durable_phase(*engine, state, *warmup_command_count,
                                        DurablePhase::warmup, next_order_id);
  if (!warmup.has_value()) {
    if (state.failure.has_value()) {
      report_durable_error(durable_phase_name(DurablePhase::warmup), state.failure->code,
                           state.failure->detail);
    }
    (void)engine->stop();
    cleanup();
    return false;
  }
  const auto warmup_metrics = engine->metrics(1);
  if (std::holds_alternative<Error>(warmup_metrics)) {
    const auto& error = std::get<Error>(warmup_metrics);
    report_durable_error("warmup_metrics", "metrics_failed",
                         error_code_message(error.code) + " message=" + error.message);
    (void)engine->stop();
    cleanup();
    return false;
  }
  const auto measured = run_durable_phase(*engine, state, *command_count,
                                          DurablePhase::measured, next_order_id);
  if (!measured.has_value()) {
    if (state.failure.has_value()) {
      report_durable_error(durable_phase_name(DurablePhase::measured), state.failure->code,
                           state.failure->detail);
    }
    (void)engine->stop();
    cleanup();
    return false;
  }
  const auto final_metrics = engine->metrics(1);
  const auto stop_status = engine->stop();
  if (std::holds_alternative<Error>(final_metrics) ||
      std::holds_alternative<Error>(stop_status)) {
    if (std::holds_alternative<Error>(final_metrics)) {
      const auto& error = std::get<Error>(final_metrics);
      report_durable_error("final_metrics", "metrics_failed",
                           error_code_message(error.code) + " message=" + error.message);
    }
    if (std::holds_alternative<Error>(stop_status)) {
      const auto& error = std::get<Error>(stop_status);
      report_durable_error("stop", "engine_stop_failed",
                           error_code_message(error.code) + " message=" + error.message);
    }
    cleanup();
    return false;
  }

  const auto& initial = std::get<MetricsSnapshot>(initial_metrics);
  const auto& after_warmup = std::get<MetricsSnapshot>(warmup_metrics);
  const auto& after_measured = std::get<MetricsSnapshot>(final_metrics);
  const auto expected_warmup = *warmup_command_count;
  const auto expected_measured = *command_count;
  const auto warmup_commands = counter_delta(after_warmup.commands, initial.commands);
  const auto measured_commands = counter_delta(after_measured.commands, after_warmup.commands);
  const auto measured_trades = counter_delta(after_measured.trades, after_warmup.trades);
  if (!warmup_commands.has_value() || !measured_commands.has_value() ||
      !measured_trades.has_value()) {
    report_durable_error("validation", "counter_delta_invalid");
    cleanup();
    return false;
  }
  if (*warmup_commands != expected_warmup || *measured_commands != expected_measured) {
    report_durable_error("validation", "unexpected_command_count");
    cleanup();
    return false;
  }
  if (*measured_trades != options.iterations) {
    report_durable_error("validation", "unexpected_trade_count");
    cleanup();
    return false;
  }
  if (measured->submitted != expected_measured || measured->completed != expected_measured ||
      measured->latency_samples.size() != expected_measured) {
    report_durable_error("validation", "completion_count_mismatch");
    cleanup();
    return false;
  }
  if (after_measured.active_orders != 0 || after_measured.active_price_levels != 0) {
    report_durable_error("validation", "book_not_empty");
    cleanup();
    return false;
  }

  std::error_code wal_path_error;
  const auto wal_path = std::filesystem::weakly_canonical(
      data_directory / "shard-1" / "wal", wal_path_error);
  if (wal_path_error) {
    report_durable_error("report", "wal_path_resolution_failed", data_directory.string());
    cleanup();
    return false;
  }

  auto samples = std::move(measured->latency_samples);
  std::sort(samples.begin(), samples.end());
  const auto elapsed_ns = measured->elapsed_ns;
  const auto commands_per_second = elapsed_ns == 0
                                       ? 0.0
                                       : static_cast<double>(expected_measured) *
                                             1'000'000'000.0 / static_cast<double>(elapsed_ns);
  const auto trades = *measured_trades;
  const auto trades_per_second = elapsed_ns == 0
                                     ? 0.0
                                     : static_cast<double>(trades) * 1'000'000'000.0 /
                                           static_cast<double>(elapsed_ns);
  std::cout << "engine_durable_single_instrument iterations=" << options.iterations
            << " commands=" << expected_measured << " trades=" << trades
            << " commands_per_second=" << commands_per_second
            << " trades_per_second=" << trades_per_second << " p50_us="
            << percentile_ns(samples, 50, 100) / 1'000.0 << " p99_us="
            << percentile_ns(samples, 99, 100) / 1'000.0 << " p99.9_us="
            << percentile_ns(samples, 999, 1000) / 1'000.0 << " max_us="
            << samples.back() / 1'000.0 << " elapsed_ms=" << elapsed_ns / 1'000'000.0
            << " active_orders=" << after_measured.active_orders
            << " active_levels=" << after_measured.active_price_levels
            << " group_size=" << config.runtime.group_commit_max_commands
            << " group_delay_us=" << config.runtime.group_commit_max_delay.count()
            << " fsync_mode=per_group completion_boundary=durable_callback"
            << " instrument_count=1 shard_count=1 producer_lanes=" << kDurableProducerLanes
            << " wal_path=" << wal_path << " wal_bytes=" << after_measured.wal_size_bytes
            << '\n';
  cleanup();
  return true;
}

}  // namespace

int main(const int argc, char** argv) {
  using domain::OrderBook;

  const auto options = parse_options(argc, argv);
  if (!options.has_value() || options->iterations == 0) {
    std::cerr << "usage: order_books_benchmark [--iterations=N] [--warmup=N] "
                 "[--workload=all|engine_durable_single_instrument] [--data-dir=PATH]\n";
    return 2;
  }

#if defined(__linux__)
  constexpr auto platform = "linux";
#elif defined(__APPLE__)
  constexpr auto platform = "macos";
#else
  constexpr auto platform = "other";
#endif
#if defined(NDEBUG)
  constexpr auto build_type = "Release";
#else
  constexpr auto build_type = "Debug";
#endif
  std::cout << "platform=" << platform << " compiler=" << __VERSION__
            << " build_type=" << build_type << " seed=1 iterations=" << options->iterations
            << " warmup=" << options->warmup
            << " cpu_threads=" << std::thread::hardware_concurrency()
            << " cpu_model=unavailable\n";
  const auto report_setup_failure = [](const auto& result, const std::string_view name) {
    if (std::holds_alternative<Error>(result)) {
      std::cerr << name << ": " << std::get<Error>(result).message << '\n';
    } else {
      std::cerr << name << ": unexpected setup result\n";
    }
  };

  if (options->workload == WorkloadSelection::all ||
      options->workload == WorkloadSelection::engine_durable_single_instrument) {
    if (!run_engine_durable_single_instrument(*options)) {
      return 1;
    }
  }
  if (options->workload == WorkloadSelection::engine_durable_single_instrument) {
    return 0;
  }

  OrderBook resting(1);
  if (!run_workload("resting_new", *options, [&resting](const std::uint64_t index) {
    require_order_book_success(resting.add_new(make_order(index + 1, Side::buy,
                                                          static_cast<Price>(index + 1), 1,
                                                          index + 1)));
    return WorkloadDelta{};
  }, [&resting] {
    return std::pair<std::uint64_t, std::uint64_t>{resting.active_order_count(),
                                                    resting.active_price_level_count()};
  })) {
    return 1;
  }

  OrderBook crossing(1);
  if (!run_workload("crossing_new", *options, [&crossing](const std::uint64_t index) {
    const auto base = index * 2U + 1U;
    require_order_book_success(crossing.add_new(make_order(base, Side::sell, 100, 1, base)));
    require_order_book_success(
        crossing.add_new(make_order(base + 1U, Side::buy, 101, 1, base + 1U)));
    return WorkloadDelta{2, 1};
  }, [&crossing] {
    return std::pair<std::uint64_t, std::uint64_t>{crossing.active_order_count(),
                                                    crossing.active_price_level_count()};
  })) {
    return 1;
  }

  OrderBook cancelling(1);
  if (!run_workload("cancel", *options, [&cancelling](const std::uint64_t index) {
    const auto id = index + 1U;
    require_order_book_success(cancelling.add_new(make_order(id, Side::buy, 100, 1, id)));
    require_order_book_success(cancelling.cancel(OrderId{0, id}));
    return WorkloadDelta{2, 0};
  }, [&cancelling] {
    return std::pair<std::uint64_t, std::uint64_t>{cancelling.active_order_count(),
                                                    cancelling.active_price_level_count()};
  })) {
    return 1;
  }

  OrderBook amending(1);
  if (!run_workload("amend_decrease", *options, [&amending](const std::uint64_t index) {
    const auto id = index + 1U;
    require_order_book_success(amending.add_new(make_order(id, Side::buy, 100, 2, id)));
    require_order_book_success(amending.amend_quantity(OrderId{0, id}, 1, id + 1U));
    return WorkloadDelta{2, 0};
  }, [&amending] {
    return std::pair<std::uint64_t, std::uint64_t>{amending.active_order_count(),
                                                    amending.active_price_level_count()};
  })) {
    return 1;
  }

  OrderBook increasing(1);
  if (!run_workload("amend_increase", *options, [&increasing](const std::uint64_t index) {
    const auto id = index + 1U;
    require_order_book_success(increasing.add_new(make_order(id, Side::buy, 100, 1, id)));
    require_order_book_success(increasing.amend_quantity(OrderId{0, id}, 2, id + 1U));
    return WorkloadDelta{2, 0};
  }, [&increasing] {
    return std::pair<std::uint64_t, std::uint64_t>{increasing.active_order_count(),
                                                    increasing.active_price_level_count()};
  })) {
    return 1;
  }

  OrderBook replacing(1);
  if (!run_workload("replace_crossing", *options,
               [&replacing](const std::uint64_t index) {
                 const auto base = index * 2U + 1U;
                 require_order_book_success(
                     replacing.add_new(make_order(base, Side::sell, 100, 1, base)));
                 require_order_book_success(replacing.add_new(
                     make_order(base + 1U, Side::buy, 99, 1, base + 1U)));
                 require_order_book_success(
                     replacing.replace(OrderId{0, base + 1U}, 101, 1, base + 2U));
                 return WorkloadDelta{3, 1};
               }, [&replacing] {
                 return std::pair<std::uint64_t, std::uint64_t>{
                     replacing.active_order_count(), replacing.active_price_level_count()};
               })) {
    return 1;
  }

  OrderBook multiple_match(1);
  if (!run_workload("multiple_match", *options,
               [&multiple_match](const std::uint64_t index) {
                 const auto base = index * 5U + 1U;
                 for (std::uint64_t maker = 0; maker < 4; ++maker) {
                   require_order_book_success(multiple_match.add_new(
                       make_order(base + maker, Side::sell, 100, 1, base + maker)));
                 }
                 require_order_book_success(multiple_match.add_new(
                     make_order(base + 4U, Side::buy, 100, 4, base + 4U)));
                 return WorkloadDelta{5, 4};
               }, [&multiple_match] {
                 return std::pair<std::uint64_t, std::uint64_t>{
                     multiple_match.active_order_count(), multiple_match.active_price_level_count()};
               })) {
    return 1;
  }

  OrderBook mixed(1);
  if (!run_workload("mixed_single_instrument", *options,
               [&mixed](const std::uint64_t index) {
                 const auto id = index + 1U;
                 WorkloadDelta delta;
                 require_order_book_success(mixed.add_new(
                     make_order(id, Side::buy, static_cast<Price>(100 + index % 3U), 2, id)));
                 if (index % 2U == 0U) {
                   require_order_book_success(mixed.amend_quantity(OrderId{0, id}, 1, id + 1U));
                   ++delta.commands;
                 }
                 if (index % 3U == 0U) {
                   require_order_book_success(mixed.cancel(OrderId{0, id}));
                   ++delta.commands;
                 }
                 return delta;
               }, [&mixed] {
                 return std::pair<std::uint64_t, std::uint64_t>{mixed.active_order_count(),
                                                                 mixed.active_price_level_count()};
               })) {
    return 1;
  }

  const auto run_id = std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(static_cast<unsigned long long>(::getpid()));
  const auto data_directory =
      std::filesystem::temp_directory_path() / ("order_books_benchmark_wal-" + run_id);
  std::error_code ignored;
  std::filesystem::remove_all(data_directory, ignored);
  auto wal_result = storage::Wal::open(data_directory, 1, 1U * 1024U * 1024U);
  if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(wal_result)) {
    report_setup_failure(wal_result, "durable_group_commit setup failed");
    std::filesystem::remove_all(data_directory, ignored);
    return 1;
  }
  auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(wal_result));
  constexpr std::uint64_t durable_group_size = 256;
  if (!run_workload(
          "durable_group_commit", *options,
          [&wal](const std::uint64_t index) {
            if (index > std::numeric_limits<std::uint64_t>::max() / durable_group_size) {
              throw std::runtime_error("durable group sequence overflow");
            }
            const auto first_sequence = index * durable_group_size + 1U;
            for (std::uint64_t offset = 0; offset < durable_group_size; ++offset) {
              const auto appended = wal->append(make_committed(first_sequence + offset));
              if (std::holds_alternative<Error>(appended)) {
                throw std::runtime_error("WAL append failed: " +
                                         std::get<Error>(appended).message);
              }
            }
            const auto synced = wal->sync();
            if (std::holds_alternative<Error>(synced)) {
              throw std::runtime_error("WAL sync failed: " +
                                       std::get<Error>(synced).message);
            }
            return WorkloadDelta{durable_group_size, 0};
          },
          [] { return std::pair<std::uint64_t, std::uint64_t>{0, 0}; },
          "wal_path=" + data_directory.string() + " group_size=" +
              std::to_string(durable_group_size) + " fsync_mode=per_group")) {
    std::filesystem::remove_all(data_directory, ignored);
    return 1;
  }
  std::filesystem::remove_all(data_directory, ignored);

  const auto recovery_directory =
      std::filesystem::temp_directory_path() / ("order_books_benchmark_recovery-" + run_id);
  std::filesystem::remove_all(recovery_directory, ignored);
  auto recovery_wal_result = storage::Wal::open(recovery_directory / "wal", 1,
                                                 1U * 1024U * 1024U);
  auto recovery_snapshots_result =
      storage::SnapshotStore::open(recovery_directory / "snapshots", 1);
  if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(recovery_wal_result) ||
      !std::holds_alternative<storage::SnapshotStore>(recovery_snapshots_result)) {
    if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(recovery_wal_result)) {
      report_setup_failure(recovery_wal_result, "recovery WAL setup failed");
    }
    if (!std::holds_alternative<storage::SnapshotStore>(recovery_snapshots_result)) {
      report_setup_failure(recovery_snapshots_result, "recovery snapshot setup failed");
    }
    std::filesystem::remove_all(recovery_directory, ignored);
    return 1;
  }
  auto recovery_wal =
      std::get<std::unique_ptr<storage::Wal>>(std::move(recovery_wal_result));
  domain::ShardState genesis;
  genesis.shard_id = 1;
  genesis.current_instrument_configuration_version = 1;
  genesis.current_behavior_configuration_version = 1;
  genesis.instruments.emplace(1, InstrumentConfig{1, 1, 1, 1});
  genesis.instrument_configurations.emplace(1, genesis.instruments);
  genesis.behavior_configurations.emplace(1, ShardBehaviorConfig{});
  auto snapshots = std::get<storage::SnapshotStore>(std::move(recovery_snapshots_result));
  if (const auto status = snapshots.write(genesis); std::holds_alternative<Error>(status)) {
    std::cerr << "recovery snapshot write failed: " << std::get<Error>(status).message << '\n';
    std::filesystem::remove_all(recovery_directory, ignored);
    return 1;
  }
  constexpr std::uint64_t recovery_records = 500;
  for (std::uint64_t sequence = 1; sequence <= recovery_records; ++sequence) {
    const auto appended = recovery_wal->append(make_committed(sequence));
    if (std::holds_alternative<Error>(appended)) {
      std::cerr << "recovery WAL append failed: " << std::get<Error>(appended).message << '\n';
      std::filesystem::remove_all(recovery_directory, ignored);
      return 1;
    }
  }
  if (const auto status = recovery_wal->sync(); std::holds_alternative<Error>(status)) {
    std::cerr << "recovery WAL sync failed: " << std::get<Error>(status).message << '\n';
    std::filesystem::remove_all(recovery_directory, ignored);
    return 1;
  }
  const auto recovery_wal_bytes = recovery_wal->size_bytes();
  const auto recovery_iterations = std::max<std::uint64_t>(1U, options->iterations / 10U);
  BenchmarkOptions recovery_options;
  recovery_options.iterations = recovery_iterations;
  recovery_options.warmup = options->warmup;
  if (!run_workload(
          "recovery_snapshot_plus_wal", recovery_options,
          [&recovery_directory, recovery_records](const std::uint64_t) {
            auto opened_wal = storage::Wal::open(recovery_directory / "wal", 1,
                                                 1U * 1024U * 1024U);
            auto opened_snapshots = storage::SnapshotStore::open(
                recovery_directory / "snapshots", 1);
            if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(opened_wal)) {
              throw std::runtime_error("recovery WAL open failed");
            }
            if (!std::holds_alternative<storage::SnapshotStore>(opened_snapshots)) {
              throw std::runtime_error("recovery snapshot open failed");
            }
            auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(opened_wal));
            auto snapshot_store =
                std::get<storage::SnapshotStore>(std::move(opened_snapshots));
            auto state = snapshot_store.load_latest();
            if (std::holds_alternative<Error>(state)) {
              throw std::runtime_error("recovery snapshot load failed: " +
                                       std::get<Error>(state).message);
            }
            auto records = wal->replay();
            if (std::holds_alternative<Error>(records)) {
              throw std::runtime_error("recovery WAL replay failed: " +
                                       std::get<Error>(records).message);
            }
            auto recovered_state =
                std::get<std::optional<domain::ShardState>>(std::move(state));
            if (!recovered_state.has_value()) {
              throw std::runtime_error("recovery snapshot is missing");
            }
            domain::StateMachine machine(std::move(*recovered_state));
            for (const auto& record :
                 std::get<std::vector<domain::CommittedCommand>>(std::move(records))) {
              if (record.engine_seq <= machine.state().last_committed_engine_seq) {
                continue;
              }
              const auto result = machine.apply(record);
              if (std::holds_alternative<Error>(result)) {
                throw std::runtime_error("recovery apply failed: " +
                                         std::get<Error>(result).message);
              }
            }
            if (std::holds_alternative<Error>(domain::validate_state(machine.state()))) {
              throw std::runtime_error("recovery invariant validation failed");
            }
            return WorkloadDelta{recovery_records, 0};
          },
          [] { return std::pair<std::uint64_t, std::uint64_t>{500, 1}; },
          "snapshot_orders=0 wal_records=" + std::to_string(recovery_records) +
              " wal_bytes=" + std::to_string(recovery_wal_bytes))) {
    std::filesystem::remove_all(recovery_directory, ignored);
    return 1;
  }
  std::filesystem::remove_all(recovery_directory, ignored);
  return 0;
}
