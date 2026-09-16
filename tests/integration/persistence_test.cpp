#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "persistence/binary_codec.hpp"
#include "persistence/config_store.hpp"
#include "persistence/wal.hpp"

namespace order_books::storage {
namespace {

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(const std::string& name)
      : path_(std::filesystem::temp_directory_path() / name) {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

domain::CommittedCommand command(const EngineSeq sequence) {
  Command request;
  request.identity = CommandIdentity{17, 1, 1, sequence};
  request.instrument_id = 7;
  request.command_type = CommandType::new_order;
  request.order_id = OrderId{9, sequence};
  request.payload = NewOrderPayload{Side::buy, 100, 1};
  return domain::CommittedCommand{std::move(request), sequence,
                                  static_cast<Timestamp>(sequence), 1, 1};
}

std::vector<std::filesystem::path> wal_segments(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> result;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() == ".wal") {
      result.push_back(entry.path());
    }
  }
  return result;
}

TEST(PersistenceTest, WalRotationReplaysEveryDurableSegment) {
  TemporaryDirectory temporary("order_books_wal_rotation_test");
  const auto wal_directory = temporary.path() / "wal";
  const auto first = command(1);
  const auto segment_size = std::size_t{22} + encode_committed_command(first).size() + 11U;
  const auto first_frame_bytes = 4U + encode_committed_command(first).size() + 6U;
  const auto second = command(2);
  const auto second_frame_bytes = 4U + encode_committed_command(second).size() + 6U;
  auto opened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(opened));
  auto wal = std::get<std::unique_ptr<Wal>>(std::move(opened));
  ASSERT_TRUE(std::holds_alternative<WalPosition>(wal->append(first)));
  ASSERT_TRUE(std::holds_alternative<WalPosition>(wal->append(second)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->sync()));
  EXPECT_GE(wal_segments(wal_directory).size(), 2U);
  EXPECT_EQ(wal->durable_position().engine_seq, 2U);
  auto not_loaded = wal->bytes_after(0, 2);
  ASSERT_TRUE(std::holds_alternative<Error>(not_loaded));
  EXPECT_EQ(std::get<Error>(not_loaded).code, ErrorCode::wal_failure);

  auto replayed = wal->replay();
  ASSERT_TRUE(std::holds_alternative<std::vector<domain::CommittedCommand>>(replayed));
  ASSERT_EQ(std::get<std::vector<domain::CommittedCommand>>(replayed).size(), 2U);
  auto bytes = wal->bytes_after(0, 2);
  ASSERT_TRUE(std::holds_alternative<std::uint64_t>(bytes));
  EXPECT_EQ(std::get<std::uint64_t>(bytes), first_frame_bytes + second_frame_bytes);
  bytes = wal->bytes_after(1, 2);
  ASSERT_TRUE(std::holds_alternative<std::uint64_t>(bytes));
  EXPECT_EQ(std::get<std::uint64_t>(bytes), second_frame_bytes);
  bytes = wal->bytes_after(2, 2);
  ASSERT_TRUE(std::holds_alternative<std::uint64_t>(bytes));
  EXPECT_EQ(std::get<std::uint64_t>(bytes), 0U);
  bytes = wal->bytes_after(0, 3);
  ASSERT_TRUE(std::holds_alternative<Error>(bytes));
  EXPECT_EQ(std::get<Error>(bytes).code, ErrorCode::corrupt_wal);
  auto next = wal->next_after(1, 2);
  ASSERT_TRUE(std::holds_alternative<std::optional<domain::CommittedCommand>>(next));
  ASSERT_TRUE(std::get<std::optional<domain::CommittedCommand>>(next).has_value());
  EXPECT_EQ(std::get<std::optional<domain::CommittedCommand>>(next)->engine_seq, 2U);

  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->retain_through(1)));
  bytes = wal->bytes_after(1, 2);
  ASSERT_TRUE(std::holds_alternative<std::uint64_t>(bytes));
  EXPECT_EQ(std::get<std::uint64_t>(bytes), second_frame_bytes);
  bytes = wal->bytes_after(0, 2);
  ASSERT_TRUE(std::holds_alternative<Error>(bytes));
  EXPECT_EQ(std::get<Error>(bytes).code, ErrorCode::corrupt_wal);

  const auto fourth = command(4);
  ASSERT_TRUE(std::holds_alternative<WalPosition>(wal->append(fourth)));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->sync()));
  bytes = wal->bytes_after(1, 4);
  ASSERT_TRUE(std::holds_alternative<Error>(bytes));
  EXPECT_EQ(std::get<Error>(bytes).code, ErrorCode::corrupt_wal);
}

