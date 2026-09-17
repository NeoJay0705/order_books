#include "pipeline_ceiling_benchmark.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

#include "domain/invariant_checker.hpp"
#include "domain/state_machine.hpp"
#include "order_books/engine.hpp"
#include "persistence/binary_codec.hpp"
#include "persistence/snapshot_store.hpp"
#include "persistence/wal.hpp"
#include "runtime/event_publisher.hpp"
#include "runtime/metrics_registry.hpp"

namespace order_books::benchmark {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kTombstoneLimit = 1'000'000;
constexpr std::size_t kWalSegmentSize = 256U * 1024U * 1024U;
constexpr auto kPhaseTimeout = 60s;

class AcknowledgingSink final : public EventSink {
 public:
  Result<std::monostate> publish(const ShardId, const EngineSeq,
                                 const std::span<const Event>,
                                 const std::stop_token) override {
    return std::monostate{};
  }
};

struct Samples {
  std::vector<std::uint64_t> values;

  [[nodiscard]] std::uint64_t elapsed_ns() const noexcept {
    return std::accumulate(values.begin(), values.end(), std::uint64_t{0});
  }
};

std::uint64_t percentile_ns(const std::vector<std::uint64_t>& values,
                            const std::size_t numerator,
                            const std::size_t denominator) {
  if (values.empty()) {
    return 0;
  }
  const auto index = std::min(
      values.size() - 1U,
      (values.size() * numerator + denominator - 1U) / denominator - 1U);
  return values[index];
}

std::uint64_t checked_multiply(const std::uint64_t lhs, const std::uint64_t rhs) {
  if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
    throw std::runtime_error("operation count overflow");
  }
  return lhs * rhs;
}

std::uint64_t checked_add(const std::uint64_t lhs, const std::uint64_t rhs) {
  if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
    throw std::runtime_error("counter overflow");
  }
  return lhs + rhs;
}

std::string run_id() {
  return std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(static_cast<unsigned long long>(::getpid()));
}

void report_error(const std::string_view stage, const std::string_view phase,
                  const std::string_view code, const std::string_view detail = {}) {
  std::cerr << "workload=engine_pipeline_ceiling stage=" << stage
            << " phase=" << phase << " error_code=" << code;
  if (!detail.empty()) {
    std::cerr << " detail=" << detail;
  }
  std::cerr << '\n';
}

bool prepare_directory(const std::filesystem::path& path,
                       const bool require_empty = true) {
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    if (error || !std::filesystem::is_directory(path, error) || error) {
      return false;
    }
    return !require_empty ||
           (std::filesystem::is_empty(path, error) && !error);
  }
  if (error) {
    return false;
  }
  return std::filesystem::create_directories(path, error) || !error;
}

bool prepare_new_directory(const std::filesystem::path& path) {
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    return false;
  }
  if (error) {
    return false;
  }
  return std::filesystem::create_directories(path, error) || !error;
}

std::optional<std::filesystem::path> prepare_root(
    const std::optional<std::filesystem::path>& requested,
    bool& owned) {
  owned = !requested.has_value();
  const auto path = requested.value_or(
      std::filesystem::temp_directory_path() / ("order_books_pipeline-" + run_id()));
  if (!prepare_directory(path)) {
    return std::nullopt;
  }
  return path;
}

std::string error_message(const Error& error) {
  return "code=" + std::to_string(static_cast<unsigned int>(error.code)) +
         " message=" + error.message;
}

domain::ShardState make_genesis_state() {
  domain::ShardState state;
  state.shard_id = 1;
  state.current_instrument_configuration_version = 1;
  state.current_behavior_configuration_version = 1;
  state.instruments.emplace(1, InstrumentConfig{1, 1, 1, 1});
  state.instrument_configurations.emplace(1, state.instruments);
  state.behavior_configurations.emplace(
      1, ShardBehaviorConfig{1, 1'000'000, 3'600'000'000'000LL, kTombstoneLimit});
  return state;
}

domain::ShardState clone_state(const domain::ShardState& state) {
  auto decoded = storage::decode_state(storage::encode_state(state));
  if (std::holds_alternative<Error>(decoded)) {
    throw std::runtime_error("state clone failed: " +
                             error_message(std::get<Error>(decoded)));
  }
  return std::get<domain::ShardState>(std::move(decoded));
}

domain::CommittedCommand make_state_command(const EngineSeq engine_seq,
                                            const ProducerSeq producer_seq,
                                            const ProducerId producer_id,
                                            const Side side, const Price price,
                                            const std::uint64_t order_id) {
  Command command;
  command.identity = CommandIdentity{producer_id, 1, 1, producer_seq};
  command.instrument_id = 1;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{1, order_id};
  command.payload = NewOrderPayload{side, price, 1};
  return domain::CommittedCommand{std::move(command), engine_seq,
                                  static_cast<Timestamp>(engine_seq), 1, 1};
}

domain::ExecutionOutput apply_or_throw(domain::StateMachine& machine,
                                        domain::CommittedCommand command) {
  auto result = machine.apply(command);
  if (std::holds_alternative<Error>(result)) {
    throw std::runtime_error(error_message(std::get<Error>(result)));
  }
  return std::get<domain::ExecutionOutput>(std::move(result));
}

struct StateFixture {
  explicit StateFixture(const std::size_t active_orders)
      : machine(make_genesis_state()) {
    for (std::size_t index = 0; index < active_orders; ++index) {
      auto output = apply_or_throw(
          machine, make_state_command(next_engine_seq, next_producer_seq, 1, Side::buy,
                                      10 + static_cast<Price>(index % 10U),
                                      next_order_id));
      if (output.result.command_status != CommandStatus::committed) {
        throw std::runtime_error("state fixture order was not committed");
      }
      ++next_engine_seq;
      ++next_producer_seq;
      ++next_order_id;
    }
    if (std::holds_alternative<Error>(domain::validate_state(machine.state()))) {
      throw std::runtime_error("state fixture invariant validation failed");
    }
  }

