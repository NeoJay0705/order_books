#include "engine_writer_profile_benchmark.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include "order_books/event_sink.hpp"
#include "order_books/engine.hpp"
#include "engine_tail_telemetry.hpp"
#include "persistence/wal.hpp"
#include "runtime/shard_runtime.hpp"
#include "support/thread_name.hpp"
#include "thread_resource_telemetry.hpp"

namespace order_books::benchmark {
namespace {

using runtime::CompletionProfile;
using runtime::WriterGroupProfile;
using runtime::WriterProfileCollector;

constexpr auto kPhaseTimeout = std::chrono::seconds(60);
constexpr std::size_t kIngressCapacity = 65'536;

std::vector<std::string> expected_thread_roles(const std::size_t prepare_workers) {
  std::vector<std::string> roles{"ob-bench", "ob-wr-1", "ob-cmp-1", "ob-pub-1"};
  for (std::size_t lane = 1U; lane < prepare_workers; ++lane) {
    roles.push_back("ob-wp-1-" + std::to_string(lane));
  }
  return roles;
}

bool report_thread_resource_error(const std::string_view phase,
                                  const ThreadResourceError& error) {
  std::cerr << "workload=engine_writer_hot_path_profile phase=" << phase
            << " error_code=" << error.code;
  if (!error.detail.empty()) {
    std::cerr << " detail=" << error.detail;
  }
  std::cerr << '\n';
  return false;
}

bool print_thread_resource_deltas(const std::vector<ThreadResourceDelta>& deltas,
                                  const std::uint64_t commands) {
  for (const auto& delta : deltas) {
    const auto& sample = delta.sample;
    const auto cpu_per_million = per_million(sample.cpu_runtime_ns, commands);
    const auto runqueue_per_million = per_million(sample.runqueue_wait_ns, commands);
    const auto voluntary_per_million = per_million(
        sample.voluntary_context_switches, commands);
    const auto involuntary_per_million = per_million(
        sample.involuntary_context_switches, commands);
    const auto timeslices_per_million = per_million(sample.sched_timeslices, commands);
    std::string migrations = "not_measured";
    std::string migrations_per_million = "not_measured";
    if (sample.cpu_migrations.has_value()) {
      const auto normalized_migrations = per_million(*sample.cpu_migrations, commands);
      if (!normalized_migrations.has_value()) {
        std::cerr << "workload=engine_writer_hot_path_profile phase=diagnostics "
                     "error_code=thread_resource_normalization_failed\n";
        return false;
      }
      migrations = std::to_string(*sample.cpu_migrations);
      migrations_per_million = std::to_string(*normalized_migrations);
    }
    if (!cpu_per_million.has_value() || !runqueue_per_million.has_value() ||
        !voluntary_per_million.has_value() || !involuntary_per_million.has_value() ||
        !timeslices_per_million.has_value()) {
      std::cerr << "workload=engine_writer_hot_path_profile phase=diagnostics "
                   "error_code=thread_resource_normalization_failed\n";
      return false;
    }
    std::cout << "workload=engine_writer_hot_path_profile phase=thread_resource"
              << " role=" << sample.role << " tid=" << sample.tid
              << " cpu_runtime_ns=" << sample.cpu_runtime_ns
              << " runqueue_wait_ns=" << sample.runqueue_wait_ns
              << " sched_timeslices=" << sample.sched_timeslices
              << " voluntary_context_switches=" << sample.voluntary_context_switches
              << " involuntary_context_switches=" << sample.involuntary_context_switches
              << " cpu_migrations=" << migrations
              << " migrations_per_million=" << migrations_per_million
              << " cpu_seconds_per_million=" << *cpu_per_million / 1'000'000'000.0
              << " runqueue_wait_ns_per_million=" << *runqueue_per_million
              << " sched_timeslices_per_million=" << *timeslices_per_million
              << " voluntary_per_million=" << *voluntary_per_million
              << " involuntary_per_million=" << *involuntary_per_million << '\n';
  }
  return true;
}

class AcknowledgingSink final : public EventSink {
 public:
  Result<std::monostate> publish(const ShardId, const EngineSeq,
                                 const std::span<const Event>,
                                 const std::stop_token) override {
    return std::monostate{};
  }
};

class ProfileCollector final : public WriterProfileCollector {
 public:
  void reserve(const std::size_t completions) {
    completion_samples_.reserve(completions);
  }

  void reset() noexcept {
    writer_total_ = {};
    profiled_groups_ = 0;
    eligible_groups_ = 0;
    completion_samples_.clear();
    rejected_groups_ = 0;
    rejected_commands_ = 0;
    invalid_.store(false, std::memory_order_release);
  }

  void observe_profile_group(const bool sampled) noexcept override {
    (void)sampled;
    if (!add_checked(eligible_groups_, 1U)) {
      invalid_.store(true, std::memory_order_release);
    }
  }

  void observe_writer_group(const WriterGroupProfile& profile) noexcept override {
    if (!add_profile(writer_total_, profile) ||
        !add_checked(profiled_groups_, 1U)) {
      invalid_.store(true, std::memory_order_release);
    }
  }

  void observe_rejected_group(const std::uint64_t input_commands) noexcept override {
    if (!add_checked(rejected_groups_, 1U) ||
        !add_checked(rejected_commands_, input_commands)) {
      invalid_.store(true, std::memory_order_release);
    }
  }

  void observe_completion(const CompletionProfile& profile) noexcept override {
    try {
      completion_samples_.push_back(profile);
    } catch (...) {
      invalid_.store(true, std::memory_order_release);
      return;
    }
  }

  [[nodiscard]] bool invalid() const noexcept {
    return invalid_.load(std::memory_order_acquire);
  }
  [[nodiscard]] const WriterGroupProfile& writer_total() const noexcept {
    return writer_total_;
  }
  [[nodiscard]] std::uint64_t profiled_groups() const noexcept { return profiled_groups_; }
  [[nodiscard]] std::uint64_t eligible_groups() const noexcept { return eligible_groups_; }
  [[nodiscard]] const std::vector<CompletionProfile>& completion_samples() const noexcept {
    return completion_samples_;
  }
  [[nodiscard]] std::uint64_t rejected_groups() const noexcept { return rejected_groups_; }
  [[nodiscard]] std::uint64_t rejected_commands() const noexcept { return rejected_commands_; }

 private:
  static bool add_checked(std::uint64_t& target, const std::uint64_t value) noexcept {
    if (target > std::numeric_limits<std::uint64_t>::max() - value) {
      return false;
    }
    target += value;
    return true;
  }

