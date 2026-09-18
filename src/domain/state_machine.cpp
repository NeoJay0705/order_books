#include "domain/state_machine.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace order_books::domain {
namespace {

constexpr bool is_valid_side(const Side side) noexcept {
  return side == Side::buy || side == Side::sell;
}

void append_canonical_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

std::vector<std::byte> make_canonical_command(const Command& command) {
  std::vector<std::byte> bytes;
  bytes.reserve(128);
  append_canonical_u64(bytes, command.identity.producer_id);
  append_canonical_u64(bytes, command.identity.producer_epoch);
  append_canonical_u64(bytes, command.identity.producer_stream_id);
  append_canonical_u64(bytes, command.identity.producer_seq);
  append_canonical_u64(bytes, command.instrument_id);
  append_canonical_u64(bytes, static_cast<std::uint8_t>(command.command_type));
  append_canonical_u64(bytes, command.order_id.high);
  append_canonical_u64(bytes, command.order_id.low);
  append_canonical_u64(bytes, command.expected_version.has_value() ? 1U : 0U);
  if (command.expected_version.has_value()) {
    append_canonical_u64(bytes, *command.expected_version);
  }
  std::visit(
      [&bytes](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, NewOrderPayload>) {
          append_canonical_u64(bytes, static_cast<std::uint8_t>(payload.side));
          append_canonical_u64(bytes, static_cast<std::uint64_t>(payload.price));
          append_canonical_u64(bytes, static_cast<std::uint64_t>(payload.quantity));
        } else if constexpr (std::is_same_v<T, AmendQuantityPayload>) {
          append_canonical_u64(bytes,
                               static_cast<std::uint64_t>(payload.new_total_quantity));
        } else if constexpr (std::is_same_v<T, ReplaceOrderPayload>) {
          append_canonical_u64(bytes, static_cast<std::uint64_t>(payload.new_price));
          append_canonical_u64(bytes, payload.new_total_quantity.has_value() ? 1U : 0U);
          if (payload.new_total_quantity.has_value()) {
            append_canonical_u64(bytes,
                                 static_cast<std::uint64_t>(*payload.new_total_quantity));
          }
        }
      },
      command.payload);
  return bytes;
}

Error make_error(const ErrorCode code, const char* message) {
  return Error{code, message};
}

bool is_active_order(const OrderStatus status) noexcept {
  return status == OrderStatus::active || status == OrderStatus::partially_filled;
}

bool valid_order_view(const OrderView& order) noexcept {
  if (order.order_id == OrderId{} || order.instrument_id == 0 ||
      (order.side != Side::buy && order.side != Side::sell) || order.price <= 0 ||
      order.total_quantity <= 0 || order.remaining_quantity < 0 || order.version == 0 ||
      order.priority_seq == 0 || order.filled_quantity < 0 ||
      order.filled_quantity > order.total_quantity) {
    return false;
  }
  const auto quantity_sum_valid =
      order.remaining_quantity <=
          std::numeric_limits<Quantity>::max() - order.filled_quantity;
  if (order.status == OrderStatus::active) {
    return quantity_sum_valid && order.filled_quantity == 0 && order.remaining_quantity > 0 &&
           order.total_quantity == order.remaining_quantity + order.filled_quantity;
  }
  if (order.status == OrderStatus::partially_filled) {
    return quantity_sum_valid && order.filled_quantity > 0 && order.remaining_quantity > 0 &&
           order.total_quantity == order.remaining_quantity + order.filled_quantity;
  }
  if (order.status == OrderStatus::filled) {
    return order.remaining_quantity == 0 && order.filled_quantity == order.total_quantity;
  }
  return order.status == OrderStatus::cancelled && order.remaining_quantity == 0;
}

}  // namespace

StateMachine::StateMachine(ShardState state) : state_(std::move(state)) {}

std::vector<std::byte> canonical_command_bytes(const Command& command) {
  return make_canonical_command(command);
}

Status StateMachine::restore(ShardState state) {
  if (state.shard_id != state_.shard_id) {
    return make_error(ErrorCode::corrupt_snapshot, "snapshot shard mismatch");
  }
  state_ = std::move(state);
  return std::monostate{};
}

const InstrumentConfig* StateMachine::instrument_config(
    const ConfigurationVersion version, const InstrumentId instrument_id) const noexcept {
  const auto manifest = state_.instrument_configurations.find(version);
  if (manifest != state_.instrument_configurations.end()) {
    const auto iterator = manifest->second.find(instrument_id);
    return iterator == manifest->second.end() ? nullptr : &iterator->second;
  }
  if (version != state_.current_instrument_configuration_version) {
    return nullptr;
  }
  const auto iterator = state_.instruments.find(instrument_id);
  return iterator == state_.instruments.end() ? nullptr : &iterator->second;
}

const ShardBehaviorConfig* StateMachine::behavior_config(
    const ConfigurationVersion version) const noexcept {
  const auto iterator = state_.behavior_configurations.find(version);
  return iterator == state_.behavior_configurations.end() ? nullptr
                                                            : &iterator->second;
}

bool StateMachine::expected_version_matches(const Command& command,
                                            const OrderView& order) const noexcept {
  return !command.expected_version.has_value() ||
         *command.expected_version == order.version;
}

