#include "persistence/binary_codec.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace order_books::storage {
namespace {

constexpr std::size_t kMaxCollectionSize = 1'000'000;

Error corrupt(const char* message) {
  return Error{ErrorCode::corrupt_snapshot, message};
}

template <typename T>
bool write_enum(BinaryWriter& writer, const T value) {
  static_assert(std::is_enum_v<T>);
  writer.u8(static_cast<std::uint8_t>(value));
  return true;
}

template <typename T>
bool read_enum(BinaryReader& reader, T& value, const std::uint8_t max_value) {
  static_assert(std::is_enum_v<T>);
  std::uint8_t raw = 0;
  if (!reader.u8(raw) || raw == 0 || raw > max_value) {
    return false;
  }
  value = static_cast<T>(raw);
  return true;
}

void write_order_id(BinaryWriter& writer, const OrderId value) {
  writer.u64(value.high);
  writer.u64(value.low);
}

bool read_order_id(BinaryReader& reader, OrderId& value) {
  return reader.u64(value.high) && reader.u64(value.low);
}

void write_identity(BinaryWriter& writer, const CommandIdentity& identity) {
  writer.u64(identity.producer_id);
  writer.u64(identity.producer_epoch);
  writer.u32(identity.producer_stream_id);
  writer.u64(identity.producer_seq);
}

bool read_identity(BinaryReader& reader, CommandIdentity& identity) {
  return reader.u64(identity.producer_id) && reader.u64(identity.producer_epoch) &&
         reader.u32(identity.producer_stream_id) && reader.u64(identity.producer_seq);
}

void write_order_view(BinaryWriter& writer, const OrderView& order) {
  write_order_id(writer, order.order_id);
  writer.u64(order.instrument_id);
  write_enum(writer, order.side);
  writer.i64(order.price);
  writer.i64(order.total_quantity);
  writer.i64(order.remaining_quantity);
  writer.i64(order.filled_quantity);
  write_enum(writer, order.status);
  writer.u64(order.version);
  writer.u64(order.priority_seq);
}

bool read_order_view(BinaryReader& reader, OrderView& order) {
  std::uint64_t instrument = 0;
  return read_order_id(reader, order.order_id) && reader.u64(instrument) &&
         (order.instrument_id = instrument, read_enum(reader, order.side, 2)) &&
         reader.i64(order.price) && reader.i64(order.total_quantity) &&
         reader.i64(order.remaining_quantity) && reader.i64(order.filled_quantity) &&
         read_enum(reader, order.status, 4) && reader.u64(order.version) &&
         reader.u64(order.priority_seq);
}

void write_command_result(BinaryWriter& writer, const CommandResult& result);
bool read_command_result(BinaryReader& reader, CommandResult& result);

void write_payload(BinaryWriter& writer, const CommandPayload& payload) {
  std::visit(
      [&writer](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, NewOrderPayload>) {
          writer.u8(1);
          write_enum(writer, value.side);
          writer.i64(value.price);
          writer.i64(value.quantity);
        } else if constexpr (std::is_same_v<T, AmendQuantityPayload>) {
          writer.u8(2);
          writer.i64(value.new_total_quantity);
        } else if constexpr (std::is_same_v<T, ReplaceOrderPayload>) {
          writer.u8(3);
          writer.i64(value.new_price);
          writer.boolean(value.new_total_quantity.has_value());
          if (value.new_total_quantity.has_value()) {
            writer.i64(*value.new_total_quantity);
          }
        } else {
          writer.u8(4);
        }
      },
      payload);
}

bool read_payload(BinaryReader& reader, CommandPayload& payload) {
  std::uint8_t kind = 0;
  if (!reader.u8(kind)) {
    return false;
  }
  switch (kind) {
    case 1: {
      NewOrderPayload value;
      return read_enum(reader, value.side, 2) && reader.i64(value.price) &&
             reader.i64(value.quantity) && (payload = value, true);
    }
    case 2: {
      AmendQuantityPayload value;
      return reader.i64(value.new_total_quantity) && (payload = value, true);
    }
    case 3: {
      ReplaceOrderPayload value;
      bool has_quantity = false;
      if (!reader.i64(value.new_price) || !reader.boolean(has_quantity)) {
        return false;
      }
      if (has_quantity) {
        Quantity quantity = 0;
        if (!reader.i64(quantity)) {
          return false;
        }
        value.new_total_quantity = quantity;
      }
      payload = value;
      return true;
    }
    case 4:
      payload = CancelOrderPayload{};
      return true;
    default:
      return false;
  }
}