  domain::StateMachine machine;
  EngineSeq next_engine_seq{1};
  ProducerSeq next_producer_seq{1};
  std::uint64_t next_order_id{1};
};

void print_timing(const std::string_view stage, const std::string_view test_case,
                  const PipelineBenchmarkOptions& options, const Samples& samples,
                  const std::uint64_t operations, const std::uint64_t elapsed_ns,
                  const std::string_view completion_boundary,
                  const std::string_view latency_scope,
                  const std::string_view details,
                  const std::optional<double> command_equivalent_per_second = {}) {
  auto sorted = samples.values;
  std::sort(sorted.begin(), sorted.end());
  const auto operations_per_second = elapsed_ns == 0
                                        ? 0.0
                                        : static_cast<double>(operations) * 1'000'000'000.0 /
                                              static_cast<double>(elapsed_ns);
  std::cout << "engine_pipeline_ceiling stage=" << stage << " case=" << test_case
            << " iterations=" << options.iterations << " warmup=" << options.warmup
            << " operations=" << operations << " batch_size=" << options.batch_size
            << " operations_per_second=" << operations_per_second
            << " target_commands_per_second=1000000";
  if (command_equivalent_per_second.has_value()) {
    std::cout << " command_equivalent_per_second=" << *command_equivalent_per_second
              << " target_attainment_percent=" << (*command_equivalent_per_second / 10'000.0);
  }
  std::cout << " group_p50_us=" << percentile_ns(sorted, 50, 100) / 1'000.0
            << " group_p99_us=" << percentile_ns(sorted, 99, 100) / 1'000.0
            << " group_p99.9_us=" << percentile_ns(sorted, 999, 1000) / 1'000.0
            << " group_max_us="
            << (sorted.empty() ? std::string("na")
                                : std::to_string(sorted.back() / 1'000.0))
            << " elapsed_ms=" << elapsed_ns / 1'000'000.0
            << " completion_boundary=" << completion_boundary
            << " latency_scope=" << latency_scope
            << " correctness_verified=true";
  if (!details.empty()) {
    std::cout << ' ' << details;
  }
  std::cout << '\n';
}

bool run_state_machine(const PipelineBenchmarkOptions& options) {
  if (options.batch_size == 0 || options.batch_size % 2U != 0) {
    report_error("state_machine", "setup", "batch_size_must_be_even");
    return false;
  }
  try {
    StateFixture fixture(options.active_orders);
    const auto pairs_per_group = options.batch_size / 2U;
    const auto verify_output = [](const domain::ExecutionOutput& output,
                                  const EngineSeq expected_engine_seq) {
      if (output.result.command_status != CommandStatus::committed ||
          output.result.error_code != ErrorCode::none ||
          !output.result.engine_seq.has_value() ||
          *output.result.engine_seq != expected_engine_seq) {
        throw std::runtime_error("state command result mismatch");
      }
    };
    auto run_group = [&fixture, pairs_per_group, &verify_output](std::uint64_t& trades,
                                                                  std::uint64_t& events) {
      for (std::size_t pair = 0; pair < pairs_per_group; ++pair) {
        const auto sell_engine_seq = fixture.next_engine_seq;
        auto sell = apply_or_throw(
            fixture.machine,
            make_state_command(fixture.next_engine_seq, fixture.next_producer_seq, 1,
                               Side::sell, 100, fixture.next_order_id));
        verify_output(sell, sell_engine_seq);
        ++fixture.next_engine_seq;
        ++fixture.next_producer_seq;
        ++fixture.next_order_id;
        const auto buy_engine_seq = fixture.next_engine_seq;
        auto buy = apply_or_throw(
            fixture.machine,
            make_state_command(fixture.next_engine_seq, fixture.next_producer_seq, 1,
                               Side::buy, 100, fixture.next_order_id));
        verify_output(buy, buy_engine_seq);
        ++fixture.next_engine_seq;
        ++fixture.next_producer_seq;
        ++fixture.next_order_id;
        for (const auto* output : {&sell, &buy}) {
          events = checked_add(events, output->events.size());
          trades = checked_add(
              trades, static_cast<std::uint64_t>(std::count_if(
                          output->events.begin(), output->events.end(),
                          [](const Event& event) { return event.event_type == EventType::trade; })));
        }
      }
    };

    std::uint64_t ignored_trades = 0;
    std::uint64_t ignored_events = 0;
    for (std::uint64_t group = 0; group < options.warmup; ++group) {
      run_group(ignored_trades, ignored_events);
    }
    Samples samples;
    samples.values.reserve(options.iterations);
    std::uint64_t trades = 0;
    std::uint64_t events = 0;
    for (std::uint64_t group = 0; group < options.iterations; ++group) {
      const auto start = std::chrono::steady_clock::now();
      run_group(trades, events);
      samples.values.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start)
              .count()));
    }
    if (std::holds_alternative<Error>(domain::validate_state(fixture.machine.state())) ||
        fixture.machine.state().active_order_count != options.active_orders) {
      report_error("state_machine", "validation", "state_validation_failed");
      return false;
    }
    const auto commands = checked_multiply(options.iterations, options.batch_size);
    const auto expected_trades = checked_multiply(options.iterations, pairs_per_group);
    const auto warmup_commands = checked_multiply(options.warmup, options.batch_size);
    const auto expected_events = checked_multiply(commands, 2U);
    const auto expected_last_engine_seq = checked_add(
        static_cast<std::uint64_t>(options.active_orders),
        checked_add(warmup_commands, commands));
    if (trades != expected_trades || events != expected_events ||
        fixture.machine.state().last_committed_engine_seq != expected_last_engine_seq) {
      report_error("state_machine", "validation", "result_count_mismatch");
      return false;
    }
    const auto elapsed_ns = samples.elapsed_ns();
    const auto command_rate = elapsed_ns == 0
                                  ? 0.0
                                  : static_cast<double>(commands) * 1'000'000'000.0 /
                                        static_cast<double>(elapsed_ns);
    print_timing("state_machine", "crossing_pair", options, samples, commands,
                 elapsed_ns, "state_apply_return", "command_group",
                 "commands=" + std::to_string(commands) +
                     " trades=" + std::to_string(trades) +
                     " events=" + std::to_string(events) +
                     " active_orders=" +
                     std::to_string(fixture.machine.state().active_order_count) +
                     " active_levels=" +
                     std::to_string([&fixture] {
                       std::size_t levels = 0;
                       for (const auto& [unused, book] : fixture.machine.state().books) {
                         (void)unused;
                         levels += book.active_price_level_count();
                       }
                       return levels;
                     }()) +
                     " tombstone_limit=" + std::to_string(kTombstoneLimit) +
                     " last_engine_seq=" +
                     std::to_string(fixture.machine.state().last_committed_engine_seq),
                 command_rate);
    return true;
  } catch (const std::exception& error) {
    report_error("state_machine", "measured", "exception", error.what());
    return false;
  }
}

