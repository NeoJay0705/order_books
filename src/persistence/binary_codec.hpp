#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include "domain/state_machine.hpp"
#include "order_books/model.hpp"

namespace order_books::storage {

class BinaryWriter {
 public:
  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void bytes(std::span<const std::byte> value);

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return data_; }
  [[nodiscard]] std::vector<std::byte>& data() noexcept { return data_; }

 private:
  std::vector<std::byte> data_;
};

class BinaryReader {
 public:
  explicit BinaryReader(std::span<const std::byte> data) : data_(data) {}

  [[nodiscard]] bool u8(std::uint8_t& value);
  [[nodiscard]] bool u16(std::uint16_t& value);
  [[nodiscard]] bool u32(std::uint32_t& value);
  [[nodiscard]] bool u64(std::uint64_t& value);
  [[nodiscard]] bool i64(std::int64_t& value);
  [[nodiscard]] bool boolean(bool& value);
  [[nodiscard]] bool bytes(std::vector<std::byte>& value,
                           std::size_t max_size = 1'048'576);
  [[nodiscard]] bool complete() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

 private:
  [[nodiscard]] bool take(std::size_t count, std::span<const std::byte>& value);

  std::span<const std::byte> data_;
  std::size_t offset_{};
};

std::vector<std::byte> encode_command(const Command& command);
Result<Command> decode_command(std::span<const std::byte> data);

std::vector<std::byte> encode_committed_command(const domain::CommittedCommand& command);
Result<domain::CommittedCommand> decode_committed_command(
    std::span<const std::byte> data);

std::vector<std::byte> encode_state(const domain::ShardState& state);
Result<domain::ShardState> decode_state(std::span<const std::byte> data);

std::vector<std::byte> encode_command_result(const CommandResult& result);
Result<CommandResult> decode_command_result(std::span<const std::byte> data);

std::vector<std::byte> encode_instrument_manifest(
    const std::unordered_map<InstrumentId, InstrumentConfig>& manifest);
Result<std::unordered_map<InstrumentId, InstrumentConfig>> decode_instrument_manifest(
    std::span<const std::byte> data);

std::vector<std::byte> encode_behavior_config(const ShardBehaviorConfig& config);
Result<ShardBehaviorConfig> decode_behavior_config(std::span<const std::byte> data);

}  // namespace order_books::storage
