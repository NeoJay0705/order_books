#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "order_books/model.hpp"

namespace order_books::domain {

struct TradeRecord {
  Price price{};
  Quantity quantity{};
  OrderId maker_order_id;
  OrderId taker_order_id;
  Side maker_side{Side::buy};
  Quantity maker_remaining_quantity{};
  Quantity taker_remaining_quantity{};
};

struct OrderBookApplyResult {
  ErrorCode error{ErrorCode::none};
  bool changed{false};
  int active_delta{0};
  std::optional<OrderView> target;
  std::vector<OrderView> maker_updates;
  std::vector<OrderView> terminal_orders;
  std::vector<TradeRecord> trades;
};

class OrderBook {
 public:
  explicit OrderBook(InstrumentId instrument_id = 0) : instrument_id_(instrument_id) {}

  [[nodiscard]] InstrumentId instrument_id() const noexcept { return instrument_id_; }
  [[nodiscard]] std::size_t active_order_count() const noexcept { return orders_.size(); }
  [[nodiscard]] std::size_t active_price_level_count() const noexcept {
    return bids_.size() + asks_.size();
  }

  [[nodiscard]] std::optional<OrderView> find(OrderId order_id) const;
  [[nodiscard]] bool contains(OrderId order_id) const noexcept;

  OrderBookApplyResult add_new(OrderView incoming);
  OrderBookApplyResult amend_quantity(OrderId order_id, Quantity new_total,
                                      EngineSeq engine_seq);
  OrderBookApplyResult replace(OrderId order_id, Price new_price,
                               Quantity new_total, EngineSeq engine_seq);
  OrderBookApplyResult cancel(OrderId order_id);

  [[nodiscard]] std::optional<BookLevel> best(Side side) const;
  [[nodiscard]] BookDepth depth(Side side, std::size_t limit) const;
  [[nodiscard]] std::vector<OrderView> snapshot_orders() const;
  [[nodiscard]] Status restore_orders(const std::vector<OrderView>& orders);

 private:
  struct PriceLevel;
  struct OrderNode {
    OrderView view;
    PriceLevel* level{};
    OrderNode* previous{};
    OrderNode* next{};
  };

  struct PriceLevel {
    Price price{};
    Quantity total_quantity{};
    OrderNode* head{};
    OrderNode* tail{};
  };

  struct MatchStep {
    OrderId maker_order_id;
    Quantity quantity{};
    OrderVersion maker_version{};
  };

  struct MatchPlan {
    OrderView initial_order;
    OrderView final_order;
    std::vector<MatchStep> steps;
  };

  using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

  [[nodiscard]] PriceLevel& get_or_create_level(Side side, Price price);
  [[nodiscard]] Quantity level_total(Side side, Price price) const noexcept;
  [[nodiscard]] bool level_delta_is_valid(Side side, Price price, Quantity total_before,
                                           Quantity quantity_delta) const noexcept;
  [[nodiscard]] bool append_to_level(OrderNode& node, PriceLevel& level);
  void unlink_node(OrderNode& node);
  [[nodiscard]] OrderView erase_node(OrderNode& node);
  [[nodiscard]] Result<MatchPlan> plan_match(
      const OrderView& working,
      std::optional<OrderId> excluded_order_id = std::nullopt) const;
  [[nodiscard]] OrderBookApplyResult apply_match_plan(const MatchPlan& plan);
  [[nodiscard]] OrderBookApplyResult match_and_rest(OrderView working);
  [[nodiscard]] OrderNode* find_node(OrderId order_id) noexcept;
  [[nodiscard]] const OrderNode* find_node(OrderId order_id) const noexcept;

  InstrumentId instrument_id_{};
  std::unordered_map<OrderId, std::unique_ptr<OrderNode>, OrderIdHash> orders_;
  BidLevels bids_;
  AskLevels asks_;
};

}  // namespace order_books::domain
