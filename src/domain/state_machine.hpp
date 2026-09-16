#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "domain/order_book.hpp"
#include "order_books/model.hpp"

namespace order_books::domain {

struct CommittedCommand {
  Command command;
  EngineSeq engine_seq{};
  Timestamp received_at{};
  ConfigurationVersion behavior_configuration_version{};
  ConfigurationVersion instrument_configuration_version{};
};

struct ProducerState {
  ProducerEpoch current_epoch{};
  ProducerSeq last_processed_seq{};
  std::vector<std::byte> last_canonical_command;
  CommandResult last_result;
};

struct Tombstone {
  OrderId order_id;
  OrderStatus final_status{OrderStatus::cancelled};
  OrderVersion final_version{};
  EngineSeq terminal_engine_seq{};
  Timestamp terminal_time{};
};

struct ShardState {
  ShardId shard_id{};
  EngineSeq last_committed_engine_seq{};
  Timestamp logical_retention_time{};
  ConfigurationVersion current_instrument_configuration_version{};
  ConfigurationVersion current_behavior_configuration_version{};
  std::size_t active_order_count{};

  std::unordered_map<InstrumentId, InstrumentConfig> instruments;
  std::unordered_map<ConfigurationVersion,
                     std::unordered_map<InstrumentId, InstrumentConfig>>
      instrument_configurations;
  std::unordered_map<ConfigurationVersion, ShardBehaviorConfig> behavior_configurations;
  std::unordered_map<InstrumentId, OrderBook> books;
  std::unordered_map<OrderId, InstrumentId, OrderIdHash> order_locations;
  std::unordered_map<ProducerKey, ProducerState, ProducerKeyHash> producer_states;
  std::unordered_map<OrderId, Tombstone, OrderIdHash> tombstones;
  std::deque<std::pair<EngineSeq, OrderId>> tombstone_order;
};

struct ExecutionOutput {
  CommandResult result;
  std::vector<Event> events;
};

[[nodiscard]] std::vector<std::byte> canonical_command_bytes(const Command& command);

class StateMachine {
 public:
  explicit StateMachine(ShardState state);

  [[nodiscard]] const ShardState& state() const noexcept { return state_; }
  [[nodiscard]] ShardState& state() noexcept { return state_; }

  Result<ExecutionOutput> apply(const CommittedCommand& command);
  Status restore(ShardState state);

 private:
  [[nodiscard]] Result<ExecutionOutput> reject(const CommittedCommand& command,
                                               ErrorCode code);
  [[nodiscard]] Result<ExecutionOutput> reject(const CommittedCommand& command,
                                               ErrorCode code,
                                               OrderId order_id);
  [[nodiscard]] const InstrumentConfig* instrument_config(
      ConfigurationVersion version, InstrumentId instrument_id) const noexcept;
  [[nodiscard]] const ShardBehaviorConfig* behavior_config(
      ConfigurationVersion version) const noexcept;
  [[nodiscard]] bool expected_version_matches(const Command& command,
                                              const OrderView& order) const noexcept;
  [[nodiscard]] bool is_known_tombstone(OrderId order_id) const noexcept;
  void evict_tombstones();
  void record_terminal(const OrderView& order, EngineSeq engine_seq,
                       Timestamp timestamp);
  void update_locations(const OrderBookApplyResult& outcome);
  void append_events(const CommittedCommand& command,
                     const OrderBookApplyResult& outcome,
                     ExecutionOutput& output);
  void save_producer_result(const CommittedCommand& command,
                            const CommandResult& result);

  ShardState state_;
};

}  // namespace order_books::domain
