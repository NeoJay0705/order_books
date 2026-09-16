#include "persistence/snapshot_store.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

#include "persistence/binary_codec.hpp"
#include "persistence/crc32c.hpp"
#include "persistence/file_ops.hpp"

namespace order_books::storage {
namespace {

constexpr std::size_t kHeaderSize = 42;
constexpr std::uint16_t kSnapshotFormatVersion = 2;
constexpr std::size_t kInstrumentVersionOffset = 18;
constexpr std::size_t kBehaviorVersionOffset = 26;
constexpr std::size_t kPayloadSizeOffset = 34;

void append_le16(std::vector<std::byte>& data, const std::uint16_t value) {
  for (unsigned index = 0; index < 2U; ++index) {
    data.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
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

Error snapshot_error(const char* message) {
  return Error{ErrorCode::corrupt_snapshot, message};
}

std::uint64_t snapshot_sequence(const std::filesystem::path& path) {
  const auto name = path.stem().string();
  constexpr std::string_view prefix = "snapshot-";
  if (name.rfind(prefix.data(), 0) != 0) {
    return 0;
  }
  try {
    return std::stoull(name.substr(prefix.size()));
  } catch (...) {
    return 0;
  }
}

Result<std::vector<std::byte>> read_path(const std::filesystem::path& path) {
  auto descriptor = FileOps::open_read(path);
  if (std::holds_alternative<Error>(descriptor)) {
    return std::get<Error>(descriptor);
  }
  auto content = FileOps::read_all(std::get<int>(descriptor));
  FileOps::close(std::get<int>(descriptor));
  return content;
}

}  // namespace

Result<SnapshotStore> SnapshotStore::open(std::filesystem::path directory,
                                           const ShardId shard_id) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    return Error{ErrorCode::wal_failure, "cannot create snapshot directory"};
  }
  return SnapshotStore(std::move(directory), shard_id);
}

Status SnapshotStore::write(const domain::ShardState& state) {
  if (state.shard_id != shard_id_) {
    return Error{ErrorCode::corrupt_snapshot, "snapshot shard mismatch"};
  }
  const auto payload = encode_state(state);
  std::vector<std::byte> header;
  header.reserve(kHeaderSize);
  header.push_back(static_cast<std::byte>('O'));
  header.push_back(static_cast<std::byte>('B'));
  header.push_back(static_cast<std::byte>('S'));
  header.push_back(static_cast<std::byte>('N'));
  append_le16(header, kSnapshotFormatVersion);
  append_le32(header, shard_id_);
  append_le64(header, state.last_committed_engine_seq);
  append_le64(header, state.current_instrument_configuration_version);
  append_le64(header, state.current_behavior_configuration_version);
  append_le64(header, payload.size());

  std::vector<std::byte> content = header;
  content.insert(content.end(), payload.begin(), payload.end());
  append_le32(content, crc32c(content));

  const auto final_path = directory_ /
                          ("snapshot-" + std::to_string(state.last_committed_engine_seq) +
                           ".snap");
  const auto temp_path = final_path.string() + ".tmp";
  const auto descriptor = ::open(temp_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (descriptor < 0) {
    return Error{ErrorCode::wal_failure, "open snapshot temporary file failed"};
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
  std::error_code filesystem_error;
  std::filesystem::rename(temp_path, final_path, filesystem_error);
  if (filesystem_error) {
    return Error{ErrorCode::wal_failure, "rename snapshot failed"};
  }
  return FileOps::sync_directory(directory_);
}

Result<std::optional<domain::ShardState>> SnapshotStore::load_latest() const {
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".snap") {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end(), [](const auto& lhs, const auto& rhs) {
    return snapshot_sequence(lhs) > snapshot_sequence(rhs);
  });
  bool found_snapshot = false;
  for (const auto& path : paths) {
    found_snapshot = true;
    auto content_result = read_path(path);
    if (std::holds_alternative<Error>(content_result)) {
      continue;
    }
    const auto& content = std::get<std::vector<std::byte>>(content_result);
    if (content.size() < kHeaderSize + 4U ||
        content[0] != static_cast<std::byte>('O') ||
        content[1] != static_cast<std::byte>('B') ||
        content[2] != static_cast<std::byte>('S') ||
        content[3] != static_cast<std::byte>('N') ||
        read_le16(content.data() + 4) != kSnapshotFormatVersion ||
        read_le32(content.data() + 6) != shard_id_) {
      continue;
    }
    const auto payload_size = read_le64(content.data() + kPayloadSizeOffset);
    if (payload_size > content.size() - kHeaderSize - 4U ||
        payload_size != content.size() - kHeaderSize - 4U) {
      continue;
    }
    const auto expected_crc = read_le32(content.data() + kHeaderSize + payload_size);
    if (expected_crc != crc32c(std::span(content).first(content.size() - 4U))) {
      continue;
    }
    auto decoded = decode_state(
        std::span(content).subspan(kHeaderSize, static_cast<std::size_t>(payload_size)));
    if (std::holds_alternative<Error>(decoded)) {
      continue;
    }
    auto state = std::get<domain::ShardState>(std::move(decoded));
    if (state.shard_id != shard_id_ ||
        state.last_committed_engine_seq != read_le64(content.data() + 10) ||
        state.current_instrument_configuration_version !=
            read_le64(content.data() + kInstrumentVersionOffset) ||
        state.current_behavior_configuration_version !=
            read_le64(content.data() + kBehaviorVersionOffset)) {
      continue;
    }
    return std::optional<domain::ShardState>(std::move(state));
  }
  if (found_snapshot) {
    return snapshot_error("no valid snapshot found");
  }
  return std::optional<domain::ShardState>{};
}

}  // namespace order_books::storage
