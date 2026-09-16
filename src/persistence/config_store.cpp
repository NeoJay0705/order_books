#include "persistence/config_store.hpp"

#include <algorithm>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>

#include "persistence/binary_codec.hpp"
#include "persistence/crc32c.hpp"
#include "persistence/file_ops.hpp"

namespace order_books::storage {
namespace {

constexpr std::uint16_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 27;
constexpr std::uint8_t kInstrumentKind = 1;
constexpr std::uint8_t kBehaviorKind = 2;

void append_le16(std::vector<std::byte>& data, const std::uint16_t value) {
  data.push_back(static_cast<std::byte>(value & 0xffU));
  data.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_le32(std::vector<std::byte>& data, const std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    data.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

void append_le64(std::vector<std::byte>& data, const std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    data.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

std::uint16_t read_le16(const std::byte* data) {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[0])) |
         static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[1]) << 8U);
}

std::uint32_t read_le32(const std::byte* data) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[index]))
             << (index * 8U);
  }
  return value;
}

std::uint64_t read_le64(const std::byte* data) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data[index]))
             << (index * 8U);
  }
  return value;
}

Error config_error(const ErrorCode code, const char* message) {
  return Error{code, message};
}

std::string instrument_name(const ConfigurationVersion version) {
  return "instruments-" + std::to_string(version) + ".bin";
}

std::string behavior_name(const ConfigurationVersion version) {
  return "behavior-" + std::to_string(version) + ".bin";
}

}  // namespace

Result<ConfigStore> ConfigStore::open(std::filesystem::path directory,
                                       const ShardId shard_id) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return config_error(ErrorCode::wal_failure, "cannot create config directory");
  }
  return ConfigStore(std::move(directory), shard_id);
}

Result<std::vector<std::byte>> ConfigStore::read_config(
    const std::filesystem::path& path, const std::uint8_t expected_kind,
    const ConfigurationVersion expected_version) const {
  auto descriptor = FileOps::open_read(path);
  if (std::holds_alternative<Error>(descriptor)) {
    return config_error(ErrorCode::corrupt_snapshot, "cannot open configuration manifest");
  }
  auto content = FileOps::read_all(std::get<int>(descriptor));
  FileOps::close(std::get<int>(descriptor));
  if (std::holds_alternative<Error>(content)) {
    return config_error(ErrorCode::corrupt_snapshot, "cannot read configuration manifest");
  }
  const auto& bytes = std::get<std::vector<std::byte>>(content);
  if (bytes.size() < kHeaderSize + 4U || bytes[0] != static_cast<std::byte>('O') ||
      bytes[1] != static_cast<std::byte>('B') || bytes[2] != static_cast<std::byte>('C') ||
      bytes[3] != static_cast<std::byte>('F') || read_le16(bytes.data() + 4) != kFormatVersion ||
      read_le32(bytes.data() + 6) != shard_id_ ||
      std::to_integer<std::uint8_t>(bytes[10]) != expected_kind ||
      read_le64(bytes.data() + 11) != expected_version) {
    return config_error(ErrorCode::corrupt_snapshot, "invalid configuration manifest header");
  }
  const auto payload_size = read_le64(bytes.data() + 19);
  if (payload_size != bytes.size() - kHeaderSize - 4U ||
      read_le32(bytes.data() + kHeaderSize + payload_size) !=
          crc32c(std::span(bytes).first(bytes.size() - 4U))) {
    return config_error(ErrorCode::corrupt_snapshot, "invalid configuration manifest checksum");
  }
  return std::vector<std::byte>(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
                                bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderSize +
                                                                            payload_size));
}

