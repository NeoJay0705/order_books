#pragma once

#include <filesystem>
#include <optional>

#include "domain/state_machine.hpp"
#include "order_books/model.hpp"

namespace order_books::storage {

class SnapshotStore {
 public:
  static Result<SnapshotStore> open(std::filesystem::path directory, ShardId shard_id);

  Status write(const domain::ShardState& state);
  Result<std::optional<domain::ShardState>> load_latest() const;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept {
    return directory_;
  }

 private:
  SnapshotStore(std::filesystem::path directory, ShardId shard_id)
      : directory_(std::move(directory)), shard_id_(shard_id) {}

  std::filesystem::path directory_;
  ShardId shard_id_{};
};

}  // namespace order_books::storage
