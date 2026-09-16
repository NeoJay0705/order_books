#include <gtest/gtest.h>

#include <utility>

#include "domain/state_machine.hpp"

namespace order_books::domain {
namespace {

ShardState make_state() {
  ShardState state;
  state.shard_id = 1;
  state.current_instrument_configuration_version = 1;
  state.current_behavior_configuration_version = 1;
  state.instruments.emplace(7, InstrumentConfig{7, 1, 1, 1});
  state.instrument_configurations.emplace(1, state.instruments);
  state.behavior_configurations.emplace(1, ShardBehaviorConfig{});
  return state;
}

Command new_order(const ProducerSeq sequence, const InstrumentId instrument,
                  const OrderId order_id) {
  Command command;
  command.identity = CommandIdentity{42, 1, 1, sequence};
  command.instrument_id = instrument;
  command.command_type = CommandType::new_order;
  command.order_id = order_id;
  command.payload = NewOrderPayload{Side::buy, 100, 5};
  return command;
}

CommittedCommand committed(Command command, const EngineSeq sequence) {
  return CommittedCommand{std::move(command), sequence, 1000 +
                                                      static_cast<Timestamp>(sequence),
                          1, 1};
}

TEST(StateMachineTest, UnknownInstrumentIsDurableBusinessRejection) {
  StateMachine machine(make_state());
  auto execution = machine.apply(committed(new_order(1, 999, {4, 1}), 1));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(execution));
  const auto& output = std::get<ExecutionOutput>(execution);
  EXPECT_EQ(output.result.command_status, CommandStatus::rejected);
  EXPECT_EQ(output.result.error_code, ErrorCode::unknown_instrument);
  ASSERT_EQ(output.events.size(), 1U);
  EXPECT_EQ(output.events.front().event_type, EventType::command_rejected);
  EXPECT_EQ(machine.state().last_committed_engine_seq, 1U);
  EXPECT_TRUE(machine.state().books.empty());
}

TEST(StateMachineTest, AppliesOrderAndRejectsVersionConflict) {
  StateMachine machine(make_state());
  auto first = machine.apply(committed(new_order(1, 7, {5, 1}), 1));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(first));
  EXPECT_EQ(std::get<ExecutionOutput>(first).result.command_status,
            CommandStatus::committed);

  Command amend;
  amend.identity = CommandIdentity{42, 1, 1, 2};
  amend.instrument_id = 7;
  amend.command_type = CommandType::amend_quantity;
  amend.order_id = {5, 1};
  amend.expected_version = 99;
  amend.payload = AmendQuantityPayload{6};
  auto second = machine.apply(committed(std::move(amend), 2));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(second));
  EXPECT_EQ(std::get<ExecutionOutput>(second).result.error_code,
            ErrorCode::version_conflict);
  EXPECT_EQ(machine.state().last_committed_engine_seq, 2U);
}

}  // namespace
}  // namespace order_books::domain