bool run_invariant_validation(const PipelineBenchmarkOptions& options) {
  try {
    StateFixture fixture(options.active_orders);
    for (std::uint64_t group = 0; group < options.warmup; ++group) {
      if (std::holds_alternative<Error>(domain::validate_state(fixture.machine.state()))) {
        throw std::runtime_error("warmup invariant validation failed");
      }
    }
    Samples samples;
    samples.values.reserve(options.iterations);
    for (std::uint64_t group = 0; group < options.iterations; ++group) {
      const auto start = std::chrono::steady_clock::now();
      if (std::holds_alternative<Error>(domain::validate_state(fixture.machine.state()))) {
        throw std::runtime_error("measured invariant validation failed");
      }
      samples.values.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start)
              .count()));
    }
    const auto elapsed_ns = samples.elapsed_ns();
    const auto validations_per_second = elapsed_ns == 0
                                             ? 0.0
                                             : static_cast<double>(options.iterations) *
                                                   1'000'000'000.0 /
                                                   static_cast<double>(elapsed_ns);
    const auto command_equivalent = elapsed_ns == 0
                                         ? 0.0
                                         : static_cast<double>(checked_multiply(
                                               options.iterations, options.batch_size)) *
                                               1'000'000'000.0 /
                                               static_cast<double>(elapsed_ns);
    print_timing(
        "invariant_validation", "fixed_state", options, samples, options.iterations,
        elapsed_ns, "invariant_return", "validation_call",
        "validations=" + std::to_string(options.iterations) +
            " validations_per_second=" + std::to_string(validations_per_second) +
            " amortized_commands_per_second=" + std::to_string(command_equivalent) +
            " active_orders=" + std::to_string(fixture.machine.state().active_order_count) +
            " active_levels=" +
            std::to_string([&fixture] {
              std::size_t levels = 0;
              for (const auto& [unused, book] : fixture.machine.state().books) {
                (void)unused;
                levels += book.active_price_level_count();
              }
              return levels;
            }()) +
            " producer_states=" +
            std::to_string(fixture.machine.state().producer_states.size()) +
            " tombstones=" + std::to_string(fixture.machine.state().tombstones.size()),
        command_equivalent);
    return true;
  } catch (const std::exception& error) {
    report_error("invariant_validation", "measured", "exception", error.what());
    return false;
  }
}

constexpr std::size_t kWriterMetricCallsPerCommand = 11;
constexpr std::size_t kPublisherMetricCallsPerCommand = 5;
constexpr std::string_view kWriterMetricNames =
    "queue_depth,queue_latency_us,wal_commit_latency_us,execution_latency_us,"
    "end_to_end_latency_us,commands,trades,wal_size_bytes,active_orders,"
    "active_instruments,active_price_levels";
constexpr std::string_view kPublisherMetricNames =
    "replayed_records,publish_latency_us,event_publish_lag_events,"
    "event_publish_lag_bytes,event_publish_lag_age_ns";

void observe_writer_metric_mix(runtime::MetricsRegistry& registry,
                               const std::uint64_t index) {
  registry.observe("queue_depth", index % 256U);
  registry.observe("queue_latency_us", index % 100U);
  registry.observe("wal_commit_latency_us", 1U + index % 10U);
  registry.observe("execution_latency_us", 1U + index % 10U);
  registry.observe("end_to_end_latency_us", 2U + index % 20U);
  registry.observe("commands", 1);
  registry.observe("trades", index % 2U);
  registry.observe("wal_size_bytes", 122U * (index + 1U));
  registry.observe("active_orders", index % 10U);
  registry.observe("active_instruments", 1);
  registry.observe("active_price_levels", index % 4U);
}

