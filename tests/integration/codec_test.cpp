#include <gtest/gtest.h>

#include "persistence/binary_codec.hpp"

namespace order_books::storage {
namespace {

TEST(BinaryCodecTest, CommandRoundTripPreservesVariantAndIdentity) {
  Command command;
  command.identity = CommandIdentity{9, 3, 2, 17};
  command.instrument_id = 11;
  command.command_type = CommandType::replace_order;
  command.order_id = {8, 13};
  command.expected_version = 4;
  command.payload = ReplaceOrderPayload{101, Quantity{12}};

  const auto encoded = encode_command(command);
  const auto decoded = decode_command(encoded);
  ASSERT_TRUE(std::holds_alternative<Command>(decoded));
  const auto& actual = std::get<Command>(decoded);
  EXPECT_EQ(actual.identity, command.identity);
  EXPECT_EQ(actual.instrument_id, command.instrument_id);
  EXPECT_EQ(actual.command_type, command.command_type);
  EXPECT_EQ(actual.order_id, command.order_id);
  EXPECT_EQ(actual.expected_version, command.expected_version);
  ASSERT_TRUE(std::holds_alternative<ReplaceOrderPayload>(actual.payload));
  EXPECT_EQ(std::get<ReplaceOrderPayload>(actual.payload),
            std::get<ReplaceOrderPayload>(command.payload));
}

TEST(BinaryCodecTest, RejectsTrailingBytes) {
  Command command;
  command.identity = CommandIdentity{1, 1, 1, 1};
  command.instrument_id = 1;
  command.command_type = CommandType::cancel_order;
  command.order_id = {1, 2};
  command.payload = CancelOrderPayload{};
  auto encoded = encode_command(command);
  encoded.push_back(std::byte{0});
  const auto decoded = decode_command(encoded);
  ASSERT_TRUE(std::holds_alternative<Error>(decoded));
  EXPECT_EQ(std::get<Error>(decoded).code, ErrorCode::corrupt_snapshot);
}

}  // namespace
}  // namespace order_books::storage
