#include <gtest/gtest.h>

#include <limits>

#include "domain/order_book.hpp"

namespace order_books::domain {
namespace {

OrderView order(const OrderId id, const Side side, const Price price,
               const Quantity quantity, const EngineSeq priority) {
  return OrderView{id, 7, side, price, quantity, quantity, 0,
                   OrderStatus::active, 1, priority};
}

TEST(OrderBookTest, MatchesAtMakerPriceAndRemovesFilledTaker) {
  OrderBook book(7);
  const auto maker = book.add_new(order({1, 1}, Side::sell, 101, 5, 1));
  ASSERT_EQ(maker.error, ErrorCode::none);

  const auto taker = book.add_new(order({1, 2}, Side::buy, 102, 3, 2));
  ASSERT_EQ(taker.error, ErrorCode::none);
  ASSERT_EQ(taker.trades.size(), 1U);
  EXPECT_EQ(taker.trades.front().price, 101);
  EXPECT_EQ(taker.trades.front().quantity, 3);
  ASSERT_TRUE(taker.target.has_value());
  EXPECT_EQ(taker.target->status, OrderStatus::filled);
  EXPECT_EQ(taker.target->remaining_quantity, 0);
  ASSERT_TRUE(book.find({1, 1}).has_value());
  EXPECT_EQ(book.find({1, 1})->remaining_quantity, 2);
  EXPECT_EQ(book.find({1, 1})->status, OrderStatus::partially_filled);
  EXPECT_FALSE(book.contains({1, 2}));
}

TEST(OrderBookTest, PreservesTimePriorityAtEqualPrice) {
  OrderBook book(7);
  ASSERT_EQ(book.add_new(order({2, 1}, Side::sell, 100, 2, 1)).error,
            ErrorCode::none);
  ASSERT_EQ(book.add_new(order({2, 2}, Side::sell, 100, 2, 2)).error,
            ErrorCode::none);

  const auto taker = book.add_new(order({2, 3}, Side::buy, 100, 3, 3));
  ASSERT_EQ(taker.error, ErrorCode::none);
  ASSERT_EQ(taker.trades.size(), 2U);
  EXPECT_EQ(taker.trades[0].maker_order_id, (OrderId{2, 1}));
  EXPECT_EQ(taker.trades[1].maker_order_id, (OrderId{2, 2}));
  EXPECT_EQ(taker.trades[1].maker_remaining_quantity, 1);
}

TEST(OrderBookTest, DepthUsesBookSideOrdering) {
  OrderBook book(7);
  ASSERT_EQ(book.add_new(order({3, 1}, Side::buy, 100, 1, 1)).error,
            ErrorCode::none);
  ASSERT_EQ(book.add_new(order({3, 2}, Side::buy, 101, 2, 2)).error,
            ErrorCode::none);
  const auto depth = book.depth(Side::buy, 2);
  ASSERT_EQ(depth.levels.size(), 2U);
  EXPECT_EQ(depth.levels[0].price, 101);
  EXPECT_EQ(depth.levels[1].price, 100);
}

TEST(OrderBookTest, AmendIncreaseOverflowLeavesOrderAndFifoUnchanged) {
  OrderBook book(7);
  ASSERT_EQ(book.add_new(order({4, 1}, Side::buy, 100,
                               std::numeric_limits<Quantity>::max() - 1, 1))
                .error,
            ErrorCode::none);
  ASSERT_EQ(book.add_new(order({4, 2}, Side::buy, 100, 1, 2)).error,
            ErrorCode::none);

  const auto before = book.snapshot_orders();
  const auto result = book.amend_quantity({4, 2}, 2, 3);
  EXPECT_EQ(result.error, ErrorCode::numeric_overflow);
  EXPECT_EQ(book.snapshot_orders(), before);
  ASSERT_TRUE(book.find({4, 2}).has_value());
  EXPECT_EQ(book.find({4, 2})->version, 1U);
  ASSERT_TRUE(book.best(Side::buy).has_value());
  EXPECT_EQ(book.best(Side::buy)->total_quantity,
            std::numeric_limits<Quantity>::max());
}

TEST(OrderBookTest, ReplaceOverflowLeavesOriginalOrderUnchanged) {
  OrderBook book(7);
  ASSERT_EQ(book.add_new(order({5, 1}, Side::buy, 100,
                               std::numeric_limits<Quantity>::max(), 1))
                .error,
            ErrorCode::none);
  ASSERT_EQ(book.add_new(order({5, 2}, Side::buy, 101, 1, 2)).error,
            ErrorCode::none);

  const auto before = book.snapshot_orders();
  const auto result = book.replace({5, 2}, 100, 2, 3);
  EXPECT_EQ(result.error, ErrorCode::numeric_overflow);
  EXPECT_EQ(book.snapshot_orders(), before);
  ASSERT_TRUE(book.find({5, 2}).has_value());
  EXPECT_EQ(book.find({5, 2})->price, 101);
  EXPECT_EQ(book.find({5, 2})->version, 1U);
}

TEST(OrderBookTest, MatchVersionOverflowLeavesAllOrdersUnchanged) {
  OrderBook book(7);
  const OrderView maker{{6, 1}, 7, Side::sell, 100, 1, 1, 0,
                        OrderStatus::active, std::numeric_limits<OrderVersion>::max(), 1};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(book.restore_orders({maker})));
  const auto before = book.snapshot_orders();

  const auto result = book.add_new(order({6, 2}, Side::buy, 100, 1, 2));
  EXPECT_EQ(result.error, ErrorCode::numeric_overflow);
  EXPECT_EQ(book.snapshot_orders(), before);
}

}  // namespace
}  // namespace order_books::domain