void write_command(BinaryWriter& writer, const Command& command) {
  write_identity(writer, command.identity);
  writer.u64(command.instrument_id);
  write_enum(writer, command.command_type);
  write_order_id(writer, command.order_id);
  writer.boolean(command.expected_version.has_value());
  if (command.expected_version.has_value()) {
    writer.u64(*command.expected_version);
  }
  write_payload(writer, command.payload);
}

bool read_command(BinaryReader& reader, Command& command) {
  std::uint64_t instrument = 0;
  if (!read_identity(reader, command.identity) || !reader.u64(instrument) ||
      !read_enum(reader, command.command_type, 4) ||
      !read_order_id(reader, command.order_id)) {
    return false;
  }
  command.instrument_id = instrument;
  bool has_version = false;
  if (!reader.boolean(has_version)) {
    return false;
  }
  if (has_version) {
    OrderVersion version = 0;
    if (!reader.u64(version)) {
      return false;
    }
    command.expected_version = version;
  }
  return read_payload(reader, command.payload);
}

void write_order_status_optional(BinaryWriter& writer,
                                 const std::optional<OrderStatus>& value) {
  writer.boolean(value.has_value());
  if (value.has_value()) {
    write_enum(writer, *value);
  }
}

bool read_order_status_optional(BinaryReader& reader,
                                std::optional<OrderStatus>& value) {
  bool present = false;
  if (!reader.boolean(present)) {
    return false;
  }
  if (!present) {
    value.reset();
    return true;
  }
  OrderStatus status;
  if (!read_enum(reader, status, 4)) {
    return false;
  }
  value = status;
  return true;
}

void write_u64_optional(BinaryWriter& writer, const std::optional<std::uint64_t>& value) {
  writer.boolean(value.has_value());
  if (value.has_value()) {
    writer.u64(*value);
  }
}

bool read_u64_optional(BinaryReader& reader, std::optional<std::uint64_t>& value) {
  bool present = false;
  if (!reader.boolean(present)) {
    return false;
  }
  if (!present) {
    value.reset();
    return true;
  }
  std::uint64_t number = 0;
  if (!reader.u64(number)) {
    return false;
  }
  value = number;
  return true;
}

void write_command_result(BinaryWriter& writer, const CommandResult& result) {
  write_identity(writer, result.identity);
  write_u64_optional(writer, result.engine_seq);
  write_enum(writer, result.command_status);
  writer.u16(static_cast<std::uint16_t>(result.error_code));
  write_order_id(writer, result.order_id);
  write_order_status_optional(writer, result.order_status);
  write_u64_optional(writer, result.order_version);
  writer.boolean(result.remaining_quantity.has_value());
  if (result.remaining_quantity.has_value()) {
    writer.i64(*result.remaining_quantity);
  }
  writer.boolean(result.filled_quantity.has_value());
  if (result.filled_quantity.has_value()) {
    writer.i64(*result.filled_quantity);
  }
}

bool read_command_result(BinaryReader& reader, CommandResult& result) {
  std::uint16_t error = 0;
  if (!read_identity(reader, result.identity) ||
      !read_u64_optional(reader, result.engine_seq) ||
      !read_enum(reader, result.command_status, 4) || !reader.u16(error) ||
      !read_order_id(reader, result.order_id) ||
      !read_order_status_optional(reader, result.order_status) ||
      !read_u64_optional(reader, result.order_version)) {
    return false;
  }
  if (error > static_cast<std::uint16_t>(ErrorCode::engine_unavailable)) {
    return false;
  }
  result.error_code = static_cast<ErrorCode>(error);
  bool present = false;
  if (!reader.boolean(present)) {
    return false;
  }
  if (present) {
    Quantity quantity = 0;
    if (!reader.i64(quantity)) {
      return false;
    }
    result.remaining_quantity = quantity;
  }
  if (!reader.boolean(present)) {
    return false;
  }
  if (present) {
    Quantity quantity = 0;
    if (!reader.i64(quantity)) {
      return false;
    }
    result.filled_quantity = quantity;
  }
  return true;
}

