#include "thread_resource_telemetry.hpp"

#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace order_books::benchmark {
namespace {

ThreadResourceError error(std::string code, std::string detail = {}) {
  return ThreadResourceError{std::move(code), std::move(detail)};
}

std::string trim_line(std::string value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

std::optional<std::uint64_t> parse_unsigned(std::string_view value) noexcept {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t result{};
  const auto* first = value.data();
  const auto* last = first + value.size();
  const auto parsed = std::from_chars(first, last, result);
  if (parsed.ec != std::errc{} || parsed.ptr != last) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::uint64_t> parse_status_value(const std::string_view text,
                                                const std::string_view key) noexcept {
  std::size_t line_start = 0;
  while (line_start <= text.size()) {
    const auto line_end = text.find('\n', line_start);
    const auto line = text.substr(line_start,
                                  line_end == std::string_view::npos
                                      ? text.size() - line_start
                                      : line_end - line_start);
    if (line.starts_with(key)) {
      const auto separator = line.find(':', key.size());
      if (separator == std::string_view::npos) {
        return std::nullopt;
      }
      return parse_unsigned(line.substr(separator + 1U));
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 1U;
  }
  return std::nullopt;
}

bool parse_schedstat(const std::string_view text, std::uint64_t& runtime,
                     std::uint64_t& runqueue, std::uint64_t& timeslices) noexcept {
  std::istringstream input{std::string(text)};
  return static_cast<bool>(input >> runtime >> runqueue >> timeslices);
}

std::optional<std::uint64_t> parse_migrations(const std::string_view text) noexcept {
  std::size_t line_start = 0;
  constexpr std::string_view key = "se.nr_migrations";
  while (line_start <= text.size()) {
    const auto line_end = text.find('\n', line_start);
    const auto line = text.substr(line_start,
                                  line_end == std::string_view::npos
                                      ? text.size() - line_start
                                      : line_end - line_start);
    if (line.starts_with(key)) {
      const auto separator = line.find(':', key.size());
      if (separator == std::string_view::npos) {
        return std::nullopt;
      }
      return parse_unsigned(line.substr(separator + 1U));
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    line_start = line_end + 1U;
  }
  return std::nullopt;
}

bool checked_subtract(const std::uint64_t after, const std::uint64_t before,
                      std::uint64_t& result) noexcept {
  if (after < before) {
    return false;
  }
  result = after - before;
  return true;
}

std::optional<std::uint64_t> parse_tid(const std::string_view text) noexcept {
  return parse_unsigned(text);
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return std::nullopt;
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

}  // namespace

ThreadResourceSnapshotResult parse_thread_resource_sample(
    const std::uint64_t tid, std::string role, const std::string_view status,
    const std::string_view schedstat, const std::string_view sched) {
  try {
    const auto voluntary = parse_status_value(status, "voluntary_ctxt_switches");
    const auto involuntary = parse_status_value(status, "nonvoluntary_ctxt_switches");
    if (!voluntary.has_value() || !involuntary.has_value()) {
      return error("status_field_missing", role);
    }
    std::uint64_t runtime{};
    std::uint64_t runqueue{};
    std::uint64_t timeslices{};
    if (!parse_schedstat(schedstat, runtime, runqueue, timeslices)) {
      return error("schedstat_invalid", role);
    }
    return ThreadResourceSnapshot{std::vector<ThreadResourceSample>{ThreadResourceSample{
        tid, std::move(role), runtime, runqueue, timeslices, *voluntary, *involuntary,
        parse_migrations(sched)}}};
  } catch (...) {
    return error("sample_parse_exception");
  }
}

ThreadResourceSnapshotResult capture_thread_resources(
    const std::span<const std::string> expected_roles) {
#if !defined(__linux__)
  (void)expected_roles;
  return error("unsupported_platform", "thread diagnostics require Linux procfs");
#else
  try {
    if (expected_roles.empty()) {
      return error("expected_roles_empty");
    }
    std::unordered_set<std::string> expected(expected_roles.begin(), expected_roles.end());
    if (expected.size() != expected_roles.size()) {
      return error("expected_roles_duplicate");
    }
    std::map<std::string, ThreadResourceSample> found;
    std::error_code filesystem_error;
    std::filesystem::directory_iterator iterator("/proc/self/task", filesystem_error);
    if (filesystem_error) {
      return error("procfs_unavailable", filesystem_error.message());
    }
    for (const auto& entry : iterator) {
      const auto tid = parse_tid(entry.path().filename().string());
      if (!tid.has_value()) {
        continue;
      }
      const auto comm = read_file(entry.path() / "comm");
      if (!comm.has_value()) {
        return error("thread_comm_unreadable", entry.path().string());
      }
      const auto role = trim_line(*comm);
      if (!expected.contains(role)) {
        continue;
      }
      const auto status = read_file(entry.path() / "status");
      const auto schedstat = read_file(entry.path() / "schedstat");
      const auto sched = read_file(entry.path() / "sched");
      if (!status.has_value() || !schedstat.has_value()) {
        return error("thread_resource_unreadable", role);
      }
      const auto parsed = parse_thread_resource_sample(*tid, role, *status, *schedstat,
                                                       sched.value_or(std::string{}));
      if (std::holds_alternative<ThreadResourceError>(parsed)) {
        return parsed;
      }
      const auto& sample = std::get<ThreadResourceSnapshot>(parsed).samples.front();
      if (!found.emplace(role, sample).second) {
        return error("thread_role_duplicate", role);
      }
    }
    if (found.size() != expected.size()) {
      for (const auto& role : expected) {
        if (!found.contains(role)) {
          return error("thread_role_missing", role);
        }
      }
      return error("thread_role_count_mismatch");
    }
    ThreadResourceSnapshot result;
    result.samples.reserve(found.size());
    for (auto& [unused_role, sample] : found) {
      (void)unused_role;
      result.samples.push_back(std::move(sample));
    }
    return result;
  } catch (const std::exception& exception) {
    return error("procfs_exception", exception.what());
  } catch (...) {
    return error("procfs_exception");
  }
#endif
}

ThreadResourceDeltaResult subtract_thread_resources(
    const ThreadResourceSnapshot& before,
    const ThreadResourceSnapshot& after) {
  try {
    std::map<std::string, const ThreadResourceSample*> before_by_role;
    for (const auto& sample : before.samples) {
      if (!before_by_role.emplace(sample.role, &sample).second) {
        return error("before_role_duplicate", sample.role);
      }
    }
    std::vector<ThreadResourceDelta> result;
    result.reserve(after.samples.size());
    std::unordered_set<std::string> after_roles;
    for (const auto& current : after.samples) {
      if (!after_roles.emplace(current.role).second) {
        return error("after_role_duplicate", current.role);
      }
      const auto iterator = before_by_role.find(current.role);
      if (iterator == before_by_role.end() || iterator->second->tid != current.tid) {
        return error("snapshot_role_mismatch", current.role);
      }
      const auto& previous = *iterator->second;
      ThreadResourceSample delta;
      delta.tid = current.tid;
      delta.role = current.role;
      if (!checked_subtract(current.cpu_runtime_ns, previous.cpu_runtime_ns,
                            delta.cpu_runtime_ns) ||
          !checked_subtract(current.runqueue_wait_ns, previous.runqueue_wait_ns,
                            delta.runqueue_wait_ns) ||
          !checked_subtract(current.sched_timeslices, previous.sched_timeslices,
                            delta.sched_timeslices) ||
          !checked_subtract(current.voluntary_context_switches,
                            previous.voluntary_context_switches,
                            delta.voluntary_context_switches) ||
          !checked_subtract(current.involuntary_context_switches,
                            previous.involuntary_context_switches,
                            delta.involuntary_context_switches)) {
        return error("counter_regressed", current.role);
      }
      if (current.cpu_migrations.has_value() && previous.cpu_migrations.has_value()) {
        std::uint64_t migration_delta{};
        if (!checked_subtract(*current.cpu_migrations, *previous.cpu_migrations,
                              migration_delta)) {
          return error("migration_counter_regressed", current.role);
        }
        delta.cpu_migrations = migration_delta;
      }
      result.push_back(ThreadResourceDelta{std::move(delta)});
    }
    if (result.size() != before_by_role.size()) {
      return error("snapshot_role_count_mismatch");
    }
    return result;
  } catch (const std::exception& exception) {
    return error("delta_exception", exception.what());
  } catch (...) {
    return error("delta_exception");
  }
}

std::optional<std::uint64_t> per_million(const std::uint64_t value,
                                         const std::uint64_t commands) noexcept {
  if (commands == 0U) {
    return std::nullopt;
  }
  const auto scaled = static_cast<long double>(value) * 1'000'000.0L /
                      static_cast<long double>(commands);
  if (scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(scaled);
}

}  // namespace order_books::benchmark
