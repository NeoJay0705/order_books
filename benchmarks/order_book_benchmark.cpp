#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
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
#include <sys/resource.h>
#include <unistd.h>

#include "domain/order_book.hpp"
#include "domain/invariant_checker.hpp"
#include "domain/state_machine.hpp"
#include "order_books/engine.hpp"
#include "engine_tail_telemetry.hpp"
#include "engine_writer_profile_benchmark.hpp"
#include "pipeline_ceiling_benchmark.hpp"
#include "wal_latency_sampler.hpp"
#include "persistence/snapshot_store.hpp"
#include "persistence/wal.hpp"

namespace {

using namespace order_books;

constexpr std::uint64_t kIterations = 2'000;
constexpr std::uint64_t kWarmup = 100;
constexpr std::size_t kDurableProducerLanes = 1'024;
constexpr std::size_t kDurableIngressQueueCapacity = 65'536;
constexpr std::size_t kPipelineProducerLanes = 1'024;
constexpr std::size_t kPipelineIngressQueueCapacity = 65'536;
constexpr auto kDurablePhaseTimeout = std::chrono::seconds(60);
constexpr std::size_t kWalCeilingSegmentSize = 256U * 1024U * 1024U;

enum class WorkloadSelection {
  all,
  engine_durable_single_instrument,
  wal_write_ceiling,
  engine_pipeline_ceiling,
  engine_writer_hot_path_profile,
};

enum class WalSyncMode { none, per_group };
enum class WalPhaseProfileMode { off, on };
enum class WriterPhaseProfileMode { off, on };
enum class WalRotationDiagnosticMode { control, trigger };

struct BenchmarkOptions {
  std::uint64_t iterations{kIterations};
  std::uint64_t warmup{kWarmup};
  WorkloadSelection workload{WorkloadSelection::all};
  std::uint64_t wal_group_size{256};
  std::size_t wal_segment_size_bytes{kWalCeilingSegmentSize};
  std::uint64_t wal_no_rotation_epoch_commands{};
  WalSyncMode wal_sync_mode{WalSyncMode::per_group};
  WalPhaseProfileMode wal_phase_profile{WalPhaseProfileMode::off};
  std::optional<WalRotationDiagnosticMode> wal_rotation_diagnostic;
  WriterPhaseProfileMode writer_phase_profile{WriterPhaseProfileMode::off};
  bool writer_apply_subprofile{};
  std::size_t engine_group_size{256};
  std::chrono::microseconds engine_group_delay{200};
  std::size_t engine_producer_lanes{kDurableProducerLanes};
  std::size_t wal_prepare_workers{1};
  std::size_t wal_parallel_prepare_min_commands{4096};
  std::uint64_t writer_profile_sample_every{1};
  benchmark::PipelineStage pipeline_stage{benchmark::PipelineStage::all};
  benchmark::PipelineCommandScenario pipeline_command_scenario{
      benchmark::PipelineCommandScenario::new_crossing_pair};
  std::size_t pipeline_batch_size{256};
  std::size_t pipeline_active_orders{};
  std::size_t pipeline_producer_lanes{kPipelineProducerLanes};
  std::size_t publisher_cursor_persist_max_commands{256};
  std::chrono::microseconds publisher_cursor_persist_max_delay{1000};
  bool pipeline_options_set{};
  bool pipeline_command_scenario_parse_error{};
  bool engine_producer_lanes_parse_error{};
  bool wal_prepare_workers_parse_error{};
  bool wal_prepare_min_commands_parse_error{};
  bool wal_prepare_options_set{};
  bool wal_segment_size_parse_error{};
  bool wal_segment_size_option_set{};
  bool wal_phase_profile_parse_error{};
  bool wal_phase_profile_option_set{};
  bool wal_rotation_diagnostic_parse_error{};
  bool wal_rotation_diagnostic_option_set{};
  bool wal_no_rotation_epoch_parse_error{};
  bool wal_no_rotation_epoch_option_set{};
  bool wal_measurement_marker_parse_error{};
  bool wal_measurement_marker_option_set{};
  bool writer_phase_profile_parse_error{};
  bool writer_phase_profile_option_set{};
  bool writer_apply_subprofile_parse_error{};
  bool writer_apply_subprofile_option_set{};
  bool writer_profile_sample_parse_error{};
  bool writer_profile_sample_option_set{};
  bool engine_tail_telemetry_parse_error{};
  bool engine_tail_telemetry_option_set{};
  bool engine_tail_state_sampling{true};
  bool engine_tail_state_sampling_parse_error{};
  bool engine_tail_state_sampling_option_set{};
  std::optional<std::filesystem::path> data_directory;
  std::optional<std::filesystem::path> wal_measurement_marker;
  std::optional<std::filesystem::path> engine_tail_telemetry_output;
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
  if (value == "wal_write_ceiling") {
    return WorkloadSelection::wal_write_ceiling;
  }
  if (value == "engine_pipeline_ceiling") {
    return WorkloadSelection::engine_pipeline_ceiling;
  }
  if (value == "engine_writer_hot_path_profile") {
    return WorkloadSelection::engine_writer_hot_path_profile;
  }
  return std::nullopt;
}

std::string_view workload_name(const WorkloadSelection workload) {
  switch (workload) {
    case WorkloadSelection::all:
      return "all";
    case WorkloadSelection::engine_durable_single_instrument:
      return "engine_durable_single_instrument";
    case WorkloadSelection::wal_write_ceiling:
      return "wal_write_ceiling";
    case WorkloadSelection::engine_pipeline_ceiling:
      return "engine_pipeline_ceiling";
    case WorkloadSelection::engine_writer_hot_path_profile:
      return "engine_writer_hot_path_profile";
  }
  return "unknown";
}

std::optional<WalSyncMode> parse_wal_sync_mode(const std::string_view value) {
  if (value == "none") {
    return WalSyncMode::none;
  }
  if (value == "per_group") {
    return WalSyncMode::per_group;
  }
  return std::nullopt;
}

std::optional<WalPhaseProfileMode> parse_wal_phase_profile(
    const std::string_view value) {
  if (value == "off") {
    return WalPhaseProfileMode::off;
  }
  if (value == "on") {
    return WalPhaseProfileMode::on;
  }
  return std::nullopt;
}

std::optional<WalRotationDiagnosticMode> parse_wal_rotation_diagnostic(
    const std::string_view value) {
  if (value == "control") {
    return WalRotationDiagnosticMode::control;
  }
  if (value == "trigger") {
    return WalRotationDiagnosticMode::trigger;
  }
  return std::nullopt;
}

std::optional<WriterPhaseProfileMode> parse_writer_phase_profile(
    const std::string_view value) {
  if (value == "off") {
    return WriterPhaseProfileMode::off;
  }
  if (value == "on") {
    return WriterPhaseProfileMode::on;
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
    } else if (const auto group_size = parse_positive_option(argument, "--wal-group-size=")) {
      options.wal_group_size = *group_size;
    } else if (argument.starts_with("--wal-segment-size-bytes=")) {
      options.wal_segment_size_option_set = true;
      const auto segment_size = parse_positive_option(
          argument, "--wal-segment-size-bytes=");
      if (!segment_size.has_value() ||
          *segment_size > std::numeric_limits<std::size_t>::max()) {
        options.wal_segment_size_parse_error = true;
      } else {
        options.wal_segment_size_bytes = static_cast<std::size_t>(*segment_size);
      }
    } else if (argument.starts_with("--wal-no-rotation-epoch-commands=")) {
      options.wal_no_rotation_epoch_option_set = true;
      const auto epoch_commands = parse_positive_option(
          argument, "--wal-no-rotation-epoch-commands=");
      if (!epoch_commands.has_value()) {
        options.wal_no_rotation_epoch_parse_error = true;
      } else {
        options.wal_no_rotation_epoch_commands = *epoch_commands;
      }
    } else if (argument.starts_with("--wal-measurement-marker=")) {
      options.wal_measurement_marker_option_set = true;
      const auto path = argument.substr(
          std::string_view("--wal-measurement-marker=").size());
      if (path.empty()) {
        options.wal_measurement_marker_parse_error = true;
      } else {
        options.wal_measurement_marker = std::filesystem::path(path);
      }
    } else if (argument.starts_with("--wal-rotation-diagnostic=")) {
      options.wal_rotation_diagnostic_option_set = true;
      const auto diagnostic = parse_wal_rotation_diagnostic(
          argument.substr(std::string_view("--wal-rotation-diagnostic=").size()));
      if (!diagnostic.has_value()) {
        options.wal_rotation_diagnostic_parse_error = true;
      } else {
        options.wal_rotation_diagnostic = *diagnostic;
      }
    } else if (const auto group_size = parse_positive_option(argument, "--engine-group-size=")) {
      options.engine_group_size = static_cast<std::size_t>(*group_size);
    } else if (const auto delay =
                   parse_positive_option(argument, "--engine-group-delay-us=")) {
      options.engine_group_delay = std::chrono::microseconds(*delay);
    } else if (argument.starts_with("--engine-producer-lanes=")) {
      const auto producer_lanes = parse_positive_option(argument, "--engine-producer-lanes=");
      if (!producer_lanes.has_value()) {
        options.engine_producer_lanes_parse_error = true;
      } else if (*producer_lanes > std::numeric_limits<std::size_t>::max()) {
        options.engine_producer_lanes_parse_error = true;
      } else {
        options.engine_producer_lanes = static_cast<std::size_t>(*producer_lanes);
      }
    } else if (argument.starts_with("--wal-prepare-workers=")) {
      options.wal_prepare_options_set = true;
      const auto workers = parse_positive_option(argument, "--wal-prepare-workers=");
      if (!workers.has_value() || *workers > std::numeric_limits<std::size_t>::max()) {
        options.wal_prepare_workers_parse_error = true;
      } else {
        options.wal_prepare_workers = static_cast<std::size_t>(*workers);
      }
    } else if (argument.starts_with("--wal-parallel-prepare-min-commands=")) {
      options.wal_prepare_options_set = true;
      const auto minimum = parse_positive_option(
          argument, "--wal-parallel-prepare-min-commands=");
      if (!minimum.has_value() || *minimum > std::numeric_limits<std::size_t>::max()) {
        options.wal_prepare_min_commands_parse_error = true;
      } else {
        options.wal_parallel_prepare_min_commands = static_cast<std::size_t>(*minimum);
      }
    } else if (const auto batch_size =
                   parse_positive_option(argument, "--pipeline-batch-size=")) {
      options.pipeline_batch_size = static_cast<std::size_t>(*batch_size);
      options.pipeline_options_set = true;
    } else if (const auto active_orders =
                   parse_positive_option(argument, "--pipeline-active-orders=")) {
      options.pipeline_active_orders = static_cast<std::size_t>(*active_orders);
      options.pipeline_options_set = true;
    } else if (const auto producer_lanes =
                   parse_positive_option(argument, "--pipeline-producer-lanes=")) {
      options.pipeline_producer_lanes = static_cast<std::size_t>(*producer_lanes);
      options.pipeline_options_set = true;
    } else if (const auto max_commands = parse_positive_option(
                   argument, "--publisher-cursor-persist-max-commands=")) {
      options.publisher_cursor_persist_max_commands = static_cast<std::size_t>(*max_commands);
      options.pipeline_options_set = true;
    } else if (const auto max_delay = parse_positive_option(
                   argument, "--publisher-cursor-persist-max-delay-us=")) {
      options.publisher_cursor_persist_max_delay = std::chrono::microseconds(*max_delay);
      options.pipeline_options_set = true;
    } else if (argument.starts_with("--pipeline-stage=")) {
      const auto stage = benchmark::parse_pipeline_stage(
          argument.substr(std::string_view("--pipeline-stage=").size()));
      if (!stage.has_value()) {
        return std::nullopt;
      }
      options.pipeline_stage = *stage;
      options.pipeline_options_set = true;
    } else if (argument.starts_with("--pipeline-command-scenario=")) {
      const auto scenario = benchmark::parse_pipeline_command_scenario(
          argument.substr(std::string_view("--pipeline-command-scenario=").size()));
      if (!scenario.has_value()) {
        options.pipeline_command_scenario_parse_error = true;
      } else {
        options.pipeline_command_scenario = *scenario;
        options.pipeline_options_set = true;
      }
    } else if (argument.starts_with("--workload=")) {
      const auto workload = parse_workload(argument.substr(std::string_view("--workload=").size()));
      if (!workload.has_value()) {
        return std::nullopt;
      }
      options.workload = *workload;
    } else if (argument.starts_with("--wal-sync=")) {
      const auto sync_mode = parse_wal_sync_mode(
          argument.substr(std::string_view("--wal-sync=").size()));
      if (!sync_mode.has_value()) {
        return std::nullopt;
      }
      options.wal_sync_mode = *sync_mode;
    } else if (argument.starts_with("--wal-phase-profile=")) {
      options.wal_phase_profile_option_set = true;
      const auto profile = parse_wal_phase_profile(
          argument.substr(std::string_view("--wal-phase-profile=").size()));
      if (!profile.has_value()) {
        options.wal_phase_profile_parse_error = true;
      } else {
        options.wal_phase_profile = *profile;
      }
    } else if (argument.starts_with("--writer-phase-profile=")) {
      options.writer_phase_profile_option_set = true;
      const auto profile = parse_writer_phase_profile(
          argument.substr(std::string_view("--writer-phase-profile=").size()));
      if (!profile.has_value()) {
        options.writer_phase_profile_parse_error = true;
      } else {
        options.writer_phase_profile = *profile;
      }
    } else if (argument.starts_with("--writer-apply-subprofile=")) {
      options.writer_apply_subprofile_option_set = true;
      const auto profile = parse_writer_phase_profile(
          argument.substr(std::string_view("--writer-apply-subprofile=").size()));
      if (!profile.has_value()) {
        options.writer_apply_subprofile_parse_error = true;
      } else {
        options.writer_apply_subprofile = *profile == WriterPhaseProfileMode::on;
      }
    } else if (argument.starts_with("--writer-profile-sample-every=")) {
      options.writer_profile_sample_option_set = true;
      const auto sample_every = parse_positive_option(
          argument, "--writer-profile-sample-every=");
      if (!sample_every.has_value()) {
        options.writer_profile_sample_parse_error = true;
      } else {
        options.writer_profile_sample_every = *sample_every;
      }
    } else if (argument.starts_with("--data-dir=")) {
      const auto path = argument.substr(std::string_view("--data-dir=").size());
      if (path.empty()) {
        return std::nullopt;
      }
      options.data_directory = std::filesystem::path(path);
    } else if (argument.starts_with("--engine-tail-telemetry-output=")) {
      options.engine_tail_telemetry_option_set = true;
      const auto path = argument.substr(
          std::string_view("--engine-tail-telemetry-output=").size());
      if (path.empty()) {
        options.engine_tail_telemetry_parse_error = true;
      } else {
        options.engine_tail_telemetry_output = std::filesystem::path(path);
      }
    } else if (argument.starts_with("--engine-tail-state-sampling=")) {
      options.engine_tail_state_sampling_option_set = true;
      const auto value = argument.substr(
          std::string_view("--engine-tail-state-sampling=").size());
      if (value == "on") {
        options.engine_tail_state_sampling = true;
      } else if (value == "off") {
        options.engine_tail_state_sampling = false;
      } else {
        options.engine_tail_state_sampling_parse_error = true;
      }
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

std::string optional_metric_value(
    const std::optional<std::uint64_t>& value) {
  return value.has_value() ? std::to_string(*value) : "na";
}

void report_wal_error(const std::string_view phase, const std::string_view code,
                      const std::string_view detail = {}) {
  std::cerr << "workload=wal_write_ceiling phase=" << phase << " error_code=" << code;
  if (!detail.empty()) {
    std::cerr << " detail=" << detail;
  }
  std::cerr << '\n';
}

bool write_wal_measurement_marker(const std::optional<std::filesystem::path>& path,
                                  const std::string_view marker,
                                  const bool truncate) {
  if (!path.has_value()) {
    return true;
  }
  std::ofstream output(*path, truncate ? std::ios::out | std::ios::trunc
                                       : std::ios::out | std::ios::app);
  if (!output) {
    report_wal_error("measurement", "marker_open_failed", path->string());
    return false;
  }
  output << marker << '\n';
  output.flush();
  if (!output) {
    report_wal_error("measurement", "marker_write_failed", path->string());
    return false;
  }
  return true;
}

std::optional<std::uint64_t> count_wal_segments(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return std::nullopt;
  }
  std::uint64_t count = 0;
  const std::filesystem::directory_iterator end;
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      return std::nullopt;
    }
    std::error_code entry_error;
    if (iterator->is_regular_file(entry_error) && !entry_error &&
        iterator->path().extension() == ".wal") {
      if (count == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
      }
      ++count;
    } else if (entry_error) {
      return std::nullopt;
    }
  }
  return count;
}

