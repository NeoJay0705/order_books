#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <variant>
#include <utility>

#include "domain/invariant_checker.hpp"
#include "domain/state_machine.hpp"
#include "persistence/binary_codec.hpp"

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

Command cancel_order(const ProducerSeq sequence, const InstrumentId instrument,
                     const OrderId order_id) {
  Command command;
  command.identity = CommandIdentity{42, 1, 1, sequence};
  command.instrument_id = instrument;
  command.command_type = CommandType::cancel_order;
  command.order_id = order_id;
  command.payload = CancelOrderPayload{};
  return command;
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

TEST(StateMachineTest, ProfiledApplyPreservesMatchingAndReportsChildren) {
  StateMachine normal(make_state());
  StateMachine profiled(make_state());

  auto maker = new_order(1, 7, {4, 1});
  maker.payload = NewOrderPayload{Side::sell, 100, 5};
  const auto normal_maker = normal.apply(committed(maker, 1));
  StateMachineApplyProfile profile;
  const auto profiled_maker = profiled.apply_profiled(committed(maker, 1), profile);
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(normal_maker));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(profiled_maker));
  EXPECT_EQ(std::get<ExecutionOutput>(normal_maker).result,
            std::get<ExecutionOutput>(profiled_maker).result);
  EXPECT_EQ(std::get<ExecutionOutput>(normal_maker).events,
            std::get<ExecutionOutput>(profiled_maker).events);
  EXPECT_EQ(profile.commands, 1U);
  EXPECT_EQ(profile.events,
            static_cast<std::uint64_t>(std::get<ExecutionOutput>(profiled_maker)
                                           .events.size()));

  auto taker = new_order(2, 7, {4, 2});
  taker.payload = NewOrderPayload{Side::buy, 100, 5};
  const auto normal_taker = normal.apply(committed(taker, 2));
  const auto profiled_taker = profiled.apply_profiled(committed(taker, 2), profile);
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(normal_taker));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(profiled_taker));
  EXPECT_EQ(std::get<ExecutionOutput>(normal_taker).result,
            std::get<ExecutionOutput>(profiled_taker).result);
  EXPECT_EQ(std::get<ExecutionOutput>(normal_taker).events,
            std::get<ExecutionOutput>(profiled_taker).events);
  EXPECT_EQ(profile.commands, 1U);
  EXPECT_EQ(profile.events,
            static_cast<std::uint64_t>(std::get<ExecutionOutput>(profiled_taker)
                                           .events.size()));
  EXPECT_EQ(profile.trades, 1U);
  EXPECT_EQ(profiled.state().last_committed_engine_seq, 2U);
  EXPECT_EQ(profiled.state().active_order_count, normal.state().active_order_count);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(validate_state(profiled.state())));
}

