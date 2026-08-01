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
                    ("sunshine-clipboard-test-" +
                     uuid_util::uuid_t::generate().string());
      fs::create_directories(source_root / "folder" / "empty");
      std::ofstream(
        source_root / "folder" / "hello.txt",
        std::ios::binary
      ) << "hello";
    }

    void TearDown() override {
      clipboard_file_store::clear_for_tests();
      std::error_code error;
      fs::remove_all(source_root, error);
    }

    fs::path source_root;
  };

  struct regular_file_t {
    std::uint32_t index {UINT32_MAX};
    std::uint64_t size {};
  };

  regular_file_t first_regular_file(
    const std::vector<std::uint8_t> &manifest
  ) {
    LI_CLIPBOARD_FILE_MANIFEST_HEADER header;
    if (!LiDecodeClipboardFileManifestHeader(
          manifest.data(),
          manifest.size(),
          &header
        )) {
      return {};
    }

    std::size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
    for (std::uint32_t index = 0; index < header.entryCount; ++index) {
      LI_CLIPBOARD_FILE_MANIFEST_ENTRY entry;
      if (!LiDecodeClipboardFileManifestEntry(
            manifest.data(),
            manifest.size(),
            &offset,
            &entry
          )) {
        return {};
      }
      if (entry.type == LI_CLIPBOARD_FILE_TYPE_REGULAR) {
        return {.index = index, .size = entry.size};
      }
    }
    return {};
  }
}  // namespace

TEST_F(ClipboardFileStoreTest, RegistersLocalOfferWithoutScanning) {
  const auto missing = source_root / "not-yet-present";
  auto offer = clipboard_file_store::register_local_offer(
    {missing},
    10,
    "lazy"
  );
  ASSERT_TRUE(offer.ok) << offer.error;

  auto manifest = clipboard_file_store::get_manifest(offer.id, 10);
  EXPECT_FALSE(manifest.ok);
  EXPECT_FALSE(manifest.error.empty());

  std::ofstream(missing, std::ios::binary) << "created after copy";
  manifest = clipboard_file_store::get_manifest(offer.id, 10);
  ASSERT_TRUE(manifest.ok) << manifest.error;
  EXPECT_TRUE(LiIsValidClipboardFileManifest(
    manifest.bytes.data(),
    manifest.bytes.size()
  ));
}

