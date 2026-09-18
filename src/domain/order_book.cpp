#include "domain/order_book.hpp"

#include <algorithm>
#include <limits>

namespace order_books::domain {
namespace {

bool checked_add(const Quantity lhs, const Quantity rhs, Quantity& result) noexcept {
  if ((rhs > 0 && lhs > std::numeric_limits<Quantity>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<Quantity>::min() - rhs)) {
    return false;
  }
  result = lhs + rhs;
  return true;
}

bool checked_increment(const OrderVersion value, OrderVersion& result) noexcept {
  if (value == std::numeric_limits<OrderVersion>::max()) {
    return false;
  }
  result = value + 1U;
  return true;
}

OrderBookApplyResult failure(const ErrorCode code) noexcept {
  OrderBookApplyResult result;
  result.error = code;
  return result;
}

}  // namespace

std::optional<OrderView> OrderBook::find(const OrderId order_id) const {
  const auto* node = find_node(order_id);
  if (node == nullptr) {
    return std::nullopt;
  }
  return node->view;
}

bool OrderBook::contains(const OrderId order_id) const noexcept {
  return orders_.find(order_id) != orders_.end();
}

OrderBook::PriceLevel& OrderBook::get_or_create_level(const Side side,
                                                       const Price price) {
  if (side == Side::buy) {
    auto [iterator, inserted] = bids_.try_emplace(price);
    if (inserted) {
      iterator->second.price = price;
    }
    return iterator->second;
  }

  auto [iterator, inserted] = asks_.try_emplace(price);
  if (inserted) {
    iterator->second.price = price;
  }
  return iterator->second;
}

bool OrderBook::append_to_level(OrderNode& node, PriceLevel& level) {
  Quantity aggregate = 0;
  if (!checked_add(level.total_quantity, node.view.remaining_quantity, aggregate)) {
    return false;
  }
  node.level = &level;
  node.previous = level.tail;
  node.next = nullptr;
  if (level.tail != nullptr) {
    level.tail->next = &node;
  } else {
    level.head = &node;
  }
  level.tail = &node;
  level.total_quantity = aggregate;
  return true;
}

void OrderBook::unlink_node(OrderNode& node) {
  auto* level = node.level;
  if (level == nullptr) {
    return;
  }

  if (node.previous != nullptr) {
    node.previous->next = node.next;
  } else {
    level->head = node.next;
  }
  if (node.next != nullptr) {
    node.next->previous = node.previous;
  } else {
    level->tail = node.previous;
  }
  level->total_quantity -= node.view.remaining_quantity;

  if (level->head == nullptr) {
    if (node.view.side == Side::buy) {
      bids_.erase(level->price);
    } else {
      asks_.erase(level->price);
    }
  }

  node.level = nullptr;
  node.previous = nullptr;
  node.next = nullptr;
}

OrderView OrderBook::erase_node(OrderNode& node) {
  const auto view = node.view;
  unlink_node(node);
  orders_.erase(view.order_id);
  return view;
}

OrderBook::OrderNode* OrderBook::find_node(const OrderId order_id) noexcept {
  const auto iterator = orders_.find(order_id);
  return iterator == orders_.end() ? nullptr : iterator->second.get();
}

const OrderBook::OrderNode* OrderBook::find_node(const OrderId order_id) const noexcept {
  const auto iterator = orders_.find(order_id);
  return iterator == orders_.end() ? nullptr : iterator->second.get();
}

Quantity OrderBook::level_total(const Side side, const Price price) const noexcept {
  if (side == Side::buy) {
    const auto iterator = bids_.find(price);
    return iterator == bids_.end() ? Quantity{0} : iterator->second.total_quantity;
  }
  const auto iterator = asks_.find(price);
  return iterator == asks_.end() ? Quantity{0} : iterator->second.total_quantity;
}

bool OrderBook::level_delta_is_valid(const Side side, const Price price,
                                     const Quantity total_before,
                                     const Quantity quantity_delta) const noexcept {
  Quantity expected_after = 0;
  return checked_add(total_before, quantity_delta, expected_after) &&
         level_total(side, price) == expected_after;
}

OrderBookApplyResult OrderBook::add_new(OrderView incoming) {
  if (contains(incoming.order_id)) {
    return failure(ErrorCode::duplicate_order_id);
  }
  incoming.status = OrderStatus::active;
  incoming.version = 1;
  return match_and_rest(incoming);
}

Result<OrderBook::MatchPlan> OrderBook::plan_match(
    const OrderView& working, const std::optional<OrderId> excluded_order_id) const {
  MatchPlan plan;
  plan.initial_order = working;
  plan.final_order = working;
  plan.final_order.status = OrderStatus::active;

  const auto plan_level = [&plan](const auto& levels, const bool incoming_buy) -> bool {
    for (const auto& [price, level] : levels) {
      if ((incoming_buy && plan.final_order.price < price) ||
          (!incoming_buy && plan.final_order.price > price)) {
        break;
      }
      for (const auto* maker = level.head;
           maker != nullptr && plan.final_order.remaining_quantity > 0;
           maker = maker->next) {
        const auto quantity = std::min(plan.final_order.remaining_quantity,
                                       maker->view.remaining_quantity);
        Quantity maker_filled = 0;
        Quantity taker_filled = 0;
        OrderVersion maker_version = 0;
        if (!checked_add(maker->view.filled_quantity, quantity, maker_filled) ||
            !checked_add(plan.final_order.filled_quantity, quantity, taker_filled) ||
            !checked_increment(maker->view.version, maker_version)) {
          return false;
        }
        plan.steps.push_back(MatchStep{maker->view.order_id, quantity, maker_version});
        plan.final_order.remaining_quantity -= quantity;
        plan.final_order.filled_quantity = taker_filled;
      }
      if (plan.final_order.remaining_quantity == 0) {
        break;
      }
    }
    return true;
  };

  if (working.side == Side::buy) {
    if (!plan_level(asks_, true)) {
      return Error{ErrorCode::numeric_overflow, "matching arithmetic overflow"};
    }
  } else if (working.side == Side::sell) {
    if (!plan_level(bids_, false)) {
      return Error{ErrorCode::numeric_overflow, "matching arithmetic overflow"};
    }
  } else {
    return Error{ErrorCode::invalid_side, "invalid matching side"};
  }

  if (plan.final_order.remaining_quantity > 0) {
    Quantity destination_total = 0;
    if (working.side == Side::buy) {
      const auto level = bids_.find(working.price);
      if (level != bids_.end()) {
        destination_total = level->second.total_quantity;
        if (excluded_order_id.has_value()) {
          const auto* excluded = find_node(*excluded_order_id);
          if (excluded != nullptr && excluded->level == &level->second &&
              !checked_add(destination_total, -excluded->view.remaining_quantity,
                           destination_total)) {
            return Error{ErrorCode::numeric_overflow, "destination aggregate overflow"};
          }
        }
      }
    } else {
      const auto level = asks_.find(working.price);
      if (level != asks_.end()) {
        destination_total = level->second.total_quantity;
        if (excluded_order_id.has_value()) {
          const auto* excluded = find_node(*excluded_order_id);
          if (excluded != nullptr && excluded->level == &level->second &&
              !checked_add(destination_total, -excluded->view.remaining_quantity,
                           destination_total)) {
            return Error{ErrorCode::numeric_overflow, "destination aggregate overflow"};
          }
        }
      }
    }
    Quantity destination_after = 0;
    if (!checked_add(destination_total, plan.final_order.remaining_quantity,
                     destination_after)) {
      return Error{ErrorCode::numeric_overflow, "destination aggregate overflow"};
    }
  }

  plan.final_order.status = plan.final_order.remaining_quantity == 0
                                ? OrderStatus::filled
                                : (plan.final_order.filled_quantity == 0
                                       ? OrderStatus::active
                                       : OrderStatus::partially_filled);
  plan.final_order.priority_seq = plan.final_order.priority_seq == 0
                                      ? plan.final_order.version
                                      : plan.final_order.priority_seq;
  return plan;
}

OrderBookApplyResult OrderBook::apply_match_plan(const MatchPlan& plan) {
  OrderBookApplyResult result;
  result.trades.reserve(plan.steps.size());
  result.maker_updates.reserve(plan.steps.size());

  auto working = plan.initial_order;
  for (const auto& step : plan.steps) {
    auto* maker = find_node(step.maker_order_id);
    if (maker == nullptr || maker->level == nullptr) {
      return failure(ErrorCode::corrupt_snapshot);
    }
    const auto quantity = step.quantity;
    const auto maker_side = maker->view.side;
    const auto maker_price = maker->view.price;
    const auto level_before = maker->level->total_quantity;
    const auto maker_before = maker->view;
    OrderVersion expected_maker_version = 0;
    if (!checked_increment(maker_before.version, expected_maker_version) ||
        expected_maker_version != step.maker_version) {
      return failure(ErrorCode::corrupt_snapshot);
    }
    if (quantity <= 0 || quantity > level_before ||
        quantity > maker->view.remaining_quantity ||
        quantity > working.remaining_quantity) {
      return failure(ErrorCode::corrupt_snapshot);
    }
    Quantity maker_filled = 0;
    Quantity taker_filled = 0;
    if (!checked_add(maker->view.filled_quantity, quantity, maker_filled) ||
        !checked_add(working.filled_quantity, quantity, taker_filled)) {
      return failure(ErrorCode::corrupt_snapshot);
    }
    maker->level->total_quantity -= quantity;
    maker->view.remaining_quantity -= quantity;
    maker->view.filled_quantity = maker_filled;
    working.remaining_quantity -= quantity;
    working.filled_quantity = taker_filled;
    maker->view.status = maker->view.remaining_quantity == 0
                             ? OrderStatus::filled
                             : OrderStatus::partially_filled;
    maker->view.version = step.maker_version;
    result.trades.push_back({.price = maker->view.price,
                             .quantity = quantity,
                             .maker_order_id = maker->view.order_id,
                             .taker_order_id = plan.final_order.order_id,
                             .maker_side = maker->view.side,
                             .maker_remaining_quantity = maker->view.remaining_quantity,
                             .taker_remaining_quantity = working.remaining_quantity});
    result.maker_updates.push_back(maker->view);
    if (!level_delta_is_valid(maker_side, maker_price, level_before, -quantity) ||
        maker->view.order_id != maker_before.order_id ||
        maker->view.instrument_id != maker_before.instrument_id ||
        maker->view.side != maker_before.side || maker->view.price != maker_before.price ||
        maker->view.total_quantity != maker_before.total_quantity ||
        maker->view.version != expected_maker_version ||
        maker->view.filled_quantity != maker_filled ||
        maker->view.remaining_quantity != maker_before.remaining_quantity - quantity ||
        maker->view.priority_seq != maker_before.priority_seq ||
        maker->view.status != (maker->view.remaining_quantity == 0
                                   ? OrderStatus::filled
                                   : OrderStatus::partially_filled)) {
      return failure(ErrorCode::corrupt_snapshot);
    }
    if (maker->view.remaining_quantity == 0) {
      result.terminal_orders.push_back(maker->view);
      --result.active_delta;
      (void)erase_node(*maker);
    }
  }

  working.status = plan.final_order.status;
  working.priority_seq = plan.final_order.priority_seq;
  if (working.remaining_quantity > 0) {
    auto node = std::make_unique<OrderNode>();
    node->view = working;
    auto& level = get_or_create_level(working.side, working.price);
    auto* node_pointer = node.get();
    const auto level_before = level.total_quantity;
    orders_.emplace(working.order_id, std::move(node));
    if (!append_to_level(*node_pointer, level)) {
      return failure(ErrorCode::numeric_overflow);
    }
    if (!level_delta_is_valid(working.side, working.price, level_before,
                              working.remaining_quantity)) {
      return failure(ErrorCode::corrupt_snapshot);
    }
    ++result.active_delta;
  } else {
    result.terminal_orders.push_back(working);
  }

  result.changed = true;
  result.target = working;
  return result;
}

OrderBookApplyResult OrderBook::match_and_rest(OrderView working) {
  auto plan = plan_match(working);
  if (std::holds_alternative<Error>(plan)) {
    return failure(std::get<Error>(plan).code);
  }
  return apply_match_plan(std::get<MatchPlan>(std::move(plan)));
}

OrderBookApplyResult OrderBook::amend_quantity(const OrderId order_id,
                                                const Quantity new_total,
                                                const EngineSeq engine_seq) {
  auto* node = find_node(order_id);
  if (node == nullptr) {
    return failure(ErrorCode::order_not_found);
  }
  if (new_total <= 0 || new_total < node->view.filled_quantity) {
    return failure(ErrorCode::invalid_quantity);
  }
  if (new_total == node->view.total_quantity) {
    return {};
  }

  const auto original = node->view;
  if (new_total < original.total_quantity) {
    const auto new_remaining = new_total - original.filled_quantity;
    const auto level_before = node->level->total_quantity;
    OrderVersion new_version = 0;
    if (!checked_increment(original.version, new_version)) {
      return failure(ErrorCode::numeric_overflow);
    }
    Quantity level_after_remove = 0;
    Quantity level_after_update = 0;
    if (!checked_add(node->level->total_quantity, -original.remaining_quantity,
                     level_after_remove) ||
        !checked_add(level_after_remove, new_remaining, level_after_update)) {
      return failure(ErrorCode::numeric_overflow);
    }
    node->view.total_quantity = new_total;
    node->level->total_quantity = level_after_update;
    node->view.remaining_quantity = new_remaining;
    node->view.version = new_version;
    if (new_remaining == 0) {
      node->view.status = OrderStatus::cancelled;
      auto terminal = erase_node(*node);
      terminal.remaining_quantity = 0;
      OrderBookApplyResult result;
      result.changed = true;
      result.active_delta = -1;
      result.target = terminal;
      if (!level_delta_is_valid(original.side, original.price, level_before,
                                -original.remaining_quantity + new_remaining)) {
        return failure(ErrorCode::corrupt_snapshot);
      }
      result.terminal_orders.push_back(terminal);
      return result;
    }
    node->view.status = node->view.filled_quantity == 0
                             ? OrderStatus::active
                             : OrderStatus::partially_filled;
    OrderBookApplyResult result;
    result.changed = true;
    result.target = node->view;
    if (!level_delta_is_valid(original.side, original.price, level_before,
                              new_remaining - original.remaining_quantity)) {
      return failure(ErrorCode::corrupt_snapshot);
    }
    return result;
  }

  const auto filled = original.filled_quantity;
  OrderVersion next_version = 0;
  if (!checked_increment(original.version, next_version)) {
    return failure(ErrorCode::numeric_overflow);
  }
  const auto new_remaining = new_total - filled;
  const auto original_level_before = node->level->total_quantity;
  Quantity level_after_remove = 0;
  Quantity level_after_update = 0;
  if (!checked_add(node->level->total_quantity, -original.remaining_quantity,
                   level_after_remove) ||
      !checked_add(level_after_remove, new_remaining, level_after_update)) {
    return failure(ErrorCode::numeric_overflow);
  }
  (void)erase_node(*node);
  if (!level_delta_is_valid(original.side, original.price, original_level_before,
                            -original.remaining_quantity)) {
    return failure(ErrorCode::corrupt_snapshot);
  }
  OrderView replacement = original;
  replacement.total_quantity = new_total;
  replacement.remaining_quantity = new_remaining;
  replacement.status = filled == 0 ? OrderStatus::active
                                   : OrderStatus::partially_filled;
  replacement.version = next_version;
  replacement.priority_seq = engine_seq;
  auto replacement_node = std::make_unique<OrderNode>();
  replacement_node->view = replacement;
  auto& level = get_or_create_level(replacement.side, replacement.price);
  auto* replacement_pointer = replacement_node.get();
  const auto replacement_level_before = level.total_quantity;
  orders_.emplace(replacement.order_id, std::move(replacement_node));
  if (!append_to_level(*replacement_pointer, level)) {
    return failure(ErrorCode::numeric_overflow);
  }
  if (!level_delta_is_valid(replacement.side, replacement.price, replacement_level_before,
                            replacement.remaining_quantity)) {
    return failure(ErrorCode::corrupt_snapshot);
  }

  OrderBookApplyResult result;
  result.changed = true;
  result.target = replacement;
  return result;
}

OrderBookApplyResult OrderBook::replace(const OrderId order_id, const Price new_price,
                                        const Quantity new_total,
                                        const EngineSeq engine_seq) {
  auto* node = find_node(order_id);
  if (node == nullptr) {
    return failure(ErrorCode::order_not_found);
  }
  if (new_total <= 0 || new_total <= node->view.filled_quantity) {
    return failure(ErrorCode::invalid_quantity);
  }

  const auto original = node->view;
  OrderVersion next_version = 0;
  if (!checked_increment(original.version, next_version)) {
    return failure(ErrorCode::numeric_overflow);
  }
  OrderView replacement = original;
  replacement.price = new_price;
  replacement.total_quantity = new_total;
  replacement.remaining_quantity = new_total - original.filled_quantity;
  replacement.status = original.filled_quantity == 0
                           ? OrderStatus::active
                           : OrderStatus::partially_filled;
  replacement.version = next_version;
  replacement.priority_seq = engine_seq;
  auto plan = plan_match(replacement, original.order_id);
  if (std::holds_alternative<Error>(plan)) {
    return failure(std::get<Error>(plan).code);
  }
  const auto level_before = node->level->total_quantity;
  const auto original_side = original.side;
  const auto original_price = original.price;
  (void)erase_node(*node);
  if (!level_delta_is_valid(original_side, original_price, level_before,
                            -original.remaining_quantity)) {
    return failure(ErrorCode::corrupt_snapshot);
  }
  auto result = apply_match_plan(std::get<MatchPlan>(std::move(plan)));
  --result.active_delta;
  return result;
}

OrderBookApplyResult OrderBook::cancel(const OrderId order_id) {
  auto* node = find_node(order_id);
  if (node == nullptr) {
    return failure(ErrorCode::order_not_found);
  }
  OrderVersion next_version = 0;
  if (!checked_increment(node->view.version, next_version)) {
    return failure(ErrorCode::numeric_overflow);
  }
  const auto level_before = node->level->total_quantity;
  const auto original_side = node->view.side;
  const auto original_price = node->view.price;
  const auto original_remaining = node->view.remaining_quantity;
  auto cancelled = erase_node(*node);
  cancelled.status = OrderStatus::cancelled;
  cancelled.remaining_quantity = 0;
  cancelled.version = next_version;
  OrderBookApplyResult result;
  result.changed = true;
  result.active_delta = -1;
  result.target = cancelled;
  result.terminal_orders.push_back(cancelled);
  if (!level_delta_is_valid(original_side, original_price, level_before, -original_remaining)) {
    return failure(ErrorCode::corrupt_snapshot);
  }
  return result;
}

std::optional<BookLevel> OrderBook::best(const Side side) const {
  const auto* level = side == Side::buy
                          ? (bids_.empty() ? nullptr : &bids_.begin()->second)
                          : (asks_.empty() ? nullptr : &asks_.begin()->second);
  if (level == nullptr) {
    return std::nullopt;
  }
  std::size_t count = 0;
  for (auto* node = level->head; node != nullptr; node = node->next) {
    ++count;
  }
  return BookLevel{level->price, level->total_quantity, count};
}

BookDepth OrderBook::depth(const Side side, const std::size_t limit) const {
  BookDepth result;
  if (limit == 0) {
    return result;
  }

  if (side == Side::buy) {
    for (auto iterator = bids_.begin(); iterator != bids_.end() &&
                                         result.levels.size() < limit;
         ++iterator) {
      std::size_t count = 0;
      for (auto* node = iterator->second.head; node != nullptr; node = node->next) {
        ++count;
      }
      result.levels.push_back(
          {iterator->second.price, iterator->second.total_quantity, count});
    }
  } else {
    for (auto iterator = asks_.begin(); iterator != asks_.end() &&
                                         result.levels.size() < limit;
         ++iterator) {
      std::size_t count = 0;
      for (auto* node = iterator->second.head; node != nullptr; node = node->next) {
        ++count;
      }
      result.levels.push_back(
          {iterator->second.price, iterator->second.total_quantity, count});
    }
  }
  return result;
}

std::vector<OrderView> OrderBook::snapshot_orders() const {
  std::vector<OrderView> result;
  result.reserve(orders_.size());
  const auto append = [&result](const auto& levels) {
    for (const auto& [unused_price, level] : levels) {
      (void)unused_price;
      for (auto* node = level.head; node != nullptr; node = node->next) {
        result.push_back(node->view);
      }
    }
  };
  append(bids_);
  append(asks_);
  return result;
}

Status OrderBook::restore_orders(const std::vector<OrderView>& orders) {
  orders_.clear();
  bids_.clear();
  asks_.clear();
  for (const auto& view : orders) {
    if (view.instrument_id != instrument_id_ ||
        (view.side != Side::buy && view.side != Side::sell) || view.price <= 0 ||
        view.total_quantity <= 0 || view.remaining_quantity <= 0 ||
        view.filled_quantity < 0 || view.filled_quantity > view.total_quantity ||
        view.remaining_quantity != view.total_quantity - view.filled_quantity ||
        (view.status != OrderStatus::active &&
         view.status != OrderStatus::partially_filled) ||
        view.version == 0 || view.priority_seq == 0) {
      return Error{ErrorCode::corrupt_snapshot, "invalid order in snapshot"};
    }
    if (contains(view.order_id)) {
      return Error{ErrorCode::corrupt_snapshot, "duplicate order in snapshot"};
    }
    const auto has_invalid_fifo = [this, &view] {
      if (view.side == Side::buy) {
        const auto level_iterator = bids_.find(view.price);
        return level_iterator != bids_.end() && level_iterator->second.tail != nullptr &&
               view.priority_seq <= level_iterator->second.tail->view.priority_seq;
      }
      const auto level_iterator = asks_.find(view.price);
      return level_iterator != asks_.end() && level_iterator->second.tail != nullptr &&
             view.priority_seq <= level_iterator->second.tail->view.priority_seq;
    }();
    if (has_invalid_fifo) {
      return Error{ErrorCode::corrupt_snapshot, "invalid FIFO priority in snapshot"};
    }
    auto node = std::make_unique<OrderNode>();
    node->view = view;
    auto& level = get_or_create_level(view.side, view.price);
    auto* node_pointer = node.get();
    orders_.emplace(view.order_id, std::move(node));
    if (!append_to_level(*node_pointer, level)) {
      return Error{ErrorCode::corrupt_snapshot, "order aggregate overflows snapshot"};
    }
  }
  return std::monostate{};
}

}  // namespace order_books::domain