TEST(StateMachineTest, ProfiledApplyCoversMutationAndRejectBranches) {
  StateMachine normal(make_state());
  StateMachine profiled(make_state());

  const auto expect_equal = [](const Result<ExecutionOutput>& expected,
                               const Result<ExecutionOutput>& actual) {
    ASSERT_EQ(expected.index(), actual.index());
    if (std::holds_alternative<Error>(expected)) {
      EXPECT_EQ(std::get<Error>(expected), std::get<Error>(actual));
      return;
    }
    EXPECT_EQ(std::get<ExecutionOutput>(expected).result,
              std::get<ExecutionOutput>(actual).result);
    EXPECT_EQ(std::get<ExecutionOutput>(expected).events,
              std::get<ExecutionOutput>(actual).events);
  };

  const auto apply_pair = [&](Command command, const EngineSeq sequence) {
    const auto expected = normal.apply(committed(command, sequence));
    StateMachineApplyProfile profile;
    const auto actual = profiled.apply_profiled(committed(std::move(command), sequence),
                                                profile);
    expect_equal(expected, actual);
    EXPECT_EQ(profile.commands, 1U);
    if (const auto* output = std::get_if<ExecutionOutput>(&actual); output != nullptr) {
      EXPECT_EQ(profile.events, static_cast<std::uint64_t>(output->events.size()));
      EXPECT_EQ(profile.trades, static_cast<std::uint64_t>(std::count_if(
                                   output->events.begin(), output->events.end(),
                                   [](const Event& event) {
                                     return event.event_type == EventType::trade;
                                   })));
    }
    return profile;
  };

  auto maker = new_order(1, 7, {6, 1});
  maker.payload = NewOrderPayload{Side::sell, 100, 5};
  const auto new_profile = apply_pair(maker, 1);
  EXPECT_EQ(new_profile.commands, 1U);

  Command amend;
  amend.identity = CommandIdentity{42, 1, 1, 2};
  amend.instrument_id = 7;
  amend.command_type = CommandType::amend_quantity;
  amend.order_id = {6, 1};
  amend.payload = AmendQuantityPayload{3};
  const auto amend_profile = apply_pair(amend, 2);
  EXPECT_EQ(amend_profile.trades, 0U);

  Command replace;
  replace.identity = CommandIdentity{42, 1, 1, 3};
  replace.instrument_id = 7;
  replace.command_type = CommandType::replace_order;
  replace.order_id = {6, 1};
  replace.payload = ReplaceOrderPayload{101, 4};
  const auto replace_profile = apply_pair(replace, 3);
  EXPECT_EQ(replace_profile.commands, 1U);

  const auto cancel_profile = apply_pair(cancel_order(4, 7, {6, 1}), 4);
  EXPECT_EQ(cancel_profile.commands, 1U);

  auto crossing_maker = new_order(5, 7, {6, 2});
  crossing_maker.payload = NewOrderPayload{Side::sell, 100, 5};
  apply_pair(crossing_maker, 5);
  auto crossing_taker = new_order(6, 7, {6, 3});
  crossing_taker.payload = NewOrderPayload{Side::buy, 100, 5};
  const auto crossing_profile = apply_pair(crossing_taker, 6);
  EXPECT_EQ(crossing_profile.trades, 1U);

  auto duplicate = new_order(7, 7, {6, 2});
  const auto rejected_profile = apply_pair(duplicate, 7);
  EXPECT_EQ(rejected_profile.events, 1U);
  EXPECT_EQ(rejected_profile.trades, 0U);

  EXPECT_EQ(profiled.state().last_committed_engine_seq,
            normal.state().last_committed_engine_seq);
  EXPECT_EQ(profiled.state().active_order_count, normal.state().active_order_count);
  EXPECT_EQ(storage::encode_state(normal.state()), storage::encode_state(profiled.state()));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(validate_state(normal.state())));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(validate_state(profiled.state())));
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

TEST(StateMachineTest, ValidatesTerminalMakerAndTakerTransition) {
  auto state = make_state();
  state.behavior_configurations.at(1).terminal_tombstone_max_count = 1;
  StateMachine machine(std::move(state));

  auto maker = new_order(1, 7, {5, 1});
  maker.payload = NewOrderPayload{Side::sell, 100, 5};
  auto first = machine.apply(committed(std::move(maker), 1));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(first));

  auto taker = new_order(2, 7, {5, 2});
  taker.payload = NewOrderPayload{Side::buy, 100, 5};
  auto second = machine.apply(committed(std::move(taker), 2));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(second));
  const auto& output = std::get<ExecutionOutput>(second);
  EXPECT_EQ(output.result.command_status, CommandStatus::committed);
  EXPECT_EQ(output.result.order_status, OrderStatus::filled);
  EXPECT_TRUE(machine.state().books.at(7).snapshot_orders().empty());
  EXPECT_EQ(machine.state().active_order_count, 0U);
  EXPECT_EQ(machine.state().tombstones.size(), 1U);
  EXPECT_EQ(machine.state().tombstone_order.size(), 1U);
}