void observe_publisher_metric_mix(runtime::MetricsRegistry& registry,
                                  const std::uint64_t index) {
  registry.observe("replayed_records", 1);
  registry.observe("publish_latency_us", 1U + index % 10U);
  registry.observe("event_publish_lag_events", index % 256U);
  registry.observe("event_publish_lag_bytes", 122U * (index + 1U));
  registry.observe("event_publish_lag_age_ns", 1'000U + index % 100U);
}

bool run_metrics_single(const PipelineBenchmarkOptions& options) {
  NullMetricsSink downstream;
  runtime::MetricsRegistry registry(downstream);
  const auto commands_per_phase = checked_multiply(options.iterations, options.batch_size);
  for (std::uint64_t index = 0;
       index < checked_multiply(options.warmup, options.batch_size); ++index) {
    observe_writer_metric_mix(registry, index);
  }
  Samples samples;
  samples.values.reserve(options.iterations);
  std::uint64_t index = 0;
  for (std::uint64_t group = 0; group < options.iterations; ++group) {
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t command = 0; command < options.batch_size; ++command) {
      observe_writer_metric_mix(registry, index++);
    }
    samples.values.push_back(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count()));
  }
  const auto snapshot = registry.snapshot();
  const auto expected_commands = checked_add(
      checked_multiply(options.warmup, options.batch_size), commands_per_phase);
  if (snapshot.commands != expected_commands) {
    report_error("metrics", "validation", "metric_command_count_mismatch");
    return false;
  }
  const auto elapsed_ns = samples.elapsed_ns();
  const auto rate = elapsed_ns == 0
                        ? 0.0
                        : static_cast<double>(commands_per_phase) * 1'000'000'000.0 /
                              static_cast<double>(elapsed_ns);
  const auto observations =
      checked_multiply(commands_per_phase, kWriterMetricCallsPerCommand);
  const auto observations_rate =
      elapsed_ns == 0
          ? 0.0
          : static_cast<double>(observations) * 1'000'000'000.0 /
                static_cast<double>(elapsed_ns);
  print_timing("metrics", "writer_only", options, samples, observations, elapsed_ns,
               "metrics_return", "metric_group",
               "contention_mode=single_worker writer_metric_calls_per_command=" +
                   std::to_string(kWriterMetricCallsPerCommand) +
                   " publisher_metric_calls_per_command=" +
                   std::to_string(kPublisherMetricCallsPerCommand) +
                   " writer_metric_names=" + std::string(kWriterMetricNames) +
                   " observations=" + std::to_string(observations) +
                   " observations_per_second=" + std::to_string(observations_rate),
               rate);
  return true;
}

struct ContendedMetricResult {
  Samples samples;
  std::uint64_t commands{};
  std::uint64_t elapsed_ns{};
};

bool run_metrics_parallel(const PipelineBenchmarkOptions& options,
                          const bool separate_registries) {
  NullMetricsSink shared_downstream;
  runtime::MetricsRegistry shared_registry(shared_downstream);
  runtime::MetricsRegistry writer_registry(shared_downstream);
  runtime::MetricsRegistry publisher_registry(shared_downstream);
  auto* writer_metrics = separate_registries ? &writer_registry : &shared_registry;
  auto* publisher_metrics = separate_registries ? &publisher_registry : &shared_registry;
  const auto commands_per_worker = checked_multiply(options.iterations, options.batch_size);
  const auto warmup_commands = checked_multiply(options.warmup, options.batch_size);
  const auto run_worker = [warmup_commands](runtime::MetricsRegistry& registry,
                                            const auto observer,
                                            const std::uint64_t seed) {
    for (std::uint64_t index = 0; index < warmup_commands; ++index) {
      observer(registry, seed + index);
    }
  };
  run_worker(*writer_metrics, observe_writer_metric_mix, 0);
  run_worker(*publisher_metrics, observe_publisher_metric_mix, commands_per_worker + 1U);
  ContendedMetricResult first;
  ContendedMetricResult second;
  first.samples.values.reserve(options.iterations);
  second.samples.values.reserve(options.iterations);
  std::chrono::steady_clock::time_point wall_start;
  std::barrier start_barrier(3, [&wall_start]() noexcept {
    wall_start = std::chrono::steady_clock::now();
  });
  const auto measure_worker = [&options, &start_barrier](runtime::MetricsRegistry* registry,
                                                          const auto observer,
                                                          const std::uint64_t seed,
                                                          ContendedMetricResult& result) {
    start_barrier.arrive_and_wait();
    const auto worker_start = std::chrono::steady_clock::now();
    std::uint64_t index = seed;
    for (std::uint64_t group = 0; group < options.iterations; ++group) {
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t command = 0; command < options.batch_size; ++command) {
        observer(*registry, index++);
      }
      result.samples.values.push_back(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - start)
              .count()));
    }
    result.commands = checked_multiply(options.iterations, options.batch_size);
    result.elapsed_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - worker_start)
            .count());
  };
  std::thread first_thread(measure_worker, writer_metrics, observe_writer_metric_mix, 0,
                           std::ref(first));
  std::thread second_thread(measure_worker, publisher_metrics, observe_publisher_metric_mix,
                            commands_per_worker + 1U, std::ref(second));
  start_barrier.arrive_and_wait();
  first_thread.join();
  second_thread.join();
  const auto elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - wall_start)
          .count());
  const auto writer_snapshot = writer_metrics->snapshot();
  const auto publisher_snapshot = publisher_metrics->snapshot();
  const auto expected_writer_commands = checked_add(warmup_commands, first.commands);
  const auto expected_publisher_commands = checked_add(warmup_commands, second.commands);
  if (first.samples.values.size() != options.iterations ||
      second.samples.values.size() != options.iterations ||
      writer_snapshot.commands != expected_writer_commands ||
      publisher_snapshot.replayed_records != expected_publisher_commands ||
      publisher_snapshot.publish_latency.count != expected_publisher_commands) {
    report_error("metrics", "validation", "parallel_metric_count_mismatch");
    return false;
  }
  auto samples = std::move(first.samples.values);
  samples.insert(samples.end(), second.samples.values.begin(), second.samples.values.end());
  Samples aggregate{std::move(samples)};
  const auto writer_observations =
      checked_multiply(first.commands, kWriterMetricCallsPerCommand);
  const auto publisher_observations =
      checked_multiply(second.commands, kPublisherMetricCallsPerCommand);
  const auto observations = checked_add(writer_observations, publisher_observations);
  const auto observations_rate =
      elapsed_ns == 0
          ? 0.0
          : static_cast<double>(observations) * 1'000'000'000.0 /
                static_cast<double>(elapsed_ns);
  const auto command_pairs = std::min(first.commands, second.commands);
  const auto rate = elapsed_ns == 0
                        ? 0.0
                        : static_cast<double>(command_pairs) * 1'000'000'000.0 /
                              static_cast<double>(elapsed_ns);
  const auto worker_rate = [](const std::uint64_t commands,
                              const std::uint64_t duration_ns) {
    return duration_ns == 0
               ? 0.0
               : static_cast<double>(commands) * 1'000'000'000.0 /
                     static_cast<double>(duration_ns);
  };
  const auto test_case = separate_registries
                             ? "writer_publisher_separate_registries"
                             : "writer_publisher_contended";
  const auto contention_mode = separate_registries
                                   ? "separate_registries_two_workers"
                                   : "shared_registry_two_workers";
  print_timing("metrics", test_case, options, aggregate,
               observations, elapsed_ns, "metrics_return",
               "concurrent_worker_group",
               "contention_mode=" + std::string(contention_mode) +
                   " writer_elapsed_ms=" +
                   std::to_string(first.elapsed_ns / 1'000'000.0) +
                   " publisher_elapsed_ms=" +
                   std::to_string(second.elapsed_ns / 1'000'000.0) +
                   " writer_commands_per_second=" +
                   std::to_string(worker_rate(first.commands, first.elapsed_ns)) +
                   " publisher_commands_per_second=" +
                   std::to_string(worker_rate(second.commands, second.elapsed_ns)) +
                   " writer_commands=" + std::to_string(first.commands) +
                   " publisher_commands=" + std::to_string(second.commands) +
                   " writer_metric_calls_per_command=" +
                   std::to_string(kWriterMetricCallsPerCommand) +
                   " publisher_metric_calls_per_command=" +
                   std::to_string(kPublisherMetricCallsPerCommand) +
                   " writer_metric_names=" + std::string(kWriterMetricNames) +
                   " publisher_metric_names=" + std::string(kPublisherMetricNames) +
                   " writer_observations=" + std::to_string(writer_observations) +
                   " publisher_observations=" + std::to_string(publisher_observations) +
                   " observations=" + std::to_string(observations) +
                   " observations_per_second=" + std::to_string(observations_rate),
               rate);
  return true;
}