TEST_F(ClipboardFileStoreTest, TransfersManifestAndChunksOnDemand) {
  constexpr std::uint64_t host_origin = 11;
  constexpr std::uint64_t client_origin = 22;
  auto local = clipboard_file_store::register_local_offer(
    {source_root / "folder"},
    client_origin,
    "local"
  );
  ASSERT_TRUE(local.ok) << local.error;

  EXPECT_FALSE(clipboard_file_store::get_manifest(
    local.id,
    host_origin
  ).ok);
  auto local_manifest = clipboard_file_store::get_manifest(
    local.id,
    client_origin
  );
  ASSERT_TRUE(local_manifest.ok) << local_manifest.error;
  ASSERT_TRUE(LiIsValidClipboardFileManifest(
    local_manifest.bytes.data(),
    local_manifest.bytes.size()
  ));

  LI_CLIPBOARD_FILE_MANIFEST_HEADER header;
  ASSERT_TRUE(LiDecodeClipboardFileManifestHeader(
    local_manifest.bytes.data(),
    local_manifest.bytes.size(),
    &header
  ));
  EXPECT_EQ(header.entryCount, 3);
  EXPECT_EQ(header.fileCount, 1);
  EXPECT_EQ(header.totalFileBytes, 5);

  auto remote = clipboard_file_store::register_remote_offer(
    client_origin,
    "remote"
  );
  ASSERT_TRUE(remote.ok) << remote.error;
  ASSERT_TRUE(clipboard_file_store::resolve_remote_offer(
    remote.id,
    client_origin
  ).ok);

  auto pending_manifest = std::async(std::launch::async, [&]() {
    return clipboard_file_store::get_manifest(
      remote.id,
      client_origin
    );
  });
  auto manifest_request = clipboard_file_store::poll_remote_request(
    remote.id,
    client_origin,
    1
  );
  ASSERT_TRUE(manifest_request.found) << manifest_request.error;
  EXPECT_EQ(
    manifest_request.kind,
    clipboard_file_store::request_kind_e::manifest
  );
  auto manifest_fulfilled =
    clipboard_file_store::fulfill_remote_request(
      remote.id,
      client_origin,
      manifest_request.request_id,
      local_manifest.bytes,
      local_manifest.sha256
    );
  ASSERT_TRUE(manifest_fulfilled.ok) << manifest_fulfilled.error;
  ASSERT_EQ(
    pending_manifest.wait_for(std::chrono::seconds(1)),
    std::future_status::ready
  );
  auto remote_manifest = pending_manifest.get();
  ASSERT_TRUE(remote_manifest.ok) << remote_manifest.error;
  EXPECT_EQ(remote_manifest.bytes, local_manifest.bytes);

  const auto file = first_regular_file(remote_manifest.bytes);
  ASSERT_NE(file.index, UINT32_MAX);
  auto pending_chunk = std::async(std::launch::async, [&]() {
    return clipboard_file_store::read_chunk(
      remote.id,
      client_origin,
      file.index,
      0,
      static_cast<std::size_t>(file.size)
    );
  });
  auto chunk_request = clipboard_file_store::poll_remote_request(
    remote.id,
    client_origin,
    1
  );
  ASSERT_TRUE(chunk_request.found) << chunk_request.error;
  EXPECT_EQ(
    chunk_request.kind,
    clipboard_file_store::request_kind_e::chunk
  );
  EXPECT_EQ(chunk_request.file_index, file.index);
  EXPECT_EQ(chunk_request.offset, 0);
  EXPECT_EQ(chunk_request.length, file.size);

  auto source_chunk = clipboard_file_store::read_chunk(
    local.id,
    client_origin,
    file.index,
    chunk_request.offset,
    chunk_request.length
  );
  ASSERT_TRUE(source_chunk.ok) << source_chunk.error;
  auto chunk_fulfilled =
    clipboard_file_store::fulfill_remote_request(
      remote.id,
      client_origin,
      chunk_request.request_id,
      source_chunk.bytes,
      source_chunk.sha256
    );
  ASSERT_TRUE(chunk_fulfilled.ok) << chunk_fulfilled.error;

  ASSERT_EQ(
    pending_chunk.wait_for(std::chrono::seconds(1)),
    std::future_status::ready
  );
  auto received = pending_chunk.get();
  ASSERT_TRUE(received.ok) << received.error;
  EXPECT_EQ(
    std::string(received.bytes.begin(), received.bytes.end()),
    "hello"
  );
}

TEST_F(ClipboardFileStoreTest, ReleasesLocalOfferWithAuthorizedClient) {
  constexpr std::uint64_t host_origin = 51;
  constexpr std::uint64_t client_origin = 52;
  auto local = clipboard_file_store::register_local_offer(
    {source_root / "folder"},
    client_origin,
    "client-bound"
  );
  ASSERT_TRUE(local.ok) << local.error;

  clipboard_file_store::release_origin(host_origin);
  EXPECT_TRUE(clipboard_file_store::get_manifest(
    local.id,
    client_origin
  ).ok);

  clipboard_file_store::release_origin(client_origin);
  EXPECT_FALSE(clipboard_file_store::get_manifest(
    local.id,
    client_origin
  ).ok);
}

TEST_F(ClipboardFileStoreTest, BoundsSourcesPerAuthorizedOrigin) {
  constexpr std::uint64_t client_origin = 61;
  std::vector<std::string> ids;
  for (std::size_t index = 0;
       index < clipboard_file_store::max_sources_per_origin;
       ++index) {
    auto offer = clipboard_file_store::register_local_offer(
      {source_root / "folder"},
      client_origin,
      "bounded-" + std::to_string(index)
    );
    ASSERT_TRUE(offer.ok) << offer.error;
    ids.push_back(std::move(offer.id));
  }

  auto overflow = clipboard_file_store::register_local_offer(
    {source_root / "folder"},
    client_origin,
    "bounded-overflow"
  );
  EXPECT_FALSE(overflow.ok);
  EXPECT_EQ(overflow.error, "source_limit");

  EXPECT_FALSE(clipboard_file_store::release_local_source(
    ids.front(),
    client_origin + 1
  ).ok);
  ASSERT_TRUE(clipboard_file_store::release_local_source(
    ids.front(),
    client_origin
  ).ok);
  EXPECT_TRUE(clipboard_file_store::register_local_offer(
    {source_root / "folder"},
    client_origin,
    "bounded-after-release"
  ).ok);
}