template <typename T>
void sort_by_id(std::vector<T>& values) {
  std::sort(values.begin(), values.end(), [](const T& lhs, const T& rhs) {
    return lhs.first < rhs.first;
  });
}

}  // namespace

void BinaryWriter::u8(const std::uint8_t value) { data_.push_back(static_cast<std::byte>(value)); }

void BinaryWriter::u16(const std::uint16_t value) {
  for (unsigned index = 0; index < 2U; ++index) {
    u8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
  }
}

void BinaryWriter::u32(const std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    u8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
  }
}

void BinaryWriter::u64(const std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    u8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
  }
}

void BinaryWriter::i64(const std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void BinaryWriter::boolean(const bool value) { u8(value ? 1U : 0U); }

void BinaryWriter::bytes(const std::span<const std::byte> value) {
  u64(value.size());
  data_.insert(data_.end(), value.begin(), value.end());
}

bool BinaryReader::take(const std::size_t count, std::span<const std::byte>& value) {
  if (offset_ > data_.size() || count > data_.size() - offset_) {
    return false;
  }
  value = data_.subspan(offset_, count);
  offset_ += count;
  return true;
}

bool BinaryReader::u8(std::uint8_t& value) {
  std::span<const std::byte> bytes_view;
  if (!take(1, bytes_view)) {
    return false;
  }
  value = std::to_integer<std::uint8_t>(bytes_view.front());
  return true;
}

bool BinaryReader::u16(std::uint16_t& value) {
  value = 0;
  for (unsigned index = 0; index < 2U; ++index) {
    std::uint8_t byte = 0;
    if (!u8(byte)) {
      return false;
    }
    value |= static_cast<std::uint16_t>(byte) << (index * 8U);
  }
  return true;
}

bool BinaryReader::u32(std::uint32_t& value) {
  value = 0;
  for (unsigned index = 0; index < 4U; ++index) {
    std::uint8_t byte = 0;
    if (!u8(byte)) {
      return false;
    }
    value |= static_cast<std::uint32_t>(byte) << (index * 8U);
  }
  return true;
}

bool BinaryReader::u64(std::uint64_t& value) {
  value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    std::uint8_t byte = 0;
    if (!u8(byte)) {
      return false;
    }
    value |= static_cast<std::uint64_t>(byte) << (index * 8U);
  }
  return true;
}

bool BinaryReader::i64(std::int64_t& value) {
  std::uint64_t raw = 0;
  if (!u64(raw)) {
    return false;
  }
  value = static_cast<std::int64_t>(raw);
  return true;
}

bool BinaryReader::boolean(bool& value) {
  std::uint8_t raw = 0;
  if (!u8(raw) || raw > 1U) {
    return false;
  }
  value = raw != 0;
  return true;
}

bool BinaryReader::bytes(std::vector<std::byte>& value, const std::size_t max_size) {
  std::uint64_t size = 0;
  if (!u64(size) || size > max_size || size > data_.size() - offset_) {
    return false;
  }
  std::span<const std::byte> view;
  if (!take(static_cast<std::size_t>(size), view)) {
    return false;
  }
  value.assign(view.begin(), view.end());
  return true;
}

std::vector<std::byte> encode_command(const Command& command) {
  BinaryWriter writer;
  write_command(writer, command);
  return writer.data();
}

Result<Command> decode_command(const std::span<const std::byte> data) {
  BinaryReader reader(data);
  Command command;
  if (!read_command(reader, command) || !reader.complete()) {
    return corrupt("invalid command encoding");
  }
  return command;
}