bool StateMachine::is_known_tombstone(const OrderId order_id) const noexcept {
  return state_.tombstones.find(order_id) != state_.tombstones.end();
}

Status StateMachine::validate_location_entry(const OrderId order_id,
                                             const InstrumentId indexed_instrument) const {
  const auto book = state_.books.find(indexed_instrument);
  if (book == state_.books.end()) {
    return make_error(ErrorCode::corrupt_snapshot,
                      "order location points to a missing book");
  }
  const auto order = book->second.find(order_id);
  if (!order.has_value() || order->instrument_id != indexed_instrument) {
    return make_error(ErrorCode::corrupt_snapshot,
                      "order location points to a missing order");
  }
  return std::monostate{};
}

Status StateMachine::validate_transition(const CommittedCommand& command,
                                          const ExecutionOutput& output,
                                          const OrderBookApplyResult* outcome,
                                          const std::size_t active_orders_before,
                                          const std::optional<OrderView>& target_before,
                                          const std::span<const OrderId> evicted_tombstones) const {
  const auto invalid = [](const char* message) -> Status {
    return make_error(ErrorCode::corrupt_snapshot, message);
  };
  const auto checked_quantity_add = [](const Quantity lhs, const Quantity rhs,
                                       Quantity& result) noexcept {
    if ((rhs > 0 && lhs > std::numeric_limits<Quantity>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<Quantity>::min() - rhs)) {
      return false;
    }
    result = lhs + rhs;
    return true;
  };
  const auto validate_event_header = [this, &command](const Event& event,
                                                       const std::size_t index) {
    return event.id.shard_id == state_.shard_id && event.id.engine_seq == command.engine_seq &&
           event.id.event_index == index && event.command_identity == command.command.identity &&
           event.instrument_id == command.command.instrument_id &&
           event.occurred_at == command.received_at;
  };

  if (state_.last_committed_engine_seq != command.engine_seq ||
      state_.logical_retention_time < command.received_at ||
      output.result.identity != command.command.identity ||
      !output.result.engine_seq.has_value() ||
      *output.result.engine_seq != command.engine_seq) {
    return invalid("state transition result metadata mismatch");
  }

  const ProducerKey producer_key{command.command.identity.producer_id,
                                 command.command.identity.producer_stream_id};
  const auto producer = state_.producer_states.find(producer_key);
  if (producer == state_.producer_states.end() ||
      producer->second.current_epoch != command.command.identity.producer_epoch ||
      producer->second.last_processed_seq != command.command.identity.producer_seq ||
      producer->second.last_result != output.result) {
    return invalid("state transition producer result mismatch");
  }

  if (state_.order_locations.size() != state_.active_order_count ||
      state_.tombstone_order.size() != state_.tombstones.size()) {
    return invalid("state transition aggregate mismatch");
  }

  if (outcome == nullptr) {
    if (state_.active_order_count != active_orders_before || output.events.size() != 1U ||
        output.result.command_status != CommandStatus::rejected ||
        output.result.error_code == ErrorCode::none) {
      return invalid("rejected transition changed active order count");
    }
    if (!validate_event_header(output.events.front(), 0U) ||
        output.events.front().event_type != EventType::command_rejected) {
      return invalid("rejected transition event mismatch");
    }
    const auto* payload = std::get_if<CommandRejectedEventPayload>(
        &output.events.front().payload);
    if (payload == nullptr || payload->command_type != command.command.command_type ||
        payload->order_id != output.result.order_id ||
        payload->error_code != output.result.error_code) {
      return invalid("rejected transition payload mismatch");
    }
    return std::monostate{};
  } else {
    if (state_.current_behavior_configuration_version !=
            command.behavior_configuration_version ||
        state_.current_instrument_configuration_version !=
            command.instrument_configuration_version ||
        behavior_config(command.behavior_configuration_version) == nullptr ||
        instrument_config(command.instrument_configuration_version,
                          command.command.instrument_id) == nullptr) {
      return invalid("state transition configuration mismatch");
    }

    const auto delta = outcome->active_delta;
    if (delta >= 0) {
      const auto increase = static_cast<std::size_t>(delta);
      if (active_orders_before > std::numeric_limits<std::size_t>::max() - increase ||
          state_.active_order_count != active_orders_before + increase) {
        return invalid("state transition active order delta mismatch");
      }
    } else {
      const auto decrease = static_cast<std::size_t>(-(delta + 1)) + 1U;
      if (decrease > active_orders_before ||
          state_.active_order_count != active_orders_before - decrease) {
        return invalid("state transition active order delta mismatch");
      }
    }

    if (outcome->maker_updates.size() != outcome->trades.size()) {
      return invalid("maker update and trade count mismatch");
    }
    if (outcome->target.has_value() != outcome->changed) {
      return invalid("transition target and status mismatch");
    }

    std::size_t terminal_cursor = 0;
    std::size_t evicted_terminal_count = 0;
    bool retained_terminal_seen = false;
    const auto validate_terminal_reference =
        [this, &invalid, &terminal_cursor, &evicted_terminal_count,
         &retained_terminal_seen, &outcome,
         &command](const OrderView& expected) -> Status {
      if (terminal_cursor >= outcome->terminal_orders.size() ||
          outcome->terminal_orders[terminal_cursor] != expected ||
          !valid_order_view(expected) || is_active_order(expected.status) ||
          expected.instrument_id != command.command.instrument_id) {
        return invalid("touched terminal order mismatch");
      }
      if (state_.order_locations.contains(expected.order_id)) {
        return invalid("terminal order location was not removed");
      }
      const auto book = state_.books.find(expected.instrument_id);
      if (book != state_.books.end() && book->second.contains(expected.order_id)) {
        return invalid("terminal order was not removed from book");
      }
      const auto tombstone = state_.tombstones.find(expected.order_id);
      if (tombstone == state_.tombstones.end()) {
        if (retained_terminal_seen) {
          return invalid("terminal eviction is not a prefix");
        }
        ++evicted_terminal_count;
      } else if (tombstone->second.order_id != expected.order_id ||
                 tombstone->second.final_status != expected.status ||
                 tombstone->second.final_version != expected.version ||
                 tombstone->second.terminal_engine_seq != command.engine_seq ||
                 tombstone->second.terminal_time != command.received_at) {
        return invalid("touched terminal tombstone mismatch");
      } else {
        retained_terminal_seen = true;
      }
      ++terminal_cursor;
      return std::monostate{};
    };

    std::size_t terminal_maker_count = 0;

    const auto validate_active_order =
        [this, &invalid, instrument_id = command.command.instrument_id](
            const OrderView& expected) -> Status {
      if (!valid_order_view(expected) || !is_active_order(expected.status) ||
          expected.instrument_id != instrument_id) {
        return invalid("invalid touched active order");
      }
      if (state_.tombstones.contains(expected.order_id)) {
        return invalid("active order has terminal tombstone");
      }
      const auto location = state_.order_locations.find(expected.order_id);
      if (location == state_.order_locations.end() || location->second != expected.instrument_id) {
        return invalid("touched order location mismatch");
      }
      const auto book = state_.books.find(expected.instrument_id);
      if (book == state_.books.end()) {
        return invalid("touched order book missing");
      }
      const auto actual = book->second.find(expected.order_id);
      if (!actual.has_value() || *actual != expected) {
        return invalid("touched order view mismatch");
      }
      return std::monostate{};
    };
    Quantity taker_quantity = 0;
    Quantity taker_remaining = 0;
    Quantity taker_filled = 0;
    Quantity target_total = 0;
    if (target_before.has_value()) {
      if (!valid_order_view(*target_before) || !is_active_order(target_before->status) ||
          target_before->instrument_id != command.command.instrument_id ||
          target_before->order_id != command.command.order_id) {
        return invalid("invalid transition target before state");
      }
      target_total = target_before->total_quantity;
      taker_filled = target_before->filled_quantity;
      switch (command.command.command_type) {
        case CommandType::amend_quantity: {
          const auto* payload = std::get_if<AmendQuantityPayload>(&command.command.payload);
          if (payload == nullptr) {
            return invalid("invalid amend transition payload");
          }
          target_total = payload->new_total_quantity;
          break;
        }
        case CommandType::replace_order: {
          const auto* payload = std::get_if<ReplaceOrderPayload>(&command.command.payload);
          if (payload == nullptr) {
            return invalid("invalid replace transition payload");
          }
          target_total = payload->new_total_quantity.value_or(target_total);
          break;
        }
        case CommandType::cancel_order:
        case CommandType::new_order:
          break;
      }
      if (target_total <= 0 || target_total < taker_filled) {
        return invalid("invalid transition target quantity");
      }
      taker_remaining = target_total - taker_filled;
    } else {
      const auto* payload = std::get_if<NewOrderPayload>(&command.command.payload);
      if (command.command.command_type != CommandType::new_order || payload == nullptr ||
          payload->quantity <= 0) {
        return invalid("missing transition target before state");
      }
      target_total = payload->quantity;
      taker_remaining = payload->quantity;
    }

    for (std::size_t index = 0; index < outcome->maker_updates.size(); ++index) {
      const auto& maker = outcome->maker_updates[index];
      const auto& trade = outcome->trades[index];
      if (!valid_order_view(maker) || maker.instrument_id != command.command.instrument_id ||
          trade.quantity <= 0 || trade.maker_order_id != maker.order_id ||
          trade.price != maker.price || trade.maker_side != maker.side ||
          trade.maker_remaining_quantity != maker.remaining_quantity ||
          trade.taker_order_id != command.command.order_id ||
          trade.maker_remaining_quantity < 0 || trade.taker_remaining_quantity < 0) {
        return invalid("maker transition or trade mismatch");
      }
      const auto status = is_active_order(maker.status)
                              ? validate_active_order(maker)
                              : validate_terminal_reference(maker);
      if (std::holds_alternative<Error>(status)) {
        return status;
      }

      Quantity maker_before_remaining = 0;
      if (!checked_quantity_add(maker.remaining_quantity, trade.quantity,
                                maker_before_remaining) ||
          maker.filled_quantity < trade.quantity) {
        return invalid("maker quantity conservation mismatch");
      }
      const auto maker_before_filled = maker.filled_quantity - trade.quantity;
      Quantity maker_before_total = 0;
      if (maker_before_remaining <= 0 ||
          maker_before_filled > maker.total_quantity ||
          !checked_quantity_add(maker_before_filled, maker_before_remaining,
                                maker_before_total) ||
          maker_before_total != maker.total_quantity) {
        return invalid("maker quantity conservation mismatch");
      }
      if (maker.version <= 1U) {
        return invalid("maker version transition mismatch");
      }
      const auto expected_maker_status = maker.remaining_quantity == 0
                                             ? OrderStatus::filled
                                             : OrderStatus::partially_filled;
      if (maker.status != expected_maker_status) {
        return invalid("maker status transition mismatch");
      }
      if (!is_active_order(maker.status)) {
        ++terminal_maker_count;
      }
      if (!checked_quantity_add(taker_quantity, trade.quantity, taker_quantity) ||
          taker_quantity > taker_remaining ||
          taker_remaining - taker_quantity != trade.taker_remaining_quantity) {
        return invalid("taker quantity conservation mismatch");
      }
    }

    if (outcome->target.has_value()) {
      const auto& target = *outcome->target;
      if (!valid_order_view(target) || target.order_id != command.command.order_id ||
          target.instrument_id != command.command.instrument_id ||
          target.total_quantity != target_total) {
        return invalid("transition target mismatch");
      }
      const auto expected_remaining = command.command.command_type == CommandType::cancel_order
                                          ? Quantity{0}
                                          : taker_remaining - taker_quantity;
      Quantity expected_filled = 0;
      if (!checked_quantity_add(taker_filled, taker_quantity, expected_filled) ||
          target.remaining_quantity != expected_remaining ||
          target.filled_quantity != expected_filled) {
        return invalid("transition target quantity mismatch");
      }
      const auto status = is_active_order(target.status)
                              ? validate_active_order(target)
                              : validate_terminal_reference(target);
      if (std::holds_alternative<Error>(status)) {
        return status;
      }
      if (target_before.has_value()) {
        if (target_before->version == std::numeric_limits<OrderVersion>::max() ||
            target.version != target_before->version + 1U ||
            target.side != target_before->side) {
          return invalid("target version or side transition mismatch");
        }
        switch (command.command.command_type) {
          case CommandType::amend_quantity: {
            const auto* payload = std::get_if<AmendQuantityPayload>(&command.command.payload);
            if (payload == nullptr || target.price != target_before->price ||
                target.priority_seq != (payload->new_total_quantity > target_before->total_quantity
                                            ? command.engine_seq
                                            : target_before->priority_seq)) {
              return invalid("amend target transition mismatch");
            }
            break;
          }
          case CommandType::replace_order: {
            const auto* payload = std::get_if<ReplaceOrderPayload>(&command.command.payload);
            if (payload == nullptr || target.price != payload->new_price ||
                target.priority_seq != command.engine_seq) {
              return invalid("replace target transition mismatch");
            }
            break;
          }
          case CommandType::cancel_order:
            if (target.price != target_before->price ||
                target.priority_seq != target_before->priority_seq) {
              return invalid("cancel target transition mismatch");
            }
            break;
          case CommandType::new_order:
            return invalid("new order reached existing target transition path");
        }
      } else {
        const auto* payload = std::get_if<NewOrderPayload>(&command.command.payload);
        if (command.command.command_type != CommandType::new_order || payload == nullptr ||
            target.version != 1U || target.priority_seq != command.engine_seq ||
            target.side != payload->side || target.price != payload->price) {
          return invalid("new target transition mismatch");
        }
      }
      if (output.result.order_status != target.status ||
          output.result.order_version != target.version ||
          output.result.remaining_quantity != target.remaining_quantity ||
          output.result.filled_quantity != target.filled_quantity) {
        return invalid("transition result target mismatch");
      }
    } else if (!target_before.has_value()) {
      return invalid("new transition target missing");
    } else if (taker_quantity != 0) {
      return invalid("trade transition has no target");
    } else if (target_before.has_value() &&
               (output.result.order_status != target_before->status ||
                output.result.order_version != target_before->version ||
                output.result.remaining_quantity != target_before->remaining_quantity ||
                output.result.filled_quantity != target_before->filled_quantity)) {
      return invalid("no-change result target mismatch");
    }
    const auto expected_status = outcome->changed ? CommandStatus::committed
                                                  : CommandStatus::no_change;
    if (output.result.command_status != expected_status ||
        output.result.error_code != ErrorCode::none ||
        output.result.order_id != command.command.order_id) {
      return invalid("transition result status mismatch");
    }

    const auto target_was_active = target_before.has_value() && is_active_order(target_before->status);
    const auto target_is_active = outcome->target.has_value()
                                      ? is_active_order(outcome->target->status)
                                      : (!outcome->changed && target_was_active);
    const auto expected_active_delta = (target_is_active ? 1 : 0) -
                                       (target_was_active ? 1 : 0) -
                                       static_cast<int>(terminal_maker_count);
    if (outcome->active_delta != expected_active_delta) {
      return invalid("transition active delta mismatch");
    }

    if (terminal_cursor != outcome->terminal_orders.size()) {
      return invalid("terminal transition set mismatch");
    }
    if (evicted_terminal_count != 0U) {
      if (evicted_terminal_count > evicted_tombstones.size()) {
        return invalid("terminal tombstone eviction mismatch");
      }
      const auto eviction_offset = evicted_tombstones.size() - evicted_terminal_count;
      for (std::size_t index = 0; index < evicted_terminal_count; ++index) {
        if (evicted_tombstones[eviction_offset + index] !=
            outcome->terminal_orders[index].order_id) {
          return invalid("terminal tombstone eviction mismatch");
        }
      }
    }

    std::size_t event_index = 0;
    for (std::size_t index = 0; index < outcome->trades.size(); ++index) {
      if (event_index >= output.events.size() ||
          !validate_event_header(output.events[event_index], event_index) ||
          output.events[event_index].event_type != EventType::trade) {
        return invalid("trade event header mismatch");
      }
      const auto* payload = std::get_if<TradeEventPayload>(&output.events[event_index].payload);
      const auto& trade = outcome->trades[index];
      if (payload == nullptr || *payload != TradeEventPayload{
          TradeId{state_.shard_id, command.engine_seq, static_cast<std::uint32_t>(index)},
          trade.price, trade.quantity, trade.maker_order_id, trade.taker_order_id,
          trade.maker_side, trade.maker_remaining_quantity, trade.taker_remaining_quantity}) {
        return invalid("trade event payload mismatch");
      }
      ++event_index;
    }
    for (const auto& maker : outcome->maker_updates) {
      if (event_index >= output.events.size() ||
          !validate_event_header(output.events[event_index], event_index) ||
          output.events[event_index].event_type != EventType::order_updated) {
        return invalid("maker event header mismatch");
      }
      const auto* payload =
          std::get_if<OrderUpdatedEventPayload>(&output.events[event_index].payload);
      if (payload == nullptr || payload->order != maker || payload->update_reason != ErrorCode::none) {
        return invalid("maker event payload mismatch");
      }
      ++event_index;
    }
    if (outcome->target.has_value() && outcome->changed) {
      if (event_index >= output.events.size() ||
          !validate_event_header(output.events[event_index], event_index) ||
          output.events[event_index].event_type != EventType::order_updated) {
        return invalid("target event header mismatch");
      }
      const auto* payload =
          std::get_if<OrderUpdatedEventPayload>(&output.events[event_index].payload);
      if (payload == nullptr || payload->order != *outcome->target ||
          payload->update_reason != ErrorCode::none) {
        return invalid("target event payload mismatch");
      }
      ++event_index;
    }
    if (event_index != output.events.size()) {
      return invalid("unexpected transition event");
    }
  }
  if (output.events.size() > std::numeric_limits<std::uint32_t>::max()) {
    return invalid("state transition event index overflow");
  }
  return std::monostate{};
}

