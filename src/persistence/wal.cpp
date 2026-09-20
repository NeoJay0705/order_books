#include "persistence/wal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "persistence/binary_codec.hpp"
#include "persistence/crc32c.hpp"
#include "persistence/file_ops.hpp"
#include "support/thread_name.hpp"

namespace order_books::storage {
namespace {

using ProfileClock = std::chrono::steady_clock;

std::uint64_t profile_elapsed_ns(const ProfileClock::time_point start,
                                 const ProfileClock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

void add_profile_ns(std::uint64_t& target, const std::uint64_t value) {
  if (target > std::numeric_limits<std::uint64_t>::max() - value) {
    target = std::numeric_limits<std::uint64_t>::max();
    return;
  }
  target += value;
}

constexpr std::uint16_t kRecordVersion = 1;
constexpr std::size_t kHeaderSize = 22;
constexpr std::size_t kMaxRecordSize = 1U * 1024U * 1024U;

Error wal_error(const ErrorCode code, const char* message) {
  return Error{code, message};
}

Error corrupt_at(const std::filesystem::path& path, const ShardId shard_id,
                 const std::size_t offset, const std::string_view detail) {
  return Error{ErrorCode::corrupt_wal,
               "shard=" + std::to_string(shard_id) + " path=" + path.string() +
                   " offset=" + std::to_string(offset) + " " + std::string(detail)};
}

void write_le32(std::vector<std::byte>& bytes, const std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

void write_le16(std::vector<std::byte>& bytes, const std::uint16_t value) {
  for (unsigned index = 0; index < 2U; ++index) {
    bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

void write_le64(std::vector<std::byte>& bytes, const std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
  }
}

std::uint32_t read_le32(const std::byte* bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index]))
             << (index * 8U);
  }
  return value;
}

std::uint16_t read_le16(const std::byte* bytes) {
  std::uint16_t value = 0;
  for (unsigned index = 0; index < 2U; ++index) {
    value |= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[index]))
             << (index * 8U);
  }
  return value;
}

std::uint64_t read_le64(const std::byte* bytes) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8U; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index]))
             << (index * 8U);
  }
  return value;
}

std::uint64_t segment_sequence(const std::filesystem::path& path) {
  try {
    return std::stoull(path.stem().string());
  } catch (...) {
    return 0;
  }
}

bool segment_less(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
  const auto left_sequence = segment_sequence(lhs);
  const auto right_sequence = segment_sequence(rhs);
  if (left_sequence != right_sequence) {
    return left_sequence < right_sequence;
  }
  return lhs.string() < rhs.string();
}

Result<EngineSeq> segment_sequence_checked(const std::filesystem::path& path) {
  const auto value = segment_sequence(path);
  if (value == 0 || path.stem().string() != std::to_string(value)) {
    return Error{ErrorCode::corrupt_wal, "invalid WAL segment filename"};
  }
  return value;
}

Result<std::vector<std::byte>> encode_frame(
    const domain::CommittedCommand& command, WalAppendProfile* profile) {
  const auto payload_start = profile == nullptr ? ProfileClock::time_point{}
                                                : ProfileClock::now();
  const auto payload = encode_committed_command(command);
  if (profile != nullptr) {
    add_profile_ns(profile->payload_encode_ns,
                   profile_elapsed_ns(payload_start, ProfileClock::now()));
    add_profile_ns(profile->payload_bytes,
                   static_cast<std::uint64_t>(payload.size()));
  }

  const auto assembly_before_crc_start =
      profile == nullptr ? ProfileClock::time_point{} : ProfileClock::now();
  BinaryWriter body_writer;
  body_writer.u16(kRecordVersion);
  body_writer.data().insert(body_writer.data().end(), payload.begin(), payload.end());

  const auto crc_start = profile == nullptr ? ProfileClock::time_point{}
                                            : ProfileClock::now();
  const auto body_crc = crc32c(body_writer.data());
  const auto crc_end = profile == nullptr ? ProfileClock::time_point{}
                                          : ProfileClock::now();
  if (profile != nullptr) {
    add_profile_ns(profile->frame_assembly_ns,
                   profile_elapsed_ns(assembly_before_crc_start, crc_start));
    add_profile_ns(profile->crc_ns, profile_elapsed_ns(crc_start, crc_end));
  }
  body_writer.u32(body_crc);
  const auto& body = body_writer.data();
  if (body.size() > kMaxRecordSize) {
    return wal_error(ErrorCode::wal_failure, "WAL record exceeds maximum size");
  }

  BinaryWriter frame_writer;
  frame_writer.u32(static_cast<std::uint32_t>(body.size()));
  frame_writer.data().insert(frame_writer.data().end(), body.begin(), body.end());
  if (profile != nullptr) {
    add_profile_ns(profile->frame_assembly_ns,
                   profile_elapsed_ns(crc_end, ProfileClock::now()));
  }
  return frame_writer.data();
}

}  // namespace

struct Wal::PrepareWorkers {
  struct Range {
    std::size_t begin{};
    std::size_t end{};
  };

  struct LaneResult {
    std::vector<PreparedRecord> records;
    WalAppendProfile profile{};
    std::uint64_t task_ns{};
    std::optional<Error> error;
    std::exception_ptr exception;
    std::size_t failure_index{};
  };

  struct Job {
    std::span<const domain::CommittedCommand> commands;
    std::vector<Range> ranges;
    std::vector<LaneResult> lanes;
    std::size_t completed_background{};
    std::uint64_t generation{};
    bool profiled{};
  };

  PrepareWorkers(const std::size_t lane_count, const std::size_t segment_size,
                 const ShardId shard_id)
      : lane_count_(lane_count), segment_size_(segment_size), shard_id_(shard_id) {}