std::vector<std::byte> encode_committed_command(
    const domain::CommittedCommand& command) {
  BinaryWriter writer;
  writer.u64(command.engine_seq);
  writer.i64(command.received_at);
  writer.u64(command.behavior_configuration_version);
  writer.u64(command.instrument_configuration_version);
  const auto encoded = encode_command(command.command);
  writer.bytes(encoded);
  return writer.data();
}

Result<domain::CommittedCommand> decode_committed_command(
    const std::span<const std::byte> data) {
  BinaryReader reader(data);
  domain::CommittedCommand command;
  std::uint64_t behavior = 0;
  std::uint64_t instrument = 0;
  std::vector<std::byte> encoded;
  if (!reader.u64(command.engine_seq) || !reader.i64(command.received_at) ||
      !reader.u64(behavior) || !reader.u64(instrument) || !reader.bytes(encoded) ||
      !reader.complete()) {
    return corrupt("invalid committed command encoding");
  }
  command.behavior_configuration_version = behavior;
  command.instrument_configuration_version = instrument;
  auto decoded = decode_command(encoded);
  if (std::holds_alternative<Error>(decoded)) {
    return std::get<Error>(decoded);
  }
  command.command = std::get<Command>(std::move(decoded));
  return command;
}

std::vector<std::byte> encode_command_result(const CommandResult& result) {
  BinaryWriter writer;
  write_command_result(writer, result);
  return writer.data();
}

Result<CommandResult> decode_command_result(const std::span<const std::byte> data) {
  BinaryReader reader(data);
  CommandResult result;
  if (!read_command_result(reader, result) || !reader.complete()) {
    return corrupt("invalid command result encoding");
  }
  return result;
}

std::vector<std::byte> encode_instrument_manifest(
    const std::unordered_map<InstrumentId, InstrumentConfig>& manifest) {
  BinaryWriter writer;
  std::vector<std::pair<InstrumentId, InstrumentConfig>> entries(manifest.begin(),
                                                                  manifest.end());
  sort_by_id(entries);
  writer.u64(entries.size());
  for (const auto& [id, config] : entries) {
    writer.u64(id);
    writer.u64(config.instrument_id);
    writer.i64(config.tick_size);
    writer.i64(config.lot_size);
    writer.u32(config.assigned_shard);
  }
  return writer.data();
}

Result<std::unordered_map<InstrumentId, InstrumentConfig>> decode_instrument_manifest(
    const std::span<const std::byte> data) {
  BinaryReader reader(data);
  std::unordered_map<InstrumentId, InstrumentConfig> result;
  std::uint64_t count = 0;
  if (!reader.u64(count) || count > kMaxCollectionSize) {
    return corrupt("invalid instrument manifest count");
  }
  for (std::uint64_t index = 0; index < count; ++index) {
    InstrumentId key = 0;
    InstrumentConfig config;
    if (!reader.u64(key) || !reader.u64(config.instrument_id) ||
        !reader.i64(config.tick_size) || !reader.i64(config.lot_size) ||
        !reader.u32(config.assigned_shard) || key == 0 || config.instrument_id != key ||
        config.tick_size <= 0 || config.lot_size <= 0 || !result.emplace(key, config).second) {
      return corrupt("invalid instrument manifest");
    }
  }
  if (!reader.complete()) {
    return corrupt("trailing instrument manifest bytes");
  }
  return result;
}

std::vector<std::byte> encode_behavior_config(const ShardBehaviorConfig& config) {
  BinaryWriter writer;
  writer.u64(config.version);
  writer.u64(config.max_active_orders);
  writer.i64(config.terminal_tombstone_max_age_ns);
  writer.u64(config.terminal_tombstone_max_count);
  return writer.data();
}

Result<ShardBehaviorConfig> decode_behavior_config(const std::span<const std::byte> data) {
  BinaryReader reader(data);
  ShardBehaviorConfig config;
  if (!reader.u64(config.version) || !reader.u64(config.max_active_orders) ||
      !reader.i64(config.terminal_tombstone_max_age_ns) ||
      !reader.u64(config.terminal_tombstone_max_count) || !reader.complete() ||
      config.version == 0 || config.max_active_orders == 0 ||
      config.terminal_tombstone_max_age_ns <= 0 || config.terminal_tombstone_max_count == 0) {
    return corrupt("invalid behavior configuration");
  }
  return config;
}