bool run_metrics_contended(const PipelineBenchmarkOptions& options) {
  return run_metrics_parallel(options, false);
}

bool run_metrics_separate_registries(const PipelineBenchmarkOptions& options) {
  return run_metrics_parallel(options, true);
}

struct HandoffLane {
  ProducerId producer_id{};
  std::chrono::steady_clock::time_point started_at{};
  CommandIdentity expected{};
  bool in_flight{};
};

struct HandoffState {
  explicit HandoffState(const std::size_t lane_count) : lanes(lane_count) {
    for (std::size_t index = 0; index < lane_count; ++index) {
      lanes[index].producer_id = static_cast<ProducerId>(index + 1U);
      available.push_back(index);
    }
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::vector<HandoffLane> lanes;
  std::deque<std::size_t> available;
  std::vector<std::uint64_t> samples;
  std::optional<std::string> failure;
  std::uint64_t submitted{};
  std::uint64_t completed{};
  std::uint64_t outstanding{};
  bool measured{};
};

void set_handoff_failure(HandoffState& state, std::string message) {
  if (!state.failure.has_value()) {
    state.failure = std::move(message);
  }
  state.condition.notify_all();
}

EngineConfig make_engine_config(const std::filesystem::path& data_directory,
                                const std::size_t group_size,
                                const std::chrono::microseconds group_delay) {
  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.ingress_queue_capacity = 65'536;
  config.runtime.group_commit_max_commands = group_size;
  config.runtime.group_commit_max_delay = group_delay;
  config.runtime.snapshot_interval_commands = std::numeric_limits<std::size_t>::max();
  config.runtime.snapshot_interval = std::chrono::hours(24);
  config.runtime.event_replay_snapshot_interval_commands =
      std::numeric_limits<std::size_t>::max();
  config.runtime.event_replay_snapshot_interval = std::chrono::hours(24);
  return config;
}

Command make_handoff_command(const HandoffLane& lane, const std::uint64_t order_id) {
  Command command;
  // producer_epoch=0 reaches the runtime prepare/admission boundary and is
  // rejected before WAL append.  producer_seq remains valid so the fixture
  // does not exercise producer-sequence-gap handling instead.
  command.identity = CommandIdentity{lane.producer_id, 0, 1, 1};
  command.instrument_id = 1;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{1, order_id};
  command.payload = NewOrderPayload{Side::buy, 100, 1};
  return command;
}

bool wait_for_handoff(HandoffState& state, const std::uint64_t submitted) {
  std::unique_lock lock(state.mutex);
  const auto deadline = std::chrono::steady_clock::now() + kPhaseTimeout;
  if (!state.condition.wait_until(lock, deadline, [&state, submitted] {
        return state.failure.has_value() ||
               (state.completed == submitted && state.outstanding == 0);
      })) {
    set_handoff_failure(state, "phase completion timeout");
    return false;
  }
  return !state.failure.has_value() && state.completed == submitted &&
         state.outstanding == 0;
}

bool run_handoff_phase(Engine& engine, HandoffState& state,
                       const std::uint64_t count, const bool measured,
                       std::uint64_t& next_order_id,
                       std::uint64_t& elapsed_ns) {
  {
    std::lock_guard lock(state.mutex);
    state.failure.reset();
    state.samples.clear();
    state.submitted = 0;
    state.completed = 0;
    state.outstanding = 0;
    state.measured = measured;
  }
  const auto start = std::chrono::steady_clock::now();
  std::uint64_t submitted = 0;
  while (submitted < count) {
    std::size_t lane_index = 0;
    Command command;
    {
      std::unique_lock lock(state.mutex);
      const auto deadline = std::chrono::steady_clock::now() + kPhaseTimeout;
      if (!state.condition.wait_until(lock, deadline, [&state] {
            return state.failure.has_value() || !state.available.empty();
          })) {
        set_handoff_failure(state, "producer lane availability timeout");
        break;
      }
      if (state.failure.has_value()) {
        break;
      }
      lane_index = state.available.front();
      state.available.pop_front();
      auto& lane = state.lanes[lane_index];
      lane.started_at = std::chrono::steady_clock::now();
      command = make_handoff_command(lane, next_order_id++);
      lane.expected = command.identity;
      lane.in_flight = true;
      ++state.outstanding;
    }
    const auto submit_result = engine.submit(
        std::move(command), [&state, lane_index](CommandResult result) {
          const auto completed_at = std::chrono::steady_clock::now();
          std::lock_guard lock(state.mutex);
          if (lane_index >= state.lanes.size() || !state.lanes[lane_index].in_flight) {
            set_handoff_failure(state, "duplicate or unknown completion callback");
          } else {
            auto& lane = state.lanes[lane_index];
            if (lane.expected != result.identity) {
              set_handoff_failure(state, "completion identity mismatch");
            }
            if (result.command_status != CommandStatus::admission_error ||
                result.error_code != ErrorCode::invalid_command) {
              set_handoff_failure(state, "unexpected admission result");
            }
            if (state.measured) {
              state.samples.push_back(static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      completed_at - lane.started_at)
                      .count()));
            }
            lane.in_flight = false;
            state.available.push_back(lane_index);
            if (state.outstanding > 0) {
              --state.outstanding;
            }
            ++state.completed;
          }
          state.condition.notify_all();
        });
    if (!submit_result.queued) {
      std::lock_guard lock(state.mutex);
      auto& lane = state.lanes[lane_index];
      lane.in_flight = false;
      state.available.push_back(lane_index);
      if (state.outstanding > 0) {
        --state.outstanding;
      }
      set_handoff_failure(
          state, submit_result.error.has_value()
                    ? error_message(*submit_result.error)
                    : std::string("submit returned unqueued without error"));
      break;
    }
    ++submitted;
  }
  if (!wait_for_handoff(state, submitted)) {
    return false;
  }
  elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
  state.submitted = submitted;
  return submitted == count;
}