Status StateMachine::validate_tombstone_suffix(
    const std::span<const OrderView> terminal_orders) const {
  if (terminal_orders.empty()) {
    return std::monostate{};
  }
  if (state_.tombstone_order.size() < terminal_orders.size()) {
    return make_error(ErrorCode::corrupt_snapshot, "terminal tombstone index is truncated");
  }
  const auto offset = state_.tombstone_order.size() - terminal_orders.size();
  const auto first_tombstone = state_.tombstones.find(terminal_orders.front().order_id);
  if (first_tombstone == state_.tombstones.end()) {
    return make_error(ErrorCode::corrupt_snapshot, "terminal tombstone suffix is missing");
  }
  if (offset > 0 && state_.tombstone_order[offset - 1U].first >
                        first_tombstone->second.terminal_engine_seq) {
    return make_error(ErrorCode::corrupt_snapshot, "terminal tombstone index is out of order");
  }
  for (std::size_t index = 0; index < terminal_orders.size(); ++index) {
    const auto [sequence, order_id] = state_.tombstone_order[offset + index];
    const auto& terminal = terminal_orders[index];
    const auto iterator = state_.tombstones.find(order_id);
    if ((index > 0 &&
         sequence < state_.tombstone_order[offset + index - 1U].first) ||
        order_id != terminal.order_id || iterator == state_.tombstones.end() ||
        iterator->second.order_id != terminal.order_id ||
        iterator->second.final_status != terminal.status ||
        iterator->second.final_version != terminal.version ||
        iterator->second.terminal_engine_seq != sequence) {
      return make_error(ErrorCode::corrupt_snapshot, "terminal tombstone suffix mismatch");
    }
  }
  return std::monostate{};
}