TEST_F(ClipboardFileStoreTest, RejectsWrongOriginAndUnsolicitedResponses) {
  auto remote = clipboard_file_store::register_remote_offer(32, "remote");
  ASSERT_TRUE(remote.ok);

  EXPECT_FALSE(clipboard_file_store::resolve_remote_offer(
    remote.id,
    99
  ).ok);
  EXPECT_TRUE(clipboard_file_store::resolve_remote_offer(
    remote.id,
    32
  ).ok);
  EXPECT_FALSE(clipboard_file_store::get_manifest(remote.id, 99).ok);

  clipboard_file_store::digest_t digest {};
  EXPECT_FALSE(clipboard_file_store::fulfill_remote_request(
    remote.id,
    32,
    "not-a-request",
    {1},
    digest
  ).ok);

  auto pending = std::async(std::launch::async, [&]() {
    return clipboard_file_store::get_manifest(remote.id, 32);
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
  ASSERT_EQ(
    pending.wait_for(std::chrono::seconds(1)),
    std::future_status::ready
  );
  auto failed_manifest = pending.get();
  EXPECT_FALSE(failed_manifest.ok);
  EXPECT_EQ(failed_manifest.error, "source_unavailable");

  auto retry = std::async(std::launch::async, [&]() {
    return clipboard_file_store::get_manifest(remote.id, 32);
  });
  request = clipboard_file_store::poll_remote_request(
    remote.id,
    32,
    1
  );
  ASSERT_TRUE(request.found);
  EXPECT_EQ(
    request.kind,
    clipboard_file_store::request_kind_e::manifest
  );
  auto local = clipboard_file_store::register_local_offer(
    {source_root / "folder"},
    31,
    "retry-source"
  );
  ASSERT_TRUE(local.ok) << local.error;
  auto local_manifest = clipboard_file_store::get_manifest(local.id, 31);
  ASSERT_TRUE(local_manifest.ok) << local_manifest.error;
  auto fulfilled = clipboard_file_store::fulfill_remote_request(
    remote.id,
    32,
    request.request_id,
    local_manifest.bytes,
    local_manifest.sha256
  );
  ASSERT_TRUE(fulfilled.ok) << fulfilled.error;
  ASSERT_EQ(
    retry.wait_for(std::chrono::seconds(1)),
    std::future_status::ready
  );
  EXPECT_TRUE(retry.get().ok);

  clipboard_file_store::release_origin(32);
  EXPECT_FALSE(clipboard_file_store::resolve_remote_offer(
    remote.id,
    32
  ).ok);
}

TEST_F(ClipboardFileStoreTest, ReleasingRemoteOfferWakesLongPoll) {
  auto remote = clipboard_file_store::register_remote_offer(42, "remote");
  ASSERT_TRUE(remote.ok) << remote.error;

  auto poll = std::async(std::launch::async, [&]() {
    return clipboard_file_store::poll_remote_request(
      remote.id,
      42,
      clipboard_file_store::poll_timeout_seconds
    );
  });

  EXPECT_FALSE(clipboard_file_store::release_remote_source(
    remote.id,
    99
  ).ok);
  auto released = clipboard_file_store::release_remote_source(
    remote.id,
    42
  );
  ASSERT_TRUE(released.ok) << released.error;
  ASSERT_EQ(
    poll.wait_for(std::chrono::seconds(1)),
    std::future_status::ready
  );
  auto result = poll.get();
  EXPECT_FALSE(result.found);
  EXPECT_EQ(result.error, "not_found");

  // Release is idempotent so cancellation retries are harmless.
  EXPECT_TRUE(clipboard_file_store::release_remote_source(
    remote.id,
    42
  ).ok);
}