bool run_runtime_handoff(const PipelineBenchmarkOptions& options,
                         const std::filesystem::path& root) {
  const auto data_directory = root / "runtime_handoff";
  if (!prepare_new_directory(data_directory)) {
    report_error("runtime_handoff", "setup", "data_directory_unavailable");
    return false;
  }
  AcknowledgingSink sink;
  NullMetricsSink metrics;
  auto opened = Engine::open(
      make_engine_config(data_directory, options.engine_group_size,
                         options.engine_group_delay),
      sink, metrics);
  if (std::holds_alternative<Error>(opened)) {
    report_error("runtime_handoff", "setup", "engine_open_failed",
                 error_message(std::get<Error>(opened)));
    return false;
  }
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));
  HandoffState state(options.producer_lanes);
  std::uint64_t next_order_id = 1;
  const auto command_count = checked_multiply(options.iterations, options.batch_size);
  const auto warmup_count = checked_multiply(options.warmup, options.batch_size);
  std::uint64_t elapsed_ns = 0;
  if (!run_handoff_phase(*engine, state, warmup_count, false, next_order_id, elapsed_ns) ||
      !run_handoff_phase(*engine, state, command_count, true, next_order_id, elapsed_ns)) {
    (void)engine->stop();
    std::string detail;
    {
      std::lock_guard lock(state.mutex);
      detail = state.failure.value_or("handoff phase failed");
    }
    report_error("runtime_handoff", "measured", "handoff_failed", detail);
    return false;
  }
  const auto stop_status = engine->stop();
  if (std::holds_alternative<Error>(stop_status)) {
    report_error("runtime_handoff", "shutdown", "engine_stop_failed",
                 error_message(std::get<Error>(stop_status)));
    return false;
  }
  if (state.completed != command_count || state.samples.size() != command_count) {
    report_error("runtime_handoff", "validation", "completion_count_mismatch");
    return false;
  }
  Samples samples{std::move(state.samples)};
  const auto rate = elapsed_ns == 0
                        ? 0.0
                        : static_cast<double>(command_count) * 1'000'000'000.0 /
                              static_cast<double>(elapsed_ns);
  print_timing(
      "runtime_handoff", "admission_error_completion", options, samples, command_count,
      elapsed_ns, "admission_error_callback", "command_callback",
      "queued=" + std::to_string(command_count) +
          " callbacks=" + std::to_string(state.completed) +
          " expected_error=invalid_command producer_lanes=" +
          std::to_string(options.producer_lanes) +
          " engine_group_size=" + std::to_string(options.engine_group_size) +
          " engine_group_delay_us=" +
          std::to_string(options.engine_group_delay.count()),
      rate);
  return true;
}

