#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>
#include <unistd.h>

#include "domain/order_book.hpp"
#include "domain/invariant_checker.hpp"
#include "domain/state_machine.hpp"
#include "persistence/snapshot_store.hpp"
#include "persistence/wal.hpp"

namespace {

using namespace order_books;

constexpr std::uint64_t kIterations = 2'000;
constexpr std::uint64_t kWarmup = 100;

struct BenchmarkOptions {
  std::uint64_t iterations{kIterations};
  std::uint64_t warmup{kWarmup};
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

std::optional<BenchmarkOptions> parse_options(const int argc, char** argv) {
  BenchmarkOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (const auto iterations = parse_positive_option(argument, "--iterations=")) {
      options.iterations = *iterations;
    } else if (const auto warmup = parse_positive_option(argument, "--warmup=")) {
      options.warmup = *warmup;
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

}  // namespace

int main(const int argc, char** argv) {
  using domain::OrderBook;

  const auto options = parse_options(argc, argv);
  if (!options.has_value() || options->iterations == 0) {
    std::cerr << "usage: order_books_benchmark [--iterations=N] [--warmup=N]\n";
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
  if (!run_workload(
          "recovery_snapshot_plus_wal", BenchmarkOptions{recovery_iterations, options->warmup},
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
