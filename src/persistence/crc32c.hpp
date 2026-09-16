#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace order_books::storage {

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;

}  // namespace order_books::storage
