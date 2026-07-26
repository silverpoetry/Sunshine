#include <fstream>
#include <future>
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

TEST_F(ClipboardFileStoreTest, TransfersFileOnlyAfterAReadRequest) {
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

  auto remote = clipboard_file_store::register_remote_source(
    manifest.bytes,
    client_origin,
    "remote"
  );
  ASSERT_TRUE(remote.ok) << remote.error;
  auto resolved = clipboard_file_store::resolve_remote_source(
    remote.id,
    client_origin,
    remote.manifest_size,
    remote.manifest_sha256
  );
  ASSERT_TRUE(resolved.ok) << resolved.error;
  EXPECT_EQ(resolved.manifest, manifest.bytes);
  EXPECT_FALSE(clipboard_file_store::poll_remote_request(
                 remote.id,
                 client_origin,
                 0
  )
                 .found);

  std::size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
  std::uint32_t file_index = UINT32_MAX;
  std::uint64_t file_size = 0;
  for (std::uint32_t index = 0; index < header.entryCount; index++) {
    LI_CLIPBOARD_FILE_MANIFEST_ENTRY entry;
    ASSERT_TRUE(LiDecodeClipboardFileManifestEntry(
      manifest.bytes.data(),
      manifest.bytes.size(),
      &offset,
      &entry
    ));
    if (entry.type == LI_CLIPBOARD_FILE_TYPE_REGULAR) {
      file_index = index;
      file_size = entry.size;
      break;
    }
  }
  ASSERT_NE(file_index, UINT32_MAX);

  auto pending = std::async(std::launch::async, [&]() {
    return clipboard_file_store::request_remote_chunk(
      remote.id,
      client_origin,
      file_index,
      0,
      static_cast<std::size_t>(file_size)
    );
  });
  auto request = clipboard_file_store::poll_remote_request(
    remote.id,
    client_origin,
    1
  );
  ASSERT_TRUE(request.found) << request.error;
  EXPECT_EQ(request.file_index, file_index);
  EXPECT_EQ(request.offset, 0);
  EXPECT_EQ(request.length, file_size);

  auto source_chunk = clipboard_file_store::read_chunk(
    source.id,
    file_index,
    request.offset,
    request.length
  );
  ASSERT_TRUE(source_chunk.ok) << source_chunk.error;
  auto fulfilled = clipboard_file_store::fulfill_remote_request(
    remote.id,
    client_origin,
    request.request_id,
    source_chunk.bytes,
    source_chunk.sha256
  );
  ASSERT_TRUE(fulfilled.ok) << fulfilled.error;

  ASSERT_EQ(pending.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  auto received = pending.get();
  ASSERT_TRUE(received.ok) << received.error;
  EXPECT_EQ(std::string(received.bytes.begin(), received.bytes.end()), "hello");
}

TEST_F(ClipboardFileStoreTest, RejectsWrongOriginAndUnsolicitedResponses) {
  auto source = clipboard_file_store::register_sources(
    {source_root / "folder"},
    31,
    "source"
  );
  ASSERT_TRUE(source.ok);
  auto manifest = clipboard_file_store::get_manifest(source.id);
  ASSERT_TRUE(manifest.found);
  auto remote = clipboard_file_store::register_remote_source(
    manifest.bytes,
    32,
    "remote"
  );
  ASSERT_TRUE(remote.ok);

  EXPECT_FALSE(clipboard_file_store::resolve_remote_source(
                 remote.id,
                 99,
                 remote.manifest_size,
                 remote.manifest_sha256
  )
                 .ok);
  EXPECT_TRUE(clipboard_file_store::resolve_remote_source(
                remote.id,
                32,
                remote.manifest_size,
                remote.manifest_sha256
  )
                .ok);
  clipboard_file_store::digest_t digest {};
  EXPECT_FALSE(clipboard_file_store::fulfill_remote_request(
                 remote.id,
                 32,
                 "not-a-request",
                 {1},
                 digest
  )
                 .ok);

  LI_CLIPBOARD_FILE_MANIFEST_HEADER header;
  ASSERT_TRUE(LiDecodeClipboardFileManifestHeader(
    manifest.bytes.data(),
    manifest.bytes.size(),
    &header
  ));
  std::size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
  std::uint32_t file_index = UINT32_MAX;
  for (std::uint32_t index = 0; index < header.entryCount; ++index) {
    LI_CLIPBOARD_FILE_MANIFEST_ENTRY entry;
    ASSERT_TRUE(LiDecodeClipboardFileManifestEntry(
      manifest.bytes.data(),
      manifest.bytes.size(),
      &offset,
      &entry
    ));
    if (entry.type == LI_CLIPBOARD_FILE_TYPE_REGULAR) {
      file_index = index;
      break;
    }
  }
  ASSERT_NE(file_index, UINT32_MAX);

  auto pending = std::async(std::launch::async, [&]() {
    return clipboard_file_store::request_remote_chunk(
      remote.id,
      32,
      file_index,
      0,
      5
    );
  });
  auto request = clipboard_file_store::poll_remote_request(
    remote.id,
    32,
    1
  );
  ASSERT_TRUE(request.found);
  auto rejected = clipboard_file_store::fail_remote_request(
    remote.id,
    32,
    request.request_id,
    "source_unavailable"
  );
  ASSERT_TRUE(rejected.ok) << rejected.error;
  ASSERT_EQ(pending.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  auto failed_chunk = pending.get();
  EXPECT_FALSE(failed_chunk.ok);
  EXPECT_EQ(failed_chunk.error, "source_unavailable");

  clipboard_file_store::release_origin(32);
  EXPECT_FALSE(clipboard_file_store::resolve_remote_source(
                 remote.id,
                 32,
                 remote.manifest_size,
                 remote.manifest_sha256
  )
                 .ok);
}