std::vector<std::byte> encode_state(const domain::ShardState& state) {
  BinaryWriter writer;
  writer.u32(state.shard_id);
  writer.u64(state.last_committed_engine_seq);
  writer.i64(state.logical_retention_time);
  writer.u64(state.current_instrument_configuration_version);
  writer.u64(state.current_behavior_configuration_version);
  writer.u64(state.active_order_count);

  std::vector<std::pair<InstrumentId, InstrumentConfig>> instruments(
      state.instruments.begin(), state.instruments.end());
  sort_by_id(instruments);
  writer.u64(instruments.size());
  for (const auto& [id, config] : instruments) {
    writer.u64(id);
    writer.u64(config.instrument_id);
    writer.i64(config.tick_size);
    writer.i64(config.lot_size);
    writer.u32(config.assigned_shard);
  }

  std::vector<ConfigurationVersion> manifest_versions;
  manifest_versions.reserve(state.instrument_configurations.size());
  for (const auto& [version, unused_manifest] : state.instrument_configurations) {
    (void)unused_manifest;
    manifest_versions.push_back(version);
  }
  std::sort(manifest_versions.begin(), manifest_versions.end());
  writer.u64(manifest_versions.size());
  for (const auto version : manifest_versions) {
    writer.u64(version);
    const auto& manifest = state.instrument_configurations.at(version);
    std::vector<std::pair<InstrumentId, InstrumentConfig>> entries(manifest.begin(),
                                                                    manifest.end());
    sort_by_id(entries);
    writer.u64(entries.size());
    for (const auto& [id, config] : entries) {
      writer.u64(id);
      writer.u64(config.instrument_id);
      writer.i64(config.tick_size);
      writer.i64(config.lot_size);
      writer.u32(config.assigned_shard);
    }
  }

  std::vector<std::pair<ConfigurationVersion, ShardBehaviorConfig>> behaviors(
      state.behavior_configurations.begin(), state.behavior_configurations.end());
  sort_by_id(behaviors);
  writer.u64(behaviors.size());
  for (const auto& [version, config] : behaviors) {
    writer.u64(version);
    writer.u64(config.version);
    writer.u64(config.max_active_orders);
    writer.i64(config.terminal_tombstone_max_age_ns);
    writer.u64(config.terminal_tombstone_max_count);
  }

  std::vector<std::pair<InstrumentId, const domain::OrderBook*>> books;
  books.reserve(state.books.size());
  for (const auto& [id, book] : state.books) {
    books.emplace_back(id, &book);
  }
  sort_by_id(books);
  writer.u64(books.size());
  for (const auto& [id, book] : books) {
    writer.u64(id);
    const auto orders = book->snapshot_orders();
    writer.u64(orders.size());
    for (const auto& order : orders) {
      write_order_view(writer, order);
    }
  }

  std::vector<std::pair<ProducerKey, const domain::ProducerState*>> producers;
  producers.reserve(state.producer_states.size());
  for (const auto& [key, producer] : state.producer_states) {
    producers.emplace_back(key, &producer);
  }
  std::sort(producers.begin(), producers.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.first.producer_id < rhs.first.producer_id ||
                     (lhs.first.producer_id == rhs.first.producer_id &&
                      lhs.first.stream_id < rhs.first.stream_id);
            });
  writer.u64(producers.size());
  for (const auto& [key, producer] : producers) {
    writer.u64(key.producer_id);
    writer.u32(key.stream_id);
    writer.u64(producer->current_epoch);
    writer.u64(producer->last_processed_seq);
    writer.bytes(producer->last_canonical_command);
    const auto result = encode_command_result(producer->last_result);
    writer.bytes(result);
  }

  std::vector<domain::Tombstone> tombstones;
  tombstones.reserve(state.tombstones.size());
  for (const auto& [unused_id, tombstone] : state.tombstones) {
    (void)unused_id;
    tombstones.push_back(tombstone);
  }
  std::sort(tombstones.begin(), tombstones.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.terminal_engine_seq < rhs.terminal_engine_seq ||
           (lhs.terminal_engine_seq == rhs.terminal_engine_seq &&
            lhs.order_id < rhs.order_id);
  });
  writer.u64(tombstones.size());
  for (const auto& tombstone : tombstones) {
    write_order_id(writer, tombstone.order_id);
    write_enum(writer, tombstone.final_status);
    writer.u64(tombstone.final_version);
    writer.u64(tombstone.terminal_engine_seq);
    writer.i64(tombstone.terminal_time);
  }
  writer.u64(state.tombstone_order.size());
  for (const auto& [seq, id] : state.tombstone_order) {
    writer.u64(seq);
    write_order_id(writer, id);
  }
  return writer.data();
}

