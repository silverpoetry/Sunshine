#include <fstream>
#include <gtest/gtest.h>
#include <openssl/evp.h>

extern "C" {
#include <moonlight-common-c/src/Clipboard.h>
}

#include "src/desktop_file_store.h"
#include "src/utility.h"
#include "src/uuid.h"

#ifdef _WIN32
  #include <windows.h>
  #ifdef uuid_t
    #undef uuid_t
  #endif
#endif

namespace {
  namespace fs = std::filesystem;

  desktop_file_store::digest_t sha256(const std::vector<std::uint8_t> &bytes) {
    desktop_file_store::digest_t digest {};
    unsigned int length = 0;
    EVP_Digest(
      bytes.data(),
      bytes.size(),
      digest.data(),
      &length,
      EVP_sha256(),
      nullptr
    );
    EXPECT_EQ(length, digest.size());
    return digest;
  }

  std::vector<std::uint8_t> make_manifest() {
    const std::string directory = "folder";
    const std::string file = "folder/hello.txt";
    std::vector<std::uint8_t> manifest(
      LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE +
      2 * LI_CLIPBOARD_FILE_MANIFEST_ENTRY_HEADER_SIZE +
      directory.size() +
      file.size()
    );
    LI_CLIPBOARD_FILE_MANIFEST_HEADER header {
      .entryCount = 2,
      .fileCount = 1,
      .totalFileBytes = 5,
    };
    EXPECT_TRUE(LiEncodeClipboardFileManifestHeader(
      manifest.data(),
      manifest.size(),
      &header
    ));
    std::size_t offset = LI_CLIPBOARD_FILE_MANIFEST_HEADER_SIZE;
    LI_CLIPBOARD_FILE_MANIFEST_ENTRY directory_entry {
      .type = LI_CLIPBOARD_FILE_TYPE_DIRECTORY,
      .pathLength = static_cast<std::uint32_t>(directory.size()),
      .size = 0,
      .modifiedTimeMs = 0,
      .path = reinterpret_cast<const std::uint8_t *>(directory.data()),
    };
    std::size_t encoded_length = 0;
    EXPECT_TRUE(LiEncodeClipboardFileManifestEntry(
      manifest.data() + offset,
      manifest.size() - offset,
      &directory_entry,
      &encoded_length
    ));
    offset += encoded_length;
    LI_CLIPBOARD_FILE_MANIFEST_ENTRY file_entry {
      .type = LI_CLIPBOARD_FILE_TYPE_REGULAR,
      .pathLength = static_cast<std::uint32_t>(file.size()),
      .size = 5,
      .modifiedTimeMs = 0,
      .path = reinterpret_cast<const std::uint8_t *>(file.data()),
    };
    EXPECT_TRUE(LiEncodeClipboardFileManifestEntry(
      manifest.data() + offset,
      manifest.size() - offset,
      &file_entry,
      &encoded_length
    ));
    EXPECT_TRUE(LiIsValidClipboardFileManifest(
      manifest.data(),
      manifest.size()
    ));
    return manifest;
  }

  class DesktopFileStoreTest: public testing::Test {
  protected:
    void SetUp() override {
      desktop = fs::temp_directory_path() /
                ("sunshine-desktop-transfer-test-" +
                 uuid_util::uuid_t::generate().string());
      fs::create_directories(desktop);
      desktop_file_store::set_desktop_for_tests(desktop);
      desktop_file_store::clear_for_tests();
    }

    void TearDown() override {
      desktop_file_store::clear_for_tests();
      desktop_file_store::set_desktop_for_tests({});
      std::error_code error;
      fs::remove_all(desktop, error);
    }

    fs::path desktop;
    const std::string token = std::string(64, 'a');
  };
}  // namespace