Status ConfigStore::write_config(const std::filesystem::path& path, const std::uint8_t kind,
                                 const ConfigurationVersion version,
                                 const std::span<const std::byte> payload) {
  std::vector<std::byte> content;
  content.reserve(kHeaderSize + payload.size() + 4U);
  content.push_back(static_cast<std::byte>('O'));
  content.push_back(static_cast<std::byte>('B'));
  content.push_back(static_cast<std::byte>('C'));
  content.push_back(static_cast<std::byte>('F'));
  append_le16(content, kFormatVersion);
  append_le32(content, shard_id_);
  content.push_back(static_cast<std::byte>(kind));
  append_le64(content, version);
  append_le64(content, payload.size());
  content.insert(content.end(), payload.begin(), payload.end());
  append_le32(content, crc32c(content));

  const auto temporary = path.string() + ".tmp";
  const auto descriptor = ::open(temporary.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (descriptor < 0) {
    return config_error(ErrorCode::wal_failure, "open configuration temporary file failed");
  }
  const auto write_status = FileOps::write_all(descriptor, content);
  if (std::holds_alternative<Error>(write_status)) {
    FileOps::close(descriptor);
    return std::get<Error>(write_status);
  }
  const auto sync_status = FileOps::sync_file(descriptor);
  FileOps::close(descriptor);
  if (std::holds_alternative<Error>(sync_status)) {
    return std::get<Error>(sync_status);
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    return config_error(ErrorCode::wal_failure, "rename configuration manifest failed");
  }
  return FileOps::sync_directory(directory_);
}

Result<std::unordered_map<ConfigurationVersion,
                          std::unordered_map<InstrumentId, InstrumentConfig>>>
ConfigStore::load_instrument_manifests() const {
  std::unordered_map<ConfigurationVersion,
                     std::unordered_map<InstrumentId, InstrumentConfig>> result;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
    if (error) {
      return config_error(ErrorCode::corrupt_snapshot, "cannot scan configuration directory");
    }
    const auto name = entry.path().filename().string();
    if (!entry.is_regular_file() || name.rfind("instruments-", 0) != 0 ||
        entry.path().extension() != ".bin") {
      continue;
    }
    ConfigurationVersion version = 0;
    // "instruments-" is twelve bytes; keep the filename parser strict so a
    // similarly named file cannot be accepted as a different manifest.
    try {
      constexpr std::size_t prefix_size = 12U;
      constexpr std::size_t suffix_size = 4U;
      if (name.size() <= prefix_size + suffix_size ||
          name.size() - prefix_size - suffix_size == 0U) {
        return config_error(ErrorCode::corrupt_snapshot, "invalid instrument manifest name");
      }
      const auto version_text =
          name.substr(prefix_size, name.size() - prefix_size - suffix_size);
      std::size_t consumed = 0;
      version = std::stoull(version_text, &consumed);
      if (consumed != version_text.size() || version_text != std::to_string(version)) {
        return config_error(ErrorCode::corrupt_snapshot, "invalid instrument manifest name");
      }
    } catch (...) {
      return config_error(ErrorCode::corrupt_snapshot, "invalid instrument manifest name");
    }
    if (version == 0 || result.contains(version)) {
      return config_error(ErrorCode::corrupt_snapshot, "duplicate instrument manifest");
    }
    auto payload = read_config(entry.path(), kInstrumentKind, version);
    if (std::holds_alternative<Error>(payload)) {
      return std::get<Error>(payload);
    }
    auto manifest = decode_instrument_manifest(std::get<std::vector<std::byte>>(payload));
    if (std::holds_alternative<Error>(manifest)) {
      return std::get<Error>(manifest);
    }
    for (const auto& [instrument_id, config] :
         std::get<std::unordered_map<InstrumentId, InstrumentConfig>>(manifest)) {
      if (instrument_id == 0 || config.assigned_shard != shard_id_) {
        return config_error(ErrorCode::corrupt_snapshot,
                            "instrument manifest shard assignment mismatch");
      }
    }
    result.emplace(version,
                   std::get<std::unordered_map<InstrumentId, InstrumentConfig>>(
                       std::move(manifest)));
  }
  return result;
}

