#pragma once

#include "domain/state_machine.hpp"

namespace order_books::domain {

[[nodiscard]] Status validate_state(const ShardState& state);

}  // namespace order_books::domain