  void start() {
    workers_.reserve(lane_count_ - 1U);
    for (std::size_t index = 1; index < lane_count_; ++index) {
      workers_.emplace_back([this, index](std::stop_token token) {
        worker_loop(index, token);
      });
    }
  }

  PrepareWorkers(const PrepareWorkers&) = delete;
  PrepareWorkers& operator=(const PrepareWorkers&) = delete;

  ~PrepareWorkers() { stop(); }

  Result<std::vector<PreparedRecord>> prepare(
      const std::span<const domain::CommittedCommand> commands,
      const std::size_t min_parallel_commands, WalAppendProfile* profile,
      WalPrepareStats* stats) {
    const auto range_count = std::min(lane_count_, commands.size());
    const bool use_parallel = commands.size() >= min_parallel_commands &&
                               range_count > 1U;
    if (!use_parallel) {
      auto result = prepare_sequential(commands, segment_size_, profile);
      if (stats != nullptr && std::holds_alternative<std::vector<PreparedRecord>>(result)) {
        add_profile_ns(stats->tasks, 1U);
      }
      return result;
    }
    Job job;
    job.commands = commands;
    job.ranges.reserve(range_count);
    job.lanes.resize(range_count);
    const auto base_size = commands.size() / range_count;
    const auto remainder = commands.size() % range_count;
    std::size_t begin = 0;
    for (std::size_t index = 0; index < range_count; ++index) {
      const auto length = base_size + (index < remainder ? 1U : 0U);
      job.ranges.push_back(Range{begin, begin + length});
      begin += length;
    }
    {
      std::lock_guard lock(mutex_);
      job.generation = ++generation_;
      job.profiled = profile != nullptr;
      active_job_ = &job;
      job_cv_.notify_all();
    }
    run_lane(job, 0U, segment_size_, profile != nullptr);
    {
      std::unique_lock lock(mutex_);
      completed_cv_.wait(lock, [this, &job] {
        return active_job_ == &job && job.completed_background == workers_.size();
      });
      active_job_ = nullptr;
      job_cv_.notify_all();
    }

    std::optional<std::size_t> first_failure;
    for (std::size_t index = 0; index < job.lanes.size(); ++index) {
      if (job.lanes[index].error.has_value() || job.lanes[index].exception) {
        const auto failure_index = job.lanes[index].failure_index;
        if (!first_failure.has_value() || failure_index < *first_failure) {
          first_failure = failure_index;
        }
      }
    }
    if (first_failure.has_value()) {
      for (const auto& lane : job.lanes) {
        if (lane.failure_index != *first_failure) {
          continue;
        }
        if (lane.exception) {
          std::rethrow_exception(lane.exception);
        }
        return *lane.error;
      }
    }

    std::vector<PreparedRecord> records;
    records.reserve(commands.size());
    if (profile != nullptr) {
      add_profile_ns(profile->parallel_prepare_groups, 1U);
      add_profile_ns(profile->prepare_tasks,
                     static_cast<std::uint64_t>(job.lanes.size()));
    }
    for (auto& lane : job.lanes) {
      if (profile != nullptr) {
        add_profile_ns(profile->prepare_task_ns, lane.task_ns);
        merge_profile(profile, lane.profile);
      }
      for (auto& record : lane.records) {
        records.push_back(std::move(record));
      }
    }
    if (stats != nullptr) {
      add_profile_ns(stats->parallel_groups, 1U);
      add_profile_ns(stats->tasks, static_cast<std::uint64_t>(job.lanes.size()));
    }
    return records;
  }

  void stop() noexcept {
    {
      std::lock_guard lock(mutex_);
      if (stopping_) {
        return;
      }
      stopping_ = true;
      job_cv_.notify_all();
    }
    workers_.clear();
  }

 private:
  static Result<std::vector<PreparedRecord>> prepare_sequential(
      const std::span<const domain::CommittedCommand> commands,
      const std::size_t segment_size, WalAppendProfile* profile) {
    const auto task_start = profile == nullptr ? ProfileClock::time_point{}
                                                : ProfileClock::now();
    std::vector<PreparedRecord> records;
    records.reserve(commands.size());
    for (const auto& command : commands) {
      auto frame = encode_frame(command, profile);
      if (std::holds_alternative<Error>(frame)) {
        return std::get<Error>(frame);
      }
      auto prepared_frame = std::get<std::vector<std::byte>>(std::move(frame));
      if (prepared_frame.size() > segment_size - kHeaderSize) {
        return wal_error(ErrorCode::wal_failure, "WAL record cannot fit in a segment");
      }
      records.push_back(PreparedRecord{command, std::move(prepared_frame)});
    }
    if (profile != nullptr) {
      add_profile_ns(profile->prepare_task_ns,
                     profile_elapsed_ns(task_start, ProfileClock::now()));
      profile->prepare_tasks = 1U;
    }
    return records;
  }

  static void merge_profile(WalAppendProfile* destination,
                            const WalAppendProfile& source) noexcept {
    add_profile_ns(destination->payload_encode_ns, source.payload_encode_ns);
    add_profile_ns(destination->crc_ns, source.crc_ns);
    add_profile_ns(destination->frame_assembly_ns, source.frame_assembly_ns);
    add_profile_ns(destination->payload_bytes, source.payload_bytes);
  }