Result<std::unordered_map<ConfigurationVersion, ShardBehaviorConfig>>
ConfigStore::load_behavior_configs() const {
  std::unordered_map<ConfigurationVersion, ShardBehaviorConfig> result;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
    if (error) {
      return config_error(ErrorCode::corrupt_snapshot, "cannot scan configuration directory");
    }
    const auto name = entry.path().filename().string();
    if (!entry.is_regular_file() || name.rfind("behavior-", 0) != 0 ||
        entry.path().extension() != ".bin") {
      continue;
    }
    ConfigurationVersion version = 0;
    try {
      constexpr std::size_t prefix_size = 9U;
      constexpr std::size_t suffix_size = 4U;
      if (name.size() <= prefix_size + suffix_size ||
          name.size() - prefix_size - suffix_size == 0U) {
        return config_error(ErrorCode::corrupt_snapshot, "invalid behavior manifest name");
      }
      const auto version_text =
          name.substr(prefix_size, name.size() - prefix_size - suffix_size);
      std::size_t consumed = 0;
      version = std::stoull(version_text, &consumed);
      if (consumed != version_text.size() || version_text != std::to_string(version)) {
        return config_error(ErrorCode::corrupt_snapshot, "invalid behavior manifest name");
      }
    } catch (...) {
      return config_error(ErrorCode::corrupt_snapshot, "invalid behavior manifest name");
    }
    if (version == 0 || result.contains(version)) {
      return config_error(ErrorCode::corrupt_snapshot, "duplicate behavior manifest");
    }
    auto payload = read_config(entry.path(), kBehaviorKind, version);
    if (std::holds_alternative<Error>(payload)) {
      return std::get<Error>(payload);
    }
    auto behavior = decode_behavior_config(std::get<std::vector<std::byte>>(payload));
    if (std::holds_alternative<Error>(behavior) ||
        std::get<ShardBehaviorConfig>(behavior).version != version) {
      return config_error(ErrorCode::corrupt_snapshot, "invalid behavior manifest");
    }
    result.emplace(version, std::get<ShardBehaviorConfig>(std::move(behavior)));
  }
  return result;
}

Status ConfigStore::persist_instrument_manifest(
    const ConfigurationVersion version,
    const std::unordered_map<InstrumentId, InstrumentConfig>& manifest) {
  if (version == 0) {
    return config_error(ErrorCode::corrupt_snapshot, "invalid instrument manifest version");
  }
  for (const auto& [instrument_id, config] : manifest) {
    if (instrument_id == 0 || config.instrument_id != instrument_id ||
        config.tick_size <= 0 || config.lot_size <= 0 ||
        config.assigned_shard != shard_id_) {
      return config_error(ErrorCode::corrupt_snapshot, "invalid instrument manifest");
    }
  }
  const auto path = directory_ / instrument_name(version);
  const auto payload = encode_instrument_manifest(manifest);
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    if (error) {
      return config_error(ErrorCode::wal_failure, "stat instrument manifest failed");
    }
    auto existing = read_config(path, kInstrumentKind, version);
    if (std::holds_alternative<Error>(existing)) {
      return std::get<Error>(existing);
    }
    if (std::get<std::vector<std::byte>>(existing) != payload) {
      return config_error(ErrorCode::corrupt_snapshot,
                          "instrument configuration version is immutable");
    }
    return std::monostate{};
  }
  return write_config(path, kInstrumentKind, version, payload);
}

Status ConfigStore::persist_behavior_config(const ConfigurationVersion version,
                                            const ShardBehaviorConfig& config) {
  if (version == 0 || config.version != version) {
    return config_error(ErrorCode::corrupt_snapshot,
                        "behavior configuration version mismatch");
  }
  const auto path = directory_ / behavior_name(version);
  const auto payload = encode_behavior_config(config);
  std::error_code error;
  if (std::filesystem::exists(path, error)) {
    if (error) {
      return config_error(ErrorCode::wal_failure, "stat behavior manifest failed");
    }
    auto existing = read_config(path, kBehaviorKind, version);
    if (std::holds_alternative<Error>(existing)) {
      return std::get<Error>(existing);
    }
    if (std::get<std::vector<std::byte>>(existing) != payload) {
      return config_error(ErrorCode::corrupt_snapshot,
                          "behavior configuration version is immutable");
    }
    return std::monostate{};
  }
  return write_config(path, kBehaviorKind, version, payload);
}

}  // namespace order_books::storage