  static bool add_profile(WriterGroupProfile& target,
                          const WriterGroupProfile& sample) noexcept {
    bool valid = true;
    const auto add = [&valid](std::uint64_t& destination, const std::uint64_t value) {
      if (!add_checked(destination, value)) {
        valid = false;
      }
    };
    add(target.input_commands, sample.input_commands);
    add(target.accepted_commands, sample.accepted_commands);
    add(target.writer_cycle_ns, sample.writer_cycle_ns);
    add(target.writer_service_ns, sample.writer_service_ns);
    add(target.group_collect_ns, sample.group_collect_ns);
    add(target.group_wait_ns, sample.group_wait_ns);
    add(target.admission_ns, sample.admission_ns);
    add(target.wal_append_ns, sample.wal_append_ns);
    add(target.wal_sync_ns, sample.wal_sync_ns);
    add(target.apply_ns, sample.apply_ns);
    add(target.publisher_notify_ns, sample.publisher_notify_ns);
    add(target.post_apply_ns, sample.post_apply_ns);
    add(target.completion_enqueue_ns, sample.completion_enqueue_ns);
    add(target.apply.precheck_ns, sample.apply.precheck_ns);
    add(target.apply.book_apply_ns, sample.apply.book_apply_ns);
    add(target.apply.state_update_ns, sample.apply.state_update_ns);
    add(target.apply.output_events_ns, sample.apply.output_events_ns);
    add(target.apply.producer_result_ns, sample.apply.producer_result_ns);
    add(target.apply.incremental_validation_ns,
        sample.apply.incremental_validation_ns);
    add(target.apply.commands, sample.apply.commands);
    add(target.apply.events, sample.apply.events);
    add(target.apply.trades, sample.apply.trades);
    add(target.wal.lock_wait_ns, sample.wal.lock_wait_ns);
    add(target.wal.prepare_ns, sample.wal.prepare_ns);
    add(target.wal.prepare_task_ns, sample.wal.prepare_task_ns);
    add(target.wal.parallel_prepare_groups, sample.wal.parallel_prepare_groups);
    add(target.wal.prepare_tasks, sample.wal.prepare_tasks);
    add(target.wal.plan_copy_ns, sample.wal.plan_copy_ns);
    add(target.wal.payload_encode_ns, sample.wal.payload_encode_ns);
    add(target.wal.crc_ns, sample.wal.crc_ns);
    add(target.wal.frame_assembly_ns, sample.wal.frame_assembly_ns);
    add(target.wal.chunk_copy_ns, sample.wal.chunk_copy_ns);
    add(target.wal.rotation_ns, sample.wal.rotation_ns);
    add(target.wal.write_ns, sample.wal.write_ns);
    add(target.wal.publish_ns, sample.wal.publish_ns);
    add(target.wal.frame_bytes, sample.wal.frame_bytes);
    add(target.wal.payload_bytes, sample.wal.payload_bytes);
    add(target.wal.data_write_calls, sample.wal.data_write_calls);
    add(target.wal.rotations, sample.wal.rotations);
    return valid;
  }