  static void run_lane(Job& job, const std::size_t lane_index,
                       const std::size_t segment_size, const bool profiled) noexcept {
    auto& lane = job.lanes[lane_index];
    const auto range = job.ranges[lane_index];
    const auto start = profiled ? ProfileClock::now() : ProfileClock::time_point{};
    try {
      lane.records.reserve(range.end - range.begin);
      for (std::size_t index = range.begin; index < range.end; ++index) {
        auto frame = encode_frame(job.commands[index], profiled ? &lane.profile : nullptr);
        if (std::holds_alternative<Error>(frame)) {
          lane.failure_index = index;
          lane.error = std::get<Error>(std::move(frame));
          break;
        }
        auto prepared_frame =
            std::get<std::vector<std::byte>>(std::move(frame));
        if (prepared_frame.size() > segment_size - kHeaderSize) {
          lane.failure_index = index;
          lane.error = wal_error(ErrorCode::wal_failure,
                                 "WAL record cannot fit in a segment");
          break;
        }
        lane.records.push_back(
            PreparedRecord{job.commands[index], std::move(prepared_frame)});
      }
    } catch (...) {
      lane.failure_index = range.begin + lane.records.size();
      lane.exception = std::current_exception();
    }
    if (profiled) {
      lane.task_ns = profile_elapsed_ns(start, ProfileClock::now());
    }
  }

  void worker_loop(const std::size_t lane_index, const std::stop_token token) noexcept {
    support::set_current_thread_name("ob-wp-" + std::to_string(shard_id_) + "-" +
                                     std::to_string(lane_index));
    std::uint64_t last_generation = 0;
    for (;;) {
      Job* job = nullptr;
      {
        std::unique_lock lock(mutex_);
        job_cv_.wait(lock, token, [this, &last_generation] {
          return stopping_ || (active_job_ != nullptr &&
                               active_job_->generation != last_generation);
        });
        if (stopping_ || token.stop_requested()) {
          return;
        }
        job = active_job_;
        last_generation = job->generation;
        if (lane_index >= job->ranges.size()) {
          ++job->completed_background;
          job = nullptr;
        }
      }
      if (job == nullptr) {
        completed_cv_.notify_one();
        continue;
      }
      run_lane(*job, lane_index, segment_size_, job->profiled);
      {
        std::lock_guard lock(mutex_);
        ++job->completed_background;
      }
      completed_cv_.notify_one();
    }
  }

  std::size_t lane_count_{};
  std::size_t segment_size_{};
  ShardId shard_id_{};
  std::mutex mutex_;
  std::condition_variable_any job_cv_;
  std::condition_variable completed_cv_;
  Job* active_job_{};
  std::uint64_t generation_{};
  bool stopping_{false};
  std::vector<std::jthread> workers_;
};

Wal::Wal(std::filesystem::path directory, const ShardId shard_id,
         const std::size_t segment_size, const WalPrepareOptions prepare_options)
    : directory_(std::move(directory)),
      shard_id_(shard_id),
      segment_size_(segment_size),
      prepare_options_(prepare_options) {}