Result<std::vector<OrderId>> StateMachine::evict_tombstones() {
  std::vector<OrderId> evicted;
  const auto* behavior = behavior_config(state_.current_behavior_configuration_version);
  if (behavior == nullptr) {
    return evicted;
  }
  while (!state_.tombstone_order.empty()) {
    const auto [sequence, order_id] = state_.tombstone_order.front();
    const auto iterator = state_.tombstones.find(order_id);
    if (iterator == state_.tombstones.end()) {
      return make_error(ErrorCode::corrupt_snapshot,
                        "terminal tombstone index points to missing entry");
    }
    if (order_id == OrderId{} || sequence == 0 ||
        sequence > state_.last_committed_engine_seq ||
        iterator->second.order_id != order_id ||
        iterator->second.terminal_engine_seq != sequence ||
        (iterator->second.final_status != OrderStatus::filled &&
         iterator->second.final_status != OrderStatus::cancelled) ||
        iterator->second.final_version == 0 ||
        iterator->second.terminal_time > state_.logical_retention_time) {
      return make_error(ErrorCode::corrupt_snapshot, "invalid terminal tombstone prefix");
    }
    const auto age = state_.logical_retention_time - iterator->second.terminal_time;
    const bool expired = age >= behavior->terminal_tombstone_max_age_ns;
    const bool over_count = state_.tombstones.size() > behavior->terminal_tombstone_max_count;
    if (!expired && !over_count) {
      break;
    }
    state_.tombstones.erase(iterator);
    state_.tombstone_order.pop_front();
    evicted.push_back(order_id);
  }
  return evicted;
}