TEST(PersistenceTest, LastSegmentPartialTailIsTruncated) {
  TemporaryDirectory temporary("order_books_wal_tail_test");
  const auto wal_directory = temporary.path() / "wal";
  const auto record = command(1);
  const auto segment_size = std::size_t{22} + encode_committed_command(record).size() + 11U;
  auto opened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(opened));
  auto wal = std::get<std::unique_ptr<Wal>>(std::move(opened));
  auto position = wal->append(record);
  ASSERT_TRUE(std::holds_alternative<WalPosition>(position));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->sync()));
  const auto segment = std::get<WalPosition>(position).segment;
  const auto valid_size = std::get<WalPosition>(position).end_offset;
  wal.reset();

  {
    std::ofstream file(segment, std::ios::binary | std::ios::app);
    ASSERT_TRUE(file.good());
    file.write("\x01\x02", 2);
  }
  auto reopened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(reopened));
  auto replayed_wal = std::get<std::unique_ptr<Wal>>(std::move(reopened));
  auto replayed = replayed_wal->replay();
  ASSERT_TRUE(std::holds_alternative<std::vector<domain::CommittedCommand>>(replayed));
  EXPECT_EQ(std::filesystem::file_size(segment), valid_size);
}

TEST(PersistenceTest, MiddleSegmentPartialTailFailsWithoutTruncation) {
  TemporaryDirectory temporary("order_books_wal_middle_tail_test");
  const auto wal_directory = temporary.path() / "wal";
  const auto first = command(1);
  const auto segment_size = std::size_t{22} + encode_committed_command(first).size() + 11U;
  auto opened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(opened));
  auto wal = std::get<std::unique_ptr<Wal>>(std::move(opened));
  auto first_position = wal->append(first);
  ASSERT_TRUE(std::holds_alternative<WalPosition>(first_position));
  ASSERT_TRUE(std::holds_alternative<WalPosition>(wal->append(command(2))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->sync()));
  const auto first_segment = std::get<WalPosition>(first_position).segment;
  const auto original_size = std::filesystem::file_size(first_segment);
  wal.reset();

  {
    std::ofstream file(first_segment, std::ios::binary | std::ios::app);
    ASSERT_TRUE(file.good());
    file.write("\x01\x02", 2);
  }
  const auto corrupted_size = std::filesystem::file_size(first_segment);
  auto reopened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(reopened));
  auto replayed_wal = std::get<std::unique_ptr<Wal>>(std::move(reopened));
  auto replayed = replayed_wal->replay();
  ASSERT_TRUE(std::holds_alternative<Error>(replayed));
  EXPECT_EQ(std::get<Error>(replayed).code, ErrorCode::corrupt_wal);
  EXPECT_EQ(std::filesystem::file_size(first_segment), corrupted_size);
  EXPECT_EQ(corrupted_size, original_size + 2U);
}

TEST(PersistenceTest, CompleteChecksumCorruptionFailsStop) {
  TemporaryDirectory temporary("order_books_wal_checksum_test");
  const auto wal_directory = temporary.path() / "wal";
  const auto record = command(1);
  const auto segment_size = std::size_t{22} + encode_committed_command(record).size() + 11U;
  auto opened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(opened));
  auto wal = std::get<std::unique_ptr<Wal>>(std::move(opened));
  auto position = wal->append(record);
  ASSERT_TRUE(std::holds_alternative<WalPosition>(position));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->sync()));
  const auto segment = std::get<WalPosition>(position).segment;
  wal.reset();

  std::fstream file(segment, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.good());
  file.seekg(28);
  char byte = 0;
  file.read(&byte, 1);
  file.seekp(28);
  byte = static_cast<char>(byte ^ 1);
  file.write(&byte, 1);
  file.close();

  auto reopened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(reopened));
  auto replayed_wal = std::get<std::unique_ptr<Wal>>(std::move(reopened));
  auto replayed = replayed_wal->replay();
  ASSERT_TRUE(std::holds_alternative<Error>(replayed));
  EXPECT_EQ(std::get<Error>(replayed).code, ErrorCode::corrupt_wal);
}

TEST(PersistenceTest, InvalidRecordLengthFailsWithoutTruncating) {
  TemporaryDirectory temporary("order_books_wal_length_test");
  const auto wal_directory = temporary.path() / "wal";
  const auto record = command(1);
  const auto segment_size = std::size_t{22} + encode_committed_command(record).size() + 11U;
  auto opened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(opened));
  auto wal = std::get<std::unique_ptr<Wal>>(std::move(opened));
  auto position = wal->append(record);
  ASSERT_TRUE(std::holds_alternative<WalPosition>(position));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->sync()));
  const auto segment = std::get<WalPosition>(position).segment;
  const auto original_size = std::filesystem::file_size(segment);
  wal.reset();

  std::fstream file(segment, std::ios::binary | std::ios::in | std::ios::out);
  ASSERT_TRUE(file.good());
  file.seekp(22);
  const char invalid_length[4] = {0, 0, 0, 0};
  file.write(invalid_length, sizeof(invalid_length));
  file.close();

  auto reopened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(reopened));
  auto replayed_wal = std::get<std::unique_ptr<Wal>>(std::move(reopened));
  auto replayed = replayed_wal->replay();
  ASSERT_TRUE(std::holds_alternative<Error>(replayed));
  EXPECT_EQ(std::get<Error>(replayed).code, ErrorCode::corrupt_wal);
  EXPECT_EQ(std::filesystem::file_size(segment), original_size);
}