Result<std::unique_ptr<Wal>> Wal::open(std::filesystem::path directory,
                                        const ShardId shard_id,
                                        const std::size_t segment_size,
                                        const WalPrepareOptions prepare_options) {
  if (segment_size <= kHeaderSize + 32U) {
    return wal_error(ErrorCode::wal_failure, "WAL segment size is too small");
  }
  if ((prepare_options.lane_count != 1U && prepare_options.lane_count != 2U &&
       prepare_options.lane_count != 4U) ||
      prepare_options.min_parallel_commands == 0U) {
    return wal_error(ErrorCode::wal_failure, "invalid WAL prepare options");
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    return wal_error(ErrorCode::wal_failure, "cannot create WAL directory");
  }

  auto wal = std::unique_ptr<Wal>(
      new Wal(std::move(directory), shard_id, segment_size, prepare_options));
  if (const auto status = wal->initialize_prepare_workers();
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  std::vector<std::filesystem::path> segments;
  for (const auto& entry : std::filesystem::directory_iterator(wal->directory_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".wal") {
      segments.push_back(entry.path());
    }
  }
  std::sort(segments.begin(), segments.end(), segment_less);
  if (segments.empty()) {
    auto status = wal->create_segment(1);
    if (std::holds_alternative<Error>(status)) {
      return std::get<Error>(status);
    }
  } else {
    std::optional<EngineSeq> previous_sequence;
    wal->active_segment_ = segments.back();
    for (const auto& segment : segments) {
      auto checked_sequence = segment_sequence_checked(segment);
      if (std::holds_alternative<Error>(checked_sequence)) {
        const auto& error = std::get<Error>(checked_sequence);
        return corrupt_at(segment, shard_id, 0U, error.message);
      }
      if (previous_sequence.has_value() &&
          std::get<EngineSeq>(checked_sequence) == *previous_sequence) {
        return corrupt_at(segment, shard_id, 0U, "duplicate WAL segment sequence");
      }
      previous_sequence = std::get<EngineSeq>(checked_sequence);
      const auto file_size = std::filesystem::file_size(segment, filesystem_error);
      if (filesystem_error) {
        return wal_error(ErrorCode::wal_failure, "cannot stat WAL segment");
      }
      wal->size_bytes_ += file_size;
    }
    wal->active_bytes_ = std::filesystem::file_size(wal->active_segment_, filesystem_error);
    if (filesystem_error || wal->active_bytes_ < kHeaderSize) {
      return wal_error(ErrorCode::corrupt_wal, "invalid active WAL segment");
    }
    auto descriptor = FileOps::open_append(wal->active_segment_);
    if (std::holds_alternative<Error>(descriptor)) {
      return std::get<Error>(descriptor);
    }
    wal->active_descriptor_ = std::get<int>(descriptor);
  }
  return wal;
}

Wal::~Wal() {
  prepare_workers_.reset();
  if (active_descriptor_ >= 0) {
    FileOps::close(active_descriptor_);
  }
}

Status Wal::initialize_prepare_workers() {
  if (prepare_options_.lane_count == 1U) {
    return std::monostate{};
  }
  try {
    prepare_workers_ = std::make_unique<PrepareWorkers>(
        prepare_options_.lane_count, segment_size_, shard_id_);
    prepare_workers_->start();
  } catch (const std::exception&) {
    return wal_error(ErrorCode::wal_failure,
                     "cannot create WAL prepare workers");
  } catch (...) {
    return wal_error(ErrorCode::wal_failure,
                     "cannot create WAL prepare workers");
  }
  return std::monostate{};
}

std::filesystem::path Wal::segment_path(const EngineSeq first_engine_seq) const {
  return directory_ / (std::to_string(first_engine_seq) + ".wal");
}

std::vector<std::byte> Wal::segment_header(const EngineSeq first_engine_seq) const {
  std::vector<std::byte> header;
  header.reserve(kHeaderSize);
  header.push_back(static_cast<std::byte>('O'));
  header.push_back(static_cast<std::byte>('B'));
  header.push_back(static_cast<std::byte>('W'));
  header.push_back(static_cast<std::byte>('L'));
  write_le16(header, 1);
  write_le32(header, shard_id_);
  write_le64(header, first_engine_seq);
  write_le32(header, crc32c(header));
  return header;
}

Status Wal::validate_segment_header(const std::span<const std::byte> bytes,
                                    const EngineSeq first_engine_seq) const {
  if (bytes.size() < kHeaderSize || bytes[0] != static_cast<std::byte>('O') ||
      bytes[1] != static_cast<std::byte>('B') || bytes[2] != static_cast<std::byte>('W') ||
      bytes[3] != static_cast<std::byte>('L') || read_le16(bytes.data() + 4) != 1U ||
      read_le32(bytes.data() + 6) != shard_id_ ||
      read_le64(bytes.data() + 10) != first_engine_seq ||
      read_le32(bytes.data() + 18) != crc32c(bytes.first(18))) {
    return wal_error(ErrorCode::corrupt_wal, "invalid WAL segment header");
  }
  return std::monostate{};
}

Status Wal::create_segment(const EngineSeq first_engine_seq) {
  if (active_descriptor_ >= 0) {
    FileOps::close(active_descriptor_);
    active_descriptor_ = -1;
  }
  active_segment_ = segment_path(first_engine_seq);
  const auto header = segment_header(first_engine_seq);
  auto descriptor = FileOps::open_append(active_segment_);
  if (std::holds_alternative<Error>(descriptor)) {
    return std::get<Error>(descriptor);
  }
  const auto descriptor_value = std::get<int>(descriptor);
  const auto status = FileOps::write_all(descriptor_value, header);
  if (std::holds_alternative<Error>(status)) {
    FileOps::close(descriptor_value);
    return std::get<Error>(status);
  }
  const auto sync_status = FileOps::sync_file(descriptor_value);
  if (std::holds_alternative<Error>(sync_status)) {
    FileOps::close(descriptor_value);
    return std::get<Error>(sync_status);
  }
  const auto directory_status = FileOps::sync_directory(directory_);
  if (std::holds_alternative<Error>(directory_status)) {
    FileOps::close(descriptor_value);
    return std::get<Error>(directory_status);
  }
  active_descriptor_ = descriptor_value;
  active_bytes_ = header.size();
  size_bytes_ += header.size();
  last_appended_position_ = WalPosition{0, active_segment_, active_bytes_};
  active_dirty_ = false;
  return std::monostate{};
}

Status Wal::sync_active_unlocked() {
  const auto was_dirty = active_dirty_;
  if (active_descriptor_ < 0) {
    return wal_error(ErrorCode::wal_failure, "active WAL descriptor is not open");
  }
  const auto status = FileOps::sync_file(active_descriptor_);
  if (std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  if (was_dirty) {
    durable_position_ = last_appended_position_;
  }
  active_dirty_ = false;
  return std::monostate{};
}

Status Wal::rebuild_record_index_unlocked() {
  std::uint64_t cumulative_frame_bytes = 0;
  std::uint64_t continuity_id = 0;
  std::optional<EngineSeq> previous_sequence;
  for (const auto& record : records_) {
    if (record.frame_bytes > std::numeric_limits<std::uint64_t>::max() -
                                 cumulative_frame_bytes) {
      return wal_error(ErrorCode::wal_failure, "WAL frame byte index overflow");
    }
    if (previous_sequence.has_value() &&
        (*previous_sequence == std::numeric_limits<EngineSeq>::max() ||
         record.command.engine_seq != *previous_sequence + 1U)) {
      if (continuity_id == std::numeric_limits<std::uint64_t>::max()) {
        return wal_error(ErrorCode::wal_failure, "WAL continuity index overflow");
      }
      ++continuity_id;
    }
    cumulative_frame_bytes += record.frame_bytes;
    previous_sequence = record.command.engine_seq;
  }

  cumulative_frame_bytes = 0;
  continuity_id = 0;
  previous_sequence.reset();
  for (auto& record : records_) {
    if (previous_sequence.has_value() &&
        (*previous_sequence == std::numeric_limits<EngineSeq>::max() ||
         record.command.engine_seq != *previous_sequence + 1U)) {
      ++continuity_id;
    }
    cumulative_frame_bytes += record.frame_bytes;
    record.cumulative_frame_bytes = cumulative_frame_bytes;
    record.continuity_id = continuity_id;
    previous_sequence = record.command.engine_seq;
  }
  return std::monostate{};
}

Result<std::vector<Wal::PreparedRecord>> Wal::prepare_records_unlocked(
    const std::span<const domain::CommittedCommand> commands,
    WalAppendProfile* profile) {
  if (prepare_workers_ != nullptr) {
    return prepare_workers_->prepare(commands,
                                     prepare_options_.min_parallel_commands,
                                     profile, &prepare_stats_);
  }
  std::vector<PreparedRecord> records;
  records.reserve(commands.size());
  const auto task_start = profile == nullptr ? ProfileClock::time_point{}
                                              : ProfileClock::now();
  for (const auto& command : commands) {
    auto frame = encode_frame(command, profile);
    if (std::holds_alternative<Error>(frame)) {
      return std::get<Error>(frame);
    }
    const auto& frame_bytes = std::get<std::vector<std::byte>>(frame);
    if (frame_bytes.size() > segment_size_ - kHeaderSize) {
      return wal_error(ErrorCode::wal_failure, "WAL record cannot fit in a segment");
    }
    records.push_back(PreparedRecord{command,
                                     std::get<std::vector<std::byte>>(std::move(frame))});
  }
  if (profile != nullptr) {
    add_profile_ns(profile->prepare_task_ns,
                   profile_elapsed_ns(task_start, ProfileClock::now()));
    profile->prepare_tasks = 1U;
  }
  add_profile_ns(prepare_stats_.tasks, 1U);
  return records;
}

Result<WalPosition> Wal::append(const domain::CommittedCommand& command) {
  std::lock_guard lock(mutex_);
  const std::span<const domain::CommittedCommand> commands(&command, 1);
  auto prepared = prepare_records_unlocked(commands, nullptr);
  if (std::holds_alternative<Error>(prepared)) {
    return std::get<Error>(prepared);
  }
  return append_prepared_unlocked(
      std::get<std::vector<PreparedRecord>>(std::move(prepared)), nullptr);
}

Result<WalPosition> Wal::append_batch(
    const std::span<const domain::CommittedCommand> commands) {
  std::lock_guard lock(mutex_);
  return append_batch_unlocked(commands, nullptr);
}

Result<WalPosition> Wal::append_batch_profiled(
    const std::span<const domain::CommittedCommand> commands,
    WalAppendProfile& profile) {
  profile = {};
  const auto lock_start = ProfileClock::now();
  std::unique_lock lock(mutex_);
  if (commands.empty()) {
    return append_batch_unlocked(commands, nullptr);
  }
  profile.lock_wait_ns = profile_elapsed_ns(lock_start, ProfileClock::now());
  return append_batch_unlocked(commands, &profile);
}

Result<WalPosition> Wal::append_batch_unlocked(
    const std::span<const domain::CommittedCommand> commands,
    WalAppendProfile* profile) {
  if (commands.empty()) {
    return wal_error(ErrorCode::wal_failure, "WAL append batch is empty");
  }
  const auto prepare_start = profile == nullptr ? ProfileClock::time_point{}
                                                : ProfileClock::now();
  auto prepared = prepare_records_unlocked(commands, profile);
  if (std::holds_alternative<Error>(prepared)) {
    return std::get<Error>(prepared);
  }
  if (profile != nullptr) {
    profile->prepare_ns = profile_elapsed_ns(prepare_start, ProfileClock::now());
  }
  return append_prepared_unlocked(
      std::get<std::vector<PreparedRecord>>(std::move(prepared)), profile);
}

Result<WalPosition> Wal::append_prepared_unlocked(
    std::vector<PreparedRecord> records, WalAppendProfile* profile) {
  if (records.empty()) {
    return wal_error(ErrorCode::wal_failure, "WAL append batch is empty");
  }
  if (records.size() > records_.max_size() - records_.size()) {
    return wal_error(ErrorCode::wal_failure, "WAL record count overflow");
  }

  const auto plan_start = profile == nullptr ? ProfileClock::time_point{}
                                             : ProfileClock::now();

  std::vector<CachedRecord> cached_records;
  cached_records.reserve(records.size());
  std::uint64_t cumulative_frame_bytes = records_.empty()
                                              ? 0U
                                              : records_.back().cumulative_frame_bytes;
  std::uint64_t continuity_id = records_.empty() ? 0U : records_.back().continuity_id;
  std::optional<EngineSeq> previous_sequence;
  if (!records_.empty()) {
    previous_sequence = records_.back().command.engine_seq;
  }
  std::uint64_t frame_bytes_total = 0;
  for (const auto& record : records) {
    const auto frame_bytes = static_cast<std::uint64_t>(record.frame.size());
    if (frame_bytes > std::numeric_limits<std::uint64_t>::max() - cumulative_frame_bytes ||
        frame_bytes > std::numeric_limits<std::uint64_t>::max() - frame_bytes_total) {
      return wal_error(ErrorCode::wal_failure, "WAL frame byte index overflow");
    }
    cumulative_frame_bytes += frame_bytes;
    frame_bytes_total += frame_bytes;
    if (previous_sequence.has_value() &&
        (*previous_sequence == std::numeric_limits<EngineSeq>::max() ||
         record.command.engine_seq != *previous_sequence + 1U)) {
      if (continuity_id == std::numeric_limits<std::uint64_t>::max()) {
        return wal_error(ErrorCode::wal_failure, "WAL continuity index overflow");
      }
      ++continuity_id;
    }
    cached_records.push_back(
        CachedRecord{record.command, frame_bytes, cumulative_frame_bytes, continuity_id});
    previous_sequence = record.command.engine_seq;
  }
  if (frame_bytes_total > std::numeric_limits<std::uint64_t>::max() - size_bytes_) {
    return wal_error(ErrorCode::wal_failure, "WAL size overflow");
  }
  const auto required_capacity = records_.size() + records.size();
  if (required_capacity > records_.capacity()) {
    const auto current_capacity = records_.capacity();
    const auto growth = std::max(records.size(), current_capacity / 2U);
    const auto grown_capacity =
        growth > records_.max_size() - current_capacity
            ? records_.max_size()
            : current_capacity + growth;
    records_.reserve(std::max(required_capacity, grown_capacity));
  }

  struct PreparedChunk {
    std::size_t first_record{};
    std::size_t record_count{};
    bool rotate_before{};
    std::vector<std::byte> bytes;
  };
  std::vector<PreparedChunk> chunks;
  chunks.reserve(records.size());
  auto simulated_active_bytes = active_bytes_;
  bool rotate_before = false;
  std::size_t first_record = 0;
  while (first_record < records.size()) {
    if (simulated_active_bytes > segment_size_) {
      simulated_active_bytes = kHeaderSize;
      rotate_before = true;
    }
    std::size_t end_record = first_record;
    std::uint64_t chunk_bytes = 0;
    while (end_record < records.size()) {
      const auto frame_size = static_cast<std::uint64_t>(records[end_record].frame.size());
      const auto available = static_cast<std::uint64_t>(segment_size_) - simulated_active_bytes;
      if (chunk_bytes > available || frame_size > available - chunk_bytes ||
          frame_size > std::numeric_limits<std::uint64_t>::max() - chunk_bytes) {
        break;
      }
      chunk_bytes += frame_size;
      ++end_record;
    }
    if (end_record == first_record) {
      simulated_active_bytes = kHeaderSize;
      rotate_before = true;
      continue;
    }
    const auto copy_start = profile != nullptr ? ProfileClock::now()
                                               : ProfileClock::time_point{};
    std::vector<std::byte> bytes;
    bytes.reserve(static_cast<std::size_t>(chunk_bytes));
    for (std::size_t index = first_record; index < end_record; ++index) {
      const auto& frame = records[index].frame;
      bytes.insert(bytes.end(), frame.begin(), frame.end());
    }
    if (profile != nullptr) {
      add_profile_ns(profile->chunk_copy_ns,
                     profile_elapsed_ns(copy_start, ProfileClock::now()));
    }
    chunks.push_back(PreparedChunk{first_record, end_record - first_record,
                                   rotate_before, std::move(bytes)});
    rotate_before = false;
    simulated_active_bytes += chunk_bytes;
    first_record = end_record;
  }

  if (profile != nullptr) {
    profile->plan_copy_ns = profile_elapsed_ns(plan_start, ProfileClock::now());
    profile->frame_bytes = frame_bytes_total;
  }

  WalPosition last_position;
  for (const auto& chunk : chunks) {
    const auto rotation_start = profile != nullptr && chunk.rotate_before
                                    ? ProfileClock::now()
                                    : ProfileClock::time_point{};
    if (chunk.rotate_before) {
      if (active_dirty_) {
        auto status = sync_active_unlocked();
        if (std::holds_alternative<Error>(status)) {
          return std::get<Error>(status);
        }
      }
      auto status = create_segment(records[chunk.first_record].command.engine_seq);
      if (std::holds_alternative<Error>(status)) {
        return std::get<Error>(status);
      }
      if (profile != nullptr) {
        add_profile_ns(profile->rotations, 1U);
      }
    }
    if (profile != nullptr && chunk.rotate_before) {
      add_profile_ns(profile->rotation_ns,
                     profile_elapsed_ns(rotation_start, ProfileClock::now()));
    }
    if (active_descriptor_ < 0) {
      return wal_error(ErrorCode::wal_failure, "active WAL descriptor is not open");
    }
    const auto write_start = profile == nullptr ? ProfileClock::time_point{}
                                                : ProfileClock::now();
    const auto status = FileOps::write_all(active_descriptor_, chunk.bytes);
    if (std::holds_alternative<Error>(status)) {
      return std::get<Error>(status);
    }
    if (profile != nullptr) {
      add_profile_ns(profile->write_ns,
                     profile_elapsed_ns(write_start, ProfileClock::now()));
      add_profile_ns(profile->data_write_calls, 1U);
    }
    const auto publish_start = profile == nullptr ? ProfileClock::time_point{}
                                                  : ProfileClock::now();
    for (std::size_t offset = 0; offset < chunk.record_count; ++offset) {
      const auto record_index = chunk.first_record + offset;
      records_.push_back(std::move(cached_records[record_index]));
    }
    const auto chunk_bytes = static_cast<std::uint64_t>(chunk.bytes.size());
    active_bytes_ += chunk_bytes;
    size_bytes_ += chunk_bytes;
    const auto last_record_index = chunk.first_record + chunk.record_count - 1U;
    last_position = WalPosition{records[last_record_index].command.engine_seq,
                                active_segment_, active_bytes_};
    last_appended_position_ = last_position;
    active_dirty_ = true;
    if (profile != nullptr) {
      add_profile_ns(profile->publish_ns,
                     profile_elapsed_ns(publish_start, ProfileClock::now()));
    }
  }
  return last_position;
}

Status Wal::sync() {
  std::lock_guard lock(mutex_);
  return sync_active_unlocked();
}

Result<std::vector<domain::CommittedCommand>> Wal::replay() {
  std::lock_guard lock(mutex_);
  if (records_loaded_) {
    std::vector<domain::CommittedCommand> commands;
    commands.reserve(records_.size());
    for (const auto& record : records_) {
      commands.push_back(record.command);
    }
    return commands;
  }
  std::vector<std::filesystem::path> segments;
  std::error_code filesystem_error;
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".wal") {
      segments.push_back(entry.path());
    }
  }
  std::sort(segments.begin(), segments.end(), segment_less);
  std::vector<domain::CommittedCommand> commands;
  std::vector<CachedRecord> cached_records;
  std::optional<EngineSeq> previous;
  for (std::size_t segment_index = 0; segment_index < segments.size(); ++segment_index) {
    const auto& segment = segments[segment_index];
    const bool is_last_segment = segment_index + 1U == segments.size();
    auto descriptor = FileOps::open_read(segment);
    if (std::holds_alternative<Error>(descriptor)) {
      return std::get<Error>(descriptor);
    }
    auto bytes = FileOps::read_all(std::get<int>(descriptor));
    FileOps::close(std::get<int>(descriptor));
    if (std::holds_alternative<Error>(bytes)) {
      return std::get<Error>(bytes);
    }
    const auto& content = std::get<std::vector<std::byte>>(bytes);
    if (content.size() < kHeaderSize) {
      return wal_error(ErrorCode::corrupt_wal, "short WAL segment");
    }
    auto first_seq = std::uint64_t{0};
    for (unsigned index = 0; index < 8U; ++index) {
      first_seq |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(content[10 + index]))
                   << (index * 8U);
    }
    auto header_status = validate_segment_header(std::span(content).first(kHeaderSize), first_seq);
    if (std::holds_alternative<Error>(header_status)) {
      return corrupt_at(segment, shard_id_, 0U, "invalid WAL segment header");
    }
    auto filename_sequence = segment_sequence_checked(segment);
    if (std::holds_alternative<Error>(filename_sequence)) {
      return corrupt_at(segment, shard_id_, 0U,
                        std::get<Error>(filename_sequence).message);
    }
    if (std::get<EngineSeq>(filename_sequence) != first_seq) {
      return corrupt_at(segment, shard_id_, 0U,
                        "WAL segment filename/header mismatch expected=" +
                            std::to_string(std::get<EngineSeq>(filename_sequence)) +
                            " actual=" + std::to_string(first_seq));
    }
    std::size_t offset = kHeaderSize;
    bool segment_has_record = false;
    while (offset < content.size()) {
      if (content.size() - offset < 4U) {
        if (!is_last_segment) {
          return corrupt_at(segment, shard_id_, offset,
                            "partial WAL record in middle segment");
        }
        auto truncate_status = FileOps::truncate_file(segment, offset);
        if (std::holds_alternative<Error>(truncate_status)) {
          return std::get<Error>(truncate_status);
        }
        break;
      }
      const auto length = read_le32(content.data() + offset);
      if (length < 6U || length > kMaxRecordSize) {
        return corrupt_at(segment, shard_id_, offset, "invalid WAL record length");
      }
      if (content.size() - offset - 4U < length) {
        if (!is_last_segment) {
          return corrupt_at(segment, shard_id_, offset,
                            "partial WAL record in middle segment");
        }
        auto truncate_status = FileOps::truncate_file(segment, offset);
        if (std::holds_alternative<Error>(truncate_status)) {
          return std::get<Error>(truncate_status);
        }
        break;
      }
      const auto frame = std::span(content).subspan(offset + 4U, length);
      const auto expected_crc = read_le32(frame.data() + length - 4U);
      if (expected_crc != crc32c(frame.first(length - 4U))) {
        return corrupt_at(segment, shard_id_, offset, "WAL record checksum mismatch");
      }
      if (read_le16(frame.data()) != kRecordVersion) {
        return corrupt_at(segment, shard_id_, offset, "unsupported WAL record version");
      }
      auto decoded = decode_committed_command(
          frame.subspan(2U, length - 6U));
      if (std::holds_alternative<Error>(decoded)) {
        return corrupt_at(segment, shard_id_, offset, "WAL command payload decode failed");
      }
      auto command = std::get<domain::CommittedCommand>(std::move(decoded));
      if (command.engine_seq == 0) {
        return corrupt_at(segment, shard_id_, offset, "WAL engine sequence is zero");
      }
      if (!segment_has_record && command.engine_seq != first_seq) {
        return corrupt_at(segment, shard_id_, offset,
                          "WAL segment first sequence mismatch expected=" +
                              std::to_string(first_seq) +
                              " actual=" + std::to_string(command.engine_seq));
      }
      if (previous.has_value() &&
          (*previous == std::numeric_limits<EngineSeq>::max() ||
           command.engine_seq != *previous + 1U)) {
        const auto expected = *previous == std::numeric_limits<EngineSeq>::max()
                                  ? std::numeric_limits<EngineSeq>::max()
                                  : *previous + 1U;
        return corrupt_at(segment, shard_id_, offset,
                          "WAL engine sequence gap expected=" +
                              std::to_string(expected) +
                              " actual=" + std::to_string(command.engine_seq));
      }
      segment_has_record = true;
      previous = command.engine_seq;
      last_appended_position_ = WalPosition{command.engine_seq, segment,
                                            offset + 4U + length};
      const auto frame_bytes = static_cast<std::uint64_t>(4U + length);
      std::uint64_t cumulative_frame_bytes = frame_bytes;
      std::uint64_t continuity_id = 0;
      if (!cached_records.empty()) {
        const auto& previous_record = cached_records.back();
        if (frame_bytes > std::numeric_limits<std::uint64_t>::max() -
                              previous_record.cumulative_frame_bytes) {
          return wal_error(ErrorCode::wal_failure, "WAL frame byte index overflow");
        }
        cumulative_frame_bytes =
            previous_record.cumulative_frame_bytes + frame_bytes;
        continuity_id = previous_record.continuity_id;
        if (previous_record.command.engine_seq == std::numeric_limits<EngineSeq>::max() ||
            command.engine_seq != previous_record.command.engine_seq + 1U) {
          if (continuity_id == std::numeric_limits<std::uint64_t>::max()) {
            return wal_error(ErrorCode::wal_failure, "WAL continuity index overflow");
          }
          ++continuity_id;
        }
      }
      commands.push_back(std::move(command));
      cached_records.push_back(
          CachedRecord{commands.back(), frame_bytes, cumulative_frame_bytes, continuity_id});
      offset += 4U + length;
    }
    if (offset < content.size()) {
      const auto removed = content.size() - offset;
      size_bytes_ = removed > size_bytes_ ? 0 : size_bytes_ - removed;
      if (segment == active_segment_) {
        active_bytes_ = offset;
      }
    }
    if (!is_last_segment && offset == kHeaderSize) {
      return corrupt_at(segment, shard_id_, kHeaderSize, "empty non-tail WAL segment");
    }
  }
  if (!commands.empty()) {
    durable_position_ = last_appended_position_;
  }
  records_ = std::move(cached_records);
  records_loaded_ = true;
  active_dirty_ = false;
  return commands;
}