struct WalSegmentSnapshot {
  std::uint64_t count{};
  std::uint64_t first_engine_seq{};
  std::uint64_t active_engine_seq{};
  std::uint64_t active_offset{};
};

std::optional<WalSegmentSnapshot> inspect_wal_segments(
    const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return std::nullopt;
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> segments;
  const std::filesystem::directory_iterator end;
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      return std::nullopt;
    }
    std::error_code entry_error;
    const auto& path = iterator->path();
    if (!iterator->is_regular_file(entry_error) || entry_error) {
      if (entry_error) {
        return std::nullopt;
      }
      continue;
    }
    if (path.extension() != ".wal") {
      continue;
    }
    const auto stem = path.stem().string();
    std::size_t consumed = 0;
    std::uint64_t sequence = 0;
    try {
      sequence = std::stoull(stem, &consumed);
    } catch (...) {
      return std::nullopt;
    }
    if (sequence == 0U || consumed != stem.size()) {
      return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, entry_error);
    if (entry_error) {
      return std::nullopt;
    }
    segments.emplace_back(sequence, size);
  }
  if (segments.empty()) {
    return std::nullopt;
  }
  std::sort(segments.begin(), segments.end());
  if (segments.size() > std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  return WalSegmentSnapshot{static_cast<std::uint64_t>(segments.size()),
                            segments.front().first,
                            segments.back().first,
                            segments.back().second};
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

std::optional<std::string_view> prepare_data_directory(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto exists = std::filesystem::exists(path, error);
  if (error) {
    return "data_directory_inspection_failed";
  }
  if (exists) {
    if (!std::filesystem::is_directory(path, error) || error) {
      return "data_directory_not_directory";
    }
    if (!std::filesystem::is_empty(path, error) || error) {
      return "data_directory_not_empty";
    }
    return std::nullopt;
  }
  if (!std::filesystem::create_directories(path, error) && error) {
    return "data_directory_create_failed";
  }
  return std::nullopt;
}

using benchmark_wal::LatencyAggregate;
using benchmark_wal::latency_sample_stride;
using benchmark_wal::observe_latency;

struct WalGroupSamples {
  std::vector<std::uint64_t> append_ns;
  std::vector<std::uint64_t> sync_ns;
  std::vector<std::uint64_t> total_ns;
  std::vector<std::uint64_t> fixture_build_ns;
  std::vector<std::uint64_t> wal_append_call_ns;
  std::vector<std::uint64_t> lock_wait_ns;
  std::vector<std::uint64_t> prepare_ns;
  std::vector<std::uint64_t> prepare_task_ns;
  std::vector<std::uint64_t> plan_copy_ns;
  std::vector<std::uint64_t> rotation_ns;
  std::vector<std::uint64_t> rotation_sync_ns;
  std::vector<std::uint64_t> rotation_header_write_ns;
  std::vector<std::uint64_t> rotation_header_sync_ns;
  std::vector<std::uint64_t> rotation_directory_sync_ns;
  std::vector<std::uint64_t> write_ns;
  std::vector<std::uint64_t> publish_ns;
  LatencyAggregate append;
  LatencyAggregate sync;
  LatencyAggregate total;
  LatencyAggregate fixture_build;
  LatencyAggregate wal_append_call;
  LatencyAggregate lock_wait;
  LatencyAggregate prepare;
  LatencyAggregate prepare_task;
  LatencyAggregate plan_copy;
  LatencyAggregate rotation;
  LatencyAggregate rotation_sync;
  LatencyAggregate rotation_header_write;
  LatencyAggregate rotation_header_sync;
  LatencyAggregate rotation_directory_sync;
  LatencyAggregate write;
  LatencyAggregate publish;
  std::uint64_t sample_stride{1};
  std::uint64_t profiled_groups{};
  std::uint64_t profiled_commands{};
  std::uint64_t profiled_frame_bytes{};
  std::uint64_t profiled_data_write_calls{};
  std::uint64_t profiled_rotations{};
  std::uint64_t parallel_prepare_groups{};
  std::uint64_t prepare_tasks{};
};

struct WalProfileTotals {
  std::uint64_t fixture_build{};
  std::uint64_t wal_append_call{};
  std::uint64_t lock_wait{};
  std::uint64_t prepare{};
  std::uint64_t prepare_task{};
  std::uint64_t plan_copy{};
  std::uint64_t rotation{};
  std::uint64_t rotation_sync{};
  std::uint64_t rotation_header_write{};
  std::uint64_t rotation_header_sync{};
  std::uint64_t rotation_directory_sync{};
  std::uint64_t write{};
  std::uint64_t publish{};
  std::uint64_t sync{};
  std::uint64_t group_total{};
};

bool checked_add(std::uint64_t& target, const std::uint64_t value) {
  if (target > std::numeric_limits<std::uint64_t>::max() - value) {
    return false;
  }
  target += value;
  return true;
}

std::optional<storage::WalPrepareStats> prepare_stats_delta(
    const storage::WalPrepareStats& before,
    const storage::WalPrepareStats& after) {
  if (after.parallel_groups < before.parallel_groups || after.tasks < before.tasks) {
    return std::nullopt;
  }
  return storage::WalPrepareStats{after.parallel_groups - before.parallel_groups,
                                  after.tasks - before.tasks};
}

std::optional<std::uint64_t> checked_multiply(const std::uint64_t lhs,
                                              const std::uint64_t rhs) {
  if (lhs != 0U && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    return std::nullopt;
  }
  return lhs * rhs;
}

std::optional<std::uint64_t> checked_sum(const std::uint64_t lhs,
                                         const std::uint64_t rhs) {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
    return std::nullopt;
  }
  return lhs + rhs;
}

std::optional<WalProfileTotals> profile_totals(const WalGroupSamples& samples) {
  return WalProfileTotals{samples.fixture_build.total_ns,
                          samples.wal_append_call.total_ns,
                          samples.lock_wait.total_ns,
                          samples.prepare.total_ns,
                          samples.prepare_task.total_ns,
                          samples.plan_copy.total_ns,
                          samples.rotation.total_ns,
                          samples.rotation_sync.total_ns,
                          samples.rotation_header_write.total_ns,
                          samples.rotation_header_sync.total_ns,
                          samples.rotation_directory_sync.total_ns,
                          samples.write.total_ns,
                          samples.publish.total_ns,
                          samples.sync.total_ns,
                          samples.total.total_ns};
}