TEST(PersistenceTest, SegmentFilenameAndHeaderMustAgree) {
  TemporaryDirectory temporary("order_books_wal_filename_test");
  const auto wal_directory = temporary.path() / "wal";
  const auto record = command(1);
  const auto segment_size = std::size_t{22} + encode_committed_command(record).size() + 11U;
  auto opened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(opened));
  auto wal = std::get<std::unique_ptr<Wal>>(std::move(opened));
  auto position = wal->append(record);
  ASSERT_TRUE(std::holds_alternative<WalPosition>(position));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->sync()));
  const auto original_segment = std::get<WalPosition>(position).segment;
  wal.reset();

  const auto renamed_segment = wal_directory / "2.wal";
  std::error_code rename_error;
  std::filesystem::rename(original_segment, renamed_segment, rename_error);
  ASSERT_FALSE(rename_error);
  auto reopened = Wal::open(wal_directory, 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(reopened));
  auto replayed_wal = std::get<std::unique_ptr<Wal>>(std::move(reopened));
  auto replayed = replayed_wal->replay();
  ASSERT_TRUE(std::holds_alternative<Error>(replayed));
  EXPECT_EQ(std::get<Error>(replayed).code, ErrorCode::corrupt_wal);
}

TEST(PersistenceTest, EngineSequenceGapFailsStop) {
  TemporaryDirectory temporary("order_books_wal_sequence_test");
  const auto wal_directory = temporary.path() / "wal";
  const auto record = command(1);
  auto opened = Wal::open(wal_directory, 1, 1U * 1024U * 1024U);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(opened));
  auto wal = std::get<std::unique_ptr<Wal>>(std::move(opened));
  ASSERT_TRUE(std::holds_alternative<WalPosition>(wal->append(record)));
  ASSERT_TRUE(std::holds_alternative<WalPosition>(wal->append(command(3))));
  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->sync()));
  auto replayed = wal->replay();
  ASSERT_TRUE(std::holds_alternative<Error>(replayed));
  EXPECT_EQ(std::get<Error>(replayed).code, ErrorCode::corrupt_wal);
  EXPECT_NE(std::get<Error>(replayed).message.find("expected=2"), std::string::npos);
}

TEST(PersistenceTest, ConfigurationManifestRoundTripUsesVersionedFilename) {
  TemporaryDirectory temporary("order_books_config_store_test");
  auto opened = ConfigStore::open(temporary.path() / "config", 1);
  ASSERT_TRUE(std::holds_alternative<ConfigStore>(opened));
  auto store = std::get<ConfigStore>(std::move(opened));
  const std::unordered_map<InstrumentId, InstrumentConfig> manifest{
      {7, InstrumentConfig{7, 5, 10, 1}}};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      store.persist_instrument_manifest(3, manifest)));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      store.persist_instrument_manifest(3, manifest)));
  EXPECT_TRUE(std::holds_alternative<Error>(store.persist_instrument_manifest(
      3, {{7, InstrumentConfig{7, 6, 10, 1}}})));
  const ShardBehaviorConfig behavior{4, 100, 1000, 200};
  ASSERT_TRUE(std::holds_alternative<std::monostate>(
      store.persist_behavior_config(4, behavior)));
  EXPECT_TRUE(std::holds_alternative<Error>(
      store.persist_behavior_config(4, ShardBehaviorConfig{4, 200, 1000, 200})));

  auto loaded_instruments = store.load_instrument_manifests();
  using InstrumentManifests =
      std::unordered_map<ConfigurationVersion,
                         std::unordered_map<InstrumentId, InstrumentConfig>>;
  const bool instruments_loaded = std::holds_alternative<InstrumentManifests>(
      loaded_instruments);
  ASSERT_TRUE(instruments_loaded);
  EXPECT_EQ(std::get<InstrumentManifests>(loaded_instruments).at(3), manifest);
  auto loaded_behaviors = store.load_behavior_configs();
  using BehaviorConfigs = std::unordered_map<ConfigurationVersion, ShardBehaviorConfig>;
  const bool behaviors_loaded = std::holds_alternative<BehaviorConfigs>(loaded_behaviors);
  ASSERT_TRUE(behaviors_loaded);
  EXPECT_EQ(std::get<BehaviorConfigs>(loaded_behaviors).at(4), behavior);
  EXPECT_TRUE(std::filesystem::exists(temporary.path() / "config" / "instruments-3.bin"));
}

}  // namespace
}  // namespace order_books::storage
