#include "persistence/file_ops.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace order_books::storage {
namespace {

Error system_error(const ErrorCode code, const char* operation) {
  return Error{code, std::string(operation) + ": " + std::strerror(errno)};
}

}  // namespace

Result<int> FileOps::open_append(const std::filesystem::path& path) {
  const auto descriptor = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (descriptor < 0) {
    return system_error(ErrorCode::wal_failure, "open append");
  }
  return descriptor;
}

Result<int> FileOps::open_read(const std::filesystem::path& path) {
  const auto descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return system_error(ErrorCode::corrupt_wal, "open read");
  }
  return descriptor;
}

Status FileOps::write_all(const int descriptor,
                          const std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto result = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return system_error(ErrorCode::wal_failure, "write");
    }
    offset += static_cast<std::size_t>(result);
  }
  return std::monostate{};
}

Result<std::vector<std::byte>> FileOps::read_all(const int descriptor) {
  std::vector<std::byte> result;
  std::byte buffer[16 * 1024];
  while (true) {
    const auto count = ::read(descriptor, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      return system_error(ErrorCode::corrupt_wal, "read");
    }
    if (count == 0) {
      break;
    }
    result.insert(result.end(), buffer, buffer + count);
  }
  return result;
}

Status FileOps::sync_file(const int descriptor) {
  while (::fsync(descriptor) != 0) {
    if (errno == EINTR) {
      continue;
    }
    return system_error(ErrorCode::wal_failure, "fsync");
  }
  return std::monostate{};
}

Status FileOps::sync_directory(const std::filesystem::path& directory) {
#ifdef O_DIRECTORY
  constexpr int directory_flag = O_DIRECTORY;
#else
  constexpr int directory_flag = 0;
#endif
  const auto descriptor = ::open(directory.c_str(), O_RDONLY | directory_flag);
  if (descriptor < 0) {
    return system_error(ErrorCode::wal_failure, "open directory");
  }
  const auto result = sync_file(descriptor);
  close(descriptor);
  return result;
}

Status FileOps::truncate_file(const std::filesystem::path& path,
                              const std::uint64_t size) {
  const auto descriptor = ::open(path.c_str(), O_WRONLY);
  if (descriptor < 0) {
    return system_error(ErrorCode::wal_failure, "open truncate");
  }
  if (::ftruncate(descriptor, static_cast<off_t>(size)) != 0) {
    const auto error = system_error(ErrorCode::wal_failure, "truncate");
    close(descriptor);
    return error;
  }
  const auto status = sync_file(descriptor);
  close(descriptor);
  return status;
}

void FileOps::close(const int descriptor) noexcept { (void)::close(descriptor); }

}  // namespace order_books::storage
