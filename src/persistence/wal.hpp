#pragma once

#include <cstddef>
#include <cstdint>
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

class Wal {
 public:
  static Result<std::unique_ptr<Wal>> open(std::filesystem::path directory,
                                           ShardId shard_id,
                                           std::size_t segment_size);

  Result<WalPosition> append(const domain::CommittedCommand& command);
  Status sync();
  Result<std::vector<domain::CommittedCommand>> replay();
  Result<std::optional<domain::CommittedCommand>> next_after(EngineSeq sequence,
                                                              EngineSeq upper_bound);
  Result<std::uint64_t> bytes_after(EngineSeq sequence, EngineSeq upper_bound) const;
  Status retain_through(EngineSeq watermark);

  [[nodiscard]] std::uint64_t size_bytes() const noexcept;
  [[nodiscard]] EngineSeq last_engine_seq() const noexcept;
  [[nodiscard]] WalPosition durable_position() const;

 private:
  Wal(std::filesystem::path directory, ShardId shard_id, std::size_t segment_size)
      : directory_(std::move(directory)),
        shard_id_(shard_id),
        segment_size_(segment_size) {}

  Status create_segment(EngineSeq first_engine_seq);
  Status sync_active_unlocked();
  [[nodiscard]] std::filesystem::path segment_path(EngineSeq first_engine_seq) const;
  [[nodiscard]] std::vector<std::byte> segment_header(EngineSeq first_engine_seq) const;
  Status validate_segment_header(std::span<const std::byte> bytes,
                                 EngineSeq first_engine_seq) const;

  struct CachedRecord {
    domain::CommittedCommand command;
    std::uint64_t frame_bytes{};
  };

  std::filesystem::path directory_;
  ShardId shard_id_{};
  std::size_t segment_size_{};
  std::filesystem::path active_segment_;
  std::uint64_t active_bytes_{};
  std::uint64_t size_bytes_{};
  WalPosition last_appended_position_;
  WalPosition durable_position_;
  bool active_dirty_{false};
  std::vector<CachedRecord> records_;
  bool records_loaded_{false};
  mutable std::mutex mutex_;
};

}  // namespace order_books::storage