TEST(StateMachineTest, ValidatesAmendReplaceAndCancelTransitions) {
  StateMachine machine(make_state());

  auto first = machine.apply(committed(new_order(1, 7, {5, 1}), 1));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(first));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));

  Command amend;
  amend.identity = CommandIdentity{42, 1, 1, 2};
  amend.instrument_id = 7;
  amend.command_type = CommandType::amend_quantity;
  amend.order_id = {5, 1};
  amend.payload = AmendQuantityPayload{3};
  auto decreased = machine.apply(committed(std::move(amend), 2));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(decreased));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));

  Command increase;
  increase.identity = CommandIdentity{42, 1, 1, 3};
  increase.instrument_id = 7;
  increase.command_type = CommandType::amend_quantity;
  increase.order_id = {5, 1};
  increase.payload = AmendQuantityPayload{7};
  auto increased = machine.apply(committed(std::move(increase), 3));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(increased));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));

  auto maker = new_order(1, 7, {8, 1});
  maker.identity = CommandIdentity{43, 1, 1, 1};
  maker.payload = NewOrderPayload{Side::sell, 101, 4};
  auto maker_execution = machine.apply(committed(std::move(maker), 4));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(maker_execution));

  Command replace;
  replace.identity = CommandIdentity{42, 1, 1, 4};
  replace.instrument_id = 7;
  replace.command_type = CommandType::replace_order;
  replace.order_id = {5, 1};
  replace.payload = ReplaceOrderPayload{101, 7};
  auto replaced = machine.apply(committed(std::move(replace), 5));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(replaced));
  EXPECT_EQ(std::get<ExecutionOutput>(replaced).result.order_status,
            OrderStatus::partially_filled);
  EXPECT_EQ(std::get<ExecutionOutput>(replaced).result.remaining_quantity, 3);
  ASSERT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));

  Command cancel;
  cancel.identity = CommandIdentity{42, 1, 1, 5};
  cancel.instrument_id = 7;
  cancel.command_type = CommandType::cancel_order;
  cancel.order_id = {5, 1};
  cancel.payload = CancelOrderPayload{};
  auto cancelled = machine.apply(committed(std::move(cancel), 6));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(cancelled));
  EXPECT_EQ(std::get<ExecutionOutput>(cancelled).result.order_status, OrderStatus::cancelled);
  EXPECT_TRUE(machine.state().books.at(7).snapshot_orders().empty());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));
}

TEST(StateMachineTest, ValidatesMultipleMakerQuantityConservation) {
  StateMachine machine(make_state());

  auto first_command = new_order(1, 7, {6, 1});
  first_command.payload = NewOrderPayload{Side::sell, 100, 5};
  auto first = machine.apply(committed(std::move(first_command), 1));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(first));
  auto second_command = new_order(2, 7, {6, 2});
  second_command.payload = NewOrderPayload{Side::sell, 100, 5};
  auto second = machine.apply(committed(std::move(second_command), 2));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(second));

  auto taker = new_order(3, 7, {6, 3});
  taker.payload = NewOrderPayload{Side::buy, 100, 7};
  auto execution = machine.apply(committed(std::move(taker), 3));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(execution));
  const auto& output = std::get<ExecutionOutput>(execution);
  EXPECT_EQ(output.result.order_status, OrderStatus::filled);
  EXPECT_EQ(output.events.size(), 5U);
  EXPECT_EQ(std::count_if(output.events.begin(), output.events.end(), [](const auto& event) {
              return event.event_type == EventType::trade;
            }),
            2U);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));
}

TEST(StateMachineTest, RejectsMissingTombstoneIndexInsteadOfHealingIt) {
  auto state = make_state();
  state.tombstone_order.emplace_back(1, OrderId{9, 9});
  StateMachine machine(std::move(state));

  auto execution = machine.apply(committed(new_order(1, 7, {9, 1}), 1));
  ASSERT_TRUE(std::holds_alternative<Error>(execution));
  EXPECT_EQ(std::get<Error>(execution).code, ErrorCode::corrupt_snapshot);
}

