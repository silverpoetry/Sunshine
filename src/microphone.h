/**
 * @file src/microphone.h
 * @brief Secure microphone uplink receiver.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "crypto.h"

namespace microphone {
  struct receive_result_t {
    bool authenticated {};
    bool ended {};
    std::vector<std::uint8_t> reply;
  };

  /**
   * Receives the Moonlight microphone RTP stream carried on the existing audio
   * UDP socket. Network-facing work is limited to authentication, replay
   * protection, queueing, and receiver-report generation.
   */
  class receiver_t {
  public:
    receiver_t(const crypto::aes_t &input_key, const crypto::aes_t &input_iv,
               std::string_view audio_ping_payload);
    ~receiver_t();

    receiver_t(const receiver_t &) = delete;
    receiver_t &operator=(const receiver_t &) = delete;

    bool start();
    void stop();
    receive_result_t receive(std::string_view datagram);
    bool valid() const;

  private:
    class impl_t;
    std::unique_ptr<impl_t> impl;
  };
}  // namespace microphone
