#pragma once

#include <cstdint>

#include "persistence/wal.hpp"

namespace order_books::runtime {

struct WriterProfileOptions {
  // Detailed phase timing is collected on every Nth command group.  N=1
  // preserves the original diagnostic behavior; larger values keep the
  // normal unprofiled path for the other groups.
  std::uint64_t sample_every{1};
};

// Internal, opt-in diagnostics for the single-shard writer benchmark.  This
// header is deliberately kept under src/ and is not part of the installed API.
struct WriterGroupProfile {
  std::uint64_t input_commands{};
  std::uint64_t accepted_commands{};
  std::uint64_t writer_cycle_ns{};
  std::uint64_t writer_service_ns{};
  std::uint64_t group_collect_ns{};
  std::uint64_t group_wait_ns{};
  std::uint64_t admission_ns{};
  std::uint64_t wal_append_ns{};
  std::uint64_t wal_sync_ns{};
  std::uint64_t apply_ns{};
  std::uint64_t publisher_notify_ns{};
  std::uint64_t post_apply_ns{};
  std::uint64_t completion_enqueue_ns{};
  storage::WalAppendProfile wal;
};

struct CompletionProfile {
  std::uint64_t completions{};
  std::uint64_t queue_residence_ns{};
  std::uint64_t callback_service_ns{};
  std::uint64_t max_queue_depth{};
};

class WriterProfileCollector {
 public:
  virtual ~WriterProfileCollector() = default;

  // Called once for each measured command group.  The default keeps existing
  // internal collectors source-compatible; diagnostic collectors may use it
  // to report sampling coverage.
  virtual void observe_profile_group(bool sampled) noexcept { (void)sampled; }
  virtual void observe_writer_group(const WriterGroupProfile& profile) noexcept = 0;
  virtual void observe_rejected_group(std::uint64_t input_commands) noexcept = 0;
  virtual void observe_completion(const CompletionProfile& profile) noexcept = 0;
};

}  // namespace order_books::runtime