  WriterGroupProfile writer_total_{};
  std::uint64_t profiled_groups_{};
  std::uint64_t eligible_groups_{};
  std::vector<CompletionProfile> completion_samples_;
  std::uint64_t rejected_groups_{};
  std::uint64_t rejected_commands_{};
  std::atomic<bool> invalid_{false};
};

struct ProducerLane {
  ProducerId producer_id{};
  ProducerSeq next_sequence{1};
};

struct RunState {
  explicit RunState(const std::size_t lane_count)
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
  std::size_t outstanding{};
  std::uint64_t completed{};
  std::optional<std::string> failure;
};

Command make_command(const ProducerLane& lane, const Side side, const std::uint64_t order_id) {
  Command command;
  command.identity = CommandIdentity{lane.producer_id, 1, 1, lane.next_sequence};
  command.instrument_id = 1;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{1, order_id};
  command.payload = NewOrderPayload{side, 100, 1};
  return command;
}

void set_failure(RunState& state, std::string message) {
  if (!state.failure.has_value()) {
    state.failure = std::move(message);
  }
  state.condition.notify_all();
}

std::optional<std::uint64_t> doubled(const std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() / 2U) {
    return std::nullopt;
  }
  return value * 2U;
}

std::optional<std::uint64_t> counter_delta(const std::uint64_t after,
                                           const std::uint64_t before) {
  if (after < before) {
    return std::nullopt;
  }
  return after - before;
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

std::uint64_t elapsed_ns(const std::chrono::steady_clock::time_point start,
                         const std::chrono::steady_clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

bool prepare_data_directory(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    if (error) {
      return false;
    }
    std::filesystem::create_directories(path, error);
    return !error;
  }
  if (error || !std::filesystem::is_directory(path, error) || error) {
    return false;
  }
  std::filesystem::directory_iterator iterator(path, error);
  return !error && iterator == std::filesystem::directory_iterator{};
}

bool wait_for_phase(RunState& state, const std::uint64_t submitted,
                    const std::chrono::steady_clock::time_point deadline) {
  std::unique_lock lock(state.mutex);
  if (!state.condition.wait_until(lock, deadline, [&state, submitted] {
        return state.failure.has_value() || state.completed == submitted;
      })) {
    set_failure(state, "phase_timeout");
    return false;
  }
  return !state.failure.has_value() && state.completed == submitted && state.outstanding == 0;
}

bool run_phase(runtime::ShardRuntime& runtime, RunState& state,
               const std::uint64_t command_count, const bool collect_latency,
               std::uint64_t& next_order_id, std::uint64_t& elapsed) {
  {
    std::lock_guard lock(state.mutex);
    state.completed = 0;
    state.outstanding = 0;
    state.failure.reset();
    state.latency_samples.clear();
    if (collect_latency && command_count <= std::numeric_limits<std::size_t>::max()) {
      state.latency_samples.reserve(static_cast<std::size_t>(command_count));
    }
  }
  const auto phase_start = std::chrono::steady_clock::now();
  std::uint64_t submitted = 0;
  while (submitted < command_count) {
    std::size_t lane_index = 0;
    Command command;
    {
      std::unique_lock lock(state.mutex);
      if (!state.condition.wait_for(lock, kPhaseTimeout, [&state] {
            return state.failure.has_value() || !state.available_lanes.empty();
          })) {
        set_failure(state, "producer_lane_timeout");
        break;
      }
      if (state.failure.has_value()) {
        break;
      }
      lane_index = state.available_lanes.front();
      state.available_lanes.pop_front();
      auto& lane = state.lanes[lane_index];
      const auto command_start = std::chrono::steady_clock::now();
      command = make_command(lane, submitted % 2U == 0U ? Side::sell : Side::buy,
                             next_order_id++);
      state.started_at[lane_index] = command_start;
      state.expected[lane_index] = command.identity;
      state.in_flight[lane_index] = true;
      ++state.outstanding;
    }

    const auto submit_result = runtime.submit(
        std::move(command), [&state, lane_index, collect_latency](CommandResult result) {
          const auto completed_at = std::chrono::steady_clock::now();
          std::lock_guard lock(state.mutex);
          if (lane_index >= state.in_flight.size() || !state.in_flight[lane_index]) {
            set_failure(state, "duplicate_completion");
            return;
          }
          if (state.expected[lane_index] != result.identity) {
            set_failure(state, "completion_identity_mismatch");
          } else if (result.command_status != CommandStatus::committed) {
            set_failure(state, "command_not_committed");
          }
          if (collect_latency && !state.failure.has_value()) {
            state.latency_samples.push_back(
                elapsed_ns(state.started_at[lane_index], completed_at));
          }
          state.in_flight[lane_index] = false;
          state.available_lanes.push_back(lane_index);
          if (state.outstanding > 0) {
            --state.outstanding;
          }
          ++state.completed;
          state.condition.notify_all();
        });
    if (!submit_result.queued) {
      std::lock_guard lock(state.mutex);
      state.in_flight[lane_index] = false;
      state.available_lanes.push_back(lane_index);
      if (state.outstanding > 0) {
        --state.outstanding;
      }
      set_failure(state, "submit_rejected");
      break;
    }
    {
      std::lock_guard lock(state.mutex);
      ++state.lanes[lane_index].next_sequence;
    }
    ++submitted;
  }

  const auto deadline = std::chrono::steady_clock::now() + kPhaseTimeout;
  if (!wait_for_phase(state, submitted, deadline)) {
    return false;
  }
  elapsed = elapsed_ns(phase_start, std::chrono::steady_clock::now());
  return true;
}

using ProfileTotals = WriterGroupProfile;

bool add_checked(std::uint64_t& target, const std::uint64_t value) noexcept {
  if (target > std::numeric_limits<std::uint64_t>::max() - value) {
    return false;
  }
  target += value;
  return true;
}

std::uint64_t percentile(std::vector<std::uint64_t> samples, const std::size_t numerator,
                        const std::size_t denominator) {
  if (samples.empty()) {
    return 0;
  }
  std::sort(samples.begin(), samples.end());
  const auto index = std::min(
      samples.size() - 1U,
      (samples.size() * numerator + denominator - 1U) / denominator - 1U);
  return samples[index];
}

bool print_profile(const ProfileCollector& collector, const std::uint64_t measured_commands,
                   const std::uint64_t measured_elapsed_ns,
                   const std::uint64_t profile_sample_every,
                   const bool apply_subprofile) {
  if (collector.invalid()) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=profile "
                 "error_code=writer_profile_invalid\n";
    return false;
  }
  const ProfileTotals& totals = collector.writer_total();
  if (totals.input_commands != totals.accepted_commands ||
      collector.rejected_commands() != 0U ||
      collector.completion_samples().size() != totals.accepted_commands) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=profile "
                 "error_code=profile_count_mismatch\n";
    return false;
  }
  std::uint64_t writer_children = 0;
  std::uint64_t cycle_children = 0;
  std::uint64_t prepare_children = 0;
  std::uint64_t apply_children = 0;
  if (!add_checked(writer_children, totals.admission_ns) ||
      !add_checked(writer_children, totals.wal_append_ns) ||
      !add_checked(writer_children, totals.wal_sync_ns) ||
      !add_checked(writer_children, totals.apply_ns) ||
      !add_checked(writer_children, totals.publisher_notify_ns) ||
      !add_checked(writer_children, totals.post_apply_ns) ||
      !add_checked(writer_children, totals.completion_enqueue_ns) ||
      !add_checked(cycle_children, totals.group_collect_ns) ||
      !add_checked(cycle_children, totals.writer_service_ns) ||
      !add_checked(prepare_children, totals.wal.payload_encode_ns) ||
      !add_checked(prepare_children, totals.wal.crc_ns) ||
      !add_checked(prepare_children, totals.wal.frame_assembly_ns)) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=profile "
                 "error_code=profile_counter_overflow\n";
    return false;
  }
  if (apply_subprofile &&
      (!add_checked(apply_children, totals.apply.precheck_ns) ||
       !add_checked(apply_children, totals.apply.book_apply_ns) ||
       !add_checked(apply_children, totals.apply.state_update_ns) ||
       !add_checked(apply_children, totals.apply.output_events_ns) ||
       !add_checked(apply_children, totals.apply.producer_result_ns) ||
       !add_checked(apply_children, totals.apply.incremental_validation_ns))) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=profile "
                 "error_code=profile_counter_overflow\n";
    return false;
  }
  if (writer_children > totals.writer_service_ns ||
      cycle_children > totals.writer_cycle_ns ||
      totals.group_wait_ns > totals.group_collect_ns ||
      prepare_children > totals.wal.prepare_task_ns ||
      (apply_subprofile && apply_children > totals.apply_ns) ||
      totals.wal.chunk_copy_ns > totals.wal.plan_copy_ns) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=profile "
                 "error_code=profile_hierarchy_invalid\n";
    return false;
  }
  if (apply_subprofile && totals.apply.commands != totals.accepted_commands) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=profile "
                 "error_code=apply_profile_count_mismatch\n";
    return false;
  }
  const auto service_unattributed = totals.writer_service_ns - writer_children;
  const auto cycle_unattributed = totals.writer_cycle_ns - cycle_children;
  const auto active_collect = totals.group_collect_ns - totals.group_wait_ns;
  const auto share = [&totals](const std::uint64_t value) {
    return totals.writer_service_ns == 0
               ? 0.0
               : static_cast<double>(value) * 100.0 /
                     static_cast<double>(totals.writer_service_ns);
  };
  const auto apply_share = [&totals](const std::uint64_t value) {
    return totals.apply_ns == 0
               ? 0.0
               : static_cast<double>(value) * 100.0 /
                     static_cast<double>(totals.apply_ns);
  };
  const auto wal_share = [&totals](const std::uint64_t value) {
    return totals.wal_append_ns == 0
               ? 0.0
               : static_cast<double>(value) * 100.0 /
                     static_cast<double>(totals.wal_append_ns);
  };
  const auto derived_ceiling = [](const double ns_per_command) {
    return ns_per_command <= 0.0 ? 0.0 : 1'000'000'000.0 / ns_per_command;
  };
  const auto headroom = [](const double ceiling) {
    return ceiling <= 0.0 ? 0.0 : (ceiling / 1'000'000.0 - 1.0) * 100.0;
  };
  const auto per_command = [&totals](const std::uint64_t value) {
    return totals.accepted_commands == 0U
               ? 0.0
               : static_cast<double>(value) /
                     static_cast<double>(totals.accepted_commands);
  };
  const auto emit_ceiling = [&](const std::string_view name,
                                const double ns_per_command) {
    const bool observed = ns_per_command > 0.0;
    std::cout << ' ' << name << "_ceiling_observed=" << (observed ? "true" : "false");
    if (observed) {
      const auto ceiling = derived_ceiling(ns_per_command);
      std::cout << ' ' << name << "_derived_ceiling_commands_per_second=" << ceiling
                << ' ' << name << "_headroom_percent=" << headroom(ceiling);
    }
  };
  const auto emit_phase_ceiling = [&](const std::string_view name,
                                      const std::uint64_t total_ns) {
    const auto ns_per_command = per_command(total_ns);
    emit_ceiling(name, ns_per_command);
  };
  const auto emit_apply_phase = [&](const std::string_view name,
                                    const std::uint64_t total_ns) {
    const auto ns_per_command = totals.apply.commands == 0U
                                    ? 0.0
                                    : static_cast<double>(total_ns) /
                                          static_cast<double>(totals.apply.commands);
    emit_ceiling(name, ns_per_command);
    std::cout << ' ' << name << "_apply_parent_share_percent="
              << apply_share(total_ns) << ' ' << name
              << "_writer_service_share_percent=" << share(total_ns);
  };
  const auto emit_wal_phase = [&](const std::string_view name,
                                  const std::uint64_t total_ns) {
    const auto ns_per_command = per_command(total_ns);
    emit_ceiling(name, ns_per_command);
    std::cout << ' ' << name << "_wal_parent_share_percent=" << wal_share(total_ns)
              << ' ' << name << "_writer_service_share_percent=" << share(total_ns);
  };
  const auto prepare_remainder = totals.wal.prepare_task_ns - prepare_children;
  const auto plan_copy_remainder = totals.wal.plan_copy_ns - totals.wal.chunk_copy_ns;
  const auto completion_residence = [&collector](const auto selector) {
    std::vector<std::uint64_t> values;
    values.reserve(collector.completion_samples().size());
    for (const auto& sample : collector.completion_samples()) {
      values.push_back(selector(sample));
    }
    return values;
  };
  const auto residence = completion_residence(
      [](const CompletionProfile& sample) { return sample.queue_residence_ns; });
  const auto callback = completion_residence(
      [](const CompletionProfile& sample) { return sample.callback_service_ns; });
  std::uint64_t callback_total = 0;
  std::uint64_t max_depth = 0;
  std::uint64_t completion_count = 0;
  for (const auto& sample : collector.completion_samples()) {
    if (!add_checked(completion_count, sample.completions) ||
        !add_checked(callback_total, sample.callback_service_ns)) {
      std::cerr << "workload=engine_writer_hot_path_profile phase=profile "
                   "error_code=profile_counter_overflow\n";
      return false;
    }
    max_depth = std::max(max_depth, sample.max_queue_depth);
  }
  if (completion_count != totals.accepted_commands) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=profile "
                 "error_code=completion_count_mismatch\n";
    return false;
  }
  const auto service_rps = callback_total == 0
                               ? 0.0
                               : static_cast<double>(completion_count) * 1'000'000'000.0 /
                                     static_cast<double>(callback_total);
  const auto throughput = measured_elapsed_ns == 0
                              ? 0.0
                              : static_cast<double>(measured_commands) * 1'000'000'000.0 /
                                    static_cast<double>(measured_elapsed_ns);
  std::cout << "workload=engine_writer_hot_path_profile phase=profile"
            << " writer_profile_sample_every=" << profile_sample_every
            << " writer_apply_subprofile=" << (apply_subprofile ? "on" : "off")
            << " eligible_groups=" << collector.eligible_groups()
            << " profiled_groups=" << collector.profiled_groups()
            << " profiled_input_commands=" << totals.input_commands
            << " profiled_accepted_commands=" << totals.accepted_commands
            << " profiled_commands=" << totals.accepted_commands
            << " completion_profiled_commands=" << completion_count
            << " rejected_groups=" << collector.rejected_groups()
            << " rejected_commands=" << collector.rejected_commands()
            << " commands_per_second=" << throughput
            << " writer_service_ns=" << totals.writer_service_ns
            << " writer_service_ns_per_command=" << per_command(totals.writer_service_ns)
            << " writer_cycle_ns=" << totals.writer_cycle_ns
            << " writer_cycle_ns_per_command=" << per_command(totals.writer_cycle_ns)
            << " group_collect_ns=" << totals.group_collect_ns
            << " group_collect_ns_per_command=" << per_command(totals.group_collect_ns)
            << " group_wait_ns=" << totals.group_wait_ns
            << " group_wait_ns_per_command=" << per_command(totals.group_wait_ns)
            << " active_collect_ns=" << active_collect
            << " active_collect_ns_per_command=" << per_command(active_collect)
            << " admission_ns=" << totals.admission_ns
            << " wal_append_ns=" << totals.wal_append_ns
            << " wal_sync_ns=" << totals.wal_sync_ns
            << " apply_ns=" << totals.apply_ns
            << " publisher_notify_ns=" << totals.publisher_notify_ns
            << " post_apply_ns=" << totals.post_apply_ns
            << " completion_enqueue_ns=" << totals.completion_enqueue_ns
            << " writer_unattributed_ns=" << service_unattributed
            << " cycle_unattributed_ns=" << cycle_unattributed
            << " admission_share_percent=" << share(totals.admission_ns)
            << " admission_ns_per_command=" << per_command(totals.admission_ns)
            << " wal_append_share_percent=" << share(totals.wal_append_ns)
            << " wal_append_ns_per_command=" << per_command(totals.wal_append_ns)
            << " wal_sync_share_percent=" << share(totals.wal_sync_ns)
            << " wal_sync_ns_per_command=" << per_command(totals.wal_sync_ns)
            << " apply_share_percent=" << share(totals.apply_ns)
            << " apply_ns_per_command=" << per_command(totals.apply_ns)
            << " publisher_notify_share_percent=" << share(totals.publisher_notify_ns)
            << " publisher_notify_ns_per_command="
            << per_command(totals.publisher_notify_ns)
            << " post_apply_share_percent=" << share(totals.post_apply_ns)
            << " post_apply_ns_per_command=" << per_command(totals.post_apply_ns)
            << " completion_enqueue_share_percent=" << share(totals.completion_enqueue_ns)
            << " completion_enqueue_ns_per_command="
            << per_command(totals.completion_enqueue_ns)
            << " writer_unattributed_share_percent=" << share(service_unattributed)
            << " cycle_unattributed_ns_per_command=" << per_command(cycle_unattributed);
  emit_ceiling("writer_service", per_command(totals.writer_service_ns));
  if (apply_subprofile) {
    const auto apply_children_ns_per_command =
        totals.apply.commands == 0U
            ? 0.0
            : static_cast<double>(apply_children) /
                  static_cast<double>(totals.apply.commands);
    const auto apply_unattributed = totals.apply_ns - apply_children;
    const auto apply_unattributed_ns_per_command = totals.apply.commands == 0U
                                                       ? 0.0
                                                       : static_cast<double>(apply_unattributed) /
                                                             static_cast<double>(
                                                                 totals.apply.commands);
    std::cout << " apply_precheck_ns=" << totals.apply.precheck_ns
              << " apply_precheck_ns_per_command="
              << (totals.apply.commands == 0U
                      ? 0.0
                      : static_cast<double>(totals.apply.precheck_ns) /
                            static_cast<double>(totals.apply.commands))
              << " apply_book_apply_ns=" << totals.apply.book_apply_ns
              << " apply_book_apply_ns_per_command="
              << (totals.apply.commands == 0U
                      ? 0.0
                      : static_cast<double>(totals.apply.book_apply_ns) /
                            static_cast<double>(totals.apply.commands))
              << " apply_state_update_ns=" << totals.apply.state_update_ns
              << " apply_state_update_ns_per_command="
              << (totals.apply.commands == 0U
                      ? 0.0
                      : static_cast<double>(totals.apply.state_update_ns) /
                            static_cast<double>(totals.apply.commands))
              << " apply_output_events_ns=" << totals.apply.output_events_ns
              << " apply_output_events_ns_per_command="
              << (totals.apply.commands == 0U
                      ? 0.0
                      : static_cast<double>(totals.apply.output_events_ns) /
                            static_cast<double>(totals.apply.commands))
              << " apply_producer_result_ns=" << totals.apply.producer_result_ns
              << " apply_producer_result_ns_per_command="
              << (totals.apply.commands == 0U
                      ? 0.0
                      : static_cast<double>(totals.apply.producer_result_ns) /
                            static_cast<double>(totals.apply.commands))
              << " apply_incremental_validation_ns="
              << totals.apply.incremental_validation_ns
              << " apply_incremental_validation_ns_per_command="
              << (totals.apply.commands == 0U
                      ? 0.0
                      : static_cast<double>(totals.apply.incremental_validation_ns) /
                            static_cast<double>(totals.apply.commands))
              << " apply_children_ns=" << apply_children
              << " apply_children_ns_per_command=" << apply_children_ns_per_command
              << " apply_unattributed_ns=" << apply_unattributed
              << " apply_unattributed_ns_per_command=" << apply_unattributed_ns_per_command
              << " apply_unattributed_apply_parent_share_percent="
              << apply_share(apply_unattributed)
              << " apply_unattributed_writer_service_share_percent="
              << share(apply_unattributed)
              << " apply_commands=" << totals.apply.commands
              << " apply_events=" << totals.apply.events
              << " apply_trades=" << totals.apply.trades;
  }
  emit_phase_ceiling("active_collect", active_collect);
  emit_phase_ceiling("admission", totals.admission_ns);
  emit_phase_ceiling("wal_append", totals.wal_append_ns);
  emit_phase_ceiling("wal_sync", totals.wal_sync_ns);
  emit_phase_ceiling("apply", totals.apply_ns);
  emit_phase_ceiling("publisher_notify", totals.publisher_notify_ns);
  emit_phase_ceiling("post_apply", totals.post_apply_ns);
  emit_phase_ceiling("completion_enqueue", totals.completion_enqueue_ns);
  if (apply_subprofile) {
    emit_apply_phase("apply_precheck", totals.apply.precheck_ns);
    emit_apply_phase("apply_book_apply", totals.apply.book_apply_ns);
    emit_apply_phase("apply_state_update", totals.apply.state_update_ns);
    emit_apply_phase("apply_output_events", totals.apply.output_events_ns);
    emit_apply_phase("apply_producer_result", totals.apply.producer_result_ns);
    emit_apply_phase("apply_incremental_validation", totals.apply.incremental_validation_ns);
  }
  emit_wal_phase("wal_prepare", totals.wal.prepare_ns);
  emit_wal_phase("wal_prepare_task", totals.wal.prepare_task_ns);
  emit_wal_phase("wal_payload_encode", totals.wal.payload_encode_ns);
  emit_wal_phase("wal_crc", totals.wal.crc_ns);
  emit_wal_phase("wal_frame_assembly", totals.wal.frame_assembly_ns);
  emit_wal_phase("wal_plan_copy", totals.wal.plan_copy_ns);
  emit_wal_phase("wal_chunk_copy", totals.wal.chunk_copy_ns);
  emit_wal_phase("wal_lock_wait", totals.wal.lock_wait_ns);
  emit_wal_phase("wal_publish", totals.wal.publish_ns);
  emit_wal_phase("wal_write", totals.wal.write_ns);
  emit_wal_phase("wal_rotation", totals.wal.rotation_ns);
  std::cout << " wal_prepare_ns=" << totals.wal.prepare_ns
            << " wal_prepare_ns_per_command=" << per_command(totals.wal.prepare_ns)
            << " wal_prepare_task_ns=" << totals.wal.prepare_task_ns
            << " wal_prepare_task_ns_per_command="
            << per_command(totals.wal.prepare_task_ns)
            << " wal_parallel_prepare_groups=" << totals.wal.parallel_prepare_groups
            << " wal_prepare_tasks=" << totals.wal.prepare_tasks
            << " wal_payload_encode_ns=" << totals.wal.payload_encode_ns
            << " wal_payload_encode_ns_per_command="
            << per_command(totals.wal.payload_encode_ns)
            << " wal_crc_ns=" << totals.wal.crc_ns
            << " wal_crc_ns_per_command=" << per_command(totals.wal.crc_ns)
            << " wal_frame_assembly_ns=" << totals.wal.frame_assembly_ns
            << " wal_frame_assembly_ns_per_command="
            << per_command(totals.wal.frame_assembly_ns)
            << " wal_prepare_task_remainder_ns=" << prepare_remainder
            << " wal_prepare_task_remainder_ns_per_command="
            << per_command(prepare_remainder)
            << " wal_plan_copy_ns=" << totals.wal.plan_copy_ns
            << " wal_plan_copy_ns_per_command=" << per_command(totals.wal.plan_copy_ns)
            << " wal_chunk_copy_ns=" << totals.wal.chunk_copy_ns
            << " wal_chunk_copy_ns_per_command=" << per_command(totals.wal.chunk_copy_ns)
            << " wal_plan_copy_remainder_ns=" << plan_copy_remainder
            << " wal_plan_copy_remainder_ns_per_command=" << per_command(plan_copy_remainder)
            << " wal_lock_wait_ns=" << totals.wal.lock_wait_ns
            << " wal_lock_wait_ns_per_command=" << per_command(totals.wal.lock_wait_ns)
            << " wal_publish_ns=" << totals.wal.publish_ns
            << " wal_publish_ns_per_command=" << per_command(totals.wal.publish_ns)
            << " wal_rotation_ns=" << totals.wal.rotation_ns
            << " wal_rotation_ns_per_command=" << per_command(totals.wal.rotation_ns)
            << " wal_write_ns=" << totals.wal.write_ns
            << " wal_write_ns_per_command=" << per_command(totals.wal.write_ns)
            << " wal_frame_bytes=" << totals.wal.frame_bytes
            << " wal_payload_bytes=" << totals.wal.payload_bytes
            << " wal_data_write_calls=" << totals.wal.data_write_calls
            << " wal_rotations=" << totals.wal.rotations
            << " completion_count=" << completion_count
            << " completion_service_rps=" << service_rps
            << " completion_queue_p50_us=" << percentile(residence, 50, 100) / 1'000.0
            << " completion_queue_p99_us=" << percentile(residence, 99, 100) / 1'000.0
            << " completion_queue_p99_9_us=" << percentile(residence, 999, 1000) / 1'000.0
            << " completion_queue_max_us=" << percentile(residence, 1000, 1000) / 1'000.0
            << " completion_callback_p50_us=" << percentile(callback, 50, 100) / 1'000.0
            << " completion_callback_p99_us=" << percentile(callback, 99, 100) / 1'000.0
            << " completion_callback_p99_9_us=" << percentile(callback, 999, 1000) / 1'000.0
            << " completion_callback_max_us=" << percentile(callback, 1000, 1000) / 1'000.0
            << " completion_max_queue_depth=" << max_depth << '\n';
  return true;
}

}  // namespace

