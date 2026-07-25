/**
 * @file tests/unit/test_microphone.cpp
 * @brief Tests for the secure microphone uplink receiver.
 */
#include "../tests_common.h"

#include <src/microphone.h>

namespace {
  std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      bytes.push_back(static_cast<std::uint8_t>(
        std::stoul(std::string {hex.substr(i, 2)}, nullptr, 16)));
    }
    return bytes;
  }

  std::string_view as_string_view(const std::vector<std::uint8_t> &bytes) {
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
  }
}  // namespace

TEST(MicrophoneTest, AuthenticatesCommonCWireFormatAndRejectsReplay) {
  crypto::aes_t input_key(16);
  crypto::aes_t input_iv(16);
  std::array<char, 16> ping_payload {};
  for (std::size_t i = 0; i < 16; ++i) {
    input_key[i] = static_cast<std::uint8_t>(i);
    input_iv[i] = static_cast<std::uint8_t>(i + 0x10);
    ping_payload[i] = static_cast<char>(i + 0x20);
  }

  microphone::receiver_t receiver {
    input_key, input_iv, {ping_payload.data(), ping_payload.size()}
  };
  ASSERT_TRUE(receiver.valid());

  // Generated from the protocol's HKDF-SHA256, RFC 3711 AES-CM KDF, and
  // RFC 7714 AES-128-GCM wire layout. Payload bytes are F8 FF FE.
  auto rtp = from_hex(
    "80ef123401020304112233443e5d4d099d3187aca36aa29ee53e1b578c96f2");
  auto result = receiver.receive(as_string_view(rtp));
  ASSERT_TRUE(result.authenticated);
  ASSERT_FALSE(result.ended);
  ASSERT_EQ(result.reply.size(), 52);
  EXPECT_EQ(result.reply[0], 0x81);
  EXPECT_EQ(result.reply[1], 201);

  auto replay = receiver.receive(as_string_view(rtp));
  EXPECT_FALSE(replay.authenticated);

  rtp[12] ^= 0x01;
  auto tampered = receiver.receive(as_string_view(rtp));
  EXPECT_FALSE(tampered.authenticated);
}

TEST(MicrophoneTest, ResetsStreamStateAfterAuthenticatedSrtcpBye) {
  crypto::aes_t input_key(16);
  crypto::aes_t input_iv(16);
  std::array<char, 16> ping_payload {};
  for (std::size_t i = 0; i < 16; ++i) {
    input_key[i] = static_cast<std::uint8_t>(i);
    input_iv[i] = static_cast<std::uint8_t>(i + 0x10);
    ping_payload[i] = static_cast<char>(i + 0x20);
  }

  microphone::receiver_t receiver {
    input_key, input_iv, {ping_payload.data(), ping_payload.size()}
  };
  const auto rtp = from_hex(
    "80ef123401020304112233443e5d4d099d3187aca36aa29ee53e1b578c96f2");
  ASSERT_TRUE(receiver.receive(as_string_view(rtp)).authenticated);

  auto bye = from_hex(
    "81cb00011122334442c9bab2bb00b961298f4085aac0f6d780000000");
  auto result = receiver.receive(as_string_view(bye));
  EXPECT_TRUE(result.authenticated);
  EXPECT_TRUE(result.ended);

  EXPECT_FALSE(receiver.receive(as_string_view(rtp)).authenticated);
  EXPECT_FALSE(receiver.receive(as_string_view(bye)).authenticated);

  // A new microphone activation in the same streaming session has a fresh
  // SSRC and sequence space and must be accepted after the prior BYE.
  const auto restarted_rtp = from_hex(
    "80ef234505060708556677888c87cb24ed68bd715acfc52953281c407a3e5d");
  EXPECT_TRUE(receiver.receive(as_string_view(restarted_rtp)).authenticated);
}

TEST(MicrophoneTest, AuthenticatedMarkerStartsNewSourceWhenByeIsLost) {
  crypto::aes_t input_key(16);
  crypto::aes_t input_iv(16);
  std::array<char, 16> ping_payload {};
  for (std::size_t i = 0; i < 16; ++i) {
    input_key[i] = static_cast<std::uint8_t>(i);
    input_iv[i] = static_cast<std::uint8_t>(i + 0x10);
    ping_payload[i] = static_cast<char>(i + 0x20);
  }

  microphone::receiver_t receiver {
    input_key, input_iv, {ping_payload.data(), ping_payload.size()}
  };
  const auto old_rtp = from_hex(
    "80ef123401020304112233443e5d4d099d3187aca36aa29ee53e1b578c96f2");
  const auto restarted_rtp = from_hex(
    "80ef234505060708556677888c87cb24ed68bd715acfc52953281c407a3e5d");

  ASSERT_TRUE(receiver.receive(as_string_view(old_rtp)).authenticated);
  EXPECT_TRUE(receiver.receive(as_string_view(restarted_rtp)).authenticated);
  EXPECT_FALSE(receiver.receive(as_string_view(old_rtp)).authenticated);
}
