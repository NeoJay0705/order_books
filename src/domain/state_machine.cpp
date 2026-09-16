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

void StateMachine::evict_tombstones() {
  const auto* behavior = behavior_config(state_.current_behavior_configuration_version);
  if (behavior == nullptr) {
    return;
  }
  while (!state_.tombstone_order.empty()) {
    const auto [sequence, order_id] = state_.tombstone_order.front();
    const auto iterator = state_.tombstones.find(order_id);
    if (iterator == state_.tombstones.end()) {
      state_.tombstone_order.pop_front();
      continue;
    }
    const auto age = state_.logical_retention_time - iterator->second.terminal_time;
    const bool expired = age >= behavior->terminal_tombstone_max_age_ns;
    const bool over_count = state_.tombstones.size() > behavior->terminal_tombstone_max_count;
    if (!expired && !over_count) {
      break;
    }
    state_.tombstones.erase(iterator);
    state_.tombstone_order.pop_front();
    (void)sequence;
  }
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
  evict_tombstones();
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
  evict_tombstones();

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
    const auto existing_book = state_.books.find(request.instrument_id);
    if (request.order_id == OrderId{} ||
        (existing_book != state_.books.end() &&
         existing_book->second.contains(request.order_id)) ||
        state_.order_locations.find(request.order_id) != state_.order_locations.end() ||
        is_known_tombstone(request.order_id)) {
      return reject(command, ErrorCode::duplicate_order_id);
    }
    const auto* behavior = behavior_config(command.behavior_configuration_version);
    if (state_.active_order_count >= behavior->max_active_orders) {
      return reject(command, ErrorCode::shard_capacity_exceeded);
    }
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
    evict_tombstones();
    return output;
  }

  const auto location = state_.order_locations.find(request.order_id);
  if (location == state_.order_locations.end() ||
      location->second != request.instrument_id) {
    if (is_known_tombstone(request.order_id)) {
      return reject(command, ErrorCode::order_already_terminal);
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
  evict_tombstones();
  return output;
}

}  // namespace order_books::domain
