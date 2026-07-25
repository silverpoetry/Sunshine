#include <gtest/gtest.h>

#include "src/clipboard_blob_store.h"

class ClipboardBlobStoreTest: public testing::Test {
protected:
  void SetUp() override {
    clipboard_blob_store::clear_for_tests();
  }

  void TearDown() override {
    clipboard_blob_store::clear_for_tests();
  }
};

TEST_F(ClipboardBlobStoreTest, IdempotentRetryReturnsSameBlob) {
  std::vector<std::uint8_t> bytes {'h', 'e', 'l', 'l', 'o'};
  auto first = clipboard_blob_store::put(bytes, "text/plain", 7, "retry-key");
  auto second = clipboard_blob_store::put(bytes, "text/plain", 7, "retry-key");

  ASSERT_TRUE(first.ok);
  ASSERT_TRUE(second.ok);
  EXPECT_EQ(first.id, second.id);
  EXPECT_EQ(first.sha256, second.sha256);
  EXPECT_EQ(clipboard_blob_store::get(first.id).origin_id, 7);
}

TEST_F(ClipboardBlobStoreTest, ConflictingRetryIsRejected) {
  auto first = clipboard_blob_store::put({1, 2, 3}, "image/png", 9, "retry-key");
  auto second = clipboard_blob_store::put({1, 2, 4}, "image/png", 9, "retry-key");

  ASSERT_TRUE(first.ok);
  EXPECT_FALSE(second.ok);
  EXPECT_EQ(second.error, "idempotency_conflict");
}

TEST_F(ClipboardBlobStoreTest, RejectsInvalidBounds) {
  EXPECT_FALSE(clipboard_blob_store::put({}, "image/png", 1, "key").ok);
  EXPECT_FALSE(clipboard_blob_store::put({1}, "image/png", 0, "key").ok);
  EXPECT_FALSE(clipboard_blob_store::put({1}, "image/png", 1, "").ok);
}
