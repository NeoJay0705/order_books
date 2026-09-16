#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <span>
#include <unordered_map>
#include <vector>

#include "domain/state_machine.hpp"
#include "order_books/model.hpp"

namespace order_books::storage {

class ConfigStore {
 public:
  static Result<ConfigStore> open(std::filesystem::path directory, ShardId shard_id);

  Result<std::unordered_map<ConfigurationVersion,
                            std::unordered_map<InstrumentId, InstrumentConfig>>>
  load_instrument_manifests() const;
  Result<std::unordered_map<ConfigurationVersion, ShardBehaviorConfig>>
  load_behavior_configs() const;

  Status persist_instrument_manifest(
      ConfigurationVersion version,
      const std::unordered_map<InstrumentId, InstrumentConfig>& manifest);
  Status persist_behavior_config(ConfigurationVersion version,
                                 const ShardBehaviorConfig& config);

 private:
  ConfigStore(std::filesystem::path directory, ShardId shard_id)
      : directory_(std::move(directory)), shard_id_(shard_id) {}

  Result<std::vector<std::byte>> read_config(const std::filesystem::path& path,
                                             std::uint8_t expected_kind,
                                             ConfigurationVersion expected_version) const;
  Status write_config(const std::filesystem::path& path, std::uint8_t kind,
                      ConfigurationVersion version,
                      std::span<const std::byte> payload);

  std::filesystem::path directory_;
  ShardId shard_id_{};
};

}  // namespace order_books::storage
