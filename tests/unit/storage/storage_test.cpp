#include <storage/storage.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using confide::common::ErrCode;
using confide::common::RelativePath;
using confide::common::Sha256;
using confide::common::Sha256Digest;
using confide::common::TransactionId;
using confide::storage::Document;
using confide::storage::FileStore;

namespace fs = std::filesystem;

namespace {

RelativePath rel(std::string_view s) {
  return RelativePath::parse(s).value();
}

std::string read_all(confide::storage::Download& d) {
  std::string out;
  std::vector<std::byte> buf(7);  // small on purpose: exercises chunking
  while (true) {
    const auto n = d.read(buf).value();
    if (n == 0) {
      break;
    }
    out.append(reinterpret_cast<const char*>(buf.data()), n);  // NOLINT
  }
  return out;
}

std::size_t count_entries(const fs::path& dir) {
  return static_cast<std::size_t>(std::distance(fs::directory_iterator{dir}, {}));
}

class FileStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() / (std::string{"confide_storage_"} + info->name());
    fs::remove_all(dir_);
    store_.emplace(FileStore::open(dir_).value());
    ASSERT_TRUE(store_->create_transaction(id_));
  }

  void TearDown() override {
    store_.reset();
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void put(std::string_view path, std::string_view content) {
    auto up = store_->begin_upload(id_, rel(path)).value();
    ASSERT_TRUE(up.write(content));
    ASSERT_TRUE(up.commit());
  }

  fs::path dir_;
  std::optional<FileStore> store_;
  TransactionId id_ = TransactionId::generate();
};

TEST_F(FileStoreTest, OpenCreatesLayoutAndClearsTmp) {
  EXPECT_TRUE(fs::is_directory(dir_ / "transactions"));
  std::ofstream(dir_ / "tmp" / "leftover.part") << "x";
  auto reopened = FileStore::open(dir_);
  ASSERT_TRUE(reopened);
  EXPECT_EQ(count_entries(dir_ / "tmp"), 0U);
  EXPECT_TRUE(reopened->has_transaction(id_));
}

TEST_F(FileStoreTest, UploadInChunksThenDownload) {
  auto up = store_->begin_upload(id_, rel("report/img/b.png")).value();
  ASSERT_TRUE(up.write("hello "));
  ASSERT_TRUE(up.write("world"));
  EXPECT_EQ(up.size(), 11U);

  // Not visible until commit.
  EXPECT_EQ(store_->stat(id_, rel("report/img/b.png")).error().code, ErrCode::notfound);

  const auto stored = up.commit().value();
  EXPECT_EQ(stored.size, 11U);
  EXPECT_EQ(stored.sha256, Sha256::of("hello world").value());
  EXPECT_EQ(count_entries(dir_ / "tmp"), 0U);

  auto d = store_->open_read(id_, rel("report/img/b.png")).value();
  EXPECT_EQ(d.size(), 11U);
  EXPECT_EQ(read_all(d), "hello world");
}

TEST_F(FileStoreTest, CommitWithMatchingChecksum) {
  auto up = store_->begin_upload(id_, rel("a.txt")).value();
  ASSERT_TRUE(up.write("abc"));
  EXPECT_TRUE(up.commit(Sha256::of("abc").value()));
}

TEST_F(FileStoreTest, ChecksumMismatchLeavesNothing) {
  auto up = store_->begin_upload(id_, rel("a.txt")).value();
  ASSERT_TRUE(up.write("abc"));
  const auto r = up.commit(Sha256::of("abd").value());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, ErrCode::integrity);
  EXPECT_EQ(store_->stat(id_, rel("a.txt")).error().code, ErrCode::notfound);
  EXPECT_EQ(count_entries(dir_ / "tmp"), 0U);
  EXPECT_EQ(up.write("more").error().code, ErrCode::internal);
}

TEST_F(FileStoreTest, DroppedUploadRemovesTempFile) {
  {
    auto up = store_->begin_upload(id_, rel("a.txt")).value();
    ASSERT_TRUE(up.write("partial"));
    EXPECT_EQ(count_entries(dir_ / "tmp"), 1U);
  }
  EXPECT_EQ(count_entries(dir_ / "tmp"), 0U);
  EXPECT_EQ(store_->stat(id_, rel("a.txt")).error().code, ErrCode::notfound);
}

TEST_F(FileStoreTest, MovedUploadKeepsTempFile) {
  auto up = store_->begin_upload(id_, rel("a.txt")).value();
  ASSERT_TRUE(up.write("x"));
  auto moved = std::move(up);
  ASSERT_TRUE(moved.write("y"));
  EXPECT_EQ(moved.commit().value().size, 2U);
}

