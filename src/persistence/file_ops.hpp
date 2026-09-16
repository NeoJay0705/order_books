#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "order_books/model.hpp"

namespace order_books::storage {

class FileOps {
 public:
  static Result<int> open_append(const std::filesystem::path& path);
  static Result<int> open_read(const std::filesystem::path& path);
  static Status write_all(int descriptor, std::span<const std::byte> bytes);
  static Result<std::vector<std::byte>> read_all(int descriptor);
  static Status sync_file(int descriptor);
  static Status sync_directory(const std::filesystem::path& directory);
  static Status truncate_file(const std::filesystem::path& path,
                              std::uint64_t size);
  static void close(int descriptor) noexcept;
};

}  // namespace order_books::storage
