#include "runtime/event_publisher.hpp"

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <fcntl.h>
#include <limits>
#include <string>
#include <span>
#include <thread>
#include <unistd.h>

#include "persistence/binary_codec.hpp"
#include "persistence/crc32c.hpp"
#include "persistence/file_ops.hpp"
#include "domain/invariant_checker.hpp"
#include "support/thread_name.hpp"

namespace order_books::runtime {
namespace {

constexpr std::size_t kCursorSize = 4U + 2U + 4U + 8U + 4U;

Timestamp system_now_ns() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

std::filesystem::path cursor_path(const storage::SnapshotStore& snapshots) {
  return snapshots.directory() / "publisher.cursor";
}

std::vector<std::byte> cursor_bytes(const ShardId shard_id, const EngineSeq sequence) {
  storage::BinaryWriter body;
  body.u16(1);
  body.u32(shard_id);
  body.u64(sequence);
  auto bytes = body.data();
  storage::BinaryWriter result;
  result.data().push_back(static_cast<std::byte>('O'));
  result.data().push_back(static_cast<std::byte>('B'));
  result.data().push_back(static_cast<std::byte>('C'));
  result.data().push_back(static_cast<std::byte>('R'));
  result.data().insert(result.data().end(), bytes.begin(), bytes.end());
  result.u32(storage::crc32c(result.data()));
  return result.data();
}

}  // namespace

Result<std::unique_ptr<EventPublisher>> EventPublisher::open(
    domain::ShardState initial_state, storage::Wal& wal,
    storage::SnapshotStore replay_snapshots, EventSink& sink,
    MetricsSink& metrics, const std::size_t snapshot_interval_commands,
    const std::chrono::minutes snapshot_interval,
    const std::size_t publisher_cursor_persist_max_commands,
    const std::chrono::microseconds publisher_cursor_persist_max_delay) {
  const auto live_sequence = initial_state.last_committed_engine_seq;
  const auto make_genesis = [](domain::ShardState state) {
    state.last_committed_engine_seq = 0;
    state.logical_retention_time = 0;
    state.active_order_count = 0;
    state.books.clear();
    state.order_locations.clear();
    state.producer_states.clear();
    state.tombstones.clear();
    state.tombstone_order.clear();
    return state;
  };

  std::optional<domain::ShardState> genesis = make_genesis(std::move(initial_state));
  auto loaded = replay_snapshots.load_latest();
  if (std::holds_alternative<Error>(loaded)) {
    return std::get<Error>(loaded);
  }
  auto optional_state = std::get<std::optional<domain::ShardState>>(std::move(loaded));
  EngineSeq snapshot_sequence = 0;
  bool using_snapshot = optional_state.has_value();
  domain::ShardState base_state;
  if (optional_state.has_value()) {
    base_state = std::move(*optional_state);
    snapshot_sequence = base_state.last_committed_engine_seq;
    if (snapshot_sequence > live_sequence) {
      return Error{ErrorCode::corrupt_snapshot,
                   "publisher replay snapshot is ahead of live state"};
    }
  } else {
    using_snapshot = false;
    base_state = std::move(*genesis);
    genesis.reset();
  }
  EngineSeq replay_snapshot_base = using_snapshot ? snapshot_sequence : 0;
  if (const auto status = domain::validate_state(base_state);
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  auto publisher = std::unique_ptr<EventPublisher>(new EventPublisher(
      std::move(base_state), wal, std::move(replay_snapshots), sink, metrics,
      snapshot_interval_commands, snapshot_interval,
      publisher_cursor_persist_max_commands, publisher_cursor_persist_max_delay,
      snapshot_sequence));
  auto loaded_cursor = publisher->load_cursor();
  if (std::holds_alternative<Error>(loaded_cursor)) {
    publisher->published_cursor_ = snapshot_sequence;
  } else {
    const auto persisted_cursor = std::get<EngineSeq>(loaded_cursor);
    publisher->published_cursor_ = persisted_cursor;
    if (using_snapshot && persisted_cursor < snapshot_sequence) {
      // The latest replay snapshot is newer than the durable cursor.  It
      // cannot be used as a base because doing so would skip unconfirmed
      // commands; replay from the immutable configuration genesis instead.
      if (!genesis.has_value()) {
        return Error{ErrorCode::corrupt_snapshot, "publisher genesis state is unavailable"};
      }
      if (const auto status = publisher->state_machine_.restore(std::move(*genesis));
          std::holds_alternative<Error>(status)) {
        return std::get<Error>(status);
      }
      genesis.reset();
      using_snapshot = false;
      replay_snapshot_base = 0;
      publisher->last_snapshot_seq_ = 0;
      publisher->replay_snapshot_seq_.store(0, std::memory_order_release);
    }
  }
  if (publisher->published_cursor_ >
      publisher->state_machine_.state().last_committed_engine_seq) {
    auto records = wal.replay();
    if (std::holds_alternative<Error>(records)) {
      return std::get<Error>(records);
    }
    for (const auto& record : std::get<std::vector<domain::CommittedCommand>>(records)) {
      if (record.engine_seq <= publisher->state_machine_.state().last_committed_engine_seq) {
        continue;
      }
      if (record.engine_seq > publisher->published_cursor_) {
        break;
      }
      auto execution = publisher->state_machine_.apply(record);
      if (std::holds_alternative<Error>(execution)) {
        return std::get<Error>(execution);
      }
      metrics.observe("replayed_records", 1);
    }
    if (publisher->state_machine_.state().last_committed_engine_seq !=
        publisher->published_cursor_) {
      return Error{ErrorCode::corrupt_wal, "publisher cursor is ahead of WAL"};
    }
  }
  if (const auto status = domain::validate_state(publisher->state_machine_.state());
      std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  publisher->last_snapshot_seq_ = replay_snapshot_base;
  publisher->confirmed_cursor_.store(publisher->published_cursor_,
                                     std::memory_order_release);
  publisher->durable_cursor_.store(publisher->published_cursor_,
                                   std::memory_order_release);
  publisher->replay_snapshot_seq_.store(replay_snapshot_base, std::memory_order_release);
  publisher->publishable_seq_.store(publisher->published_cursor_,
                                    std::memory_order_release);
  return publisher;
}

EventPublisher::EventPublisher(domain::ShardState state, storage::Wal& wal,
                               storage::SnapshotStore replay_snapshots,
                               EventSink& sink, MetricsSink& metrics,
                               const std::size_t snapshot_interval_commands,
                               const std::chrono::minutes snapshot_interval,
                               const std::size_t publisher_cursor_persist_max_commands,
                               const std::chrono::microseconds publisher_cursor_persist_max_delay,
                               const EngineSeq cursor)
    : state_machine_(std::move(state)),
      wal_(wal),
      replay_snapshots_(std::move(replay_snapshots)),
      sink_(sink),
      metrics_(metrics),
      snapshot_interval_commands_(snapshot_interval_commands),
      snapshot_interval_(snapshot_interval),
      publisher_cursor_persist_max_commands_(publisher_cursor_persist_max_commands),
      publisher_cursor_persist_max_delay_(publisher_cursor_persist_max_delay),
      published_cursor_(cursor),
      confirmed_cursor_(cursor),
      durable_cursor_(cursor),
      replay_snapshot_seq_(cursor),
      last_snapshot_seq_(cursor),
      last_snapshot_time_(std::chrono::steady_clock::now()) {}

EventPublisher::~EventPublisher() { (void)stop(); }

void EventPublisher::start() {
  std::lock_guard lock(mutex_);
  if (!started_) {
    started_ = true;
    thread_ = std::jthread([this](std::stop_token token) { run(token); });
  }
}

void EventPublisher::notify_publishable(const storage::WalPosition position) {
  const auto engine_seq = position.engine_seq;
  auto current = publishable_seq_.load(std::memory_order_relaxed);
  while (current < engine_seq &&
         !publishable_seq_.compare_exchange_weak(current, engine_seq,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed)) {
  }
  condition_.notify_all();
}

std::uint64_t EventPublisher::lag_bytes() noexcept {
  const auto cursor = confirmed_cursor_.load(std::memory_order_acquire);
  const auto durable_head = wal_.durable_position().engine_seq;
  if (durable_head <= cursor) {
    return 0;
  }
  const auto bytes = wal_.bytes_after(cursor, durable_head);
  if (std::holds_alternative<std::uint64_t>(bytes)) {
    return std::get<std::uint64_t>(bytes);
  }
  failed_.store(true, std::memory_order_release);
  condition_.notify_all();
  return std::numeric_limits<std::uint64_t>::max();
}

std::optional<Timestamp> EventPublisher::oldest_unconfirmed_received_at() {
  const auto cursor = confirmed_cursor_.load(std::memory_order_acquire);
  const auto next = wal_.next_after(cursor, wal_.durable_position().engine_seq);
  if (std::holds_alternative<Error>(next)) {
    return std::nullopt;
  }
  const auto& record = std::get<std::optional<domain::CommittedCommand>>(next);
  return record.has_value() ? std::optional<Timestamp>(record->received_at) : std::nullopt;
}

Status EventPublisher::persist_cursor(const EngineSeq cursor) {
  const auto final = cursor_path(replay_snapshots_);
  const auto temporary = final.string() + ".tmp";
  const auto bytes = cursor_bytes(state_machine_.state().shard_id, cursor);
  const auto descriptor = ::open(temporary.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (descriptor < 0) {
    return Error{ErrorCode::wal_failure, "open publisher cursor failed"};
  }
  const auto write_status = storage::FileOps::write_all(descriptor, bytes);
  if (std::holds_alternative<Error>(write_status)) {
    storage::FileOps::close(descriptor);
    return std::get<Error>(write_status);
  }
  const auto sync_status = storage::FileOps::sync_file(descriptor);
  storage::FileOps::close(descriptor);
  if (std::holds_alternative<Error>(sync_status)) {
    return std::get<Error>(sync_status);
  }
  std::error_code filesystem_error;
  std::filesystem::rename(temporary, final, filesystem_error);
  if (filesystem_error) {
    return Error{ErrorCode::wal_failure, "rename publisher cursor failed"};
  }
  return storage::FileOps::sync_directory(replay_snapshots_.directory());
}

Result<EngineSeq> EventPublisher::load_cursor() const {
  const auto path = cursor_path(replay_snapshots_);
  if (!std::filesystem::exists(path)) {
    return Error{ErrorCode::engine_unavailable, "publisher cursor is absent"};
  }
  auto descriptor = storage::FileOps::open_read(path);
  if (std::holds_alternative<Error>(descriptor)) {
    return std::get<Error>(descriptor);
  }
  auto content = storage::FileOps::read_all(std::get<int>(descriptor));
  storage::FileOps::close(std::get<int>(descriptor));
  if (std::holds_alternative<Error>(content)) {
    return std::get<Error>(content);
  }
  const auto& bytes = std::get<std::vector<std::byte>>(content);
  if (bytes.size() != kCursorSize || bytes[0] != static_cast<std::byte>('O') ||
      bytes[1] != static_cast<std::byte>('B') || bytes[2] != static_cast<std::byte>('C') ||
      bytes[3] != static_cast<std::byte>('R')) {
    return Error{ErrorCode::corrupt_snapshot, "invalid publisher cursor"};
  }
  const auto expected_crc = std::to_integer<std::uint32_t>(bytes[kCursorSize - 4U]) |
                            (std::to_integer<std::uint32_t>(bytes[kCursorSize - 3U]) << 8U) |
                            (std::to_integer<std::uint32_t>(bytes[kCursorSize - 2U]) << 16U) |
                            (std::to_integer<std::uint32_t>(bytes[kCursorSize - 1U]) << 24U);
  if (storage::crc32c(std::span(bytes).first(bytes.size() - 4U)) != expected_crc) {
    return Error{ErrorCode::corrupt_snapshot, "invalid publisher cursor checksum"};
  }
  const std::span<const std::byte> byte_span(bytes);
  storage::BinaryReader reader(byte_span.subspan(4U, bytes.size() - 8U));
  std::uint16_t version = 0;
  std::uint32_t shard = 0;
  EngineSeq sequence = 0;
  if (!reader.u16(version) || !reader.u32(shard) || !reader.u64(sequence) ||
      !reader.complete() || version != 1U || shard != state_machine_.state().shard_id) {
    return Error{ErrorCode::corrupt_snapshot, "invalid publisher cursor fields"};
  }
  return sequence;
}

Status EventPublisher::flush_cursor_if_dirty() {
  const auto durable = durable_cursor_.load(std::memory_order_acquire);
  if (published_cursor_ == durable) {
    dirty_since_.reset();
    return std::monostate{};
  }
  const auto cursor = published_cursor_;
  if (const auto status = persist_cursor(cursor); std::holds_alternative<Error>(status)) {
    return std::get<Error>(status);
  }
  durable_cursor_.store(cursor, std::memory_order_release);
  dirty_since_.reset();
  successful_cursor_persists_.fetch_add(1, std::memory_order_relaxed);
  return std::monostate{};
}

void EventPublisher::run(const std::stop_token stop_token) {
  support::set_current_thread_name("ob-pub-" +
                                   std::to_string(state_machine_.state().shard_id));
  std::vector<Event> pending_events;
  EngineSeq pending_seq = 0;
  auto retry_delay = std::chrono::milliseconds(1);
  while (!stop_token.stop_requested()) {
    if (pending_seq == 0) {
      const auto now = std::chrono::steady_clock::now();
      const auto durable = durable_cursor_.load(std::memory_order_acquire);
      if (published_cursor_ > durable && dirty_since_.has_value() &&
          now - *dirty_since_ >= publisher_cursor_persist_max_delay_) {
        if (const auto status = flush_cursor_if_dirty(); std::holds_alternative<Error>(status)) {
          metrics_.observe("publisher_cursor_error", 1);
          failed_.store(true, std::memory_order_release);
          return;
        }
        continue;
      }

      const auto publishable = publishable_seq_.load(std::memory_order_acquire);
      if (published_cursor_ >= publishable) {
        std::unique_lock lock(mutex_);
        if (dirty_since_.has_value()) {
          const auto deadline = *dirty_since_ + publisher_cursor_persist_max_delay_;
          const auto awakened = condition_.wait_until(
              lock, stop_token, deadline, [this, &stop_token] {
                return stop_token.stop_requested() ||
                       publishable_seq_.load(std::memory_order_acquire) > published_cursor_;
              });
          if (stop_token.stop_requested()) {
            break;
          }
          if (!awakened) {
            lock.unlock();
            if (const auto status = flush_cursor_if_dirty();
                std::holds_alternative<Error>(status)) {
              metrics_.observe("publisher_cursor_error", 1);
              failed_.store(true, std::memory_order_release);
              return;
            }
          }
        } else {
          condition_.wait(lock, stop_token, [this, &stop_token] {
            return stop_token.stop_requested() ||
                   publishable_seq_.load(std::memory_order_acquire) > published_cursor_;
          });
        }
        continue;
      }
    }

    if (pending_seq == 0) {
      const auto publishable = publishable_seq_.load(std::memory_order_acquire);
      auto next = wal_.next_after(published_cursor_, publishable);
      if (std::holds_alternative<Error>(next)) {
        metrics_.observe("publisher_replay_error", 1);
        failed_.store(true, std::memory_order_release);
        return;
      }
      const auto& record = std::get<std::optional<domain::CommittedCommand>>(next);
      if (!record.has_value()) {
        continue;
      }
      auto execution = state_machine_.apply(*record);
      if (std::holds_alternative<Error>(execution)) {
        metrics_.observe("publisher_state_error", 1);
        failed_.store(true, std::memory_order_release);
        return;
      }
      metrics_.observe("replayed_records", 1);
      auto output = std::get<domain::ExecutionOutput>(std::move(execution));
      pending_seq = record->engine_seq;
      pending_events = std::move(output.events);
    }

    if (!pending_events.empty()) {
      const auto publish_start = std::chrono::steady_clock::now();
      Result<std::monostate> published;
      try {
        published = sink_.publish(state_machine_.state().shard_id, pending_seq,
                                  pending_events, stop_token);
      } catch (...) {
        published = Error{ErrorCode::engine_unavailable, "event sink threw"};
      }
      if (std::holds_alternative<Error>(published)) {
        metrics_.observe("event_publish_retry", 1);
        if (stop_token.stop_requested()) {
          break;
        }
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, stop_token, retry_delay, [] {
          return false;
        });
        if (stop_token.stop_requested()) {
          break;
        }
        retry_delay = std::min(retry_delay * 2, std::chrono::milliseconds(1000));
        continue;
      }
      const auto publish_latency = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - publish_start);
      metrics_.observe("publish_latency_us",
                       static_cast<std::uint64_t>(publish_latency.count()));
    }
    retry_delay = std::chrono::milliseconds(1);
    published_cursor_ = pending_seq;
    confirmed_cursor_.store(published_cursor_, std::memory_order_release);
    if (!dirty_since_.has_value()) {
      dirty_since_ = std::chrono::steady_clock::now();
    }
    metrics_.observe("event_publish_lag_events",
                     wal_.last_engine_seq() > published_cursor_
                         ? wal_.last_engine_seq() - published_cursor_
                         : 0);
    metrics_.observe("event_publish_lag_bytes", lag_bytes());
    const auto oldest = oldest_unconfirmed_received_at();
    const auto now = system_now_ns();
    const auto lag_age = oldest.has_value() && *oldest >= 0 && now >= *oldest
                             ? static_cast<std::uint64_t>(now - *oldest)
                             : 0U;
    metrics_.observe("event_publish_lag_age_ns", lag_age);
    pending_seq = 0;
    pending_events.clear();
    const auto durable_after_ack = durable_cursor_.load(std::memory_order_acquire);
    const auto dirty_commands = published_cursor_ >= durable_after_ack
                                    ? published_cursor_ - durable_after_ack
                                    : 0;
    const auto count_trigger = dirty_commands >= publisher_cursor_persist_max_commands_;
    const auto cursor_time_trigger = dirty_since_.has_value() &&
                                     std::chrono::steady_clock::now() - *dirty_since_ >=
                                         publisher_cursor_persist_max_delay_;
    if (count_trigger || cursor_time_trigger) {
      if (const auto status = flush_cursor_if_dirty(); std::holds_alternative<Error>(status)) {
        metrics_.observe("publisher_cursor_error", 1);
        failed_.store(true, std::memory_order_release);
        return;
      }
    }
    const auto command_trigger = snapshot_interval_commands_ != 0 &&
                                 published_cursor_ - last_snapshot_seq_ >=
                                     snapshot_interval_commands_;
    const auto snapshot_time_trigger = snapshot_interval_.count() > 0 &&
                                       std::chrono::steady_clock::now() - last_snapshot_time_ >=
                                           snapshot_interval_;
    if (command_trigger || snapshot_time_trigger) {
      if (const auto status = flush_cursor_if_dirty(); std::holds_alternative<Error>(status)) {
        metrics_.observe("publisher_cursor_error", 1);
        failed_.store(true, std::memory_order_release);
        return;
      }
      if (const auto status = domain::validate_state(state_machine_.state());
          std::holds_alternative<Error>(status)) {
        metrics_.observe("publisher_state_error", 1);
        failed_.store(true, std::memory_order_release);
        return;
      }
      if (const auto status = replay_snapshots_.write(state_machine_.state());
          std::holds_alternative<Error>(status)) {
        metrics_.observe("publisher_snapshot_error", 1);
        failed_.store(true, std::memory_order_release);
        return;
      }
      last_snapshot_seq_ = published_cursor_;
      replay_snapshot_seq_.store(last_snapshot_seq_, std::memory_order_release);
      last_snapshot_time_ = std::chrono::steady_clock::now();
    }
  }

  if (failed_.load(std::memory_order_acquire)) {
    return;
  }
  if (const auto status = flush_cursor_if_dirty(); std::holds_alternative<Error>(status)) {
    metrics_.observe("publisher_cursor_error", 1);
    failed_.store(true, std::memory_order_release);
  }
}

Status EventPublisher::stop() {
  {
    std::lock_guard lock(mutex_);
    if (!started_) {
      if (failed_.load(std::memory_order_acquire)) {
        return Error{ErrorCode::engine_unavailable, "event publisher failed"};
      }
      return std::monostate{};
    }
    thread_.request_stop();
  }
  condition_.notify_all();
  thread_.join();
  started_ = false;
  if (failed_.load(std::memory_order_acquire)) {
    return Error{ErrorCode::engine_unavailable, "event publisher failed"};
  }
  return std::monostate{};
}

}  // namespace order_books::runtime
