#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace order_books::benchmark {

enum class PipelineStage {
  all,
  state_machine,
  invariant_validation,
  metrics,
  runtime_handoff,
  publisher_drain,
};

struct PipelineBenchmarkOptions {
  std::uint64_t iterations{};
  std::uint64_t warmup{};
  std::size_t batch_size{};
  std::size_t active_orders{};
  std::size_t engine_group_size{};
  std::chrono::microseconds engine_group_delay{};
  std::size_t producer_lanes{};
  std::size_t publisher_cursor_persist_max_commands{256};
  std::chrono::microseconds publisher_cursor_persist_max_delay{1000};
  std::optional<std::filesystem::path> data_directory;
};

[[nodiscard]] std::optional<PipelineStage> parse_pipeline_stage(
    std::string_view value);
[[nodiscard]] std::string_view pipeline_stage_name(PipelineStage stage) noexcept;

[[nodiscard]] bool run_pipeline_ceiling(const PipelineBenchmarkOptions& options,
                                        PipelineStage stage);

}  // namespace order_books::benchmark