TEST_F(FileStoreTest, UploadReplacesExistingFile) {
  put("a.txt", "old");
  put("a.txt", "new!");
  EXPECT_EQ(store_->stat(id_, rel("a.txt")).value().size, 4U);
}

TEST_F(FileStoreTest, FileVersusDirectoryClashIsConflict) {
  put("a", "file");
  auto up = store_->begin_upload(id_, rel("a/b")).value();
  EXPECT_EQ(up.commit().error().code, ErrCode::conflict);

  put("d/e", "x");
  auto up2 = store_->begin_upload(id_, rel("d")).value();
  EXPECT_EQ(up2.commit().error().code, ErrCode::conflict);
}

TEST_F(FileStoreTest, UnknownTransaction) {
  const auto other = TransactionId::generate();
  EXPECT_FALSE(store_->has_transaction(other));
  EXPECT_EQ(store_->begin_upload(other, rel("a")).error().code, ErrCode::notfound);
  EXPECT_EQ(store_->open_read(other, rel("a")).error().code, ErrCode::notfound);
  EXPECT_EQ(store_->list_files(other).error().code, ErrCode::notfound);
  EXPECT_EQ(store_->remove_transaction(other).error().code, ErrCode::notfound);
  EXPECT_EQ(store_->read_document(other, Document::meta).error().code, ErrCode::notfound);
}

TEST_F(FileStoreTest, CreateTwiceIsConflict) {
  EXPECT_EQ(store_->create_transaction(id_).error().code, ErrCode::conflict);
}

TEST_F(FileStoreTest, ListFilesSorted) {
  put("z.txt", "1");
  put("report/b.csv", "22");
  put("report/a.csv", "333");
  const auto files = store_->list_files(id_).value();
  ASSERT_EQ(files.size(), 3U);
  EXPECT_EQ(files[0].path.str(), "report/a.csv");
  EXPECT_EQ(files[0].size, 3U);
  EXPECT_EQ(files[1].path.str(), "report/b.csv");
  EXPECT_EQ(files[2].path.str(), "z.txt");
}

TEST_F(FileStoreTest, HashFileMatchesUpload) {
  const std::string big(3 * confide::storage::kChunkBytes / 2, 'q');
  auto up = store_->begin_upload(id_, rel("big.bin")).value();
  ASSERT_TRUE(up.write(big));
  const auto stored = up.commit().value();
  const auto hashed = store_->hash_file(id_, rel("big.bin")).value();
  EXPECT_EQ(hashed.size, big.size());
  EXPECT_EQ(hashed.sha256, stored.sha256);
}

TEST_F(FileStoreTest, RemoveFilePrunesEmptyFolders) {
  put("a/b/c.txt", "x");
  put("a/keep.txt", "y");
  ASSERT_TRUE(store_->remove_file(id_, rel("a/b/c.txt")));
  const auto files_dir = dir_ / "transactions" / id_.str() / "files";
  EXPECT_FALSE(fs::exists(files_dir / "a" / "b"));
  EXPECT_TRUE(fs::exists(files_dir / "a" / "keep.txt"));

  ASSERT_TRUE(store_->remove_file(id_, rel("a/keep.txt")));
  EXPECT_FALSE(fs::exists(files_dir / "a"));
  EXPECT_TRUE(fs::is_directory(files_dir));

  EXPECT_EQ(store_->remove_file(id_, rel("a/keep.txt")).error().code, ErrCode::notfound);
}

TEST_F(FileStoreTest, Documents) {
  ASSERT_TRUE(store_->write_document(id_, Document::meta, R"({"state":"open"})"));
  ASSERT_TRUE(store_->write_document(id_, Document::meta, R"({"state":"committed"})"));
  EXPECT_EQ(store_->read_document(id_, Document::meta).value(), R"({"state":"committed"})");
  EXPECT_EQ(store_->read_document(id_, Document::manifest).error().code, ErrCode::notfound);
  EXPECT_TRUE(store_->list_files(id_).value().empty());  // documents aren't payload
}

TEST_F(FileStoreTest, ListAndRemoveTransactions) {
  const auto second = TransactionId::generate();
  ASSERT_TRUE(store_->create_transaction(second));
  fs::create_directory(dir_ / "transactions" / "not-an-id");

  auto ids = store_->list_transactions().value();
  ASSERT_EQ(ids.size(), 2U);

  put("a.txt", "x");
  ASSERT_TRUE(store_->remove_transaction(id_));
  EXPECT_FALSE(store_->has_transaction(id_));
  ids = store_->list_transactions().value();
  ASSERT_EQ(ids.size(), 1U);
  EXPECT_EQ(ids[0], second);
}

}  // namespace