std::uint64_t elapsed_ns(const std::chrono::steady_clock::time_point start,
                         const std::chrono::steady_clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

struct ProcessCounters {
  double user_seconds{};
  double system_seconds{};
  std::uint64_t voluntary_context_switches{};
  std::uint64_t involuntary_context_switches{};
  std::uint64_t syscw{};
  std::uint64_t wchar{};
  std::uint64_t write_bytes{};
  std::uint64_t cancelled_write_bytes{};
  std::uint64_t dirty_bytes{};
  std::uint64_t writeback_bytes{};
  bool rusage_valid{};
  bool io_valid{};
  bool meminfo_valid{};
  bool aggregate_initialized{};
};

ProcessCounters capture_process_counters() {
  ProcessCounters counters;
  struct rusage usage {};
  if (::getrusage(RUSAGE_SELF, &usage) == 0) {
    counters.rusage_valid = true;
    counters.user_seconds = static_cast<double>(usage.ru_utime.tv_sec) +
                            static_cast<double>(usage.ru_utime.tv_usec) / 1'000'000.0;
    counters.system_seconds = static_cast<double>(usage.ru_stime.tv_sec) +
                              static_cast<double>(usage.ru_stime.tv_usec) / 1'000'000.0;
    counters.voluntary_context_switches = usage.ru_nvcsw;
    counters.involuntary_context_switches = usage.ru_nivcsw;
  }
#if defined(__linux__)
  std::ifstream input("/proc/self/io");
  std::string name;
  std::uint64_t value = 0;
  bool has_syscw = false;
  bool has_wchar = false;
  bool has_write_bytes = false;
  bool has_cancelled_write_bytes = false;
  while (input >> name >> value) {
    if (name == "syscw:") {
      counters.syscw = value;
      has_syscw = true;
    } else if (name == "wchar:") {
      counters.wchar = value;
      has_wchar = true;
    } else if (name == "write_bytes:") {
      counters.write_bytes = value;
      has_write_bytes = true;
    } else if (name == "cancelled_write_bytes:") {
      counters.cancelled_write_bytes = value;
      has_cancelled_write_bytes = true;
    }
  }
  counters.io_valid = has_syscw && has_wchar && has_write_bytes && has_cancelled_write_bytes;
  std::ifstream meminfo("/proc/meminfo");
  bool has_dirty = false;
  bool has_writeback = false;
  while (meminfo >> name >> value) {
    std::string unit;
    meminfo >> unit;
    if (name == "Dirty:") {
      counters.dirty_bytes = value * 1024U;
      has_dirty = true;
    } else if (name == "Writeback:") {
      counters.writeback_bytes = value * 1024U;
      has_writeback = true;
    }
  }
  counters.meminfo_valid = has_dirty && has_writeback;
#endif
  return counters;
}

ProcessCounters subtract_process_counters(const ProcessCounters& after,
                                           const ProcessCounters& before) {
  ProcessCounters delta;
  delta.rusage_valid = after.rusage_valid && before.rusage_valid;
  if (delta.rusage_valid) {
    delta.user_seconds = after.user_seconds - before.user_seconds;
    delta.system_seconds = after.system_seconds - before.system_seconds;
  }
  delta.voluntary_context_switches = after.voluntary_context_switches >=
                                             before.voluntary_context_switches
                                         ? after.voluntary_context_switches -
                                               before.voluntary_context_switches
                                         : 0;
  delta.involuntary_context_switches = after.involuntary_context_switches >=
                                               before.involuntary_context_switches
                                           ? after.involuntary_context_switches -
                                                 before.involuntary_context_switches
                                           : 0;
  delta.io_valid = after.io_valid && before.io_valid;
  if (delta.io_valid) {
    delta.syscw = after.syscw >= before.syscw ? after.syscw - before.syscw : 0;
    delta.wchar = after.wchar >= before.wchar ? after.wchar - before.wchar : 0;
    delta.write_bytes = after.write_bytes >= before.write_bytes
                            ? after.write_bytes - before.write_bytes
                            : 0;
    delta.cancelled_write_bytes = after.cancelled_write_bytes >=
                                          before.cancelled_write_bytes
                                      ? after.cancelled_write_bytes -
                                            before.cancelled_write_bytes
                                      : 0;
  }
  delta.dirty_bytes = after.dirty_bytes;
  delta.writeback_bytes = after.writeback_bytes;
  delta.meminfo_valid = after.meminfo_valid && before.meminfo_valid;
  return delta;
}

bool add_process_counters(ProcessCounters& target, const ProcessCounters& value) {
  if (!target.aggregate_initialized) {
    target = value;
    target.aggregate_initialized = true;
    return true;
  }
  target.user_seconds += value.user_seconds;
  target.system_seconds += value.system_seconds;
  if (!checked_add(target.voluntary_context_switches, value.voluntary_context_switches) ||
      !checked_add(target.involuntary_context_switches,
                   value.involuntary_context_switches)) {
    return false;
  }
  target.rusage_valid = target.rusage_valid && value.rusage_valid;
  target.io_valid = target.io_valid && value.io_valid;
  if (value.io_valid) {
    if (!checked_add(target.syscw, value.syscw) ||
        !checked_add(target.wchar, value.wchar) ||
        !checked_add(target.write_bytes, value.write_bytes) ||
        !checked_add(target.cancelled_write_bytes, value.cancelled_write_bytes)) {
      return false;
    }
  }
  target.meminfo_valid = target.meminfo_valid && value.meminfo_valid;
  if (value.meminfo_valid) {
    target.dirty_bytes = value.dirty_bytes;
    target.writeback_bytes = value.writeback_bytes;
  }
  return true;
}

std::string_view wal_sync_mode_name(const WalSyncMode mode) {
  return mode == WalSyncMode::none ? "none" : "per_group";
}

bool run_wal_groups(storage::Wal& wal, const std::uint64_t group_count,
                    const std::uint64_t group_size, const WalSyncMode sync_mode,
                    EngineSeq& next_sequence, std::uint64_t& remaining_commands,
                    const std::string_view phase, WalGroupSamples* samples,
                    const bool phase_profile) {
  for (std::uint64_t group = 0; group < group_count; ++group) {
    if (group_size == 0 || remaining_commands < group_size ||
        group_size > std::numeric_limits<std::size_t>::max() ||
        next_sequence == 0 ||
        group_size > std::numeric_limits<EngineSeq>::max() - next_sequence + 1U) {
      report_wal_error(phase, "sequence_overflow");
      return false;
    }
    const auto group_start = std::chrono::steady_clock::now();
    const auto fixture_start = phase_profile ? group_start
                                             : std::chrono::steady_clock::time_point{};
    std::vector<domain::CommittedCommand> commands;
    commands.reserve(static_cast<std::size_t>(group_size));
    for (std::uint64_t offset = 0; offset < group_size; ++offset) {
      commands.push_back(make_committed(next_sequence));
      --remaining_commands;
      if (remaining_commands > 0) {
        if (next_sequence == std::numeric_limits<EngineSeq>::max()) {
          report_wal_error(phase, "sequence_overflow");
          return false;
        }
        ++next_sequence;
      }
    }
    const auto fixture_end = phase_profile ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
    // The fixture is benchmark bookkeeping.  Keep it in process wall time, but
    // start the component service clock only once the prepared command batch is
    // complete so append/sync group samples describe the WAL path itself.
    const auto service_start = std::chrono::steady_clock::now();
    const auto append_start = service_start;
    storage::WalAppendProfile profile;
    const auto wal_call_start = phase_profile ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
    const auto appended = phase_profile
                               ? wal.append_batch_profiled(commands, profile)
                               : wal.append_batch(commands);
    if (std::holds_alternative<Error>(appended)) {
      report_wal_error(phase, "wal_append_failed",
                       std::get<Error>(appended).message);
      return false;
    }
    const auto append_end = std::chrono::steady_clock::now();
    std::uint64_t sync_duration = 0;
    if (sync_mode == WalSyncMode::per_group) {
      const auto sync_start = std::chrono::steady_clock::now();
      const auto synced = wal.sync();
      const auto sync_end = std::chrono::steady_clock::now();
      sync_duration = elapsed_ns(sync_start, sync_end);
      if (std::holds_alternative<Error>(synced)) {
        report_wal_error(phase, "wal_sync_failed", std::get<Error>(synced).message);
        return false;
      }
    }
    const auto group_end = std::chrono::steady_clock::now();
    if (samples != nullptr) {
      const auto append_duration = elapsed_ns(append_start, append_end);
      const auto sample_index = samples->append.observed;
      if (!observe_latency(samples->append_ns, samples->append, append_duration,
                           sample_index, samples->sample_stride)) {
        report_wal_error(phase, "latency_counter_overflow");
        return false;
      }
      if (sync_mode == WalSyncMode::per_group) {
        if (!observe_latency(samples->sync_ns, samples->sync, sync_duration, sample_index,
                             samples->sample_stride)) {
          report_wal_error(phase, "latency_counter_overflow");
          return false;
        }
      }
      // For append-return runs, append_ns is the service boundary and the
      // duplicate total sample only adds memory pressure.  Keep group-total
      // samples for durable runs and diagnostic/profile rounds where the
      // append/sync composition is part of the report.
      if (sync_mode == WalSyncMode::per_group || phase_profile) {
        if (!observe_latency(samples->total_ns, samples->total,
                             elapsed_ns(service_start, group_end), sample_index,
                             samples->sample_stride)) {
          report_wal_error(phase, "latency_counter_overflow");
          return false;
        }
      }
      if (phase_profile) {
        if (!observe_latency(samples->fixture_build_ns, samples->fixture_build,
                             elapsed_ns(fixture_start, fixture_end), sample_index,
                             samples->sample_stride) ||
            !observe_latency(samples->wal_append_call_ns, samples->wal_append_call,
                             elapsed_ns(wal_call_start, append_end), sample_index,
                             samples->sample_stride) ||
            !observe_latency(samples->lock_wait_ns, samples->lock_wait, profile.lock_wait_ns,
                             sample_index, samples->sample_stride) ||
            !observe_latency(samples->prepare_ns, samples->prepare, profile.prepare_ns,
                             sample_index, samples->sample_stride) ||
            !observe_latency(samples->prepare_task_ns, samples->prepare_task,
                             profile.prepare_task_ns, sample_index, samples->sample_stride) ||
            !observe_latency(samples->plan_copy_ns, samples->plan_copy, profile.plan_copy_ns,
                             sample_index, samples->sample_stride)) {
          report_wal_error(phase, "latency_counter_overflow");
          return false;
        }
        if (profile.rotations != 0U) {
          if (!observe_latency(samples->rotation_ns, samples->rotation, profile.rotation_ns,
                               sample_index, samples->sample_stride)) {
            report_wal_error(phase, "latency_counter_overflow");
            return false;
          }
        }
        if (profile.rotation_sync_ns != 0U) {
          if (!observe_latency(samples->rotation_sync_ns, samples->rotation_sync,
                               profile.rotation_sync_ns, sample_index,
                               samples->sample_stride)) {
            report_wal_error(phase, "latency_counter_overflow");
            return false;
          }
        }
        if (profile.rotation_header_write_ns != 0U) {
          if (!observe_latency(samples->rotation_header_write_ns,
                               samples->rotation_header_write,
                               profile.rotation_header_write_ns, sample_index,
                               samples->sample_stride)) {
            report_wal_error(phase, "latency_counter_overflow");
            return false;
          }
        }
        if (profile.rotation_header_sync_ns != 0U) {
          if (!observe_latency(samples->rotation_header_sync_ns,
                               samples->rotation_header_sync,
                               profile.rotation_header_sync_ns, sample_index,
                               samples->sample_stride)) {
            report_wal_error(phase, "latency_counter_overflow");
            return false;
          }
        }
        if (profile.rotation_directory_sync_ns != 0U) {
          if (!observe_latency(samples->rotation_directory_sync_ns,
                               samples->rotation_directory_sync,
                               profile.rotation_directory_sync_ns, sample_index,
                               samples->sample_stride)) {
            report_wal_error(phase, "latency_counter_overflow");
            return false;
          }
        }
        if (!observe_latency(samples->write_ns, samples->write, profile.write_ns, sample_index,
                             samples->sample_stride) ||
            !observe_latency(samples->publish_ns, samples->publish, profile.publish_ns,
                             sample_index, samples->sample_stride)) {
          report_wal_error(phase, "latency_counter_overflow");
          return false;
        }
        if (!checked_add(samples->profiled_groups, 1U) ||
            !checked_add(samples->profiled_commands,
                         static_cast<std::uint64_t>(commands.size())) ||
            !checked_add(samples->profiled_frame_bytes, profile.frame_bytes) ||
            !checked_add(samples->profiled_data_write_calls,
                         profile.data_write_calls) ||
            !checked_add(samples->profiled_rotations, profile.rotations) ||
            !checked_add(samples->parallel_prepare_groups,
                         profile.parallel_prepare_groups) ||
            !checked_add(samples->prepare_tasks, profile.prepare_tasks)) {
          report_wal_error(phase, "profile_counter_overflow");
          return false;
        }
      }
    }
  }
  return true;
}

bool validate_wal_replay(const std::vector<domain::CommittedCommand>& records,
                         const std::uint64_t expected_count) {
  if (records.size() != expected_count) {
    report_wal_error("replay", "record_count_mismatch");
    return false;
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto expected_sequence = static_cast<EngineSeq>(index + 1U);
    const auto& record = records[index];
    if (record.engine_seq != expected_sequence || record.command.instrument_id != 1 ||
        record.command.command_type != CommandType::new_order ||
        record.command != make_committed(expected_sequence).command) {
      report_wal_error("replay", "record_content_mismatch");
      return false;
    }
  }
  return true;
}

// Append-return ceiling variant used by the component campaign.  A fresh WAL
// epoch is kept below one segment so rotation durability is not mixed into the
// append-return measurement.  The production segment size and WAL semantics
// remain unchanged; only the benchmark's workload partitioning differs.
bool run_wal_write_ceiling_no_rotation(const BenchmarkOptions& options) {
  if (options.wal_group_size == 0U || options.wal_no_rotation_epoch_commands == 0U ||
      options.wal_no_rotation_epoch_commands < options.wal_group_size ||
      options.wal_no_rotation_epoch_commands % options.wal_group_size != 0U ||
      options.wal_phase_profile != WalPhaseProfileMode::off) {
    report_wal_error("setup", "invalid_no_rotation_epoch_options");
    return false;
  }
  if (options.iterations > std::numeric_limits<std::uint64_t>::max() /
                           options.wal_group_size) {
    report_wal_error("setup", "command_count_overflow");
    return false;
  }
  const auto total_commands = options.iterations * options.wal_group_size;
  const auto run_id = std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(static_cast<unsigned long long>(::getpid()));
  const bool owned_data_directory = !options.data_directory.has_value();
  const auto data_directory = options.data_directory.value_or(
      std::filesystem::temp_directory_path() / ("order_books_benchmark_wal-" + run_id));
  if (const auto error = prepare_data_directory(data_directory); error.has_value()) {
    report_wal_error("setup", *error, data_directory.string());
    return false;
  }
  const auto cleanup = [&] {
    if (owned_data_directory) {
      std::error_code ignored;
      std::filesystem::remove_all(data_directory, ignored);
    }
  };

  const auto epoch_commands = options.wal_no_rotation_epoch_commands;
  std::uint64_t remaining_commands = total_commands;
  std::uint64_t measured_commands = 0;
  std::uint64_t measured_groups = 0;
  std::uint64_t measured_elapsed = 0;
  std::uint64_t measured_wal_bytes = 0;
  std::uint64_t planned_wal_bytes = 0;
  std::uint64_t frame_bytes_per_command = 0;
  bool frame_plan_initialized = false;
  std::uint64_t epoch_count = 0;
  std::uint64_t prepare_parallel_groups = 0;
  std::uint64_t prepare_tasks = 0;
  std::uint64_t segment_id_before = 0;
  std::uint64_t segment_id_after = 0;
  std::uint64_t segment_offset_before = 0;
  std::uint64_t segment_offset_after = 0;
  ProcessCounters measured_process_before;
  ProcessCounters measured_process_after;
  ProcessCounters measured_process_counters;
  WalGroupSamples samples;
  samples.sample_stride = latency_sample_stride(options.iterations);
  const auto sample_count = options.iterations / samples.sample_stride +
                            (options.iterations % samples.sample_stride == 0U ? 0U : 1U);
  samples.append_ns.reserve(static_cast<std::size_t>(sample_count));

  while (remaining_commands != 0U) {
    const auto this_epoch_commands = std::min(epoch_commands, remaining_commands);
    if (this_epoch_commands % options.wal_group_size != 0U) {
      report_wal_error("setup", "epoch_command_count_not_batch_aligned");
      cleanup();
      return false;
    }
    if (options.warmup >
        std::numeric_limits<std::uint64_t>::max() / options.wal_group_size) {
      report_wal_error("setup", "warmup_command_count_overflow");
      cleanup();
      return false;
    }
    const auto warmup_commands = options.warmup * options.wal_group_size;
    if (warmup_commands > std::numeric_limits<std::uint64_t>::max() -
                             this_epoch_commands) {
      report_wal_error("setup", "epoch_command_count_overflow");
      cleanup();
      return false;
    }
    const auto epoch_total_commands = warmup_commands + this_epoch_commands;
    const auto epoch_path = data_directory / ("epoch-" + std::to_string(epoch_count + 1U));
    if (const auto error = prepare_data_directory(epoch_path); error.has_value()) {
      report_wal_error("setup", *error, epoch_path.string());
      cleanup();
      return false;
    }
    auto wal_result = storage::Wal::open(
        epoch_path, 1, options.wal_segment_size_bytes,
        storage::WalPrepareOptions{options.wal_prepare_workers,
                                   options.wal_parallel_prepare_min_commands});
    if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(wal_result)) {
      report_wal_error("open", "wal_open_failed",
                       std::get<Error>(wal_result).message);
      cleanup();
      return false;
    }
    auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(wal_result));
    std::uint64_t epoch_remaining = epoch_total_commands;
    EngineSeq next_sequence = 1;
    const auto warmup_start_wal_bytes = wal->size_bytes();
    const auto warmup_start_segments = inspect_wal_segments(epoch_path);
    if (!warmup_start_segments.has_value()) {
      report_wal_error("warmup", "segment_snapshot_failed", epoch_path.string());
      cleanup();
      return false;
    }
    if (!run_wal_groups(*wal, options.warmup, options.wal_group_size, WalSyncMode::none,
                        next_sequence, epoch_remaining, "warmup", nullptr, false)) {
      cleanup();
      return false;
    }
    const auto warmup_end_wal_bytes = wal->size_bytes();
    const auto warmup_end_segments = inspect_wal_segments(epoch_path);
    if (!warmup_end_segments.has_value() ||
        warmup_end_segments->count != warmup_start_segments->count ||
        warmup_end_wal_bytes < warmup_start_wal_bytes || options.warmup == 0U) {
      report_wal_error("warmup", "warmup_rotation_or_snapshot_invalid", epoch_path.string());
      cleanup();
      return false;
    }
    const auto warmup_frame_bytes = warmup_end_wal_bytes - warmup_start_wal_bytes;
    if (warmup_frame_bytes == 0U || warmup_frame_bytes % warmup_commands != 0U) {
      report_wal_error("warmup", "frame_byte_plan_invalid");
      cleanup();
      return false;
    }
    const auto measured_frame_bytes = warmup_frame_bytes / warmup_commands;
    if (!frame_plan_initialized) {
      frame_bytes_per_command = measured_frame_bytes;
      frame_plan_initialized = true;
    } else if (frame_bytes_per_command != measured_frame_bytes) {
      report_wal_error("warmup", "frame_byte_plan_changed");
      cleanup();
      return false;
    }
    if (const auto status = wal->sync(); std::holds_alternative<Error>(status)) {
      report_wal_error("warmup", "final_sync_failed", std::get<Error>(status).message);
      cleanup();
      return false;
    }
    const auto starting_wal_bytes = wal->size_bytes();
    const auto starting_segments = inspect_wal_segments(epoch_path);
    if (!starting_segments.has_value()) {
      report_wal_error("warmup", "segment_snapshot_failed", epoch_path.string());
      cleanup();
      return false;
    }
    segment_id_before = starting_segments->active_engine_seq;
    segment_offset_before = starting_segments->active_offset;
    const auto epoch_prepare_before = wal->prepare_stats();
    const auto epoch_groups = this_epoch_commands / options.wal_group_size;
    const auto planned_epoch_bytes = checked_multiply(this_epoch_commands,
                                                      frame_bytes_per_command);
    if (!planned_epoch_bytes.has_value()) {
      report_wal_error("measured", "frame_byte_plan_overflow");
      cleanup();
      return false;
    }
    if (epoch_count == 0U &&
        !write_wal_measurement_marker(options.wal_measurement_marker,
                                      "measured-window-start", true)) {
      cleanup();
      return false;
    }
    const auto process_counters_before_measured = capture_process_counters();
    if (epoch_count == 0U) {
      measured_process_before = process_counters_before_measured;
    }
    const auto measured_start = std::chrono::steady_clock::now();
    if (!run_wal_groups(*wal, epoch_groups, options.wal_group_size, WalSyncMode::none,
                        next_sequence, epoch_remaining, "measured", &samples, false)) {
      cleanup();
      return false;
    }
    const auto measured_end = std::chrono::steady_clock::now();
    const bool final_epoch = remaining_commands == this_epoch_commands;
    if (final_epoch &&
        !write_wal_measurement_marker(options.wal_measurement_marker,
                                      "measured-window-end", false)) {
      cleanup();
      return false;
    }
    const auto process_counters_after_measured = capture_process_counters();
    if (!add_process_counters(
            measured_process_counters,
            subtract_process_counters(process_counters_after_measured,
                                       process_counters_before_measured))) {
      report_wal_error("measured", "process_counter_overflow");
      cleanup();
      return false;
    }
    measured_process_after = process_counters_after_measured;
    const auto ending_wal_bytes = wal->size_bytes();
    const auto ending_segments = inspect_wal_segments(epoch_path);
    if (!ending_segments.has_value() || ending_segments->count < starting_segments->count ||
        ending_wal_bytes < starting_wal_bytes ||
        ending_segments->count != starting_segments->count ||
        ending_segments->active_engine_seq != starting_segments->active_engine_seq ||
        ending_wal_bytes - starting_wal_bytes != *planned_epoch_bytes) {
      report_wal_error("measured", "no_rotation_observation_failed", epoch_path.string());
      cleanup();
      return false;
    }
    segment_id_after = ending_segments->active_engine_seq;
    segment_offset_after = ending_segments->active_offset;
    if (epoch_remaining != 0U) {
      report_wal_error("measured", "epoch_command_count_mismatch");
      cleanup();
      return false;
    }
    const auto epoch_prepare_after = wal->prepare_stats();
    const auto epoch_prepare_delta = prepare_stats_delta(epoch_prepare_before,
                                                          epoch_prepare_after);
    if (!epoch_prepare_delta.has_value()) {
      report_wal_error("measured", "wal_prepare_stats_regressed");
      cleanup();
      return false;
    }
    if (const auto status = wal->sync(); std::holds_alternative<Error>(status)) {
      report_wal_error("finalize", "final_sync_failed", std::get<Error>(status).message);
      cleanup();
      return false;
    }
    wal.reset();
    auto reopened_result = storage::Wal::open(
        epoch_path, 1, options.wal_segment_size_bytes,
        storage::WalPrepareOptions{options.wal_prepare_workers,
                                   options.wal_parallel_prepare_min_commands});
    if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(reopened_result)) {
      report_wal_error("reopen", "wal_reopen_failed",
                       std::get<Error>(reopened_result).message);
      cleanup();
      return false;
    }
    auto reopened = std::get<std::unique_ptr<storage::Wal>>(std::move(reopened_result));
    auto replayed_result = reopened->replay();
    if (!std::holds_alternative<std::vector<domain::CommittedCommand>>(replayed_result) ||
        !validate_wal_replay(
            std::get<std::vector<domain::CommittedCommand>>(replayed_result),
            epoch_total_commands)) {
      if (std::holds_alternative<Error>(replayed_result)) {
        report_wal_error("replay", "wal_replay_failed",
                         std::get<Error>(replayed_result).message);
      }
      cleanup();
      return false;
    }
    const auto epoch_elapsed = elapsed_ns(measured_start, measured_end);
    if (!checked_add(measured_elapsed, epoch_elapsed) ||
        !checked_add(measured_commands, this_epoch_commands) ||
        !checked_add(measured_groups, epoch_groups) ||
        !checked_add(prepare_parallel_groups, epoch_prepare_delta->parallel_groups) ||
        !checked_add(prepare_tasks, epoch_prepare_delta->tasks)) {
      report_wal_error("report", "measurement_counter_overflow");
      cleanup();
      return false;
    }
    if (!checked_add(measured_wal_bytes, ending_wal_bytes - starting_wal_bytes) ||
        !checked_add(planned_wal_bytes, *planned_epoch_bytes)) {
      report_wal_error("report", "wal_byte_plan_overflow");
      cleanup();
      return false;
    }
    ++epoch_count;
    remaining_commands -= this_epoch_commands;
  }

  if (measured_commands != total_commands || measured_elapsed == 0U ||
      samples.append_ns.empty() || !frame_plan_initialized ||
      measured_wal_bytes != planned_wal_bytes) {
    report_wal_error("report", "no_rotation_measurement_invalid");
    cleanup();
    return false;
  }
  std::sort(samples.append_ns.begin(), samples.append_ns.end());
  const auto service_elapsed = samples.append.total_ns;
  if (service_elapsed == 0U) {
    report_wal_error("report", "service_elapsed_invalid");
    cleanup();
    return false;
  }
  const auto commands_per_second = static_cast<double>(measured_commands) * 1'000'000'000.0 /
                                   static_cast<double>(measured_elapsed);
  const auto service_commands_per_second =
      static_cast<double>(measured_commands) * 1'000'000'000.0 /
      static_cast<double>(service_elapsed);
  const auto wal_mib_per_second = static_cast<double>(measured_wal_bytes) * 1'000'000'000.0 /
                                  static_cast<double>(measured_elapsed) / (1024.0 * 1024.0);
  const auto service_wal_mib_per_second =
      static_cast<double>(measured_wal_bytes) * 1'000'000'000.0 /
      static_cast<double>(service_elapsed) / (1024.0 * 1024.0);
  const auto average_wal_bytes = static_cast<double>(measured_wal_bytes) /
                                static_cast<double>(measured_commands);
  const auto print_percentile = [](const std::vector<std::uint64_t>& values,
                                   const std::size_t numerator,
                                   const std::size_t denominator) {
    return values.empty() ? std::string("na")
                           : std::to_string(percentile_ns(values, numerator, denominator) /
                                            1'000.0);
  };
  std::error_code path_error;
  const auto resolved_path = std::filesystem::weakly_canonical(data_directory, path_error);
  if (path_error) {
    report_wal_error("report", "wal_path_resolution_failed", data_directory.string());
    cleanup();
    return false;
  }
  std::cout << "wal_append_no_rotation sync_mode=none completion_boundary=append_batch_return"
            << " warmup_groups=" << options.warmup << " groups=" << measured_groups
            << " group_size=" << options.wal_group_size
            << " epochs=" << epoch_count
            << " epoch_commands=" << epoch_commands
            << " commands=" << measured_commands
            << " commands_per_second=" << commands_per_second
            << " workload_wall_commands_per_second=" << commands_per_second
            << " service_commands_per_second=" << service_commands_per_second
            << " target_commands_per_second=1000000"
            << " target_attainment_percent=" << commands_per_second / 1'000'000.0 * 100.0
            << " workload_wall_target_attainment_percent="
            << commands_per_second / 1'000'000.0 * 100.0
            << " service_target_attainment_percent="
            << service_commands_per_second / 1'000'000.0 * 100.0
            << " wal_mib_per_second=" << wal_mib_per_second
            << " workload_wall_wal_mib_per_second=" << wal_mib_per_second
            << " service_wal_mib_per_second=" << service_wal_mib_per_second
            << " measured_rusage_valid="
            << (measured_process_counters.rusage_valid ? "true" : "false")
            << " measured_io_valid="
            << (measured_process_counters.io_valid ? "true" : "false")
            << " measured_meminfo_valid="
            << (measured_process_counters.meminfo_valid ? "true" : "false")
            << " measured_user_seconds=" << measured_process_counters.user_seconds
            << " measured_system_seconds=" << measured_process_counters.system_seconds
            << " measured_voluntary_context_switches="
            << measured_process_counters.voluntary_context_switches
            << " measured_involuntary_context_switches="
            << measured_process_counters.involuntary_context_switches
            << " measured_syscw="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.syscw)
                    : std::string("na"))
            << " measured_wchar="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.wchar)
                    : std::string("na"))
            << " measured_write_bytes="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.write_bytes)
                    : std::string("na"))
            << " measured_cancelled_write_bytes="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.cancelled_write_bytes)
                    : std::string("na"))
            << " average_wal_bytes_per_command=" << average_wal_bytes
            << " latency_sample_stride=" << samples.sample_stride
            << " append_latency_sample_count=" << samples.append_ns.size()
            << " append_group_p50_us=" << print_percentile(samples.append_ns, 50, 100)
            << " append_group_p99_us=" << print_percentile(samples.append_ns, 99, 100)
            << " append_group_p99.9_us=" << print_percentile(samples.append_ns, 999, 1000)
            << " append_group_max_us=" << samples.append.max_ns / 1'000.0
            << " sync_samples=0 sync_latency_sample_count=0 sync_p50_us=na sync_p99_us=na sync_p99.9_us=na sync_max_us=na"
            << " group_total_p50_us=na group_total_p99_us=na group_total_p99.9_us=na group_total_max_us=na"
            << " elapsed_ms=" << measured_elapsed / 1'000'000.0
            << " workload_wall_elapsed_ms=" << measured_elapsed / 1'000'000.0
            << " service_elapsed_ms=" << service_elapsed / 1'000'000.0
            << " wal_bytes_delta=" << measured_wal_bytes
            << " wal_bytes=" << measured_wal_bytes
            << " frame_bytes_per_command=" << frame_bytes_per_command
            << " planned_wal_bytes_delta=" << planned_wal_bytes
            << " wal_byte_plan_verified=true"
            << " measured_wal_write_calls="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.syscw)
                    : std::string("na"))
            << " measured_wal_sync_calls=0"
            << " segment_size_bytes=" << options.wal_segment_size_bytes
            << " segment_count_before=1 segment_count_after=1"
            << " segment_id_before=" << segment_id_before
            << " segment_id_after=" << segment_id_after
            << " segment_offset_before=" << segment_offset_before
            << " segment_offset_after=" << segment_offset_after
            << " measured_segment_rotations=0 rotation_scope=no_rotation"
            << " rotation_triggered=false"
            << " measured_dirty_bytes_before="
            << (measured_process_before.meminfo_valid
                    ? std::to_string(measured_process_before.dirty_bytes)
                    : std::string("na"))
            << " measured_dirty_bytes_after="
            << (measured_process_after.meminfo_valid
                    ? std::to_string(measured_process_after.dirty_bytes)
                    : std::string("na"))
            << " measured_writeback_bytes_before="
            << (measured_process_before.meminfo_valid
                    ? std::to_string(measured_process_before.writeback_bytes)
                    : std::string("na"))
            << " measured_writeback_bytes_after="
            << (measured_process_after.meminfo_valid
                    ? std::to_string(measured_process_after.writeback_bytes)
                    : std::string("na"))
            << " actual_parallel_prepare_groups=" << prepare_parallel_groups
            << " actual_prepare_tasks=" << prepare_tasks
            << " wal_path=" << resolved_path << " replay_verified=true phase_profile=off";
  if (options.wal_measurement_marker.has_value()) {
    std::cout << " wal_measurement_marker=" << *options.wal_measurement_marker;
  }
  std::cout << '\n';
  cleanup();
  return true;
}

