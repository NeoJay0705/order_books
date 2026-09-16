#pragma once

#include <span>
#include <stop_token>

#include "order_books/model.hpp"

namespace order_books {

class EventSink {
 public:
  virtual ~EventSink() = default;

  virtual Result<std::monostate> publish(ShardId shard_id,
                                         EngineSeq engine_seq,
                                         std::span<const Event> events,
                                         std::stop_token stop_token) = 0;
};

}  // namespace order_books
