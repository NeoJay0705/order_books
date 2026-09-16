#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace order_books {

using ShardId = std::uint32_t;
using InstrumentId = std::uint64_t;
using ProducerId = std::uint64_t;
using ProducerEpoch = std::uint64_t;
using ProducerStreamId = ShardId;
using ProducerSeq = std::uint64_t;
using EngineSeq = std::uint64_t;
using OrderVersion = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::int64_t;
using Timestamp = std::int64_t;
using ConfigurationVersion = std::uint64_t;

struct OrderId {
  std::uint64_t high{};
  std::uint64_t low{};

  friend constexpr bool operator==(const OrderId&, const OrderId&) = default;
  friend constexpr bool operator<(const OrderId& lhs, const OrderId& rhs) {
    return lhs.high < rhs.high ||
           (lhs.high == rhs.high && lhs.low < rhs.low);
  }
};

struct OrderIdHash {
  std::size_t operator()(const OrderId& value) const noexcept {
    const auto mixed = value.high ^ (value.low + 0x9e3779b97f4a7c15ULL +
                                     (value.high << 6U) + (value.high >> 2U));
    return static_cast<std::size_t>(mixed ^ (mixed >> 32U));
  }
};

struct CommandIdentity {
  ProducerId producer_id{};
  ProducerEpoch producer_epoch{};
  ProducerStreamId producer_stream_id{};
  ProducerSeq producer_seq{};

  friend constexpr bool operator==(const CommandIdentity&, const CommandIdentity&) = default;
};

struct ProducerKey {
  ProducerId producer_id{};
  ProducerStreamId stream_id{};

  friend constexpr bool operator==(const ProducerKey&, const ProducerKey&) = default;
};

struct ProducerKeyHash {
  std::size_t operator()(const ProducerKey& value) const noexcept {
    const auto mixed = value.producer_id ^
                       (static_cast<std::uint64_t>(value.stream_id) +
                        0x9e3779b97f4a7c15ULL + (value.producer_id << 6U) +
                        (value.producer_id >> 2U));
    return static_cast<std::size_t>(mixed ^ (mixed >> 32U));
  }
};

struct EventId {
  ShardId shard_id{};
  EngineSeq engine_seq{};
  std::uint32_t event_index{};

  friend constexpr bool operator==(const EventId&, const EventId&) = default;
};

struct TradeId {
  ShardId shard_id{};
  EngineSeq engine_seq{};
  std::uint32_t trade_index{};

  friend constexpr bool operator==(const TradeId&, const TradeId&) = default;
};

enum class Side : std::uint8_t { buy = 1, sell = 2 };

enum class CommandType : std::uint8_t {
  new_order = 1,
  amend_quantity = 2,
  replace_order = 3,
  cancel_order = 4,
};

enum class CommandStatus : std::uint8_t {
  committed = 1,
  rejected = 2,
  no_change = 3,
  admission_error = 4,
};

enum class OrderStatus : std::uint8_t {
  active = 1,
  partially_filled = 2,
  filled = 3,
  cancelled = 4,
};

enum class EventType : std::uint8_t {
  trade = 1,
  order_updated = 2,
  command_rejected = 3,
};

enum class ErrorCode : std::uint16_t {
  none = 0,
  invalid_envelope,
  invalid_command,
  unknown_instrument,
  wrong_producer_stream,
  invalid_side,
  invalid_price,
  invalid_quantity,
  numeric_overflow,
  duplicate_order_id,
  order_not_found,
  order_already_terminal,
  version_conflict,
  shard_capacity_exceeded,
  stale_producer_epoch,
  producer_sequence_gap,
  command_identity_conflict,
  duplicate_too_old,
  wal_failure,
  corrupt_wal,
  corrupt_snapshot,
  engine_storage_pressure,
  engine_unavailable,
};

struct Error {
  ErrorCode code{ErrorCode::none};
  std::string message;

  friend bool operator==(const Error&, const Error&) = default;
};

template <typename T>
using Result = std::variant<T, Error>;

using Status = std::variant<std::monostate, Error>;

struct NewOrderPayload {
  Side side{Side::buy};
  Price price{};
  Quantity quantity{};
  friend bool operator==(const NewOrderPayload&, const NewOrderPayload&) = default;
};

struct AmendQuantityPayload {
  Quantity new_total_quantity{};
  friend bool operator==(const AmendQuantityPayload&, const AmendQuantityPayload&) = default;
};

struct ReplaceOrderPayload {
  Price new_price{};
  std::optional<Quantity> new_total_quantity;
  friend bool operator==(const ReplaceOrderPayload&, const ReplaceOrderPayload&) = default;
};

