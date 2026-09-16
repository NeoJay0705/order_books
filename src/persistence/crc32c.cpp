#include "persistence/crc32c.hpp"

#include <array>
#include <cstddef>

namespace order_books::storage {
namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < table.size(); ++index) {
    auto value = index;
    for (unsigned bit = 0; bit < 8U; ++bit) {
      value = (value & 1U) != 0U ? (value >> 1U) ^ 0x82f63b78U : value >> 1U;
    }
    table[index] = value;
  }
  return table;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32c(const std::span<const std::byte> bytes) noexcept {
  std::uint32_t value = 0xffffffffU;
  for (const auto byte : bytes) {
    const auto index = static_cast<std::uint8_t>(value) ^
                       std::to_integer<std::uint8_t>(byte);
    value = (value >> 8U) ^ kTable[index];
  }
  return value ^ 0xffffffffU;
}

}  // namespace order_books::storage