TEST(StateMachineTest, RejectsMismatchedTombstoneSequence) {
  auto state = make_state();
  const auto order_id = OrderId{9, 9};
  state.tombstones.emplace(
      order_id, Tombstone{order_id, OrderStatus::filled, 1, 2, 1001});
  state.tombstone_order.emplace_back(1, order_id);
  StateMachine machine(std::move(state));

  auto execution = machine.apply(committed(new_order(1, 7, {9, 1}), 1));
  ASSERT_TRUE(std::holds_alternative<Error>(execution));
  EXPECT_EQ(std::get<Error>(execution).code, ErrorCode::corrupt_snapshot);
}

TEST(StateMachineTest, RejectsActiveOrderCountMismatch) {
  StateMachine machine(make_state());
  auto first = machine.apply(committed(new_order(1, 7, {10, 1}), 1));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(first));
  machine.state().active_order_count = 0;

  Command amend;
  amend.identity = CommandIdentity{42, 1, 1, 2};
  amend.instrument_id = 7;
  amend.command_type = CommandType::amend_quantity;
  amend.order_id = {10, 1};
  amend.payload = AmendQuantityPayload{6};
  auto execution = machine.apply(committed(std::move(amend), 2));
  ASSERT_TRUE(std::holds_alternative<Error>(execution));
  EXPECT_EQ(std::get<Error>(execution).code, ErrorCode::corrupt_snapshot);
}

TEST(StateMachineTest, RejectsExistingOrderWithCorruptLocationIndex) {
  StateMachine machine(make_state());
  auto first = machine.apply(committed(new_order(1, 7, {11, 1}), 1));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(first));
  machine.state().order_locations[{11, 1}] = 99;

  Command amend;
  amend.identity = CommandIdentity{42, 1, 1, 2};
  amend.instrument_id = 7;
  amend.command_type = CommandType::amend_quantity;
  amend.order_id = {11, 1};
  amend.payload = AmendQuantityPayload{6};
  const auto execution = machine.apply(committed(std::move(amend), 2));
  ASSERT_TRUE(std::holds_alternative<Error>(execution));
  EXPECT_EQ(std::get<Error>(execution).code, ErrorCode::corrupt_snapshot);
}

TEST(StateMachineTest, RejectsNewOrderWithCorruptLocationIndex) {
  StateMachine machine(make_state());
  auto first = machine.apply(committed(new_order(1, 7, {12, 1}), 1));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(first));
  machine.state().order_locations[{13, 1}] = 99;

  const auto execution = machine.apply(committed(new_order(2, 7, {13, 1}), 2));
  ASSERT_TRUE(std::holds_alternative<Error>(execution));
  EXPECT_EQ(std::get<Error>(execution).code, ErrorCode::corrupt_snapshot);
}

TEST(StateMachineTest, RejectsLocationIndexPointingToMissingOrder) {
  StateMachine machine(make_state());
  auto first = machine.apply(committed(new_order(1, 7, {13, 1}), 1));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(first));
  machine.state().order_locations[{13, 2}] = 7;

  const auto execution = machine.apply(committed(new_order(2, 7, {13, 2}), 2));
  ASSERT_TRUE(std::holds_alternative<Error>(execution));
  EXPECT_EQ(std::get<Error>(execution).code, ErrorCode::corrupt_snapshot);
}

TEST(StateMachineTest, RetainsTombstoneBeforeAgeAndCountPolicy) {
  auto state = make_state();
  state.behavior_configurations.at(1).terminal_tombstone_max_count = 1;
  state.behavior_configurations.at(1).terminal_tombstone_max_age_ns = 100;
  StateMachine machine(std::move(state));

  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(new_order(1, 7, {14, 1}), 1))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(cancel_order(2, 7, {14, 1}), 2))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(new_order(3, 7, {14, 2}), 3))));

  EXPECT_TRUE(machine.state().tombstones.contains({14, 1}));
  EXPECT_EQ(machine.state().tombstone_order.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));
}