void StateMachine::record_terminal(const OrderView& order, const EngineSeq engine_seq,
                                   const Timestamp timestamp) {
  state_.tombstones[order.order_id] = Tombstone{order.order_id,
                                                order.status,
                                                order.version,
                                                engine_seq,
                                                timestamp};
  state_.tombstone_order.emplace_back(engine_seq, order.order_id);
}

void StateMachine::update_locations(const OrderBookApplyResult& outcome) {
  for (const auto& order : outcome.terminal_orders) {
    state_.order_locations.erase(order.order_id);
  }
  if (outcome.target.has_value()) {
    const auto& target = *outcome.target;
    if (target.status == OrderStatus::active ||
        target.status == OrderStatus::partially_filled) {
      state_.order_locations[target.order_id] = target.instrument_id;
    } else {
      state_.order_locations.erase(target.order_id);
    }
  }
  if (outcome.active_delta < 0) {
    const auto decrement = static_cast<std::size_t>(-outcome.active_delta);
    if (decrement > state_.active_order_count) {
      state_.active_order_count = 0;
    } else {
      state_.active_order_count -= decrement;
    }
  } else {
    state_.active_order_count += static_cast<std::size_t>(outcome.active_delta);
  }
}

void StateMachine::append_events(const CommittedCommand& command,
                                 const OrderBookApplyResult& outcome,
                                 ExecutionOutput& output) {
  std::uint32_t event_index = 0;
  std::uint32_t trade_index = 0;
  for (const auto& trade : outcome.trades) {
    const TradeId trade_id{state_.shard_id, command.engine_seq, trade_index++};
    output.events.push_back(Event{
        EventId{state_.shard_id, command.engine_seq, event_index++},
        command.command.identity,
        command.command.instrument_id,
        EventType::trade,
        command.received_at,
        TradeEventPayload{trade_id,
                          trade.price,
                          trade.quantity,
                          trade.maker_order_id,
                          trade.taker_order_id,
                          trade.maker_side,
                          trade.maker_remaining_quantity,
                          trade.taker_remaining_quantity}});
  }
  for (const auto& maker : outcome.maker_updates) {
    output.events.push_back(Event{
        EventId{state_.shard_id, command.engine_seq, event_index++},
        command.command.identity,
        command.command.instrument_id,
        EventType::order_updated,
        command.received_at,
        OrderUpdatedEventPayload{maker, ErrorCode::none}});
  }
  if (outcome.target.has_value() && outcome.changed) {
    output.events.push_back(Event{
        EventId{state_.shard_id, command.engine_seq, event_index++},
        command.command.identity,
        command.command.instrument_id,
        EventType::order_updated,
        command.received_at,
        OrderUpdatedEventPayload{*outcome.target, ErrorCode::none}});
  }
}

