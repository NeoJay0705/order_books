#pragma once

#include <string_view>

namespace order_books::support {

// Best-effort name for the current OS thread.  This is intentionally an
// internal helper: thread naming is diagnostic metadata and is never part of
// runtime correctness.
void set_current_thread_name(std::string_view name) noexcept;

}  // namespace order_books::support
