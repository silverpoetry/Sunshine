#include <fstream>
#include <gtest/gtest.h>

extern "C" {
#include <moonlight-common-c/src/Clipboard.h>
}

#include "src/clipboard_file_store.h"
#include "src/utility.h"
#include "src/uuid.h"

namespace {
  namespace fs = std::filesystem;

  class ClipboardFileStoreTest: public testing::Test {
  protected:
    void SetUp() override {
      clipboard_file_store::clear_for_tests();
      source_root = fs::temp_directory_path() /
                    ("sunshine-clipboard-test-" + uuid_util::uuid_t::generate().string());
      fs::create_directories(source_root / "folder" / "empty");
      std::ofstream(source_root / "folder" / "hello.txt", std::ios::binary) << "hello";
    }

    void TearDown() override {
      clipboard_file_store::clear_for_tests();
      std::error_code error;
      fs::remove_all(source_root, error);
    }

    fs::path source_root;
  };
}  // namespace

TEST_F(ClipboardFileStoreTest, RoundTripsDirectoryThroughChunkedTransfer) {
  const std::uint64_t host_origin = 11;
  const std::uint64_t client_origin = 22;
  auto source = clipboard_file_store::register_sources(
    {source_root / "folder"},
    host_origin,
    "source"
  );
  ASSERT_TRUE(source.ok) << source.error;

  auto manifest = clipboard_file_store::get_manifest(source.id);
  ASSERT_TRUE(manifest.found);
  ASSERT_TRUE(LiIsValidClipboardFileManifest(manifest.bytes.data(), manifest.bytes.size()));

  LI_CLIPBOARD_FILE_MANIFEST_HEADER header;
  ASSERT_TRUE(LiDecodeClipboardFileManifestHeader(
    manifest.bytes.data(),
    manifest.bytes.size(),
    &header
  ));
  EXPECT_EQ(header.entryCount, 3);
  EXPECT_EQ(header.fileCount, 1);
  EXPECT_EQ(header.totalFileBytes, 5);

  auto upload = clipboard_file_store::begin_upload(
    manifest.bytes,
    client_origin,
    "upload"
  );
  ASSERT_TRUE(upload.ok) << upload.error;

  std::size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
  for (std::uint32_t index = 0; index < header.entryCount; index++) {
    LI_CLIPBOARD_FILE_MANIFEST_ENTRY entry;
    ASSERT_TRUE(LiDecodeClipboardFileManifestEntry(
      manifest.bytes.data(),
      manifest.bytes.size(),
      &offset,
      &entry
    ));
    if (entry.type != LI_CLIPBOARD_FILE_TYPE_REGULAR || entry.size == 0) {
      continue;
    }

    auto chunk = clipboard_file_store::read_chunk(
      source.id,
      index,
      0,
      static_cast<std::size_t>(entry.size)
    );
    ASSERT_TRUE(chunk.ok) << chunk.error;
    auto written = clipboard_file_store::write_chunk(
      upload.id,
      client_origin,
      index,
      0,
      chunk.bytes,
      chunk.sha256
    );
    ASSERT_TRUE(written.ok) << written.error;
  }

  auto completed = clipboard_file_store::complete_upload(upload.id, client_origin);
  ASSERT_TRUE(completed.ok) << completed.error;
  auto resolved = clipboard_file_store::resolve_upload(
    upload.id,
    client_origin,
    upload.manifest_size,
    upload.manifest_sha256
  );
  ASSERT_TRUE(resolved.ok) << resolved.error;
  ASSERT_EQ(resolved.paths.size(), 1);
  EXPECT_TRUE(fs::is_directory(resolved.paths.front()));
  EXPECT_TRUE(fs::is_directory(resolved.paths.front() / "empty"));

  std::ifstream input(resolved.paths.front() / "hello.txt", std::ios::binary);
  std::string text;
  input >> text;
  EXPECT_EQ(text, "hello");
}

TEST_F(ClipboardFileStoreTest, RejectsWrongOriginAndIncompleteUpload) {
  auto source = clipboard_file_store::register_sources(
    {source_root / "folder"},
    31,
    "source"
  );
  ASSERT_TRUE(source.ok);
  auto manifest = clipboard_file_store::get_manifest(source.id);
  ASSERT_TRUE(manifest.found);
  auto upload = clipboard_file_store::begin_upload(manifest.bytes, 32, "upload");
  ASSERT_TRUE(upload.ok);

  EXPECT_FALSE(clipboard_file_store::complete_upload(upload.id, 99).ok);
  EXPECT_FALSE(clipboard_file_store::complete_upload(upload.id, 32).ok);
  EXPECT_FALSE(clipboard_file_store::resolve_upload(
                 upload.id,
                 32,
                 upload.manifest_size,
                 upload.manifest_sha256
  )
                 .ok);
}
