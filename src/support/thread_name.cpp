#include "support/thread_name.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace order_books::support {

void set_current_thread_name(const std::string_view name) noexcept {
#if defined(__linux__) || defined(__APPLE__)
  // Linux pthread_setname_np accepts at most 15 bytes plus the terminator;
  // using the same bounded representation keeps diagnostics portable to macOS.
  std::array<char, 16> buffer{};
  const auto length = std::min(name.size(), buffer.size() - 1U);
  if (length != 0U) {
    std::memcpy(buffer.data(), name.data(), length);
  }
#if defined(__linux__)
  (void)::pthread_setname_np(::pthread_self(), buffer.data());
#else
  (void)::pthread_setname_np(buffer.data());
#endif
#else
  (void)name;
#endif
}

}  // namespace order_books::support