TEST_F(DesktopFileStoreTest, CommitsCompleteTransferWithCollisionSafeName) {
  fs::create_directories(desktop / "folder");
  const auto legacy_staging = desktop / ".moonlight-transfers";
  fs::create_directory(legacy_staging);
#ifdef _WIN32
  ASSERT_TRUE(SetFileAttributesW(
    legacy_staging.c_str(),
    FILE_ATTRIBUTE_HIDDEN
  ));
#endif
  auto manifest = make_manifest();
  auto transfer = desktop_file_store::begin(manifest, token, "request");
  ASSERT_TRUE(transfer.ok) << transfer.error;
  EXPECT_FALSE(fs::exists(legacy_staging));

  auto retry = desktop_file_store::begin(manifest, token, "request");
  ASSERT_TRUE(retry.ok) << retry.error;
  EXPECT_EQ(retry.id, transfer.id);

  const std::vector<std::uint8_t> hello {'h', 'e', 'l', 'l', 'o'};
  EXPECT_FALSE(desktop_file_store::write_chunk(
                 transfer.id,
                 std::string(64, 'b'),
                 1,
                 0,
                 hello,
                 sha256(hello)
  )
                 .ok);
  auto written = desktop_file_store::write_chunk(
    transfer.id,
    token,
    1,
    0,
    hello,
    sha256(hello)
  );
  ASSERT_TRUE(written.ok) << written.error;
  EXPECT_TRUE(desktop_file_store::write_chunk(
                transfer.id,
                token,
                1,
                0,
                hello,
                sha256(hello)
  )
                .ok);

  auto completed = desktop_file_store::complete(transfer.id, token);
  ASSERT_TRUE(completed.ok) << completed.error;
  EXPECT_TRUE(desktop_file_store::complete(transfer.id, token).ok);

  std::ifstream input(desktop / "folder (2)" / "hello.txt", std::ios::binary);
  ASSERT_TRUE(input);
  EXPECT_EQ(
    std::string(std::istreambuf_iterator<char>(input), {}),
    "hello"
  );
  EXPECT_FALSE(fs::exists(desktop / ".moonlight-transfers"));
}

TEST_F(DesktopFileStoreTest, RejectsIncompleteAndOutOfOrderData) {
  auto transfer = desktop_file_store::begin(
    make_manifest(),
    token,
    "request"
  );
  ASSERT_TRUE(transfer.ok) << transfer.error;

  const std::vector<std::uint8_t> bytes {'e', 'l'};
  EXPECT_FALSE(desktop_file_store::write_chunk(
                 transfer.id,
                 token,
                 1,
                 1,
                 bytes,
                 sha256(bytes)
  )
                 .ok);
  auto completed = desktop_file_store::complete(transfer.id, token);
  EXPECT_FALSE(completed.ok);
  EXPECT_EQ(completed.error, "upload_incomplete");
  EXPECT_FALSE(fs::exists(desktop / "folder"));
}

TEST_F(DesktopFileStoreTest, KeepsOriginalDesktopForEntireTransfer) {
  auto transfer = desktop_file_store::begin(
    make_manifest(),
    token,
    "original-desktop"
  );
  ASSERT_TRUE(transfer.ok) << transfer.error;

  const auto other_desktop = fs::temp_directory_path() /
                             ("sunshine-other-desktop-test-" +
                              uuid_util::uuid_t::generate().string());
  fs::create_directories(other_desktop);
  auto other_desktop_guard = util::fail_guard([&]() {
    desktop_file_store::set_desktop_for_tests(desktop);
    std::error_code error;
    fs::remove_all(other_desktop, error);
  });
  desktop_file_store::set_desktop_for_tests(other_desktop);

  const std::vector<std::uint8_t> hello {'h', 'e', 'l', 'l', 'o'};
  auto written = desktop_file_store::write_chunk(
    transfer.id,
    token,
    1,
    0,
    hello,
    sha256(hello)
  );
  ASSERT_TRUE(written.ok) << written.error;
  auto completed = desktop_file_store::complete(transfer.id, token);
  ASSERT_TRUE(completed.ok) << completed.error;

  EXPECT_TRUE(fs::exists(desktop / "folder" / "hello.txt"));
  EXPECT_FALSE(fs::exists(other_desktop / "folder"));
}
