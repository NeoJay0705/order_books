#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace order_books::benchmark {

struct WriterProfileBenchmarkOptions {
  std::uint64_t iterations{};
  std::uint64_t warmup{};
  std::size_t group_size{};
  std::chrono::microseconds group_delay{};
  std::size_t producer_lanes{};
  std::size_t wal_prepare_workers{1};
  std::size_t wal_parallel_prepare_min_commands{256};
  bool profile{};
  std::uint64_t profile_sample_every{1};
  std::optional<std::filesystem::path> data_directory;
};

[[nodiscard]] bool run_engine_writer_hot_path_profile(
    const WriterProfileBenchmarkOptions& options);

}  // namespace order_books::benchmark
