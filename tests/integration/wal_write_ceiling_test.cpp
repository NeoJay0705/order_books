#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "domain/state_machine.hpp"
#include "persistence/wal.hpp"

namespace order_books::storage {
namespace {

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(const std::string& name)
      : path_(std::filesystem::temp_directory_path() / name) {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (error || (!std::filesystem::create_directories(path_, error) && error)) {
      throw std::runtime_error("cannot create WAL ceiling test directory");
    }
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

domain::CommittedCommand ceiling_fixture_command(const EngineSeq sequence) {
  Command command;
  command.identity = CommandIdentity{1, 1, 1, sequence};
  command.instrument_id = 1;
  command.command_type = CommandType::new_order;
  command.order_id = OrderId{1, sequence};
  command.payload = NewOrderPayload{Side::buy, 100, 1};
  return domain::CommittedCommand{std::move(command), sequence,
                                  static_cast<Timestamp>(sequence), 1, 1};
}

TEST(WalWriteCeilingTest, FixtureRoundTripsThroughDurableWal) {
  TemporaryDirectory temporary("order_books_wal_write_ceiling_fixture_test");
  constexpr std::size_t segment_size = 256U * 1024U * 1024U;
  constexpr EngineSeq record_count = 8;

  auto opened = Wal::open(temporary.path(), 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(opened));
  auto wal = std::get<std::unique_ptr<Wal>>(std::move(opened));
  for (EngineSeq sequence = 1; sequence <= record_count; ++sequence) {
    ASSERT_TRUE(std::holds_alternative<WalPosition>(
        wal->append(ceiling_fixture_command(sequence))));
  }
  ASSERT_TRUE(std::holds_alternative<std::monostate>(wal->sync()));
  wal.reset();

  auto reopened = Wal::open(temporary.path(), 1, segment_size);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<Wal>>(reopened));
  auto replayed_wal = std::get<std::unique_ptr<Wal>>(std::move(reopened));
  auto replayed = replayed_wal->replay();
  ASSERT_TRUE(std::holds_alternative<std::vector<domain::CommittedCommand>>(replayed));
  const auto& records = std::get<std::vector<domain::CommittedCommand>>(replayed);
  ASSERT_EQ(records.size(), record_count);
  for (EngineSeq sequence = 1; sequence <= record_count; ++sequence) {
    const auto& actual = records[static_cast<std::size_t>(sequence - 1U)];
    const auto expected = ceiling_fixture_command(sequence);
    EXPECT_EQ(actual.engine_seq, expected.engine_seq);
    EXPECT_EQ(actual.command, expected.command);
  }
}

}  // namespace
}  // namespace order_books::storage
