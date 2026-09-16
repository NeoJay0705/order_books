#include "domain/invariant_checker.hpp"

#include <limits>
#include <unordered_set>

namespace order_books::domain {
namespace {

Error invalid_state(const char* message) {
  return Error{ErrorCode::corrupt_snapshot, message};
}

}  // namespace

Status validate_state(const ShardState& state) {
  if (state.current_behavior_configuration_version == 0 ||
      state.behavior_configurations.find(state.current_behavior_configuration_version) ==
          state.behavior_configurations.end()) {
    return invalid_state("active behavior configuration is absent");
  }
  if (state.current_instrument_configuration_version == 0 ||
      state.instrument_configurations.find(state.current_instrument_configuration_version) ==
          state.instrument_configurations.end()) {
    return invalid_state("active instrument configuration is absent");
  }
  if (state.instrument_configurations.at(state.current_instrument_configuration_version) !=
      state.instruments) {
    return invalid_state("active instrument manifest mismatch");
  }
  for (const auto& [version, manifest] : state.instrument_configurations) {
    if (version == 0) {
      return invalid_state("invalid instrument manifest version");
    }
    for (const auto& [instrument_id, instrument] : manifest) {
      if (instrument_id == 0 || instrument.instrument_id != instrument_id ||
          instrument.tick_size <= 0 || instrument.lot_size <= 0 ||
          instrument.assigned_shard != state.shard_id) {
        return invalid_state("invalid instrument manifest");
      }
    }
  }
  for (const auto& [version, behavior] : state.behavior_configurations) {
    if (version == 0 || behavior.version != version || behavior.max_active_orders == 0 ||
        behavior.terminal_tombstone_max_age_ns <= 0 ||
        behavior.terminal_tombstone_max_count == 0) {
      return invalid_state("invalid behavior configuration");
    }
  }

  std::unordered_set<OrderId, OrderIdHash> active_orders;
  std::size_t active_count = 0;
  for (const auto& [instrument_id, book] : state.books) {
    const auto instrument = state.instruments.find(instrument_id);
    if (instrument == state.instruments.end() || book.instrument_id() != instrument_id ||
        instrument->second.assigned_shard != state.shard_id) {
      return invalid_state("book does not match instrument manifest");
    }
    for (const auto& order : book.snapshot_orders()) {
      if ((order.status != OrderStatus::active &&
           order.status != OrderStatus::partially_filled) ||
          order.price <= 0 || order.remaining_quantity <= 0 || order.version == 0 ||
          order.priority_seq == 0 || order.filled_quantity < 0 ||
          order.filled_quantity > order.total_quantity ||
          order.remaining_quantity >
              std::numeric_limits<Quantity>::max() - order.filled_quantity ||
          order.total_quantity != order.remaining_quantity + order.filled_quantity ||
          (order.status == OrderStatus::active && order.filled_quantity != 0) ||
          (order.status == OrderStatus::partially_filled && order.filled_quantity == 0) ||
          !active_orders.emplace(order.order_id).second) {
        return invalid_state("invalid or duplicate active order");
      }
      const auto location = state.order_locations.find(order.order_id);
      if (location == state.order_locations.end() || location->second != instrument_id) {
        return invalid_state("order location index mismatch");
      }
      ++active_count;
    }
  }
  if (active_count != state.active_order_count ||
      state.order_locations.size() != active_count) {
    return invalid_state("active order aggregate mismatch");
  }
  for (const auto& [order_id, instrument_id] : state.order_locations) {
    const auto book = state.books.find(instrument_id);
    if (book == state.books.end() || !book->second.contains(order_id)) {
      return invalid_state("order location points to missing order");
    }
  }
  for (const auto& [key, producer] : state.producer_states) {
    if (key.stream_id != state.shard_id || producer.current_epoch == 0 ||
        producer.last_processed_seq == 0 || producer.last_canonical_command.empty() ||
        producer.last_result.identity.producer_id != key.producer_id ||
        producer.last_result.identity.producer_stream_id != key.stream_id ||
        producer.last_result.identity.producer_epoch != producer.current_epoch ||
        producer.last_result.identity.producer_seq != producer.last_processed_seq ||
        (producer.last_result.engine_seq.has_value() &&
         *producer.last_result.engine_seq > state.last_committed_engine_seq)) {
      return invalid_state("invalid producer state");
    }
  }
  std::unordered_set<OrderId, OrderIdHash> indexed_tombstones;
  EngineSeq previous_tombstone_sequence = 0;
  bool first_tombstone = true;
  for (const auto& [sequence, order_id] : state.tombstone_order) {
    const auto tombstone = state.tombstones.find(order_id);
    if (tombstone == state.tombstones.end() ||
        tombstone->second.terminal_engine_seq != sequence ||
        (!first_tombstone && sequence < previous_tombstone_sequence) ||
        !indexed_tombstones.emplace(order_id).second) {
      return invalid_state("terminal tombstone index mismatch");
    }
    first_tombstone = false;
    previous_tombstone_sequence = sequence;
  }
  if (indexed_tombstones.size() != state.tombstones.size()) {
    return invalid_state("terminal tombstone index is incomplete");
  }
  for (const auto& [order_id, tombstone] : state.tombstones) {
    if (tombstone.order_id != order_id ||
        (tombstone.final_status != OrderStatus::filled &&
         tombstone.final_status != OrderStatus::cancelled) ||
        tombstone.terminal_engine_seq == 0 ||
        tombstone.terminal_engine_seq > state.last_committed_engine_seq ||
        tombstone.final_version == 0 || state.order_locations.contains(order_id)) {
      return invalid_state("invalid terminal tombstone");
    }
  }
  return std::monostate{};
}

}  // namespace order_books::domain