Result<std::optional<domain::CommittedCommand>> Wal::next_after(
    const EngineSeq sequence, const EngineSeq upper_bound) {
  std::lock_guard lock(mutex_);
  if (!records_loaded_) {
    return Error{ErrorCode::wal_failure, "WAL reader is not initialized"};
  }
  const auto iterator = std::upper_bound(
      records_.begin(), records_.end(), sequence,
      [](const EngineSeq value, const CachedRecord& record) {
        return value < record.command.engine_seq;
      });
  if (iterator == records_.end() || iterator->command.engine_seq > upper_bound) {
    return std::optional<domain::CommittedCommand>{};
  }
  return std::optional<domain::CommittedCommand>(iterator->command);
}

Result<std::uint64_t> Wal::bytes_after(const EngineSeq sequence,
                                       const EngineSeq upper_bound) const {
  std::lock_guard lock(mutex_);
  if (!records_loaded_) {
    return Error{ErrorCode::wal_failure, "WAL reader is not initialized"};
  }
  if (upper_bound <= sequence) {
    return std::uint64_t{0};
  }
  if (records_.empty()) {
    return Error{ErrorCode::corrupt_wal, "WAL publisher cursor is not retained"};
  }
  const auto first = std::upper_bound(
      records_.begin(), records_.end(), sequence,
      [](const EngineSeq value, const CachedRecord& record) {
        return value < record.command.engine_seq;
      });
  if (first == records_.end() || sequence == std::numeric_limits<EngineSeq>::max() ||
      first->command.engine_seq != sequence + 1U) {
    return Error{ErrorCode::corrupt_wal, "WAL publisher cursor is not retained"};
  }
  const auto last = std::lower_bound(
      records_.begin(), records_.end(), upper_bound,
      [](const CachedRecord& record, const EngineSeq value) {
        return record.command.engine_seq < value;
      });
  if (last == records_.end() || last->command.engine_seq != upper_bound ||
      first->continuity_id != last->continuity_id ||
      first->cumulative_frame_bytes < first->frame_bytes ||
      last->cumulative_frame_bytes < first->cumulative_frame_bytes - first->frame_bytes) {
    return Error{ErrorCode::corrupt_wal, "WAL publisher range is not contiguous"};
  }
  const auto bytes_before_first = first->cumulative_frame_bytes - first->frame_bytes;
  return last->cumulative_frame_bytes - bytes_before_first;
}