Result<domain::ShardState> decode_state(const std::span<const std::byte> data) {
  BinaryReader reader(data);
  domain::ShardState state;
  std::uint64_t number = 0;
  if (!reader.u32(state.shard_id) || !reader.u64(state.last_committed_engine_seq) ||
      !reader.i64(state.logical_retention_time) ||
      !reader.u64(state.current_instrument_configuration_version) ||
      !reader.u64(state.current_behavior_configuration_version) ||
      !reader.u64(number) || number > kMaxCollectionSize) {
    return corrupt("invalid snapshot header");
  }
  state.active_order_count = static_cast<std::size_t>(number);

  auto read_count = [&reader](std::uint64_t& count) {
    return reader.u64(count) && count <= kMaxCollectionSize;
  };

  if (!read_count(number)) {
    return corrupt("invalid instrument count");
  }
  for (std::uint64_t index = 0; index < number; ++index) {
    InstrumentId key = 0;
    InstrumentConfig config;
    if (!reader.u64(key) || !reader.u64(config.instrument_id) ||
        !reader.i64(config.tick_size) || !reader.i64(config.lot_size) ||
        !reader.u32(config.assigned_shard)) {
      return corrupt("invalid instrument config");
    }
    if (key == 0 || config.instrument_id != key || config.tick_size <= 0 ||
        config.lot_size <= 0 || config.assigned_shard != state.shard_id) {
      return corrupt("invalid instrument config values");
    }
    if (!state.instruments.emplace(key, config).second) {
      return corrupt("duplicate instrument config");
    }
  }

  if (!read_count(number)) {
    return corrupt("invalid instrument manifest count");
  }
  for (std::uint64_t version_index = 0; version_index < number; ++version_index) {
    ConfigurationVersion version = 0;
    std::uint64_t entry_count = 0;
    if (!reader.u64(version) || !read_count(entry_count)) {
      return corrupt("invalid instrument manifest");
    }
    if (version == 0) {
      return corrupt("invalid instrument manifest version");
    }
    const auto [manifest_iterator, manifest_inserted] =
        state.instrument_configurations.emplace(version,
                                                std::unordered_map<InstrumentId,
                                                                   InstrumentConfig>{});
    if (!manifest_inserted) {
      return corrupt("duplicate instrument manifest version");
    }
    auto& manifest = manifest_iterator->second;
    for (std::uint64_t entry_index = 0; entry_index < entry_count; ++entry_index) {
      InstrumentId key = 0;
      InstrumentConfig config;
      if (!reader.u64(key) || !reader.u64(config.instrument_id) ||
          !reader.i64(config.tick_size) || !reader.i64(config.lot_size) ||
          !reader.u32(config.assigned_shard)) {
        return corrupt("invalid instrument manifest entry");
      }
      if (key == 0 || config.instrument_id != key || config.tick_size <= 0 ||
          config.lot_size <= 0 || config.assigned_shard != state.shard_id) {
        return corrupt("invalid instrument manifest values");
      }
      if (!manifest.emplace(key, config).second) {
        return corrupt("duplicate instrument manifest entry");
      }
    }
  }
  if (state.instrument_configurations.empty() &&
      state.current_instrument_configuration_version != 0) {
    state.instrument_configurations.emplace(state.current_instrument_configuration_version,
                                            state.instruments);
  }

  if (!read_count(number)) {
    return corrupt("invalid behavior count");
  }
  for (std::uint64_t index = 0; index < number; ++index) {
    ConfigurationVersion key = 0;
    ShardBehaviorConfig config;
    if (!reader.u64(key) || !reader.u64(config.version) ||
        !reader.u64(config.max_active_orders) ||
        !reader.i64(config.terminal_tombstone_max_age_ns) ||
        !reader.u64(config.terminal_tombstone_max_count)) {
      return corrupt("invalid behavior config");
    }
    if (key == 0 || config.version != key || config.max_active_orders == 0 ||
        config.terminal_tombstone_max_age_ns <= 0 ||
        config.terminal_tombstone_max_count == 0) {
      return corrupt("invalid behavior config values");
    }
    if (!state.behavior_configurations.emplace(key, config).second) {
      return corrupt("duplicate behavior config");
    }
  }

  if (!read_count(number)) {
    return corrupt("invalid book count");
  }
  for (std::uint64_t index = 0; index < number; ++index) {
    InstrumentId id = 0;
    std::uint64_t order_count = 0;
    if (!reader.u64(id) || !read_count(order_count)) {
      return corrupt("invalid book header");
    }
    std::vector<OrderView> orders;
    orders.reserve(static_cast<std::size_t>(order_count));
    for (std::uint64_t order_index = 0; order_index < order_count; ++order_index) {
      OrderView order;
      if (!read_order_view(reader, order)) {
        return corrupt("invalid order view");
      }
      orders.push_back(order);
      state.order_locations[order.order_id] = id;
    }
    domain::OrderBook book(id);
    auto restored = book.restore_orders(orders);
    if (std::holds_alternative<Error>(restored)) {
      return std::get<Error>(restored);
    }
    if (!state.books.emplace(id, std::move(book)).second) {
      return corrupt("duplicate order book");
    }
  }

  if (!read_count(number)) {
    return corrupt("invalid producer count");
  }
  for (std::uint64_t index = 0; index < number; ++index) {
    ProducerKey key;
    domain::ProducerState producer;
    std::vector<std::byte> encoded_result;
    if (!reader.u64(key.producer_id) || !reader.u32(key.stream_id) ||
        !reader.u64(producer.current_epoch) ||
        !reader.u64(producer.last_processed_seq) ||
        !reader.bytes(producer.last_canonical_command) ||
        !reader.bytes(encoded_result)) {
      return corrupt("invalid producer state");
    }
    auto result = decode_command_result(encoded_result);
    if (std::holds_alternative<Error>(result)) {
      return std::get<Error>(result);
    }
    producer.last_result = std::get<CommandResult>(std::move(result));
    if (!state.producer_states.emplace(key, std::move(producer)).second) {
      return corrupt("duplicate producer state");
    }
  }

  if (!read_count(number)) {
    return corrupt("invalid tombstone count");
  }
  for (std::uint64_t index = 0; index < number; ++index) {
    domain::Tombstone tombstone;
    if (!read_order_id(reader, tombstone.order_id) ||
        !read_enum(reader, tombstone.final_status, 4) ||
        !reader.u64(tombstone.final_version) ||
        !reader.u64(tombstone.terminal_engine_seq) ||
        !reader.i64(tombstone.terminal_time)) {
      return corrupt("invalid tombstone");
    }
    if (!state.tombstones.emplace(tombstone.order_id, tombstone).second) {
      return corrupt("duplicate tombstone");
    }
  }
  if (!read_count(number)) {
    return corrupt("invalid tombstone order count");
  }
  for (std::uint64_t index = 0; index < number; ++index) {
    EngineSeq sequence = 0;
    OrderId id;
    if (!reader.u64(sequence) || !read_order_id(reader, id)) {
      return corrupt("invalid tombstone order");
    }
    state.tombstone_order.emplace_back(sequence, id);
  }
  if (!reader.complete()) {
    return corrupt("trailing snapshot bytes");
  }
  return state;
}

}  // namespace order_books::storage