bool run_engine_writer_hot_path_profile(const WriterProfileBenchmarkOptions& options) {
  if (options.iterations == 0 || options.group_size == 0 ||
      options.group_size > kIngressCapacity ||
      options.group_delay.count() < 0 || options.producer_lanes == 0 ||
      options.producer_lanes > kIngressCapacity || options.profile_sample_every == 0 ||
      (options.wal_prepare_workers != 1U && options.wal_prepare_workers != 2U &&
       options.wal_prepare_workers != 4U) ||
      options.wal_parallel_prepare_min_commands == 0U) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                 "error_code=invalid_profile_options\n";
    return false;
  }
  if (options.thread_diagnostics != options.tail_telemetry_output.has_value()) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                 "error_code=writer_diagnostics_output_mismatch\n";
    return false;
  }
  const auto measured_commands = doubled(options.iterations);
  const auto warmup_commands = doubled(options.warmup);
  if (!measured_commands.has_value() || !warmup_commands.has_value()) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                 "error_code=iteration_count_overflow\n";
    return false;
  }
  if (*warmup_commands > std::numeric_limits<std::uint64_t>::max() - *measured_commands) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                 "error_code=iteration_count_overflow\n";
    return false;
  }
  if (*warmup_commands > std::numeric_limits<std::size_t>::max() ||
      *measured_commands > std::numeric_limits<std::size_t>::max()) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                 "error_code=writer_profile_sample_capacity_overflow\n";
    return false;
  }
  const auto expected_total = *warmup_commands + *measured_commands;
  const bool owned_directory = !options.data_directory.has_value();
  const auto directory = options.data_directory.value_or(
      std::filesystem::temp_directory_path() /
      ("order_books_writer_profile-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
  if (!prepare_data_directory(directory)) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                 "error_code=data_directory_not_empty_or_unavailable\n";
    return false;
  }
  const auto cleanup = [&] {
    if (owned_directory) {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
  };
  if (options.tail_telemetry_output.has_value()) {
    const auto output_parent = options.tail_telemetry_output->parent_path().empty()
                                   ? std::filesystem::path(".")
                                   : options.tail_telemetry_output->parent_path();
    std::error_code output_error;
    const auto output_status =
        std::filesystem::symlink_status(*options.tail_telemetry_output, output_error);
    if (output_error == std::errc::no_such_file_or_directory) {
      output_error.clear();
    }
    if (output_error || std::filesystem::exists(output_status) ||
        !std::filesystem::is_directory(output_parent, output_error) || output_error ||
        ::access(output_parent.c_str(), W_OK | X_OK) != 0) {
      std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                   "error_code=writer_tail_telemetry_output_unavailable\n";
      cleanup();
      return false;
    }
  }
  if (options.thread_diagnostics) {
    support::set_current_thread_name("ob-bench");
  }

  EngineConfig config;
  config.data_directory = directory;
  config.shard_ids = {1};
  config.instruments = {InstrumentConfig{1, 1, 1, 1}};
  config.runtime.ingress_queue_capacity = kIngressCapacity;
  config.runtime.group_commit_max_commands = options.group_size;
  config.runtime.group_commit_max_delay = options.group_delay;
  config.runtime.wal_prepare_lanes = options.wal_prepare_workers;
  config.runtime.wal_parallel_prepare_min_commands =
      options.wal_parallel_prepare_min_commands;
  config.runtime.snapshot_interval_commands = std::numeric_limits<std::size_t>::max();
  config.runtime.snapshot_interval = std::chrono::hours(24);
  config.runtime.event_replay_snapshot_interval_commands =
      std::numeric_limits<std::size_t>::max();
  config.runtime.event_replay_snapshot_interval = std::chrono::hours(24);

  AcknowledgingSink event_sink;
  NullMetricsSink null_metrics_sink;
  MetricsSink* metrics_sink = &null_metrics_sink;
  std::unique_ptr<EngineTailTelemetry> tail_telemetry;
  if (options.thread_diagnostics) {
    try {
      const auto measured_groups =
          (*measured_commands - 1U) / static_cast<std::uint64_t>(options.group_size) + 1U;
      tail_telemetry = std::make_unique<EngineTailTelemetry>(
          static_cast<std::size_t>(std::min(measured_groups,
                                            static_cast<std::uint64_t>(
                                                EngineTailTelemetry::kMaxMetricSamples))));
      if (!tail_telemetry->reserve()) {
        std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                     "error_code=writer_tail_telemetry_reserve_failed\n";
        cleanup();
        return false;
      }
      metrics_sink = tail_telemetry.get();
    } catch (...) {
      std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                   "error_code=writer_tail_telemetry_reserve_failed\n";
      cleanup();
      return false;
    }
  }
  ProfileCollector collector;
  if (options.profile) {
    try {
      const auto measured_groups = (*measured_commands - 1U) /
                                       static_cast<std::uint64_t>(options.group_size) + 1U;
      const auto sampled_groups =
          (measured_groups - 1U) / options.profile_sample_every + 1U;
      const auto group_size = static_cast<std::uint64_t>(options.group_size);
      const auto sampled_capacity =
          sampled_groups > std::numeric_limits<std::uint64_t>::max() / group_size
              ? *measured_commands
              : std::min(*measured_commands, sampled_groups * group_size);
      collector.reserve(static_cast<std::size_t>(sampled_capacity));
    } catch (...) {
      std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                   "error_code=writer_profile_reserve_failed\n";
      cleanup();
      return false;
    }
  }
  auto opened = runtime::ShardRuntime::open(
      1, config, event_sink, *metrics_sink, options.profile ? &collector : nullptr,
      runtime::WriterProfileOptions{options.profile_sample_every,
                                    options.apply_subprofile});
  if (std::holds_alternative<Error>(opened)) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=open error_code=engine_open_failed\n";
    cleanup();
    return false;
  }
  auto runtime = std::get<std::unique_ptr<runtime::ShardRuntime>>(std::move(opened));
  if (options.profile) {
    runtime->set_writer_profile_phase_active(false);
  }
  runtime->start();
  RunState state(options.producer_lanes);
  std::uint64_t next_order_id = 1;
  std::uint64_t warmup_elapsed = 0;
  if (!run_phase(*runtime, state, *warmup_commands, false, next_order_id, warmup_elapsed)) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=warmup error_code=phase_failed\n";
    (void)runtime->stop();
    cleanup();
    return false;
  }
  std::vector<std::string> expected_roles;
  std::optional<ThreadResourceSnapshot> resource_before;
  std::optional<ThreadResourceSnapshot> resource_after;
  if (options.thread_diagnostics) {
    expected_roles = expected_thread_roles(options.wal_prepare_workers);
    const auto captured = capture_thread_resources(expected_roles);
    if (std::holds_alternative<ThreadResourceError>(captured)) {
      report_thread_resource_error("diagnostics", std::get<ThreadResourceError>(captured));
      (void)runtime->stop();
      cleanup();
      return false;
    }
    resource_before = std::get<ThreadResourceSnapshot>(captured);
  }
  if (tail_telemetry && !tail_telemetry->begin_measured()) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=setup "
                 "error_code=writer_tail_telemetry_begin_failed\n";
    (void)runtime->stop();
    cleanup();
    return false;
  }
  const auto prepare_stats_before_measured = runtime->wal_prepare_stats();
  if (options.profile) {
    collector.reset();
    runtime->reset_writer_profile_phase();
  }
  const auto metrics_before_measured = runtime->metrics();
  std::uint64_t measured_elapsed = 0;
  if (!run_phase(*runtime, state, *measured_commands, true, next_order_id, measured_elapsed)) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=measured error_code=phase_failed\n";
    (void)runtime->stop();
    cleanup();
    return false;
  }
  if (tail_telemetry) {
    tail_telemetry->stop_collection();
  }
  if (options.thread_diagnostics) {
    const auto captured = capture_thread_resources(expected_roles);
    if (std::holds_alternative<ThreadResourceError>(captured)) {
      report_thread_resource_error("diagnostics", std::get<ThreadResourceError>(captured));
      (void)runtime->stop();
      cleanup();
      return false;
    }
    resource_after = std::get<ThreadResourceSnapshot>(captured);
  }
  const auto prepare_stats_after_measured = runtime->wal_prepare_stats();
  const auto actual_prepare_stats =
      prepare_stats_delta(prepare_stats_before_measured, prepare_stats_after_measured);
  if (!actual_prepare_stats.has_value()) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=measured "
                 "error_code=wal_prepare_stats_regressed\n";
    (void)runtime->stop();
    cleanup();
    return false;
  }
  auto measured_latency_samples = state.latency_samples;
  const auto snapshot = runtime->metrics();
  const auto stop_status = runtime->stop();
  if (std::holds_alternative<Error>(stop_status)) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=stop error_code=engine_stop_failed\n";
    cleanup();
    return false;
  }
  const auto measured_group_commits =
      counter_delta(snapshot.wal_group_commits, metrics_before_measured.wal_group_commits);
  const auto measured_group_commands =
      counter_delta(snapshot.wal_group_commands, metrics_before_measured.wal_group_commands);
  if (!measured_group_commits.has_value() || !measured_group_commands.has_value() ||
      *measured_group_commits == 0U) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=validation "
                 "error_code=measured_group_counter_regressed\n";
    cleanup();
    return false;
  }
  runtime.reset();

  std::optional<TailTelemetrySummary> tail_summary;
  if (tail_telemetry) {
    tail_summary = tail_telemetry->summary();
    if (tail_summary->telemetry_dropped_samples != 0U ||
        tail_summary->aggregate_overflow || tail_summary->sampler_error) {
      std::cerr << "workload=engine_writer_hot_path_profile phase=diagnostics "
                   "error_code=writer_tail_telemetry_invalid\n";
      cleanup();
      return false;
    }
    if (tail_summary->measured_sync_count != *measured_group_commits ||
        tail_summary->measured_group_sample_count != *measured_group_commits ||
        tail_summary->measured_group_sample_commands != *measured_group_commands) {
      std::cerr << "workload=engine_writer_hot_path_profile phase=diagnostics "
                   "error_code=writer_tail_telemetry_count_mismatch\n";
      cleanup();
      return false;
    }
    if (!tail_telemetry->write_csv(*options.tail_telemetry_output)) {
      std::cerr << "workload=engine_writer_hot_path_profile phase=diagnostics "
                   "error_code=writer_tail_telemetry_invalid\n";
      cleanup();
      return false;
    }
  }
  std::optional<std::vector<ThreadResourceDelta>> resource_deltas;
  if (options.thread_diagnostics) {
    const auto delta = subtract_thread_resources(*resource_before, *resource_after);
    if (std::holds_alternative<ThreadResourceError>(delta)) {
      report_thread_resource_error("diagnostics", std::get<ThreadResourceError>(delta));
      cleanup();
      return false;
    }
    resource_deltas = std::get<std::vector<ThreadResourceDelta>>(delta);
    if (!print_thread_resource_deltas(*resource_deltas, *measured_commands)) {
      cleanup();
      return false;
    }
  }

  auto wal_result = storage::Wal::open(directory / "shard-1" / "wal", 1,
                                       config.runtime.wal_segment_size,
                                       storage::WalPrepareOptions{
                                           config.runtime.wal_prepare_lanes,
                                           config.runtime.wal_parallel_prepare_min_commands});
  if (std::holds_alternative<Error>(wal_result)) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=recovery error_code=wal_reopen_failed\n";
    cleanup();
    return false;
  }
  auto wal = std::get<std::unique_ptr<storage::Wal>>(std::move(wal_result));
  auto replayed_result = wal->replay();
  if (std::holds_alternative<Error>(replayed_result)) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=recovery "
                 "error_code=durable_head_mismatch\n";
    cleanup();
    return false;
  }
  const auto& replayed = std::get<std::vector<domain::CommittedCommand>>(replayed_result);
  if (replayed.size() != expected_total || wal->last_engine_seq() != expected_total) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=recovery "
                 "error_code=durable_head_mismatch\n";
    cleanup();
    return false;
  }
  for (std::size_t index = 0; index < replayed.size(); ++index) {
    const auto& record = replayed[index];
    if (record.engine_seq != static_cast<EngineSeq>(index + 1U) ||
        record.command.instrument_id != 1 ||
        record.command.command_type != CommandType::new_order) {
      std::cerr << "workload=engine_writer_hot_path_profile phase=recovery "
                   "error_code=engine_sequence_mismatch\n";
      cleanup();
      return false;
    }
  }
  if (snapshot.commands != expected_total ||
      snapshot.wal_group_commands != expected_total || snapshot.wal_group_commits == 0U ||
      snapshot.trades != expected_total / 2U ||
      snapshot.active_orders != 0 || snapshot.active_price_levels != 0 ||
      measured_latency_samples.size() != *measured_commands) {
    std::cerr << "workload=engine_writer_hot_path_profile phase=validation "
                 "error_code=counter_mismatch\n";
    cleanup();
    return false;
  }
  const auto percentile_latency = percentile(measured_latency_samples, 50, 100);
  std::cout << "workload=engine_writer_hot_path_profile phase=summary"
            << " profile=" << (options.profile ? "on" : "off")
            << " commands=" << *measured_commands
            << " commands_per_second="
            << (measured_elapsed == 0
                    ? 0.0
                    : static_cast<double>(*measured_commands) * 1'000'000'000.0 /
                          static_cast<double>(measured_elapsed))
            << " p50_us=" << percentile_latency / 1'000.0
            << " p99_us=" << percentile(measured_latency_samples, 99, 100) / 1'000.0
            << " p99_9_us=" << percentile(measured_latency_samples, 999, 1000) / 1'000.0
            << " max_us=" << percentile(measured_latency_samples, 1000, 1000) / 1'000.0
            << " elapsed_ms=" << measured_elapsed / 1'000'000.0
            << " group_size=" << options.group_size
            << " group_delay_us=" << options.group_delay.count()
            << " producer_lanes=" << options.producer_lanes
            << " writer_apply_subprofile="
            << (options.apply_subprofile ? "on" : "off")
            << " wal_prepare_workers=" << options.wal_prepare_workers
            << " wal_parallel_prepare_min_commands="
            << options.wal_parallel_prepare_min_commands
            << " actual_parallel_prepare_groups=" << actual_prepare_stats->parallel_groups
            << " actual_prepare_tasks=" << actual_prepare_stats->tasks
            << " wal_group_commits=" << *measured_group_commits
            << " wal_group_commands=" << *measured_group_commands
            << " actual_commands_per_group="
            << (*measured_group_commits == 0
                    ? 0.0
                    : static_cast<double>(*measured_group_commands) /
                          static_cast<double>(*measured_group_commits))
            << " publisher_lag_events=" << snapshot.event_publish_lag_events
            << " publisher_lag_bytes=" << snapshot.event_publish_lag_bytes
            << " publisher_lag_age_ns=" << snapshot.event_publish_lag_age_ns
            << " wal_size_bytes=" << snapshot.wal_size_bytes
            << " instrument_count=1 shard_count=1 fsync_mode=per_group";
  if (tail_summary.has_value()) {
    std::cout << " thread_diagnostics=on"
              << " writer_tail_telemetry=on"
              << " writer_sync_count=" << tail_summary->measured_sync_count
              << " writer_group_sample_count=" << tail_summary->measured_group_sample_count
              << " writer_group_sample_commands="
              << tail_summary->measured_group_sample_commands
              << " writer_sync_p50_us="
              << (tail_summary->measured_sync_p50_us.has_value()
                      ? std::to_string(*tail_summary->measured_sync_p50_us)
                      : std::string("na"))
              << " writer_sync_p99_us="
              << (tail_summary->measured_sync_p99_us.has_value()
                      ? std::to_string(*tail_summary->measured_sync_p99_us)
                      : std::string("na"))
              << " writer_sync_p99_9_us="
              << (tail_summary->measured_sync_p999_us.has_value()
                      ? std::to_string(*tail_summary->measured_sync_p999_us)
                      : std::string("na"))
              << " writer_sync_max_us="
              << (tail_summary->measured_sync_max_us.has_value()
                      ? std::to_string(*tail_summary->measured_sync_max_us)
                      : std::string("na"))
              << " writer_sync_total_us=" << tail_summary->measured_sync_total_us
              << " writer_sync_over_25ms=" << tail_summary->measured_sync_over_25ms
              << " writer_sync_over_100ms=" << tail_summary->measured_sync_over_100ms
              << " writer_sync_over_250ms=" << tail_summary->measured_sync_over_250ms
              << " writer_tail_telemetry_file=" << *options.tail_telemetry_output;
  }
  std::cout << " correctness_verified=true\n";
  if (options.profile &&
      !print_profile(collector, *measured_commands, measured_elapsed,
                     options.profile_sample_every, options.apply_subprofile)) {
    cleanup();
    return false;
  }
  cleanup();
  return true;
}

}  // namespace order_books::benchmark