struct CancelOrderPayload {
  friend constexpr bool operator==(const CancelOrderPayload&, const CancelOrderPayload&) = default;
};

using CommandPayload = std::variant<NewOrderPayload, AmendQuantityPayload,
                                    ReplaceOrderPayload, CancelOrderPayload>;

struct Command {
  CommandIdentity identity;
  InstrumentId instrument_id{};
  CommandType command_type{CommandType::new_order};
  OrderId order_id;
  std::optional<OrderVersion> expected_version;
  CommandPayload payload{NewOrderPayload{}};

  friend bool operator==(const Command&, const Command&) = default;
};

struct CommandResult {
  CommandIdentity identity;
  std::optional<EngineSeq> engine_seq;
  CommandStatus command_status{CommandStatus::admission_error};
  ErrorCode error_code{ErrorCode::none};
  OrderId order_id;
  std::optional<OrderStatus> order_status;
  std::optional<OrderVersion> order_version;
  std::optional<Quantity> remaining_quantity;
  std::optional<Quantity> filled_quantity;

  friend bool operator==(const CommandResult&, const CommandResult&) = default;
};

struct OrderView {
  OrderId order_id;
  InstrumentId instrument_id{};
  Side side{Side::buy};
  Price price{};
  Quantity total_quantity{};
  Quantity remaining_quantity{};
  Quantity filled_quantity{};
  OrderStatus status{OrderStatus::active};
  OrderVersion version{};
  EngineSeq priority_seq{};

  friend bool operator==(const OrderView&, const OrderView&) = default;
};

struct BookLevel {
  Price price{};
  Quantity total_quantity{};
  std::size_t order_count{};

  friend bool operator==(const BookLevel&, const BookLevel&) = default;
};

struct BookDepth {
  std::vector<BookLevel> levels;

  friend bool operator==(const BookDepth&, const BookDepth&) = default;
};

struct TradeEventPayload {
  TradeId trade_id;
  Price price{};
  Quantity quantity{};
  OrderId maker_order_id;
  OrderId taker_order_id;
  Side maker_side{Side::buy};
  Quantity maker_remaining_quantity{};
  Quantity taker_remaining_quantity{};

  friend bool operator==(const TradeEventPayload&, const TradeEventPayload&) = default;
};

struct OrderUpdatedEventPayload {
  OrderView order;
  ErrorCode update_reason{ErrorCode::none};

  friend bool operator==(const OrderUpdatedEventPayload&,
                         const OrderUpdatedEventPayload&) = default;
};

struct CommandRejectedEventPayload {
  CommandType command_type{CommandType::new_order};
  OrderId order_id;
  ErrorCode error_code{ErrorCode::none};

  friend bool operator==(const CommandRejectedEventPayload&,
                         const CommandRejectedEventPayload&) = default;
};

using EventPayload = std::variant<TradeEventPayload, OrderUpdatedEventPayload,
                                  CommandRejectedEventPayload>;

struct Event {
  EventId id;
  CommandIdentity command_identity;
  InstrumentId instrument_id{};
  EventType event_type{EventType::trade};
  Timestamp occurred_at{};
  EventPayload payload{CommandRejectedEventPayload{}};

  friend bool operator==(const Event&, const Event&) = default;
};

struct InstrumentConfig {
  InstrumentId instrument_id{};
  Price tick_size{1};
  Quantity lot_size{1};
  ShardId assigned_shard{};

  friend bool operator==(const InstrumentConfig&, const InstrumentConfig&) = default;
};

struct ShardBehaviorConfig {
  ConfigurationVersion version{1};
  std::size_t max_active_orders{1'000'000};
  std::int64_t terminal_tombstone_max_age_ns{3'600'000'000'000LL};
  std::size_t terminal_tombstone_max_count{1'000'000};

  friend bool operator==(const ShardBehaviorConfig&,
                         const ShardBehaviorConfig&) = default;
};

inline CommandType command_type(const CommandPayload& payload) noexcept {
  return std::visit(
      [](const auto& value) -> CommandType {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, NewOrderPayload>) {
          return CommandType::new_order;
        } else if constexpr (std::is_same_v<T, AmendQuantityPayload>) {
          return CommandType::amend_quantity;
        } else if constexpr (std::is_same_v<T, ReplaceOrderPayload>) {
          return CommandType::replace_order;
        } else {
          return CommandType::cancel_order;
        }
      },
      payload);
}

}  // namespace order_books