class CountingPublisherSink final : public EventSink {
 public:
  Result<std::monostate> publish(const ShardId, const EngineSeq engine_seq,
                                 const std::span<const Event> events,
                                 const std::stop_token) override {
    const auto previous = last_sequence_.exchange(engine_seq, std::memory_order_acq_rel);
    if (previous != 0 && (previous == std::numeric_limits<EngineSeq>::max() ||
                          engine_seq != previous + 1U)) {
      failed_.store(true, std::memory_order_release);
    }
    published_calls_.fetch_add(1, std::memory_order_relaxed);
    published_events_.fetch_add(events.size(), std::memory_order_relaxed);
    return std::monostate{};
  }

  [[nodiscard]] bool failed() const noexcept {
    return failed_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t calls() const noexcept {
    return published_calls_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t events() const noexcept {
    return published_events_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<EngineSeq> last_sequence_{};
  std::atomic<std::uint64_t> published_calls_{};
  std::atomic<std::uint64_t> published_events_{};
  std::atomic<bool> failed_{false};
};

struct PublisherFixture {
  std::filesystem::path directory;
  std::unique_ptr<storage::Wal> wal;
  domain::ShardState live_state;
  storage::WalPosition head;
  std::uint64_t commands{};
};

PublisherFixture build_publisher_fixture(const std::filesystem::path& directory,
                                         const std::uint64_t command_count) {
  if (!prepare_new_directory(directory)) {
    throw std::runtime_error("publisher data directory is unavailable");
  }
  auto wal_result = storage::Wal::open(directory / "wal", 1, kWalSegmentSize);
  if (std::holds_alternative<Error>(wal_result)) {
    throw std::runtime_error(error_message(std::get<Error>(wal_result)));
  }
  auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(wal_result));
  domain::StateMachine live_machine(make_genesis_state());
  std::vector<domain::CommittedCommand> batch;
  batch.reserve(256);
  for (std::uint64_t sequence = 1; sequence <= command_count; ++sequence) {
    batch.push_back(make_state_command(
        sequence, sequence, 1, sequence % 2U == 1U ? Side::sell : Side::buy, 100,
        sequence));
    if (batch.size() == batch.capacity() || sequence == command_count) {
      auto appended = wal->append_batch(batch);
      if (std::holds_alternative<Error>(appended)) {
        throw std::runtime_error(error_message(std::get<Error>(appended)));
      }
      for (const auto& command : batch) {
        (void)apply_or_throw(live_machine, command);
      }
      batch.clear();
    }
  }
  if (const auto status = wal->sync(); std::holds_alternative<Error>(status)) {
    throw std::runtime_error(error_message(std::get<Error>(status)));
  }
  auto replayed = wal->replay();
  if (std::holds_alternative<Error>(replayed)) {
    throw std::runtime_error(error_message(std::get<Error>(replayed)));
  }
  if (std::get<std::vector<domain::CommittedCommand>>(replayed).size() != command_count) {
    throw std::runtime_error("publisher WAL replay count mismatch");
  }
  if (std::holds_alternative<Error>(domain::validate_state(live_machine.state()))) {
    throw std::runtime_error("publisher live fixture invariant validation failed");
  }
  PublisherFixture fixture{directory, std::move(wal), std::move(live_machine.state()),
                           {}, command_count};
  fixture.head = fixture.wal->durable_position();
  if (fixture.head.engine_seq != command_count) {
    throw std::runtime_error("publisher WAL head mismatch");
  }
  return fixture;
}

bool wait_for_publisher(runtime::EventPublisher& publisher, const EngineSeq head) {
  const auto deadline = std::chrono::steady_clock::now() + kPhaseTimeout;
  while (publisher.confirmed_cursor() < head && !publisher.failed()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return !publisher.failed() && publisher.confirmed_cursor() == head;
}

struct PublisherRunResult {
  std::uint64_t elapsed_ns{};
  std::uint64_t wal_bytes{};
};

PublisherRunResult run_publisher_instance(const std::filesystem::path& directory,
                                          const std::uint64_t command_count,
                                          CountingPublisherSink& sink) {
  auto fixture = build_publisher_fixture(directory, command_count);
  auto snapshots_result = storage::SnapshotStore::open(directory / "event-replay", 1);
  if (std::holds_alternative<Error>(snapshots_result)) {
    throw std::runtime_error(error_message(std::get<Error>(snapshots_result)));
  }
  auto snapshots = std::get<storage::SnapshotStore>(std::move(snapshots_result));
  NullMetricsSink metrics_downstream;
  runtime::MetricsRegistry metrics(metrics_downstream);
  auto publisher_result = runtime::EventPublisher::open(
      clone_state(fixture.live_state), *fixture.wal, std::move(snapshots), sink, metrics,
      std::numeric_limits<std::size_t>::max(), std::chrono::hours(24));
  if (std::holds_alternative<Error>(publisher_result)) {
    throw std::runtime_error(error_message(std::get<Error>(publisher_result)));
  }
  auto publisher = std::get<std::unique_ptr<runtime::EventPublisher>>(
      std::move(publisher_result));
  const auto start = std::chrono::steady_clock::now();
  publisher->start();
  publisher->notify_publishable(fixture.head);
  if (!wait_for_publisher(*publisher, fixture.head.engine_seq)) {
    (void)publisher->stop();
    throw std::runtime_error("publisher did not drain durable head");
  }
  if (const auto status = publisher->stop(); std::holds_alternative<Error>(status)) {
    throw std::runtime_error(error_message(std::get<Error>(status)));
  }
  const auto elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
  publisher.reset();

  auto reopen_snapshots_result = storage::SnapshotStore::open(directory / "event-replay", 1);
  if (std::holds_alternative<Error>(reopen_snapshots_result)) {
    throw std::runtime_error(error_message(std::get<Error>(reopen_snapshots_result)));
  }
  auto reopen_snapshots =
      std::get<storage::SnapshotStore>(std::move(reopen_snapshots_result));
  auto reopened_result = runtime::EventPublisher::open(
      clone_state(fixture.live_state), *fixture.wal, std::move(reopen_snapshots), sink, metrics,
      std::numeric_limits<std::size_t>::max(), std::chrono::hours(24));
  if (std::holds_alternative<Error>(reopened_result)) {
    throw std::runtime_error(error_message(std::get<Error>(reopened_result)));
  }
  auto reopened = std::get<std::unique_ptr<runtime::EventPublisher>>(
      std::move(reopened_result));
  if (reopened->confirmed_cursor() != fixture.head.engine_seq || sink.failed() ||
      sink.calls() != command_count) {
    (void)reopened->stop();
    throw std::runtime_error("publisher cursor or sink validation failed");
  }
  (void)reopened->stop();
  return PublisherRunResult{elapsed_ns, fixture.wal->size_bytes()};
}

bool run_publisher_drain(const PipelineBenchmarkOptions& options,
                         const std::filesystem::path& root) {
  const auto measured_commands = checked_multiply(options.iterations, options.batch_size);
  const auto warmup_commands = checked_multiply(options.warmup, options.batch_size);
  if (measured_commands == 0) {
    report_error("publisher_drain", "setup", "command_count_must_be_positive");
    return false;
  }
  try {
    if (warmup_commands != 0) {
      CountingPublisherSink warmup_sink;
      (void)run_publisher_instance(root / "publisher_warmup", warmup_commands, warmup_sink);
    }
    CountingPublisherSink measured_sink;
    const auto measured = run_publisher_instance(
        root / "publisher_measured", measured_commands, measured_sink);
    Samples samples;
    samples.values.push_back(measured.elapsed_ns);
    const auto rate = measured.elapsed_ns == 0
                          ? 0.0
                          : static_cast<double>(measured_commands) * 1'000'000'000.0 /
                                static_cast<double>(measured.elapsed_ns);
    print_timing(
        "publisher_drain", "durable_cursor", options, samples, measured_commands,
        measured.elapsed_ns, "publisher_durable_cursor", "full_backlog_drain",
        "wal_commands=" + std::to_string(measured_commands) +
            " wal_bytes=" + std::to_string(measured.wal_bytes) +
            " sink_calls=" + std::to_string(measured_sink.calls()) +
            " sink_events=" + std::to_string(measured_sink.events()) +
            " cursor_head=" + std::to_string(measured_commands) +
            " durable_cursor_verified=true",
        rate);
    return true;
  } catch (const std::exception& error) {
    report_error("publisher_drain", "measured", "exception", error.what());
    return false;
  }
}

}  // namespace

std::optional<PipelineStage> parse_pipeline_stage(const std::string_view value) {
  if (value == "all") {
    return PipelineStage::all;
  }
  if (value == "state_machine") {
    return PipelineStage::state_machine;
  }
  if (value == "invariant_validation") {
    return PipelineStage::invariant_validation;
  }
  if (value == "metrics") {
    return PipelineStage::metrics;
  }
  if (value == "runtime_handoff") {
    return PipelineStage::runtime_handoff;
  }
  if (value == "publisher_drain") {
    return PipelineStage::publisher_drain;
  }
  return std::nullopt;
}

std::string_view pipeline_stage_name(const PipelineStage stage) noexcept {
  switch (stage) {
    case PipelineStage::all:
      return "all";
    case PipelineStage::state_machine:
      return "state_machine";
    case PipelineStage::invariant_validation:
      return "invariant_validation";
    case PipelineStage::metrics:
      return "metrics";
    case PipelineStage::runtime_handoff:
      return "runtime_handoff";
    case PipelineStage::publisher_drain:
      return "publisher_drain";
  }
  return "unknown";
}

bool run_pipeline_ceiling(const PipelineBenchmarkOptions& options,
                          const PipelineStage stage) {
  bool owned_root = false;
  const auto root = prepare_root(options.data_directory, owned_root);
  if (!root.has_value()) {
    report_error("all", "setup", "data_directory_unavailable");
    return false;
  }
  const auto cleanup = [&] {
    if (owned_root) {
      std::error_code ignored;
      std::filesystem::remove_all(*root, ignored);
    }
  };
  try {
    const auto run_stage = [&](const PipelineStage selected) {
      switch (selected) {
        case PipelineStage::state_machine:
          return run_state_machine(options);
        case PipelineStage::invariant_validation:
          return run_invariant_validation(options);
        case PipelineStage::metrics:
          return run_metrics_single(options) &&
                 run_metrics_separate_registries(options) &&
                 run_metrics_contended(options);
        case PipelineStage::runtime_handoff:
          return run_runtime_handoff(options, *root);
        case PipelineStage::publisher_drain:
          return run_publisher_drain(options, *root);
        case PipelineStage::all:
          return false;
      }
      return false;
    };
    if (stage == PipelineStage::all) {
      for (const auto selected : {PipelineStage::state_machine,
                                  PipelineStage::invariant_validation,
                                  PipelineStage::metrics,
                                  PipelineStage::runtime_handoff,
                                  PipelineStage::publisher_drain}) {
        if (!run_stage(selected)) {
          cleanup();
          return false;
        }
      }
    } else if (!run_stage(stage)) {
      cleanup();
      return false;
    }
    cleanup();
    return true;
  } catch (const std::exception& error) {
    report_error(pipeline_stage_name(stage), "setup", "exception", error.what());
    cleanup();
    return false;
  }
}

}  // namespace order_books::benchmark