bool run_wal_write_ceiling(const BenchmarkOptions& options) {
  if (options.wal_rotation_diagnostic.has_value()) {
    // The diagnostic is deliberately kept separate from both the no-rotation
    // ceiling and the durable frontier.  Its measured window contains one
    // profiled target group only.
    const auto mode = *options.wal_rotation_diagnostic;
    const auto mode_name = mode == WalRotationDiagnosticMode::control ? "control" : "trigger";
    const auto run_id = std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count()) +
                        "-" + std::to_string(static_cast<unsigned long long>(::getpid()));
    const bool owned_data_directory = !options.data_directory.has_value();
    const auto data_directory = options.data_directory.value_or(
        std::filesystem::temp_directory_path() /
        (std::string("order_books_benchmark_wal-rotation-") + mode_name + "-" + run_id));
    if (const auto error = prepare_data_directory(data_directory); error.has_value()) {
      report_wal_error("setup", *error, data_directory.string());
      return false;
    }
    const auto cleanup = [&] {
      if (owned_data_directory) {
        std::error_code ignored;
        std::filesystem::remove_all(data_directory, ignored);
      }
    };
    auto wal_result = storage::Wal::open(
        data_directory, 1, options.wal_segment_size_bytes,
        storage::WalPrepareOptions{options.wal_prepare_workers,
                                   options.wal_parallel_prepare_min_commands});
    if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(wal_result)) {
      report_wal_error("open", "wal_open_failed",
                       std::get<Error>(wal_result).message);
      cleanup();
      return false;
    }
    auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(wal_result));
    const auto initial_wal_bytes = wal->size_bytes();
    const auto initial_segments = inspect_wal_segments(data_directory);
    if (!initial_segments.has_value() || initial_segments->count != 1U ||
        initial_wal_bytes == 0U) {
      report_wal_error("setup", "initial_segment_snapshot_invalid");
      cleanup();
      return false;
    }

    EngineSeq next_sequence = 1;
    std::uint64_t warmup_remaining = options.warmup;
    if (!run_wal_groups(*wal, options.warmup, 1U, WalSyncMode::none, next_sequence,
                        warmup_remaining, "rotation_warmup", nullptr, false) ||
        warmup_remaining != 0U) {
      cleanup();
      return false;
    }
    if (const auto status = wal->sync(); std::holds_alternative<Error>(status)) {
      report_wal_error("rotation_warmup", "final_sync_failed",
                       std::get<Error>(status).message);
      cleanup();
      return false;
    }
    const auto sequence_after_warmup = checked_sum(options.warmup, 1U);
    if (!sequence_after_warmup.has_value() ||
        *sequence_after_warmup > std::numeric_limits<EngineSeq>::max()) {
      report_wal_error("rotation_setup", "sequence_overflow");
      cleanup();
      return false;
    }
    next_sequence = static_cast<EngineSeq>(*sequence_after_warmup);

    const auto probe_start_bytes = wal->size_bytes();
    const auto probe_start_segments = inspect_wal_segments(data_directory);
    std::uint64_t probe_remaining = 1;
    if (!run_wal_groups(*wal, 1U, 1U, WalSyncMode::none, next_sequence,
                        probe_remaining, "rotation_setup", nullptr, false) ||
        probe_remaining != 0U) {
      cleanup();
      return false;
    }
    const auto probe_end_bytes = wal->size_bytes();
    const auto probe_end_segments = inspect_wal_segments(data_directory);
    if (!probe_start_segments.has_value() || !probe_end_segments.has_value() ||
        probe_end_segments->count != probe_start_segments->count ||
        probe_end_bytes <= probe_start_bytes) {
      report_wal_error("rotation_setup", "frame_size_probe_rotated");
      cleanup();
      return false;
    }
    const auto frame_bytes = probe_end_bytes - probe_start_bytes;
    const auto setup_snapshot = probe_end_segments.value();
    if (setup_snapshot.active_offset > options.wal_segment_size_bytes ||
        frame_bytes > options.wal_segment_size_bytes) {
      report_wal_error("rotation_setup", "frame_size_invalid");
      cleanup();
      return false;
    }
    const auto sequence_after_probe = checked_sum(*sequence_after_warmup, 1U);
    if (!sequence_after_probe.has_value() ||
        *sequence_after_probe > std::numeric_limits<EngineSeq>::max()) {
      report_wal_error("rotation_setup", "sequence_overflow");
      cleanup();
      return false;
    }
    next_sequence = static_cast<EngineSeq>(*sequence_after_probe);
    const auto available = options.wal_segment_size_bytes - setup_snapshot.active_offset;
    std::uint64_t setup_fill_commands = 0;
    if (mode == WalRotationDiagnosticMode::control) {
      if (available < frame_bytes) {
        report_wal_error("rotation_setup", "control_target_has_no_capacity");
        cleanup();
        return false;
      }
      setup_fill_commands = (available - frame_bytes) / frame_bytes;
    } else {
      setup_fill_commands = available / frame_bytes;
    }
    const auto target_sequence = checked_sum(*sequence_after_probe, setup_fill_commands);
    if (!target_sequence.has_value() ||
        *target_sequence > std::numeric_limits<EngineSeq>::max()) {
      report_wal_error("rotation_setup", "sequence_overflow");
      cleanup();
      return false;
    }

    // Fill outside the measured boundary.  Large setup groups reduce the
    // preparation overhead, while a final singleton handles the exact tail.
    constexpr std::uint64_t kSetupGroupSize = 8'192U;
    const auto large_groups = setup_fill_commands / kSetupGroupSize;
    const auto tail_commands = setup_fill_commands % kSetupGroupSize;
    std::uint64_t setup_remaining = setup_fill_commands;
    if (!run_wal_groups(*wal, large_groups, kSetupGroupSize, WalSyncMode::none,
                        next_sequence, setup_remaining, "rotation_setup", nullptr, false) ||
        !run_wal_groups(*wal, tail_commands, 1U, WalSyncMode::none, next_sequence,
                        setup_remaining, "rotation_setup", nullptr, false) ||
        setup_remaining != 0U) {
      cleanup();
      return false;
    }
    next_sequence = static_cast<EngineSeq>(*target_sequence);
    const auto before_target = inspect_wal_segments(data_directory);
    if (!before_target.has_value()) {
      report_wal_error("rotation_setup", "target_segment_snapshot_failed");
      cleanup();
      return false;
    }
    const auto target_available = options.wal_segment_size_bytes - before_target->active_offset;
    const bool target_fits = target_available >= frame_bytes;
    if ((mode == WalRotationDiagnosticMode::control && !target_fits) ||
        (mode == WalRotationDiagnosticMode::trigger && target_fits)) {
      report_wal_error("rotation_setup", "target_position_invalid");
      cleanup();
      return false;
    }

    WalGroupSamples samples;
    samples.sample_stride = 1U;
    samples.append_ns.reserve(1U);
    samples.total_ns.reserve(1U);
    samples.fixture_build_ns.reserve(1U);
    samples.wal_append_call_ns.reserve(1U);
    samples.lock_wait_ns.reserve(1U);
    samples.prepare_ns.reserve(1U);
    samples.prepare_task_ns.reserve(1U);
    samples.plan_copy_ns.reserve(1U);
    samples.rotation_ns.reserve(1U);
    samples.rotation_sync_ns.reserve(1U);
    samples.rotation_header_write_ns.reserve(1U);
    samples.rotation_header_sync_ns.reserve(1U);
    samples.rotation_directory_sync_ns.reserve(1U);
    samples.write_ns.reserve(1U);
    samples.publish_ns.reserve(1U);
    const auto process_before = capture_process_counters();
    const auto target_start_bytes = wal->size_bytes();
    const auto target_start = std::chrono::steady_clock::now();
    std::uint64_t target_remaining = 1;
    if (!run_wal_groups(*wal, 1U, 1U, WalSyncMode::none, next_sequence,
                        target_remaining, "rotation_measured", &samples, true) ||
        target_remaining != 0U) {
      cleanup();
      return false;
    }
    const auto target_end = std::chrono::steady_clock::now();
    const auto process_after = capture_process_counters();
    const auto measured_counters = subtract_process_counters(process_after, process_before);
    const auto after_target = inspect_wal_segments(data_directory);
    if (!after_target.has_value()) {
      report_wal_error("rotation_measured", "target_segment_snapshot_failed");
      cleanup();
      return false;
    }
    if (after_target->count < before_target->count) {
      report_wal_error("rotation_measured", "segment_count_regressed");
      cleanup();
      return false;
    }
    const auto measured_rotations = after_target->count - before_target->count;
    const auto expected_rotations = mode == WalRotationDiagnosticMode::control ? 0U : 1U;
    if (measured_rotations != expected_rotations) {
      report_wal_error("rotation_measured", "unexpected_rotation_count");
      cleanup();
      return false;
    }
    const auto target_end_bytes = wal->size_bytes();
    if (target_end_bytes < target_start_bytes) {
      report_wal_error("rotation_measured", "target_byte_snapshot_regressed");
      cleanup();
      return false;
    }
    const auto measured_wal_bytes = target_end_bytes - target_start_bytes;
    const auto expected_target_bytes = checked_sum(
        frame_bytes, expected_rotations == 0U ? 0U : initial_wal_bytes);
    const auto expected_control_offset =
        checked_sum(before_target->active_offset, frame_bytes);
    const auto expected_trigger_offset = checked_sum(initial_wal_bytes, frame_bytes);
    const auto expected_trigger_segments = checked_sum(before_target->count, 1U);
    const bool position_verified =
        expected_target_bytes.has_value() && expected_control_offset.has_value() &&
        expected_trigger_offset.has_value() && expected_trigger_segments.has_value() &&
        (mode == WalRotationDiagnosticMode::control
             ? after_target->count == before_target->count &&
                   after_target->active_engine_seq == before_target->active_engine_seq &&
                   after_target->active_offset == *expected_control_offset
             : after_target->count == *expected_trigger_segments &&
                   after_target->active_engine_seq == *target_sequence &&
                   after_target->active_offset == *expected_trigger_offset);
    if (!position_verified) {
      report_wal_error("rotation_measured", "rotation_target_position_mismatch");
      cleanup();
      return false;
    }
    if (measured_wal_bytes != *expected_target_bytes) {
      report_wal_error("rotation_measured", "target_byte_plan_mismatch");
      cleanup();
      return false;
    }
    if (const auto status = wal->sync(); std::holds_alternative<Error>(status)) {
      report_wal_error("rotation_finalize", "final_sync_failed",
                       std::get<Error>(status).message);
      cleanup();
      return false;
    }
    wal.reset();
    auto reopened_result = storage::Wal::open(
        data_directory, 1, options.wal_segment_size_bytes,
        storage::WalPrepareOptions{options.wal_prepare_workers,
                                   options.wal_parallel_prepare_min_commands});
    if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(reopened_result)) {
      report_wal_error("rotation_reopen", "wal_reopen_failed",
                       std::get<Error>(reopened_result).message);
      cleanup();
      return false;
    }
    auto reopened = std::get<std::unique_ptr<storage::Wal>>(std::move(reopened_result));
    const auto expected_records = static_cast<std::uint64_t>(options.warmup) +
                                  2U + setup_fill_commands;
    auto replayed_result = reopened->replay();
    if (std::holds_alternative<Error>(replayed_result)) {
      report_wal_error("rotation_replay", "wal_replay_failed",
                       std::get<Error>(replayed_result).message);
      cleanup();
      return false;
    }
    if (!validate_wal_replay(
            std::get<std::vector<domain::CommittedCommand>>(replayed_result),
            expected_records)) {
      cleanup();
      return false;
    }
    std::sort(samples.append_ns.begin(), samples.append_ns.end());
    std::sort(samples.total_ns.begin(), samples.total_ns.end());
    std::sort(samples.rotation_ns.begin(), samples.rotation_ns.end());
    std::sort(samples.rotation_sync_ns.begin(), samples.rotation_sync_ns.end());
    std::sort(samples.rotation_header_write_ns.begin(), samples.rotation_header_write_ns.end());
    std::sort(samples.rotation_header_sync_ns.begin(), samples.rotation_header_sync_ns.end());
    std::sort(samples.rotation_directory_sync_ns.begin(),
              samples.rotation_directory_sync_ns.end());
    const auto print_one = [](const std::vector<std::uint64_t>& values,
                              const std::uint64_t fallback) {
      return values.empty() ? std::to_string(fallback / 1'000.0)
                            : std::to_string(values.front() / 1'000.0);
    };
    const auto measured_elapsed = elapsed_ns(target_start, target_end);
    std::cout << "wal_rotation_diagnostic case=" << mode_name
              << " sync_mode=none completion_boundary=append_batch_return"
              << " warmup_commands=" << options.warmup
              << " setup_fill_commands=" << setup_fill_commands
              << " groups=1 group_size=1 commands=1"
              << " commands_per_second="
              << (measured_elapsed == 0U ? 0.0 : 1'000'000'000.0 / measured_elapsed)
              << " elapsed_ms=" << measured_elapsed / 1'000'000.0
              << " workload_wall_elapsed_ms=" << measured_elapsed / 1'000'000.0
              << " service_elapsed_ms=" << samples.total.total_ns / 1'000'000.0
              << " latency_sample_stride=1 append_latency_sample_count="
              << samples.append_ns.size()
              << " sync_samples=0 sync_latency_sample_count=0"
              << " append_group_p50_us=" << print_one(samples.append_ns, samples.append.max_ns)
              << " append_group_p99_us=" << print_one(samples.append_ns, samples.append.max_ns)
              << " append_group_p99.9_us=" << print_one(samples.append_ns, samples.append.max_ns)
              << " append_group_max_us=" << samples.append.max_ns / 1'000.0
              << " measured_rusage_valid=" << (measured_counters.rusage_valid ? "true" : "false")
              << " measured_io_valid=" << (measured_counters.io_valid ? "true" : "false")
              << " measured_meminfo_valid="
              << (measured_counters.meminfo_valid ? "true" : "false")
              << " measured_user_seconds=" << measured_counters.user_seconds
              << " measured_system_seconds=" << measured_counters.system_seconds
              << " measured_voluntary_context_switches="
              << measured_counters.voluntary_context_switches
              << " measured_involuntary_context_switches="
              << measured_counters.involuntary_context_switches
              << " measured_syscw="
              << (measured_counters.io_valid ? std::to_string(measured_counters.syscw)
                                              : std::string("na"))
              << " measured_wchar="
              << (measured_counters.io_valid ? std::to_string(measured_counters.wchar)
                                              : std::string("na"))
              << " measured_write_bytes="
              << (measured_counters.io_valid ? std::to_string(measured_counters.write_bytes)
                                              : std::string("na"))
              << " measured_cancelled_write_bytes="
              << (measured_counters.io_valid
                      ? std::to_string(measured_counters.cancelled_write_bytes)
                      : std::string("na"))
              << " measured_dirty_bytes_before="
              << (process_before.meminfo_valid ? std::to_string(process_before.dirty_bytes)
                                                : std::string("na"))
              << " measured_dirty_bytes_after="
              << (process_after.meminfo_valid ? std::to_string(process_after.dirty_bytes)
                                               : std::string("na"))
              << " measured_writeback_bytes_before="
              << (process_before.meminfo_valid ? std::to_string(process_before.writeback_bytes)
                                                : std::string("na"))
              << " measured_writeback_bytes_after="
              << (process_after.meminfo_valid ? std::to_string(process_after.writeback_bytes)
                                               : std::string("na"))
              << " measured_wal_write_calls="
              << (measured_counters.io_valid ? std::to_string(measured_counters.syscw)
                                              : std::string("na"))
              << " measured_wal_sync_calls=0"
              << " segment_size_bytes=" << options.wal_segment_size_bytes
              << " measured_segment_rotations=" << measured_rotations
              << " rotation_triggered=" << (measured_rotations == 1U ? "true" : "false")
              << " rotation_scope=rotation_inclusive"
              << " segment_id_before=" << before_target->active_engine_seq
              << " segment_id_after=" << after_target->active_engine_seq
              << " target_sequence=" << *target_sequence
              << " segment_offset_before=" << before_target->active_offset
              << " segment_offset_after=" << after_target->active_offset
              << " wal_bytes_delta=" << measured_wal_bytes
              << " frame_bytes_per_command=" << frame_bytes
              << " segment_header_bytes=" << initial_wal_bytes
              << " planned_wal_bytes_delta=" << *expected_target_bytes
              << " wal_byte_plan_verified=true"
              << " wal_path=" << data_directory
              << " replay_verified=true phase_profile=on"
              << " profiled_groups=" << samples.profiled_groups
              << " profiled_commands=" << samples.profiled_commands
              << " profiled_data_write_calls=" << samples.profiled_data_write_calls
              << " wal_prepare_group_total_us=" << samples.prepare.total_ns / 1'000.0
              << " wal_plan_copy_group_total_us=" << samples.plan_copy.total_ns / 1'000.0
              << " wal_write_group_total_us=" << samples.write.total_ns / 1'000.0
              << " wal_publish_group_total_us=" << samples.publish.total_ns / 1'000.0
              << " wal_rotation_group_total_us=" << samples.rotation.total_ns / 1'000.0
              << " rotation_sync_total_us=" << samples.rotation_sync.total_ns / 1'000.0
              << " rotation_header_write_total_us="
              << samples.rotation_header_write.total_ns / 1'000.0
              << " rotation_header_sync_total_us="
              << samples.rotation_header_sync.total_ns / 1'000.0
              << " rotation_directory_sync_total_us="
              << samples.rotation_directory_sync.total_ns / 1'000.0 << '\n';
    cleanup();
    return true;
  }
  if (options.wal_no_rotation_epoch_commands != 0U) {
    return run_wal_write_ceiling_no_rotation(options);
  }
  if (options.wal_group_size == 0 ||
      options.wal_group_size > std::numeric_limits<std::size_t>::max()) {
    report_wal_error("setup", "invalid_group_size");
    return false;
  }
  if (options.warmup > std::numeric_limits<std::uint64_t>::max() - options.iterations) {
    report_wal_error("setup", "group_count_overflow");
    return false;
  }
  const auto total_groups = options.warmup + options.iterations;
  if (total_groups != 0 &&
      options.wal_group_size > std::numeric_limits<std::uint64_t>::max() / total_groups) {
    report_wal_error("setup", "command_count_overflow");
    return false;
  }
  const auto total_commands = total_groups * options.wal_group_size;
  const auto run_id = std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(static_cast<unsigned long long>(::getpid()));
  const bool owned_data_directory = !options.data_directory.has_value();
  const auto data_directory = options.data_directory.value_or(
      std::filesystem::temp_directory_path() / ("order_books_benchmark_wal-" + run_id));
  if (const auto error = prepare_data_directory(data_directory); error.has_value()) {
    report_wal_error("setup", *error, data_directory.string());
    return false;
  }
  const auto cleanup = [&] {
    if (owned_data_directory) {
      std::error_code ignored;
      std::filesystem::remove_all(data_directory, ignored);
    }
  };

  auto wal_result = storage::Wal::open(
      data_directory, 1, options.wal_segment_size_bytes,
      storage::WalPrepareOptions{options.wal_prepare_workers,
                                 options.wal_parallel_prepare_min_commands});
  if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(wal_result)) {
    report_wal_error("open", "wal_open_failed",
                     std::get<Error>(wal_result).message);
    cleanup();
    return false;
  }
  auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(wal_result));
  std::optional<std::uint64_t> segment_header_bytes;
  if (options.wal_phase_profile == WalPhaseProfileMode::on) {
    segment_header_bytes = wal->size_bytes();
    if (*segment_header_bytes == 0U) {
      report_wal_error("setup", "wal_segment_header_size_zero");
      cleanup();
      return false;
    }
  }
  std::uint64_t remaining_commands = total_commands;
  EngineSeq next_sequence = 1;
  if (!run_wal_groups(*wal, options.warmup, options.wal_group_size,
                      options.wal_sync_mode, next_sequence, remaining_commands, "warmup",
                      nullptr, false)) {
    cleanup();
    return false;
  }
  if (const auto status = wal->sync(); std::holds_alternative<Error>(status)) {
    report_wal_error("warmup", "final_sync_failed", std::get<Error>(status).message);
    cleanup();
    return false;
  }
  const auto starting_wal_bytes = wal->size_bytes();
  const auto starting_segments = inspect_wal_segments(data_directory);
  if (!starting_segments.has_value()) {
    report_wal_error("warmup", "segment_snapshot_failed", data_directory.string());
    cleanup();
    return false;
  }
  const auto prepare_stats_before_measured = wal->prepare_stats();
  const auto process_counters_before_measured = capture_process_counters();

  WalGroupSamples samples;
  samples.sample_stride = latency_sample_stride(options.iterations);
  const auto sample_count = options.iterations / samples.sample_stride +
                            (options.iterations % samples.sample_stride == 0U ? 0U : 1U);
  const auto sample_capacity = static_cast<std::size_t>(sample_count);
  samples.append_ns.reserve(sample_capacity);
  if (options.wal_sync_mode == WalSyncMode::per_group ||
      options.wal_phase_profile == WalPhaseProfileMode::on) {
    samples.total_ns.reserve(sample_capacity);
  }
  if (options.wal_sync_mode == WalSyncMode::per_group) {
    samples.sync_ns.reserve(sample_capacity);
  }
  if (options.wal_phase_profile == WalPhaseProfileMode::on) {
    samples.fixture_build_ns.reserve(sample_capacity);
    samples.wal_append_call_ns.reserve(sample_capacity);
    samples.lock_wait_ns.reserve(sample_capacity);
    samples.prepare_ns.reserve(sample_capacity);
    samples.prepare_task_ns.reserve(sample_capacity);
    samples.plan_copy_ns.reserve(sample_capacity);
    samples.rotation_ns.reserve(sample_capacity);
    samples.rotation_sync_ns.reserve(sample_capacity);
    samples.rotation_header_write_ns.reserve(sample_capacity);
    samples.rotation_header_sync_ns.reserve(sample_capacity);
    samples.rotation_directory_sync_ns.reserve(sample_capacity);
    samples.write_ns.reserve(sample_capacity);
    samples.publish_ns.reserve(sample_capacity);
  }
  const auto measured_start = std::chrono::steady_clock::now();
  if (!run_wal_groups(*wal, options.iterations, options.wal_group_size,
                      options.wal_sync_mode, next_sequence, remaining_commands, "measured",
                      &samples,
                      options.wal_phase_profile == WalPhaseProfileMode::on)) {
    cleanup();
    return false;
  }
  const auto measured_end = std::chrono::steady_clock::now();
  const auto process_counters_after_measured = capture_process_counters();
  const auto measured_process_counters = subtract_process_counters(
      process_counters_after_measured, process_counters_before_measured);
  const auto prepare_stats_after_measured = wal->prepare_stats();
  const auto actual_prepare_stats =
      prepare_stats_delta(prepare_stats_before_measured, prepare_stats_after_measured);
  if (!actual_prepare_stats.has_value()) {
    report_wal_error("measured", "wal_prepare_stats_regressed");
    cleanup();
    return false;
  }
  const auto ending_wal_bytes = wal->size_bytes();
  const auto ending_segments = inspect_wal_segments(data_directory);
  if (!ending_segments.has_value() || ending_segments->count < starting_segments->count ||
      ending_wal_bytes < starting_wal_bytes) {
    report_wal_error("measured", "wal_observation_snapshot_failed", data_directory.string());
    cleanup();
    return false;
  }
  if (remaining_commands != 0) {
    report_wal_error("measured", "command_count_mismatch");
    cleanup();
    return false;
  }
  if (const auto status = wal->sync(); std::holds_alternative<Error>(status)) {
    report_wal_error("finalize", "final_sync_failed", std::get<Error>(status).message);
    cleanup();
    return false;
  }

  wal.reset();
  auto reopened_result = storage::Wal::open(
      data_directory, 1, options.wal_segment_size_bytes,
      storage::WalPrepareOptions{options.wal_prepare_workers,
                                 options.wal_parallel_prepare_min_commands});
  if (!std::holds_alternative<std::unique_ptr<storage::Wal>>(reopened_result)) {
    report_wal_error("reopen", "wal_reopen_failed",
                     std::get<Error>(reopened_result).message);
    cleanup();
    return false;
  }
  auto reopened = std::get<std::unique_ptr<storage::Wal>>(std::move(reopened_result));
  auto replayed_result = reopened->replay();
  if (!std::holds_alternative<std::vector<domain::CommittedCommand>>(replayed_result)) {
    report_wal_error("replay", "wal_replay_failed",
                     std::get<Error>(replayed_result).message);
    cleanup();
    return false;
  }
  const auto replayed = std::get<std::vector<domain::CommittedCommand>>(
      std::move(replayed_result));
  if (!validate_wal_replay(replayed, total_commands)) {
    cleanup();
    return false;
  }

  std::error_code path_error;
  const auto resolved_path = std::filesystem::weakly_canonical(data_directory, path_error);
  if (path_error) {
    report_wal_error("report", "wal_path_resolution_failed", data_directory.string());
    cleanup();
    return false;
  }
  const auto measured_elapsed = elapsed_ns(measured_start, measured_end);
  const auto service_elapsed_value =
      options.wal_sync_mode == WalSyncMode::none
          ? samples.append.total_ns
          : samples.total.total_ns;
  if (service_elapsed_value == 0U) {
    report_wal_error("report", "service_elapsed_invalid");
    cleanup();
    return false;
  }
  const auto service_elapsed = service_elapsed_value;
  const auto wal_bytes_delta = ending_wal_bytes - starting_wal_bytes;
  const auto commands_per_second = measured_elapsed == 0
                                       ? 0.0
                                       : static_cast<double>(options.iterations) *
                                             static_cast<double>(options.wal_group_size) *
                                             1'000'000'000.0 /
                                             static_cast<double>(measured_elapsed);
  const auto workload_wall_wal_mib_per_second = measured_elapsed == 0
                                                    ? 0.0
                                                    : static_cast<double>(wal_bytes_delta) *
                                                          1'000'000'000.0 /
                                                          static_cast<double>(measured_elapsed) /
                                                          (1024.0 * 1024.0);
  const auto service_commands_per_second =
      static_cast<double>(options.iterations) * static_cast<double>(options.wal_group_size) *
      1'000'000'000.0 / static_cast<double>(service_elapsed);
  const auto service_wal_mib_per_second =
      static_cast<double>(wal_bytes_delta) * 1'000'000'000.0 /
      static_cast<double>(service_elapsed) / (1024.0 * 1024.0);
  // Keep the historical headline fields tied to process wall time.  The
  // component runner consumes the explicitly named service fields below, so
  // wall and service rates cannot be mixed accidentally.
  const auto wal_mib_per_second = workload_wall_wal_mib_per_second;
  const auto target_attainment = commands_per_second / 1'000'000.0 * 100.0;
  const auto service_target_attainment = service_commands_per_second / 1'000'000.0 * 100.0;
  const auto workload_wall_target_attainment = commands_per_second / 1'000'000.0 * 100.0;
  const auto average_wal_bytes = options.iterations == 0 || options.wal_group_size == 0
                                     ? 0.0
                                     : static_cast<double>(wal_bytes_delta) /
                                           (static_cast<double>(options.iterations) *
                                            static_cast<double>(options.wal_group_size));
  const auto measured_rotations = ending_segments->count - starting_segments->count;
  std::sort(samples.append_ns.begin(), samples.append_ns.end());
  std::sort(samples.sync_ns.begin(), samples.sync_ns.end());
  std::sort(samples.total_ns.begin(), samples.total_ns.end());
  const auto sort_profile_samples = [&samples] {
    std::sort(samples.fixture_build_ns.begin(), samples.fixture_build_ns.end());
    std::sort(samples.wal_append_call_ns.begin(), samples.wal_append_call_ns.end());
    std::sort(samples.lock_wait_ns.begin(), samples.lock_wait_ns.end());
    std::sort(samples.prepare_ns.begin(), samples.prepare_ns.end());
    std::sort(samples.prepare_task_ns.begin(), samples.prepare_task_ns.end());
    std::sort(samples.plan_copy_ns.begin(), samples.plan_copy_ns.end());
    std::sort(samples.rotation_ns.begin(), samples.rotation_ns.end());
    std::sort(samples.rotation_sync_ns.begin(), samples.rotation_sync_ns.end());
    std::sort(samples.rotation_header_write_ns.begin(), samples.rotation_header_write_ns.end());
    std::sort(samples.rotation_header_sync_ns.begin(), samples.rotation_header_sync_ns.end());
    std::sort(samples.rotation_directory_sync_ns.begin(),
              samples.rotation_directory_sync_ns.end());
    std::sort(samples.write_ns.begin(), samples.write_ns.end());
    std::sort(samples.publish_ns.begin(), samples.publish_ns.end());
  };
  if (options.wal_phase_profile == WalPhaseProfileMode::on) {
    sort_profile_samples();
    const auto expected_commands = options.iterations * options.wal_group_size;
    if (samples.profiled_groups != options.iterations ||
        samples.profiled_commands != expected_commands ||
        samples.append.observed != options.iterations ||
        samples.total.observed != options.iterations ||
        samples.fixture_build.observed != options.iterations ||
        samples.wal_append_call.observed != options.iterations ||
        samples.lock_wait.observed != options.iterations ||
        samples.prepare.observed != options.iterations ||
        samples.prepare_task.observed != options.iterations ||
        samples.plan_copy.observed != options.iterations ||
        samples.rotation.observed > options.iterations ||
        samples.write.observed != options.iterations ||
        samples.publish.observed != options.iterations ||
        samples.profiled_rotations != measured_rotations ||
        samples.profiled_frame_bytes > wal_bytes_delta) {
      report_wal_error("report", "profile_counter_mismatch");
      cleanup();
      return false;
    }
    const auto profiled_header_bytes = wal_bytes_delta - samples.profiled_frame_bytes;
    const auto expected_profiled_header_bytes =
        checked_multiply(samples.profiled_rotations, *segment_header_bytes);
    if (!expected_profiled_header_bytes.has_value()) {
      report_wal_error("report", "profile_counter_overflow");
      cleanup();
      return false;
    }
    if (profiled_header_bytes != *expected_profiled_header_bytes ||
        samples.profiled_data_write_calls < options.iterations ||
        (measured_rotations == 0U &&
         samples.profiled_data_write_calls != options.iterations)) {
      report_wal_error("report", "profile_wal_observation_mismatch");
      cleanup();
      return false;
    }
  }
  const auto print_percentile = [](const std::vector<std::uint64_t>& values,
                                   const std::size_t numerator,
                                   const std::size_t denominator) {
    return values.empty() ? std::string("na")
                           : std::to_string(percentile_ns(values, numerator, denominator) /
                                            1'000.0);
  };
  const auto print_profile_percentiles = [&print_percentile](
                                             const std::string_view name,
                                             const std::vector<std::uint64_t>& values,
                                             const LatencyAggregate& aggregate) {
    std::cout << ' ' << name << "_total_us=" << aggregate.total_ns / 1'000.0
              << ' ' << name << "_p50_us=" << print_percentile(values, 50, 100)
              << ' ' << name << "_p99_us=" << print_percentile(values, 99, 100)
              << ' ' << name << "_p99.9_us=" << print_percentile(values, 999, 1000)
              << ' ' << name << "_max_us="
              << (aggregate.observed == 0U ? std::string("na")
                                            : std::to_string(aggregate.max_ns / 1'000.0));
  };
  std::optional<std::uint64_t> profile_denominator;
  std::optional<std::uint64_t> profile_unattributed;
  std::optional<WalProfileTotals> profile_total_values;
  if (options.wal_phase_profile == WalPhaseProfileMode::on) {
    const auto totals = profile_totals(samples);
    if (!totals.has_value()) {
      report_wal_error("report", "profile_phase_sum_overflow");
      cleanup();
      return false;
    }
    std::uint64_t denominator = totals->wal_append_call;
    if (options.wal_sync_mode == WalSyncMode::per_group &&
        !checked_add(denominator, totals->sync)) {
      report_wal_error("report", "profile_phase_sum_overflow");
      cleanup();
      return false;
    }
    std::uint64_t phase_sum = 0;
    const auto add_phase_sum = [&phase_sum](const std::uint64_t value) {
      return checked_add(phase_sum, value);
    };
    if (!add_phase_sum(totals->lock_wait) || !add_phase_sum(totals->prepare) ||
        !add_phase_sum(totals->plan_copy) || !add_phase_sum(totals->rotation) ||
        !add_phase_sum(totals->write) || !add_phase_sum(totals->publish) ||
        (options.wal_sync_mode == WalSyncMode::per_group &&
         !checked_add(phase_sum, totals->sync))) {
      report_wal_error("report", "profile_phase_sum_overflow");
      cleanup();
      return false;
    }
    if (phase_sum > denominator) {
      report_wal_error("report", "profile_phase_accounting_overrun");
      cleanup();
      return false;
    }
    profile_denominator = denominator;
    profile_unattributed = denominator - phase_sum;
    profile_total_values = *totals;
  }
  const auto share_percent = [](const std::uint64_t value,
                                const std::uint64_t denominator) {
    return denominator == 0U ? std::string("na")
                             : std::to_string(static_cast<double>(value) * 100.0 /
                                              static_cast<double>(denominator));
  };
  std::cout << "wal_write_ceiling sync_mode=" << wal_sync_mode_name(options.wal_sync_mode)
            << " completion_boundary="
            << (options.wal_sync_mode == WalSyncMode::none ? "append_batch_return" : "group_fsync")
            << " warmup_groups=" << options.warmup << " groups=" << options.iterations
            << " group_size=" << options.wal_group_size
            << " wal_prepare_workers=" << options.wal_prepare_workers
            << " wal_parallel_prepare_min_commands="
            << options.wal_parallel_prepare_min_commands
            << " actual_parallel_prepare_groups=" << actual_prepare_stats->parallel_groups
            << " actual_prepare_tasks=" << actual_prepare_stats->tasks
            << " commands=" << options.iterations * options.wal_group_size
            << " commands_per_second=" << commands_per_second
            << " workload_wall_commands_per_second=" << commands_per_second
            << " service_commands_per_second=" << service_commands_per_second
            << " target_commands_per_second=1000000"
            << " target_attainment_percent=" << target_attainment
            << " workload_wall_target_attainment_percent="
            << workload_wall_target_attainment
            << " service_target_attainment_percent=" << service_target_attainment
            << " wal_mib_per_second=" << wal_mib_per_second
            << " workload_wall_wal_mib_per_second=" << workload_wall_wal_mib_per_second
            << " service_wal_mib_per_second=" << service_wal_mib_per_second
            << " measured_rusage_valid="
            << (measured_process_counters.rusage_valid ? "true" : "false")
            << " measured_io_valid="
            << (measured_process_counters.io_valid ? "true" : "false")
            << " measured_meminfo_valid="
            << (measured_process_counters.meminfo_valid ? "true" : "false")
            << " measured_user_seconds=" << measured_process_counters.user_seconds
            << " measured_system_seconds=" << measured_process_counters.system_seconds
            << " measured_voluntary_context_switches="
            << measured_process_counters.voluntary_context_switches
            << " measured_involuntary_context_switches="
            << measured_process_counters.involuntary_context_switches
            << " measured_syscw="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.syscw)
                    : std::string("na"))
            << " measured_write_bytes="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.write_bytes)
                    : std::string("na"))
            << " measured_cancelled_write_bytes="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.cancelled_write_bytes)
                    : std::string("na"))
            << " average_wal_bytes_per_command=" << average_wal_bytes
            << " latency_sample_stride=" << samples.sample_stride
            << " append_latency_sample_count=" << samples.append_ns.size()
            << " append_group_p50_us="
            << print_percentile(samples.append_ns, 50, 100)
            << " append_group_p99_us=" << print_percentile(samples.append_ns, 99, 100)
            << " append_group_p99.9_us=" << print_percentile(samples.append_ns, 999, 1000)
            << " append_group_max_us="
            << (samples.append.observed == 0U ? std::string("na")
                                               : std::to_string(samples.append.max_ns / 1'000.0))
            << " sync_samples=" << samples.sync.observed
            << " sync_latency_sample_count=" << samples.sync_ns.size()
            << " sync_p50_us=" << print_percentile(samples.sync_ns, 50, 100)
            << " sync_p99_us=" << print_percentile(samples.sync_ns, 99, 100)
            << " sync_p99.9_us=" << print_percentile(samples.sync_ns, 999, 1000)
            << " sync_max_us="
            << (samples.sync.observed == 0U ? std::string("na")
                                             : std::to_string(samples.sync.max_ns / 1'000.0))
            << " group_total_latency_sample_count=" << samples.total_ns.size()
            << " group_total_p50_us=" << print_percentile(samples.total_ns, 50, 100)
            << " group_total_p99_us=" << print_percentile(samples.total_ns, 99, 100)
            << " group_total_p99.9_us=" << print_percentile(samples.total_ns, 999, 1000)
            << " group_total_max_us="
            << (samples.total.observed == 0U ? std::string("na")
                                              : std::to_string(samples.total.max_ns / 1'000.0))
            << " measured_wchar="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.wchar)
                    : std::string("na"))
            << " measured_dirty_bytes_after="
            << (measured_process_counters.meminfo_valid
                    ? std::to_string(measured_process_counters.dirty_bytes)
                    : std::string("na"))
            << " measured_writeback_bytes_after="
            << (measured_process_counters.meminfo_valid
                    ? std::to_string(measured_process_counters.writeback_bytes)
                    : std::string("na"))
            << " elapsed_ms=" << measured_elapsed / 1'000'000.0
            << " workload_wall_elapsed_ms=" << measured_elapsed / 1'000'000.0
            << " service_elapsed_ms=" << service_elapsed / 1'000'000.0
            << " wal_bytes_delta=" << wal_bytes_delta
            << " wal_bytes=" << ending_wal_bytes
            << " measured_wal_write_calls="
            << (measured_process_counters.io_valid
                    ? std::to_string(measured_process_counters.syscw)
                    : std::string("na"))
            << " measured_wal_sync_calls=" << samples.sync.observed
            << " measured_dirty_bytes_before="
            << (process_counters_before_measured.meminfo_valid
                    ? std::to_string(process_counters_before_measured.dirty_bytes)
                    : std::string("na"))
            << " measured_writeback_bytes_before="
            << (process_counters_before_measured.meminfo_valid
                    ? std::to_string(process_counters_before_measured.writeback_bytes)
                    : std::string("na"))
            << " segment_size_bytes=" << options.wal_segment_size_bytes
            << " segment_count=" << ending_segments->count
            << " measured_segment_rotations=" << measured_rotations
            << " rotation_scope=rotation_inclusive"
            << " rotation_triggered=" << (measured_rotations == 0U ? "false" : "true")
            << " segment_count_before=" << starting_segments->count
            << " segment_count_after=" << ending_segments->count
            << " segment_id_before=" << starting_segments->active_engine_seq
            << " segment_id_after=" << ending_segments->active_engine_seq
            << " segment_offset_before=" << starting_segments->active_offset
            << " segment_offset_after=" << ending_segments->active_offset
            << " wal_bytes_before=" << starting_wal_bytes
            << " wal_bytes_after=" << ending_wal_bytes
            << " wal_path=" << resolved_path << " replay_verified=true"
            << " phase_profile="
            << (options.wal_phase_profile == WalPhaseProfileMode::on ? "on" : "off");
  if (options.wal_phase_profile == WalPhaseProfileMode::on) {
    const auto denominator = profile_denominator.value();
    const auto& totals = profile_total_values.value();
    std::cout << " profiled_groups=" << samples.profiled_groups
              << " profiled_commands=" << samples.profiled_commands
              << " profiled_frame_bytes=" << samples.profiled_frame_bytes
              << " profiled_header_bytes="
              << (wal_bytes_delta - samples.profiled_frame_bytes)
              << " profiled_data_write_calls=" << samples.profiled_data_write_calls
              << " profiled_rotations=" << samples.profiled_rotations
              << " parallel_prepare_groups=" << samples.parallel_prepare_groups
              << " prepare_tasks=" << samples.prepare_tasks
              << " sync_total_us=" << totals.sync / 1'000.0
              << " group_total_us=" << totals.group_total / 1'000.0;
    print_profile_percentiles("fixture_build_group", samples.fixture_build_ns,
                              samples.fixture_build);
    print_profile_percentiles("wal_append_call_group", samples.wal_append_call_ns,
                              samples.wal_append_call);
    print_profile_percentiles("wal_lock_wait_group", samples.lock_wait_ns, samples.lock_wait);
    print_profile_percentiles("wal_prepare_group", samples.prepare_ns, samples.prepare);
    print_profile_percentiles("wal_prepare_task_group", samples.prepare_task_ns,
                              samples.prepare_task);
    print_profile_percentiles("wal_plan_copy_group", samples.plan_copy_ns, samples.plan_copy);
    print_profile_percentiles("wal_rotation_group", samples.rotation_ns, samples.rotation);
    print_profile_percentiles("wal_rotation_sync_group", samples.rotation_sync_ns,
                              samples.rotation_sync);
    print_profile_percentiles("wal_rotation_header_write_group",
                              samples.rotation_header_write_ns,
                              samples.rotation_header_write);
    print_profile_percentiles("wal_rotation_header_sync_group",
                              samples.rotation_header_sync_ns,
                              samples.rotation_header_sync);
    print_profile_percentiles("wal_rotation_directory_sync_group",
                              samples.rotation_directory_sync_ns,
                              samples.rotation_directory_sync);
    print_profile_percentiles("wal_write_group", samples.write_ns, samples.write);
    print_profile_percentiles("wal_publish_group", samples.publish_ns, samples.publish);
    std::cout << " lock_wait_share_percent="
              << share_percent(totals.lock_wait, denominator)
              << " prepare_share_percent="
              << share_percent(totals.prepare, denominator)
              << " plan_copy_share_percent="
              << share_percent(totals.plan_copy, denominator)
              << " rotation_share_percent="
              << share_percent(totals.rotation, denominator)
              << " rotation_sync_total_us="
              << totals.rotation_sync / 1'000.0
              << " rotation_header_write_total_us="
              << totals.rotation_header_write / 1'000.0
              << " rotation_header_sync_total_us="
              << totals.rotation_header_sync / 1'000.0
              << " rotation_directory_sync_total_us="
              << totals.rotation_directory_sync / 1'000.0
              << " write_share_percent="
              << share_percent(totals.write, denominator)
              << " publish_share_percent="
              << share_percent(totals.publish, denominator)
              << " sync_share_percent="
              << (options.wal_sync_mode == WalSyncMode::per_group
                      ? share_percent(totals.sync, denominator)
                      : std::string("na"))
              << " unattributed_share_percent="
              << share_percent(profile_unattributed.value(), denominator);
  }
  std::cout << '\n';
  cleanup();
  return true;
}

bool run_engine_durable_single_instrument(const BenchmarkOptions& options) {
  const auto run_id = std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(static_cast<unsigned long long>(::getpid()));
  const bool owned_data_directory = !options.data_directory.has_value();
  const auto data_directory = options.data_directory.value_or(
      std::filesystem::temp_directory_path() / ("order_books_benchmark_engine-" + run_id));
  if (const auto error = prepare_data_directory(data_directory); error.has_value()) {
    report_durable_error("setup", *error, data_directory.string());
    return false;
  }
  if (options.engine_tail_telemetry_output.has_value()) {
    std::error_code output_error;
    if (std::filesystem::exists(*options.engine_tail_telemetry_output, output_error) ||
        output_error) {
      report_durable_error("setup", "telemetry_output_exists_or_unreadable",
                           options.engine_tail_telemetry_output->string());
      if (owned_data_directory) {
        std::error_code ignored;
        std::filesystem::remove_all(data_directory, ignored);
      }
      return false;
    }
  }
  const auto cleanup_data = [&] {
    if (owned_data_directory) {
      std::error_code ignored;
      std::filesystem::remove_all(data_directory, ignored);
    }
  };

  EngineConfig config;
  config.data_directory = data_directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.ingress_queue_capacity = kDurableIngressQueueCapacity;
  config.runtime.group_commit_max_commands = options.engine_group_size;
  config.runtime.group_commit_max_delay = options.engine_group_delay;
  config.runtime.wal_prepare_lanes = options.wal_prepare_workers;
  config.runtime.wal_parallel_prepare_min_commands =
      options.wal_parallel_prepare_min_commands;
  config.runtime.snapshot_interval_commands = std::numeric_limits<std::size_t>::max();
  config.runtime.snapshot_interval = std::chrono::hours(24);
  config.runtime.event_replay_snapshot_interval_commands =
      std::numeric_limits<std::size_t>::max();
  config.runtime.event_replay_snapshot_interval = std::chrono::hours(24);

  const auto command_count = double_count(options.iterations);
  const auto warmup_command_count = double_count(options.warmup);
  if (!command_count.has_value() || !warmup_command_count.has_value()) {
    report_durable_error("setup", "iteration_count_overflow");
    cleanup_data();
    return false;
  }

  std::optional<benchmark::EngineTailTelemetry> telemetry;
  if (options.engine_tail_telemetry_output.has_value()) {
    telemetry.emplace(static_cast<std::size_t>(std::min<std::uint64_t>(
        *command_count, benchmark::EngineTailTelemetry::kMaxMetricSamples)));
    if (!telemetry->reserve()) {
      report_durable_error("setup", "tail_telemetry_reserve_failed");
      cleanup_data();
      return false;
    }
  }

  AcknowledgingSink event_sink;
  NullMetricsSink metrics_sink;
  MetricsSink* metrics_sink_to_use = &metrics_sink;
  if (telemetry.has_value()) {
    metrics_sink_to_use = &*telemetry;
  }
  auto opened = Engine::open(config, event_sink, *metrics_sink_to_use);
  if (std::holds_alternative<Error>(opened)) {
    const auto& error = std::get<Error>(opened);
    report_durable_error("open", "engine_open_failed",
                         error_code_message(error.code) + " message=" + error.message);
    cleanup_data();
    return false;
  }
  auto engine = std::get<std::unique_ptr<Engine>>(std::move(opened));
  const auto stop_telemetry = [&] {
    if (telemetry.has_value()) {
      telemetry->stop_collection();
      telemetry->stop_sampler();
    }
  };
  const auto stop_engine_after_telemetry = [&] {
    stop_telemetry();
    return engine->stop();
  };
  const auto cleanup = [&] {
    stop_telemetry();
    cleanup_data();
  };
  if (telemetry.has_value() && options.engine_tail_state_sampling &&
      !telemetry->start_sampler(*engine, 1)) {
    report_durable_error("setup", "tail_telemetry_sampler_start_failed");
    (void)stop_engine_after_telemetry();
    cleanup();
    return false;
  }
  DurableRunState state(options.engine_producer_lanes);
  std::uint64_t next_order_id = 1;

  const auto initial_metrics = engine->metrics(1);
  if (std::holds_alternative<Error>(initial_metrics)) {
    const auto& error = std::get<Error>(initial_metrics);
    report_durable_error("initial_metrics", "metrics_failed",
                         error_code_message(error.code) + " message=" + error.message);
    (void)stop_engine_after_telemetry();
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
    (void)stop_engine_after_telemetry();
    cleanup();
    return false;
  }
  const auto warmup_metrics = engine->metrics(1);
  if (std::holds_alternative<Error>(warmup_metrics)) {
    const auto& error = std::get<Error>(warmup_metrics);
    report_durable_error("warmup_metrics", "metrics_failed",
                         error_code_message(error.code) + " message=" + error.message);
    (void)stop_engine_after_telemetry();
    cleanup();
    return false;
  }
  const auto wal_directory = data_directory / "shard-1" / "wal";
  const auto starting_segment_count = count_wal_segments(wal_directory);
  if (!starting_segment_count.has_value()) {
    report_durable_error("warmup_metrics", "segment_count_failed", wal_directory.string());
    (void)stop_engine_after_telemetry();
    cleanup();
    return false;
  }
  if (telemetry.has_value() && !telemetry->begin_measured()) {
    report_durable_error("measured", "tail_telemetry_phase_transition_failed");
    (void)stop_engine_after_telemetry();
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
    (void)stop_engine_after_telemetry();
    cleanup();
    return false;
  }
  if (telemetry.has_value() && !telemetry->begin_drain()) {
    report_durable_error("drain", "tail_telemetry_phase_transition_failed");
    (void)stop_engine_after_telemetry();
    cleanup();
    return false;
  }
  const auto final_metrics = engine->metrics(1);
  bool drain_boundary_error = false;
  if (telemetry.has_value() && options.engine_tail_state_sampling &&
      std::holds_alternative<MetricsSnapshot>(final_metrics) &&
      !telemetry->record_drain_snapshot(
          std::get<MetricsSnapshot>(final_metrics),
          benchmark::TailTelemetryBoundary::drain_start)) {
    drain_boundary_error = true;
  }
  const auto stop_status = engine->stop();
  std::optional<Result<MetricsSnapshot>> drain_end_metrics;
  if (telemetry.has_value()) {
    telemetry->stop_sampler();
    if (options.engine_tail_state_sampling) {
      drain_end_metrics = engine->metrics(1);
      if (std::holds_alternative<MetricsSnapshot>(*drain_end_metrics) &&
          !telemetry->record_drain_snapshot(
              std::get<MetricsSnapshot>(*drain_end_metrics),
              benchmark::TailTelemetryBoundary::drain_end)) {
        drain_boundary_error = true;
      }
    }
    telemetry->stop_collection();
  }
  if (std::holds_alternative<Error>(final_metrics) ||
      std::holds_alternative<Error>(stop_status) ||
      (drain_end_metrics.has_value() &&
       std::holds_alternative<Error>(*drain_end_metrics)) ||
      drain_boundary_error) {
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
    if (drain_end_metrics.has_value() &&
        std::holds_alternative<Error>(*drain_end_metrics)) {
      const auto& error = std::get<Error>(*drain_end_metrics);
      report_durable_error("drain", "metrics_failed",
                           error_code_message(error.code) + " message=" + error.message);
    }
    if (drain_boundary_error) {
      report_durable_error("drain", "tail_telemetry_snapshot_failed");
    }
    if (telemetry.has_value() &&
        !telemetry->write_csv(*options.engine_tail_telemetry_output)) {
      report_durable_error("telemetry", "telemetry_output_failed");
    }
    cleanup();
    return false;
  }
  engine.reset();

  const auto& initial = std::get<MetricsSnapshot>(initial_metrics);
  const auto& after_warmup = std::get<MetricsSnapshot>(warmup_metrics);
  const auto& after_measured = std::get<MetricsSnapshot>(final_metrics);
  const auto expected_warmup = *warmup_command_count;
  const auto expected_measured = *command_count;
  if (expected_warmup > std::numeric_limits<std::uint64_t>::max() - expected_measured) {
    report_durable_error("recovery", "command_count_overflow");
    cleanup();
    return false;
  }
  const auto expected_total = expected_warmup + expected_measured;
  auto reopened_wal_result = storage::Wal::open(
      wal_directory, 1, config.runtime.wal_segment_size,
      storage::WalPrepareOptions{config.runtime.wal_prepare_lanes,
                                 config.runtime.wal_parallel_prepare_min_commands});
  if (std::holds_alternative<Error>(reopened_wal_result)) {
    report_durable_error("recovery", "wal_reopen_failed",
                         std::get<Error>(reopened_wal_result).message);
    cleanup();
    return false;
  }
  auto reopened_wal = std::get<std::unique_ptr<storage::Wal>>(
      std::move(reopened_wal_result));
  auto replayed_result = reopened_wal->replay();
  if (std::holds_alternative<Error>(replayed_result)) {
    report_durable_error("recovery", "wal_replay_failed",
                         std::get<Error>(replayed_result).message);
    cleanup();
    return false;
  }
  const auto& replayed = std::get<std::vector<domain::CommittedCommand>>(replayed_result);
  if (replayed.size() != expected_total || reopened_wal->last_engine_seq() != expected_total) {
    report_durable_error("recovery", "durable_head_mismatch");
    cleanup();
    return false;
  }
  const auto warmup_commands = counter_delta(after_warmup.commands, initial.commands);
  const auto measured_commands = counter_delta(after_measured.commands, after_warmup.commands);
  const auto measured_trades = counter_delta(after_measured.trades, after_warmup.trades);
  const auto warmup_group_commits = counter_delta(after_warmup.wal_group_commits,
                                                  initial.wal_group_commits);
  const auto warmup_group_commands = counter_delta(after_warmup.wal_group_commands,
                                                   initial.wal_group_commands);
  const auto measured_group_commits = counter_delta(after_measured.wal_group_commits,
                                                    after_warmup.wal_group_commits);
  const auto measured_group_commands = counter_delta(after_measured.wal_group_commands,
                                                     after_warmup.wal_group_commands);
  const auto warmup_parallel_groups = counter_delta(
      after_warmup.wal_parallel_prepare_groups, initial.wal_parallel_prepare_groups);
  const auto measured_parallel_groups = counter_delta(
      after_measured.wal_parallel_prepare_groups,
      after_warmup.wal_parallel_prepare_groups);
  const auto warmup_prepare_tasks = counter_delta(after_warmup.wal_prepare_tasks,
                                                   initial.wal_prepare_tasks);
  const auto measured_prepare_tasks = counter_delta(after_measured.wal_prepare_tasks,
                                                     after_warmup.wal_prepare_tasks);
  if (!warmup_commands.has_value() || !measured_commands.has_value() ||
      !measured_trades.has_value() || !warmup_group_commits.has_value() ||
      !warmup_group_commands.has_value() || !measured_group_commits.has_value() ||
      !measured_group_commands.has_value() || !warmup_parallel_groups.has_value() ||
      !measured_parallel_groups.has_value() || !warmup_prepare_tasks.has_value() ||
      !measured_prepare_tasks.has_value()) {
    report_durable_error("validation", "counter_delta_invalid");
    cleanup();
    return false;
  }
  if (*warmup_commands != expected_warmup || *measured_commands != expected_measured ||
      *warmup_group_commands != expected_warmup ||
      *measured_group_commands != expected_measured || *measured_group_commits == 0) {
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
  if (after_measured.wal_size_bytes < after_warmup.wal_size_bytes) {
    report_durable_error("validation", "wal_size_decreased");
    cleanup();
    return false;
  }
  const auto ending_segment_count = count_wal_segments(wal_directory);
  if (!ending_segment_count.has_value() || *ending_segment_count < *starting_segment_count) {
    report_durable_error("validation", "segment_count_invalid", wal_directory.string());
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

  std::optional<benchmark::TailTelemetrySummary> tail_summary;
  if (telemetry.has_value()) {
    if (!telemetry->write_csv(*options.engine_tail_telemetry_output)) {
      report_durable_error("telemetry", "telemetry_output_failed",
                           options.engine_tail_telemetry_output->string());
      cleanup();
      return false;
    }
    tail_summary = telemetry->summary();
    const auto tail_clock_valid =
        tail_summary->tail_clock_start_realtime_epoch_ns.has_value() &&
        tail_summary->tail_clock_start_uncertainty_ns.has_value() &&
        tail_summary->tail_clock_end_realtime_epoch_ns.has_value() &&
        tail_summary->tail_clock_end_steady_elapsed_ns.has_value() &&
        tail_summary->tail_clock_end_uncertainty_ns.has_value() &&
        *tail_summary->tail_clock_start_realtime_epoch_ns != 0U &&
        *tail_summary->tail_clock_end_realtime_epoch_ns != 0U &&
        *tail_summary->tail_clock_end_steady_elapsed_ns != 0U &&
        benchmark::tail_clock_anchors_consistent(
            benchmark::TailClockAnchor{
                *tail_summary->tail_clock_start_realtime_epoch_ns, 0U,
                *tail_summary->tail_clock_start_uncertainty_ns},
            benchmark::TailClockAnchor{
                *tail_summary->tail_clock_end_realtime_epoch_ns,
                *tail_summary->tail_clock_end_steady_elapsed_ns,
                *tail_summary->tail_clock_end_uncertainty_ns});
    const auto full_state_sampling_valid =
        !options.engine_tail_state_sampling ||
        (tail_summary->measured_queue_depth_max.has_value() &&
         tail_summary->drain_state_sample_count >= 2U &&
         tail_summary->drain_publisher_lag_events_first.has_value() &&
         tail_summary->drain_publisher_lag_bytes_first.has_value() &&
         tail_summary->drain_publisher_lag_age_ns_first.has_value() &&
         tail_summary->drain_publisher_lag_events_last.has_value() &&
         tail_summary->drain_publisher_lag_bytes_last.has_value() &&
         tail_summary->drain_publisher_lag_age_ns_last.has_value());
    if (!tail_clock_valid) {
      report_durable_error("telemetry", "tail_clock_anchor_invalid");
    }
    if (!tail_clock_valid || tail_summary->sampler_error || tail_summary->aggregate_overflow ||
        tail_summary->telemetry_dropped_samples != 0U ||
        tail_summary->measured_sync_count != *measured_group_commits ||
        tail_summary->measured_group_sample_count != *measured_group_commits ||
        tail_summary->measured_group_sample_commands != *measured_group_commands ||
        !full_state_sampling_valid) {
      report_durable_error("telemetry", "telemetry_validation_failed");
      cleanup();
      return false;
    }
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
  const auto actual_commands_per_group =
      static_cast<double>(*measured_group_commands) /
      static_cast<double>(*measured_group_commits);
  const auto wal_bytes_delta = after_measured.wal_size_bytes - after_warmup.wal_size_bytes;
  const auto wal_mib_per_second = elapsed_ns == 0
                                      ? 0.0
                                      : static_cast<double>(wal_bytes_delta) * 1'000'000'000.0 /
                                            static_cast<double>(elapsed_ns) /
                                            (1024.0 * 1024.0);
  const auto measured_rotations = *ending_segment_count - *starting_segment_count;
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
            << " wal_prepare_workers=" << config.runtime.wal_prepare_lanes
            << " wal_parallel_prepare_min_commands="
            << config.runtime.wal_parallel_prepare_min_commands
            << " wal_group_commits=" << *measured_group_commits
            << " wal_group_commands=" << *measured_group_commands
            << " actual_parallel_prepare_groups=" << *measured_parallel_groups
            << " actual_prepare_tasks=" << *measured_prepare_tasks
            << " wal_sync_full_run_p99_us="
            << after_measured.wal_sync_latency.p99_microseconds
            << " actual_commands_per_group=" << actual_commands_per_group
            << " wal_mib_per_second=" << wal_mib_per_second
            << " wal_bytes_delta=" << wal_bytes_delta
            << " segment_count=" << *ending_segment_count
            << " measured_segment_rotations=" << measured_rotations
            << " fsync_mode=per_group completion_boundary=durable_callback"
            << " instrument_count=1 shard_count=1 producer_lanes="
            << options.engine_producer_lanes
            << " wal_path=" << wal_path << " wal_bytes=" << after_measured.wal_size_bytes;
  if (tail_summary.has_value()) {
    std::cout << " tail_telemetry=on"
              << " tail_clock_start_realtime_epoch_ns="
              << optional_metric_value(tail_summary->tail_clock_start_realtime_epoch_ns)
              << " tail_clock_start_uncertainty_ns="
              << optional_metric_value(tail_summary->tail_clock_start_uncertainty_ns)
              << " tail_clock_end_realtime_epoch_ns="
              << optional_metric_value(tail_summary->tail_clock_end_realtime_epoch_ns)
              << " tail_clock_end_steady_elapsed_ns="
              << optional_metric_value(tail_summary->tail_clock_end_steady_elapsed_ns)
              << " tail_clock_end_uncertainty_ns="
              << optional_metric_value(tail_summary->tail_clock_end_uncertainty_ns)
              << " tail_state_sampling="
              << (options.engine_tail_state_sampling ? "on" : "off")
              << " measured_sync_count=" << tail_summary->measured_sync_count
              << " measured_sync_p50_us="
              << optional_metric_value(tail_summary->measured_sync_p50_us)
              << " measured_sync_p99_us="
              << optional_metric_value(tail_summary->measured_sync_p99_us)
              << " measured_sync_p99.9_us="
              << optional_metric_value(tail_summary->measured_sync_p999_us)
              << " measured_sync_max_us="
              << optional_metric_value(tail_summary->measured_sync_max_us)
              << " measured_sync_total_us=" << tail_summary->measured_sync_total_us
              << " measured_sync_over_25ms=" << tail_summary->measured_sync_over_25ms
              << " measured_sync_over_100ms=" << tail_summary->measured_sync_over_100ms
              << " measured_sync_over_250ms=" << tail_summary->measured_sync_over_250ms
              << " measured_group_sample_count="
              << tail_summary->measured_group_sample_count
              << " measured_group_sample_commands="
              << tail_summary->measured_group_sample_commands
              << " measured_queue_depth_max="
              << optional_metric_value(tail_summary->measured_queue_depth_max)
              << " measured_publisher_lag_events_max="
              << optional_metric_value(tail_summary->measured_publisher_lag_events_max)
              << " measured_publisher_lag_bytes_max="
              << optional_metric_value(tail_summary->measured_publisher_lag_bytes_max)
              << " measured_publisher_lag_age_ns_max="
              << optional_metric_value(tail_summary->measured_publisher_lag_age_ns_max)
              << " drain_state_sample_count="
              << tail_summary->drain_state_sample_count
              << " drain_publisher_lag_events_first="
              << (options.engine_tail_state_sampling
                      ? optional_metric_value(tail_summary->drain_publisher_lag_events_first)
                      : std::string("na"))
              << " drain_publisher_lag_bytes_first="
              << (options.engine_tail_state_sampling
                      ? optional_metric_value(tail_summary->drain_publisher_lag_bytes_first)
                      : std::string("na"))
              << " drain_publisher_lag_age_ns_first="
              << (options.engine_tail_state_sampling
                      ? optional_metric_value(tail_summary->drain_publisher_lag_age_ns_first)
                      : std::string("na"))
              << " drain_publisher_lag_events_last="
              << optional_metric_value(tail_summary->drain_publisher_lag_events_last)
              << " drain_publisher_lag_bytes_last="
              << optional_metric_value(tail_summary->drain_publisher_lag_bytes_last)
              << " drain_publisher_lag_age_ns_last="
              << optional_metric_value(tail_summary->drain_publisher_lag_age_ns_last)
              << " telemetry_dropped_samples="
              << tail_summary->telemetry_dropped_samples
              << " tail_telemetry_file="
              << *options.engine_tail_telemetry_output;
  }
  std::cout << '\n';
  cleanup();
  return true;
}

}  // namespace

void print_usage() {
  std::cerr << "usage: order_books_benchmark [--iterations=N] [--warmup=N] "
               "[--workload=all|engine_durable_single_instrument|wal_write_ceiling|"
               "engine_pipeline_ceiling|engine_writer_hot_path_profile] [--data-dir=PATH] "
               "[--wal-group-size=N] "
               "[--wal-no-rotation-epoch-commands=N] "
               "[--wal-measurement-marker=PATH] "
               "[--wal-segment-size-bytes=N] "
               "[--wal-sync=none|per_group] [--wal-phase-profile=off|on] "
               "[--wal-rotation-diagnostic=control|trigger] "
               "[--writer-phase-profile=off|on] [--writer-profile-sample-every=N] "
               "[--writer-apply-subprofile=off|on] "
               "[--engine-group-size=N] "
               "[--engine-group-delay-us=N] [--engine-producer-lanes=N] "
               "[--wal-prepare-workers=1|2|4] "
               "[--wal-parallel-prepare-min-commands=N] "
               "[--pipeline-stage=STAGE] "
               "[--pipeline-command-scenario=new_crossing_pair|new_resting_cancel|"
               "amend_quantity|replace_order] "
               "[--pipeline-batch-size=N] [--pipeline-active-orders=N] "
               "[--pipeline-producer-lanes=N] "
               "[--publisher-cursor-persist-max-commands=N] "
               "[--publisher-cursor-persist-max-delay-us=N] "
               "[--engine-tail-telemetry-output=PATH] "
               "[--engine-tail-state-sampling=on|off]\n";
}

int main(const int argc, char** argv) {
  using domain::OrderBook;

  const auto options = parse_options(argc, argv);
  if (!options.has_value()) {
    std::cerr << "workload=unknown phase=cli error_code=invalid_arguments\n";
    print_usage();
    return 2;
  }
  if (options->engine_producer_lanes_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=engine_producer_lanes_invalid\n";
    print_usage();
    return 2;
  }
  if (options->wal_prepare_workers_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_prepare_workers_invalid\n";
    print_usage();
    return 2;
  }
  if (options->wal_prepare_min_commands_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_parallel_prepare_min_commands_invalid\n";
    print_usage();
    return 2;
  }
  if (options->wal_segment_size_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_segment_size_bytes_invalid\n";
    print_usage();
    return 2;
  }
  if (options->wal_phase_profile_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_phase_profile_invalid\n";
    print_usage();
    return 2;
  }
  if (options->wal_rotation_diagnostic_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_rotation_diagnostic_invalid\n";
    print_usage();
    return 2;
  }
  if (options->wal_no_rotation_epoch_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_no_rotation_epoch_commands_invalid\n";
    print_usage();
    return 2;
  }
  if (options->wal_measurement_marker_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_measurement_marker_invalid\n";
    print_usage();
    return 2;
  }
  if (options->writer_phase_profile_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=writer_phase_profile_invalid\n";
    print_usage();
    return 2;
  }
  if (options->writer_apply_subprofile_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=writer_apply_subprofile_invalid\n";
    print_usage();
    return 2;
  }
  if (options->writer_profile_sample_parse_error ||
      options->writer_profile_sample_every == 0) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=writer_profile_sample_every_invalid\n";
    print_usage();
    return 2;
  }
  if (options->engine_tail_telemetry_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=engine_tail_telemetry_output_invalid\n";
    print_usage();
    return 2;
  }
  if (options->engine_tail_state_sampling_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=engine_tail_state_sampling_invalid\n";
    print_usage();
    return 2;
  }
  if (options->pipeline_command_scenario_parse_error) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=pipeline_command_scenario_invalid\n";
    print_usage();
    return 2;
  }
  if (options->iterations == 0) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=iterations_must_be_positive\n";
    print_usage();
    return 2;
  }
  if (options->wal_group_size == 0) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_group_size_must_be_positive\n";
    print_usage();
    return 2;
  }
  if (options->wal_segment_size_bytes == 0U) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_segment_size_bytes_must_be_positive\n";
    print_usage();
    return 2;
  }
  if (options->engine_group_size == 0) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=engine_group_size_must_be_positive\n";
    print_usage();
    return 2;
  }
  if (options->engine_producer_lanes == 0) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=engine_producer_lanes_must_be_positive\n";
    print_usage();
    return 2;
  }
  if (options->engine_producer_lanes > kDurableIngressQueueCapacity) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=engine_producer_lanes_exceed_capacity\n";
    print_usage();
    return 2;
  }
  if (options->wal_prepare_workers != 1U && options->wal_prepare_workers != 2U &&
      options->wal_prepare_workers != 4U) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_prepare_workers_must_be_1_2_or_4\n";
    print_usage();
    return 2;
  }
  if (options->wal_parallel_prepare_min_commands == 0U) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_parallel_prepare_min_commands_must_be_positive\n";
    print_usage();
    return 2;
  }
  if (options->workload == WorkloadSelection::engine_writer_hot_path_profile &&
      options->engine_group_size > kDurableIngressQueueCapacity) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=engine_group_size_exceed_capacity\n";
    print_usage();
    return 2;
  }
  if (options->pipeline_batch_size == 0) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=pipeline_batch_size_must_be_positive\n";
    print_usage();
    return 2;
  }
  if (options->workload == WorkloadSelection::engine_pipeline_ceiling &&
      options->pipeline_stage == benchmark::PipelineStage::state_machine &&
      options->pipeline_batch_size % 2U != 0U) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=batch_size_must_be_even\n";
    print_usage();
    return 2;
  }
  if (options->pipeline_producer_lanes == 0) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=pipeline_producer_lanes_must_be_positive\n";
    print_usage();
    return 2;
  }
  if (options->publisher_cursor_persist_max_commands == 0 ||
      options->publisher_cursor_persist_max_delay.count() <= 0) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=publisher_cursor_persist_limits_must_be_positive\n";
    print_usage();
    return 2;
  }
  if (options->pipeline_producer_lanes > kPipelineIngressQueueCapacity) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=pipeline_producer_lanes_exceed_capacity\n";
    print_usage();
    return 2;
  }
  if (options->pipeline_options_set &&
      options->workload != WorkloadSelection::engine_pipeline_ceiling) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=pipeline_option_requires_pipeline_workload\n";
    print_usage();
    return 2;
  }
  if (options->wal_phase_profile_option_set &&
      options->workload != WorkloadSelection::wal_write_ceiling) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_phase_profile_requires_wal_workload\n";
    print_usage();
    return 2;
  }
  if (options->wal_no_rotation_epoch_option_set &&
      options->workload != WorkloadSelection::wal_write_ceiling) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_no_rotation_epoch_requires_wal_workload\n";
    print_usage();
    return 2;
  }
  if (options->wal_measurement_marker_option_set &&
      options->workload != WorkloadSelection::wal_write_ceiling) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_measurement_marker_requires_wal_workload\n";
    print_usage();
    return 2;
  }
  if (options->wal_measurement_marker_option_set &&
      options->wal_no_rotation_epoch_commands == 0U) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_measurement_marker_requires_no_rotation\n";
    print_usage();
    return 2;
  }
  if (options->wal_segment_size_option_set &&
      options->workload != WorkloadSelection::wal_write_ceiling) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_segment_size_bytes_requires_wal_workload\n";
    print_usage();
    return 2;
  }
  if (options->wal_no_rotation_epoch_commands != 0U &&
      options->wal_sync_mode != WalSyncMode::none) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_no_rotation_epoch_requires_sync_none\n";
    print_usage();
    return 2;
  }
  if (options->wal_measurement_marker_option_set &&
      options->iterations >
          options->wal_no_rotation_epoch_commands / options->wal_group_size) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_measurement_marker_requires_single_epoch\n";
    print_usage();
    return 2;
  }
  if (options->wal_rotation_diagnostic_option_set &&
      options->workload != WorkloadSelection::wal_write_ceiling) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_rotation_diagnostic_requires_wal_workload\n";
    print_usage();
    return 2;
  }
  if (options->wal_rotation_diagnostic.has_value() &&
      (options->wal_group_size != 1U || options->iterations != 1U ||
       options->wal_sync_mode != WalSyncMode::none ||
       options->wal_phase_profile != WalPhaseProfileMode::on ||
       options->wal_no_rotation_epoch_commands != 0U)) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_rotation_diagnostic_requires_b1_profiled_single_group\n";
    print_usage();
    return 2;
  }
  if (options->writer_phase_profile_option_set &&
      options->workload != WorkloadSelection::engine_writer_hot_path_profile) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=writer_phase_profile_requires_writer_profile_workload\n";
    print_usage();
    return 2;
  }
  if (options->writer_apply_subprofile_option_set &&
      options->workload != WorkloadSelection::engine_writer_hot_path_profile) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=writer_apply_subprofile_requires_writer_profile_workload\n";
    print_usage();
    return 2;
  }
  if (options->writer_profile_sample_option_set &&
      options->workload != WorkloadSelection::engine_writer_hot_path_profile) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=writer_profile_sample_requires_writer_profile_workload\n";
    print_usage();
    return 2;
  }
  if (options->writer_apply_subprofile_option_set &&
      (!options->writer_phase_profile_option_set ||
       options->writer_phase_profile != WriterPhaseProfileMode::on)) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=writer_apply_subprofile_requires_profile_on\n";
    print_usage();
    return 2;
  }
  if (options->wal_prepare_options_set &&
      options->workload != WorkloadSelection::wal_write_ceiling &&
      options->workload != WorkloadSelection::engine_writer_hot_path_profile &&
      options->workload != WorkloadSelection::engine_durable_single_instrument) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=wal_prepare_options_requires_supported_workload\n";
    print_usage();
    return 2;
  }
  if (options->writer_profile_sample_option_set &&
      (!options->writer_phase_profile_option_set ||
       options->writer_phase_profile != WriterPhaseProfileMode::on)) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=writer_profile_sample_requires_profile_on\n";
    print_usage();
    return 2;
  }
  if (options->engine_tail_telemetry_option_set &&
      options->workload != WorkloadSelection::engine_durable_single_instrument) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=engine_tail_telemetry_requires_engine_durable_workload\n";
    print_usage();
    return 2;
  }
  if (options->engine_tail_state_sampling_option_set &&
      !options->engine_tail_telemetry_option_set) {
    std::cerr << "workload=" << workload_name(options->workload)
              << " phase=cli error_code=engine_tail_state_sampling_requires_telemetry\n";
    print_usage();
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
  if (options->workload == WorkloadSelection::wal_write_ceiling) {
    return run_wal_write_ceiling(*options) ? 0 : 1;
  }
  if (options->workload == WorkloadSelection::engine_pipeline_ceiling) {
    const benchmark::PipelineBenchmarkOptions pipeline_options{
        options->iterations,
        options->warmup,
        options->pipeline_batch_size,
        options->pipeline_active_orders,
        options->engine_group_size,
        options->engine_group_delay,
        options->pipeline_producer_lanes,
        options->publisher_cursor_persist_max_commands,
        options->publisher_cursor_persist_max_delay,
        options->pipeline_command_scenario,
        options->data_directory,
    };
    return benchmark::run_pipeline_ceiling(pipeline_options, options->pipeline_stage) ? 0 : 1;
  }
  if (options->workload == WorkloadSelection::engine_writer_hot_path_profile) {
    const benchmark::WriterProfileBenchmarkOptions writer_options{
        options->iterations,
        options->warmup,
        options->engine_group_size,
        options->engine_group_delay,
        options->engine_producer_lanes,
        options->wal_prepare_workers,
        options->wal_parallel_prepare_min_commands,
        options->writer_phase_profile == WriterPhaseProfileMode::on,
        options->writer_apply_subprofile,
        options->writer_profile_sample_every,
        options->data_directory,
    };
    return benchmark::run_engine_writer_hot_path_profile(writer_options) ? 0 : 1;
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

  auto wal_options = *options;
  wal_options.data_directory.reset();
  if (!run_wal_write_ceiling(wal_options)) {
    return 1;
  }

  const auto run_id = std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "-" + std::to_string(static_cast<unsigned long long>(::getpid()));
  std::error_code ignored;
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