Status Wal::retain_through(const EngineSeq watermark) {
  std::lock_guard lock(mutex_);
  if (watermark == 0) {
    return std::monostate{};
  }
  std::vector<std::filesystem::path> segments;
  for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".wal") {
      segments.push_back(entry.path());
    }
  }
  std::sort(segments.begin(), segments.end(), segment_less);
  std::error_code filesystem_error;
  for (std::size_t index = 0; index + 1U < segments.size(); ++index) {
    if (segments[index] == active_segment_) {
      break;
    }
    const auto next_sequence = segment_sequence(segments[index + 1U]);
    if (next_sequence == 0 || next_sequence - 1U > watermark) {
      break;
    }
    const auto bytes = std::filesystem::file_size(segments[index], filesystem_error);
    if (filesystem_error) {
      return wal_error(ErrorCode::wal_failure, "cannot stat retained WAL segment");
    }
    std::filesystem::remove(segments[index], filesystem_error);
    if (filesystem_error) {
      return wal_error(ErrorCode::wal_failure, "cannot remove retained WAL segment");
    }
    size_bytes_ = bytes > size_bytes_ ? 0 : size_bytes_ - bytes;
  }
  if (records_loaded_) {
    const auto first_retained = std::find_if(
        records_.begin(), records_.end(), [watermark](const auto& record) {
          return record.command.engine_seq > watermark;
        });
    records_.erase(records_.begin(), first_retained);
    if (const auto status = rebuild_record_index_unlocked();
        std::holds_alternative<Error>(status)) {
      return std::get<Error>(status);
    }
  }
  return FileOps::sync_directory(directory_);
}

std::uint64_t Wal::size_bytes() const noexcept {
  std::lock_guard lock(mutex_);
  return size_bytes_;
}

EngineSeq Wal::last_engine_seq() const noexcept {
  std::lock_guard lock(mutex_);
  return last_appended_position_.engine_seq;
}

WalPosition Wal::durable_position() const {
  std::lock_guard lock(mutex_);
  return durable_position_;
}

WalPrepareStats Wal::prepare_stats() const {
  std::lock_guard lock(mutex_);
  return prepare_stats_;
}

}  // namespace order_books::storage