TEST(StateMachineTest, EvictsOnlyExpiredTombstonePrefix) {
  auto state = make_state();
  state.behavior_configurations.at(1).terminal_tombstone_max_age_ns = 1;
  StateMachine machine(std::move(state));

  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(new_order(1, 7, {15, 1}), 1))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(cancel_order(2, 7, {15, 1}), 2))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(new_order(3, 7, {15, 2}), 3))));

  EXPECT_FALSE(machine.state().tombstones.contains({15, 1}));
  EXPECT_TRUE(machine.state().tombstone_order.empty());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));
}

TEST(StateMachineTest, AgeEvictionStopsAtFirstUnexpiredTombstone) {
  auto state = make_state();
  state.behavior_configurations.at(1).terminal_tombstone_max_age_ns = 3;
  StateMachine machine(std::move(state));

  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(new_order(1, 7, {17, 1}), 1))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(cancel_order(2, 7, {17, 1}), 2))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(new_order(3, 7, {17, 2}), 3))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(cancel_order(4, 7, {17, 2}), 4))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(new_order(5, 7, {17, 3}), 5))));

  EXPECT_FALSE(machine.state().tombstones.contains({17, 1}));
  EXPECT_TRUE(machine.state().tombstones.contains({17, 2}));
  ASSERT_EQ(machine.state().tombstone_order.size(), 1U);
  EXPECT_EQ(machine.state().tombstone_order.front().second, (OrderId{17, 2}));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));
}

TEST(StateMachineTest, CountEvictionRemovesOnlyOldestPrefix) {
  auto state = make_state();
  state.behavior_configurations.at(1).terminal_tombstone_max_count = 1;
  StateMachine machine(std::move(state));

  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(new_order(1, 7, {16, 1}), 1))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(cancel_order(2, 7, {16, 1}), 2))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(new_order(3, 7, {16, 2}), 3))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(cancel_order(4, 7, {16, 2}), 4))));

  EXPECT_FALSE(machine.state().tombstones.contains({16, 1}));
  EXPECT_TRUE(machine.state().tombstones.contains({16, 2}));
  ASSERT_EQ(machine.state().tombstone_order.size(), 1U);
  EXPECT_EQ(machine.state().tombstone_order.front().second, (OrderId{16, 2}));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));
}

TEST(StateMachineTest, PreAndPostEvictionUseIndependentTerminalPrefixes) {
  auto state = make_state();
  state.behavior_configurations.at(1).terminal_tombstone_max_age_ns = 3;
  state.behavior_configurations.at(1).terminal_tombstone_max_count = 2;
  StateMachine machine(std::move(state));

  auto maker = new_order(1, 7, {18, 1});
  maker.payload = NewOrderPayload{Side::sell, 100, 5};
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(std::move(maker), 1))));

  auto old_order = new_order(2, 7, {18, 2});
  old_order.payload = NewOrderPayload{Side::buy, 90, 5};
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(std::move(old_order), 2))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(cancel_order(3, 7, {18, 2}), 3))));

  auto recent_order = new_order(4, 7, {18, 3});
  recent_order.payload = NewOrderPayload{Side::buy, 90, 5};
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(std::move(recent_order), 4))));
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(cancel_order(5, 7, {18, 3}), 5))));

  auto taker = new_order(6, 7, {18, 4});
  taker.payload = NewOrderPayload{Side::buy, 100, 5};
  ASSERT_TRUE(std::holds_alternative<ExecutionOutput>(
      machine.apply(committed(std::move(taker), 6))));

  EXPECT_FALSE(machine.state().tombstones.contains({18, 2}));
  EXPECT_FALSE(machine.state().tombstones.contains({18, 3}));
  EXPECT_TRUE(machine.state().tombstones.contains({18, 1}));
  EXPECT_TRUE(machine.state().tombstones.contains({18, 4}));
  ASSERT_EQ(machine.state().tombstone_order.size(), 2U);
  EXPECT_EQ(machine.state().tombstone_order[0].second, (OrderId{18, 1}));
  EXPECT_EQ(machine.state().tombstone_order[1].second, (OrderId{18, 4}));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(validate_state(machine.state())));
}

}  // namespace
}  // namespace order_books::domain
