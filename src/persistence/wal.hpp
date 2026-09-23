#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "domain/state_machine.hpp"
#include "order_books/model.hpp"

namespace order_books::storage {

struct WalPosition {
  EngineSeq engine_seq{};
  std::filesystem::path segment;
  std::uint64_t end_offset{};
};

// Internal prototype control for bounded WAL record preparation.  The type is
// deliberately kept under src/ and is not part of the installed API.  A lane
// includes the caller that owns the WAL mutex.
struct WalPrepareOptions {
  std::size_t lane_count{1};
  std::size_t min_parallel_commands{256};
};

// Low-overhead cumulative counters used by benchmark diagnostics.  These do
// not enable per-record timing and are not part of the installed API.
struct WalPrepareStats {
  std::uint64_t parallel_groups{};
  std::uint64_t tasks{};
};

// Populated only by the benchmark-only diagnostic append path.  This type is
// intentionally kept in the internal storage header and is not part of the
// installed library API.
struct WalAppendProfile {
  std::uint64_t lock_wait_ns{};
  std::uint64_t prepare_ns{};
  std::uint64_t prepare_task_ns{};
  std::uint64_t parallel_prepare_groups{};
  std::uint64_t prepare_tasks{};
  std::uint64_t plan_copy_ns{};
  std::uint64_t payload_encode_ns{};
  std::uint64_t crc_ns{};
  std::uint64_t frame_assembly_ns{};
  std::uint64_t chunk_copy_ns{};
  std::uint64_t rotation_ns{};
  std::uint64_t rotation_sync_ns{};
  std::uint64_t rotation_header_write_ns{};
  std::uint64_t rotation_header_sync_ns{};
  std::uint64_t rotation_directory_sync_ns{};
  std::uint64_t write_ns{};
  std::uint64_t publish_ns{};
  std::uint64_t frame_bytes{};
  std::uint64_t payload_bytes{};
  std::uint64_t data_write_calls{};
  std::uint64_t rotations{};
};

class Wal {
 public:
  static Result<std::unique_ptr<Wal>> open(std::filesystem::path directory,
                                           ShardId shard_id,
                                           std::size_t segment_size,
                                           WalPrepareOptions prepare_options = {});

  ~Wal();

  Result<WalPosition> append(const domain::CommittedCommand& command);
  Result<WalPosition> append_batch(
      std::span<const domain::CommittedCommand> commands);
  Result<WalPosition> append_batch_profiled(
      std::span<const domain::CommittedCommand> commands,
      WalAppendProfile& profile);
  Status sync();
  Result<std::vector<domain::CommittedCommand>> replay();
  Result<std::optional<domain::CommittedCommand>> next_after(EngineSeq sequence,
                                                              EngineSeq upper_bound);
  Result<std::uint64_t> bytes_after(EngineSeq sequence, EngineSeq upper_bound) const;
  Status retain_through(EngineSeq watermark);

  [[nodiscard]] std::uint64_t size_bytes() const noexcept;
  [[nodiscard]] EngineSeq last_engine_seq() const noexcept;
  [[nodiscard]] WalPosition durable_position() const;
  [[nodiscard]] WalPrepareStats prepare_stats() const;

 private:
  Wal(std::filesystem::path directory, ShardId shard_id, std::size_t segment_size,
      WalPrepareOptions prepare_options);

  Status create_segment(EngineSeq first_engine_seq,
                        WalAppendProfile* profile = nullptr);
  Status sync_active_unlocked();
  [[nodiscard]] std::filesystem::path segment_path(EngineSeq first_engine_seq) const;
  [[nodiscard]] std::vector<std::byte> segment_header(EngineSeq first_engine_seq) const;
  Status validate_segment_header(std::span<const std::byte> bytes,
                                 EngineSeq first_engine_seq) const;

  struct CachedRecord {
    domain::CommittedCommand command;
    std::uint64_t frame_bytes{};
    std::uint64_t cumulative_frame_bytes{};
    std::uint64_t continuity_id{};
  };

  struct PreparedRecord {
    domain::CommittedCommand command;
    std::size_t frame_offset{};
    std::size_t frame_size{};
  };

  struct PreparedLane {
    std::vector<std::byte> frame_bytes;
    std::vector<PreparedRecord> records;
    WalAppendProfile profile{};
  };

  struct PrepareWorkers;

  Result<std::vector<PreparedLane>> prepare_records_unlocked(
      std::span<const domain::CommittedCommand> commands,
      WalAppendProfile* profile);
  Status initialize_prepare_workers();
  Result<WalPosition> append_batch_unlocked(
      std::span<const domain::CommittedCommand> commands,
      WalAppendProfile* profile);
  Result<WalPosition> append_prepared_unlocked(
      std::vector<PreparedLane> lanes, WalAppendProfile* profile);
  Status rebuild_record_index_unlocked();

  std::filesystem::path directory_;
  ShardId shard_id_{};
  std::size_t segment_size_{};
  std::filesystem::path active_segment_;
  int active_descriptor_{-1};
  std::uint64_t active_bytes_{};
  std::uint64_t size_bytes_{};
  WalPosition last_appended_position_;
  WalPosition durable_position_;
  bool active_dirty_{false};
  std::deque<CachedRecord> records_;
  bool records_loaded_{false};
  WalPrepareOptions prepare_options_{};
  WalPrepareStats prepare_stats_{};
  std::unique_ptr<PrepareWorkers> prepare_workers_;
  mutable std::mutex mutex_;
};

}  // namespace order_books::storage