void StateMachine::save_producer_result(const CommittedCommand& command,
                                        const CommandResult& result) {
  const ProducerKey key{command.command.identity.producer_id,
                        command.command.identity.producer_stream_id};
  auto& producer = state_.producer_states[key];
  producer.current_epoch = command.command.identity.producer_epoch;
  producer.last_processed_seq = command.command.identity.producer_seq;
  producer.last_canonical_command = canonical_command_bytes(command.command);
  producer.last_result = result;
}

Result<ExecutionOutput> StateMachine::reject(const CommittedCommand& command,
                                               const ErrorCode code) {
  return reject(command, code, command.command.order_id);
}

Result<ExecutionOutput> StateMachine::reject(const CommittedCommand& command,
                                               const ErrorCode code,
                                               const OrderId order_id) {
  ExecutionOutput output;
  output.result.identity = command.command.identity;
  output.result.engine_seq = command.engine_seq;
  output.result.command_status = CommandStatus::rejected;
  output.result.error_code = code;
  output.result.order_id = order_id;
  output.events.push_back(Event{
      EventId{state_.shard_id, command.engine_seq, 0},
      command.command.identity,
      command.command.instrument_id,
      EventType::command_rejected,
      command.received_at,
      CommandRejectedEventPayload{command.command.command_type, order_id, code}});
  state_.last_committed_engine_seq = command.engine_seq;
  state_.logical_retention_time =
      std::max(state_.logical_retention_time, command.received_at);
  save_producer_result(command, output.result);
  auto evicted_result = evict_tombstones();
  if (std::holds_alternative<Error>(evicted_result)) {
    return std::get<Error>(std::move(evicted_result));
  }
  const auto& evicted_tombstones = std::get<std::vector<OrderId>>(evicted_result);
  if (const auto status = validate_transition(command, output, nullptr,
                                               state_.active_order_count,
                                               std::nullopt,
                                               evicted_tombstones);
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  return output;
}

Result<ExecutionOutput> StateMachine::apply(const CommittedCommand& command) {
  if (command.engine_seq == 0 ||
      command.engine_seq != state_.last_committed_engine_seq + 1U) {
    return make_error(ErrorCode::corrupt_wal, "non-contiguous engine sequence");
  }
  if (command.command.identity.producer_epoch == 0 ||
      command.command.identity.producer_seq == 0) {
    return make_error(ErrorCode::corrupt_wal, "invalid producer identity");
  }
  if (behavior_config(command.behavior_configuration_version) == nullptr) {
    return make_error(ErrorCode::corrupt_wal, "unknown behavior configuration");
  }
  if (command.instrument_configuration_version == 0) {
    return make_error(ErrorCode::corrupt_wal, "invalid instrument configuration version");
  }
  if (command.instrument_configuration_version !=
      state_.current_instrument_configuration_version) {
    if (state_.instrument_configurations.find(command.instrument_configuration_version) ==
        state_.instrument_configurations.end()) {
      return make_error(ErrorCode::corrupt_wal, "unknown instrument configuration");
    }
    state_.current_instrument_configuration_version =
        command.instrument_configuration_version;
    state_.instruments = state_.instrument_configurations.at(
        command.instrument_configuration_version);
  }

  state_.current_behavior_configuration_version =
      command.behavior_configuration_version;
  state_.logical_retention_time =
      std::max(state_.logical_retention_time, command.received_at);
  auto pre_eviction = evict_tombstones();
  if (std::holds_alternative<Error>(pre_eviction)) {
    return std::get<Error>(std::move(pre_eviction));
  }

  const auto& request = command.command;
  if (request.payload.valueless_by_exception()) {
    return reject(command, ErrorCode::invalid_command);
  }
  if (request.command_type != command_type(request.payload)) {
    return reject(command, ErrorCode::invalid_command);
  }

  const auto* instrument = instrument_config(command.instrument_configuration_version,
                                             request.instrument_id);
  if (instrument == nullptr) {
    return reject(command, ErrorCode::unknown_instrument);
  }
  if (instrument->assigned_shard != state_.shard_id) {
    return reject(command, ErrorCode::wrong_producer_stream);
  }
  if (instrument->tick_size <= 0 || instrument->lot_size <= 0) {
    return make_error(ErrorCode::corrupt_snapshot, "invalid instrument configuration");
  }

  if (request.command_type == CommandType::new_order) {
    const auto* payload = std::get_if<NewOrderPayload>(&request.payload);
    if (payload == nullptr || !is_valid_side(payload->side)) {
      return reject(command, ErrorCode::invalid_side);
    }
    if (payload->price <= 0 || payload->price % instrument->tick_size != 0) {
      return reject(command, ErrorCode::invalid_price);
    }
    if (payload->quantity <= 0 || payload->quantity % instrument->lot_size != 0) {
      return reject(command, ErrorCode::invalid_quantity);
    }
    if (const auto location = state_.order_locations.find(request.order_id);
        location != state_.order_locations.end()) {
      if (const auto status = validate_location_entry(request.order_id, location->second);
          std::holds_alternative<Error>(status)) {
        return std::get<Error>(status);
      }
      return reject(command, ErrorCode::duplicate_order_id);
    }
    const auto existing_book = state_.books.find(request.instrument_id);
    if (request.order_id == OrderId{} ||
        (existing_book != state_.books.end() &&
         existing_book->second.contains(request.order_id)) ||
        is_known_tombstone(request.order_id)) {
      return reject(command, ErrorCode::duplicate_order_id);
    }
    const auto* behavior = behavior_config(command.behavior_configuration_version);
    if (state_.active_order_count >= behavior->max_active_orders) {
      return reject(command, ErrorCode::shard_capacity_exceeded);
    }
    const auto active_orders_before = state_.active_order_count;
    bool inserted_book = false;
    auto book_iterator = state_.books.find(request.instrument_id);
    if (book_iterator == state_.books.end()) {
      book_iterator = state_.books.emplace(request.instrument_id,
                                           OrderBook(request.instrument_id)).first;
      inserted_book = true;
    }
    auto& book = book_iterator->second;
    OrderView incoming{request.order_id,
                       request.instrument_id,
                       payload->side,
                       payload->price,
                       payload->quantity,
                       payload->quantity,
                       0,
                       OrderStatus::active,
                       1,
                       command.engine_seq};
    auto outcome = book.add_new(incoming);
    if (outcome.error != ErrorCode::none) {
      if (inserted_book) {
        state_.books.erase(request.instrument_id);
      }
      if (outcome.error == ErrorCode::corrupt_snapshot ||
          outcome.error == ErrorCode::corrupt_wal) {
        return make_error(outcome.error, "order book transition invariant failed");
      }
      return reject(command, outcome.error);
    }
    ExecutionOutput output;
    output.result.identity = request.identity;
    output.result.engine_seq = command.engine_seq;
    output.result.command_status = CommandStatus::committed;
    output.result.order_id = request.order_id;
    output.result.order_status = outcome.target->status;
    output.result.order_version = outcome.target->version;
    output.result.remaining_quantity = outcome.target->remaining_quantity;
    output.result.filled_quantity = outcome.target->filled_quantity;
    for (const auto& terminal : outcome.terminal_orders) {
      record_terminal(terminal, command.engine_seq, command.received_at);
    }
    update_locations(outcome);
    state_.last_committed_engine_seq = command.engine_seq;
    append_events(command, outcome, output);
    save_producer_result(command, output.result);
    if (const auto status = validate_tombstone_suffix(outcome.terminal_orders);
        std::holds_alternative<Error>(status)) {
      return std::get<Error>(status);
    }
    auto evicted_result = evict_tombstones();
    if (std::holds_alternative<Error>(evicted_result)) {
      return std::get<Error>(std::move(evicted_result));
    }
    const auto& evicted_tombstones = std::get<std::vector<OrderId>>(evicted_result);
    if (const auto status = validate_transition(command, output, &outcome,
                                                 active_orders_before,
                                                 std::nullopt,
                                                 evicted_tombstones);
        std::holds_alternative<Error>(status)) {
      return std::get<Error>(status);
    }
    return output;
  }

  const auto location = state_.order_locations.find(request.order_id);
  if (location == state_.order_locations.end()) {
    if (is_known_tombstone(request.order_id)) {
      return reject(command, ErrorCode::order_already_terminal);
    }
    return reject(command, ErrorCode::order_not_found);
  }
  if (location->second != request.instrument_id) {
    if (const auto status = validate_location_entry(request.order_id, location->second);
        std::holds_alternative<Error>(status)) {
      return std::get<Error>(status);
    }
    return reject(command, ErrorCode::order_not_found);
  }
  const auto book_iterator = state_.books.find(request.instrument_id);
  if (book_iterator == state_.books.end()) {
    return make_error(ErrorCode::corrupt_snapshot, "order location points to no book");
  }
  auto& book = book_iterator->second;
  auto current = book.find(request.order_id);
  if (!current.has_value()) {
    return make_error(ErrorCode::corrupt_snapshot, "order index points to no order");
  }
  if (!expected_version_matches(request, *current)) {
    return reject(command, ErrorCode::version_conflict);
  }

  const auto active_orders_before = state_.active_order_count;
  OrderBookApplyResult outcome;
  switch (request.command_type) {
    case CommandType::amend_quantity: {
      const auto* payload = std::get_if<AmendQuantityPayload>(&request.payload);
      if (payload == nullptr) {
        return reject(command, ErrorCode::invalid_command);
      }
      outcome = book.amend_quantity(request.order_id, payload->new_total_quantity,
                                    command.engine_seq);
      break;
    }
    case CommandType::replace_order: {
      const auto* payload = std::get_if<ReplaceOrderPayload>(&request.payload);
      if (payload == nullptr || payload->new_price <= 0 ||
          payload->new_price % instrument->tick_size != 0) {
        return reject(command, ErrorCode::invalid_price);
      }
      const auto total = payload->new_total_quantity.value_or(current->total_quantity);
      if (total <= 0 || total % instrument->lot_size != 0) {
        return reject(command, ErrorCode::invalid_quantity);
      }
      outcome = book.replace(request.order_id, payload->new_price, total,
                             command.engine_seq);
      break;
    }
    case CommandType::cancel_order:
      if (!std::holds_alternative<CancelOrderPayload>(request.payload)) {
        return reject(command, ErrorCode::invalid_command);
      }
      outcome = book.cancel(request.order_id);
      break;
    case CommandType::new_order:
      return make_error(ErrorCode::corrupt_wal, "new order reached existing-order path");
  }
  if (outcome.error != ErrorCode::none) {
    if (outcome.error == ErrorCode::corrupt_snapshot ||
        outcome.error == ErrorCode::corrupt_wal) {
      return make_error(outcome.error, "order book transition invariant failed");
    }
    return reject(command, outcome.error);
  }

  ExecutionOutput output;
  output.result.identity = request.identity;
  output.result.engine_seq = command.engine_seq;
  output.result.command_status = outcome.changed ? CommandStatus::committed
                                                 : CommandStatus::no_change;
  output.result.order_id = request.order_id;
  if (outcome.target.has_value()) {
    output.result.order_status = outcome.target->status;
    output.result.order_version = outcome.target->version;
    output.result.remaining_quantity = outcome.target->remaining_quantity;
    output.result.filled_quantity = outcome.target->filled_quantity;
  } else {
    output.result.order_status = current->status;
    output.result.order_version = current->version;
    output.result.remaining_quantity = current->remaining_quantity;
    output.result.filled_quantity = current->filled_quantity;
  }
  for (const auto& terminal : outcome.terminal_orders) {
    record_terminal(terminal, command.engine_seq, command.received_at);
  }
  update_locations(outcome);
  state_.last_committed_engine_seq = command.engine_seq;
  append_events(command, outcome, output);
  save_producer_result(command, output.result);
  if (const auto status = validate_tombstone_suffix(outcome.terminal_orders);
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  auto evicted_result = evict_tombstones();
  if (std::holds_alternative<Error>(evicted_result)) {
    return std::get<Error>(std::move(evicted_result));
  }
  const auto& evicted_tombstones = std::get<std::vector<OrderId>>(evicted_result);
  if (const auto status = validate_transition(command, output, &outcome,
                                               active_orders_before,
                                               current,
                                               evicted_tombstones);
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  return output;
}

}  // namespace order_books::domain
