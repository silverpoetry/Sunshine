/**
 * @file src/microphone.cpp
 * @brief Secure microphone uplink receiver.
 */

#include "microphone.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <openssl/rand.h>
#include <opus/opus.h>

extern "C" {
#include <moonlight-common-c/src/Srtp.h>
}

#include "config.h"
#include "logging.h"
#include "platform/common.h"
#include "utility.h"

namespace microphone {
  using namespace std::literals;

  namespace {
    constexpr std::size_t rtp_header_size = 12;
    constexpr std::size_t rtcp_header_size = 8;
    constexpr std::size_t replay_window_size = 64;
    constexpr std::size_t jitter_queue_limit = 64;
    constexpr std::size_t pcm_queue_limit = 8;
    constexpr auto initial_playout_delay = 40ms;
    constexpr auto frame_duration = 20ms;
    constexpr auto idle_decode_timeout = 5s;
    constexpr auto receiver_report_period = 1s;

    std::uint16_t read_be16(const std::uint8_t *data) {
      return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
    }

    std::uint32_t read_be32(const std::uint8_t *data) {
      return (static_cast<std::uint32_t>(data[0]) << 24) |
             (static_cast<std::uint32_t>(data[1]) << 16) |
             (static_cast<std::uint32_t>(data[2]) << 8) |
             data[3];
    }

    void write_be16(std::uint8_t *data, std::uint16_t value) {
      data[0] = static_cast<std::uint8_t>(value >> 8);
      data[1] = static_cast<std::uint8_t>(value);
    }

    void write_be32(std::uint8_t *data, std::uint32_t value) {
      data[0] = static_cast<std::uint8_t>(value >> 24);
      data[1] = static_cast<std::uint8_t>(value >> 16);
      data[2] = static_cast<std::uint8_t>(value >> 8);
      data[3] = static_cast<std::uint8_t>(value);
    }

    bool derive_session_keys(const crypto::aes_t &input_key,
                             const crypto::aes_t &input_iv,
                             std::string_view ping_payload,
                             SRTP_SESSION_KEYS &keys) {
      if (input_key.size() != 16 || input_iv.size() != 16 || ping_payload.size() != 16) {
        return false;
      }

      return SrtpDeriveSessionKeys(
        input_key.data(), input_iv.data(),
        reinterpret_cast<const std::uint8_t *>(ping_payload.data()), &keys);
    }

    crypto::aes_t make_rtp_iv(const std::uint8_t salt[SRTP_AEAD_SALT_LENGTH],
                              std::uint32_t ssrc, std::uint32_t roc,
                              std::uint16_t sequence) {
      crypto::aes_t iv(SRTP_AEAD_SALT_LENGTH);
      SrtpCreateRtpAeadIv(salt, ssrc, roc, sequence, iv.data());
      return iv;
    }

    crypto::aes_t make_rtcp_iv(const std::uint8_t salt[SRTP_AEAD_SALT_LENGTH],
                               std::uint32_t ssrc, std::uint32_t index) {
      crypto::aes_t iv(SRTP_AEAD_SALT_LENGTH);
      SrtpCreateRtcpAeadIv(salt, ssrc, index, iv.data());
      return iv;
    }

    struct opus_decoder_deleter {
      void operator()(OpusDecoder *decoder) const {
        opus_decoder_destroy(decoder);
      }
    };
  }  // namespace

  class receiver_t::impl_t {
  public:
    impl_t(const crypto::aes_t &input_key, const crypto::aes_t &input_iv,
           std::string_view ping_payload):
        keys_valid {derive_session_keys(input_key, input_iv, ping_payload, keys)},
        rtp_cipher {crypto::aes_t {std::begin(keys.rtpKey), std::end(keys.rtpKey)}, false},
        rtcp_encrypt_cipher {crypto::aes_t {std::begin(keys.rtcpKey), std::end(keys.rtcpKey)}, false},
        rtcp_decrypt_cipher {crypto::aes_t {std::begin(keys.rtcpKey), std::end(keys.rtcpKey)}, false} {
      if (RAND_bytes(reinterpret_cast<unsigned char *>(&receiver_ssrc),
                     sizeof(receiver_ssrc)) != 1 ||
          receiver_ssrc == 0) {
        receiver_ssrc = 0x53534D49;  // "SSMI"
      }
    }

    ~impl_t() {
      stop();
    }

    bool start() {
      if (!keys_valid) {
        BOOST_LOG(error) << "Unable to derive microphone SRTP session keys"sv;
        return false;
      }

      std::lock_guard lock {state_mutex};
      if (running) {
        return true;
      }

      int opus_error = OPUS_OK;
      decoder.reset(opus_decoder_create(ML_MICROPHONE_SAMPLE_RATE, 1, &opus_error));
      if (!decoder || opus_error != OPUS_OK) {
        BOOST_LOG(error) << "Unable to initialize microphone Opus decoder: "sv
                         << opus_strerror(opus_error);
        return false;
      }

      stopping = false;
      running = true;
      decode_thread = std::thread {[this]() { decode_loop(); }};
      sink_thread = std::thread {[this]() { sink_loop(); }};
      BOOST_LOG(info) << "Microphone uplink receiver started (SRTP, 48 kHz mono, 20 ms)"sv;
      return true;
    }

    void stop() {
      {
        std::lock_guard lock {state_mutex};
        if (!running) {
          return;
        }
        stopping = true;
      }
      packet_cv.notify_all();
      pcm_cv.notify_all();

      if (decode_thread.joinable()) {
        decode_thread.join();
      }
      if (sink_thread.joinable()) {
        sink_thread.join();
      }

      std::lock_guard lock {state_mutex};
      packets.clear();
      pcm_frames.clear();
      decoder.reset();
      running = false;
      BOOST_LOG(info) << "Microphone uplink receiver stopped: received="sv
                      << total_received_packets << ", late="sv << late_packets
                      << ", jitter_queue_drops="sv << queue_drops
                      << ", pcm_queue_drops="sv << pcm_drops;
    }

    bool valid() const {
      return keys_valid;
    }

    receive_result_t receive(std::string_view datagram) {
      if (datagram.size() < rtcp_header_size || (static_cast<std::uint8_t>(datagram[0]) >> 6) != 2) {
        return {};
      }

      const auto packet_type = static_cast<std::uint8_t>(datagram[1]) & 0x7F;
      if (packet_type == ML_MICROPHONE_PAYLOAD_TYPE) {
        return receive_rtp(datagram);
      }

      const auto rtcp_type = static_cast<std::uint8_t>(datagram[1]);
      if (rtcp_type >= 192 && rtcp_type <= 223) {
        return receive_srtcp(datagram);
      }
      return {};
    }

  private:
    struct packet_t {
      std::uint64_t index;
      std::uint32_t timestamp;
      bool marker;
      std::vector<std::uint8_t> opus;
    };

    receive_result_t receive_rtp(std::string_view datagram) {
      if (datagram.size() < rtp_header_size + SRTP_AEAD_TAG_LENGTH ||
          (static_cast<std::uint8_t>(datagram[0]) & 0x3F) != 0) {
        return {};
      }

      const auto *data = reinterpret_cast<const std::uint8_t *>(datagram.data());
      const auto sequence = read_be16(data + 2);
      const auto timestamp = read_be32(data + 4);
      const auto packet_ssrc = read_be32(data + 8);
      const bool marker = (static_cast<std::uint8_t>(datagram[1]) & 0x80) != 0;
      bool different_source;
      {
        std::lock_guard lock {state_mutex};
        if (ended_ssrcs.contains(packet_ssrc)) {
          return {};
        }
        different_source = have_ssrc && packet_ssrc != ssrc;
      }
      const auto index = different_source ? sequence : estimate_packet_index(sequence);
      const auto roc = static_cast<std::uint32_t>(index >> 16);
      auto iv = make_rtp_iv(keys.rtpSalt, packet_ssrc, roc, sequence);

      const std::size_t ciphertext_size =
        datagram.size() - rtp_header_size - SRTP_AEAD_TAG_LENGTH;
      std::string_view ciphertext {datagram.data() + rtp_header_size, ciphertext_size};
      std::string_view tag {datagram.data() + rtp_header_size + ciphertext_size,
                            SRTP_AEAD_TAG_LENGTH};
      std::string_view aad {datagram.data(), rtp_header_size};
      std::vector<std::uint8_t> plaintext;

      if (rtp_cipher.decrypt(tag, ciphertext, plaintext, &iv, aad) != 0 ||
          plaintext.empty() || plaintext.size() > 1275) {
        return {};
      }

      {
        std::lock_guard lock {state_mutex};
        if (ended_ssrcs.contains(packet_ssrc)) {
          return {};
        }
        if (have_ssrc && packet_ssrc != ssrc) {
          if (!marker) {
            return {};
          }
          reset_source_state_locked(ssrc);
        }
        if (!accept_replay_index(index)) {
          return {};
        }
        if (!have_ssrc) {
          ssrc = packet_ssrc;
          have_ssrc = true;
        }

        received_packets++;
        total_received_packets++;
        update_jitter(timestamp);

        if (running && !stopping) {
          if (playout_started && index < expected_index) {
            late_packets++;
          } else {
            if (marker && (playout_started || !packets.empty())) {
              packets.clear();
              pcm_frames.clear();
              playout_started = false;
              stream_generation++;
            }
            packets.emplace(index, packet_t {index, timestamp, marker, std::move(plaintext)});
            while (packets.size() > jitter_queue_limit) {
              packets.erase(packets.begin());
              queue_drops++;
            }
          }
        }
      }
      packet_cv.notify_one();

      receive_result_t result;
      result.authenticated = true;
      const auto now = std::chrono::steady_clock::now();
      if (now - last_report_time >= receiver_report_period) {
        result.reply = make_receiver_report();
        last_report_time = now;
      }
      return result;
    }

    receive_result_t receive_srtcp(std::string_view datagram) {
      if (datagram.size() < rtcp_header_size + SRTP_AEAD_TAG_LENGTH + 4) {
        return {};
      }

      const auto *data = reinterpret_cast<const std::uint8_t *>(datagram.data());
      const auto wire_index = read_be32(data + datagram.size() - 4);
      if ((wire_index & 0x80000000U) == 0) {
        return {};
      }
      const auto index = wire_index & 0x7FFFFFFFU;
      const auto sender_ssrc = read_be32(data + 4);
      const auto ciphertext_size =
        datagram.size() - rtcp_header_size - SRTP_AEAD_TAG_LENGTH - 4;
      std::array<std::uint8_t, rtcp_header_size + 4> aad {};
      std::copy_n(data, rtcp_header_size, aad.begin());
      std::copy_n(data + datagram.size() - 4, 4, aad.begin() + rtcp_header_size);
      auto iv = make_rtcp_iv(keys.rtcpSalt, sender_ssrc, index);
      std::string_view ciphertext {datagram.data() + rtcp_header_size, ciphertext_size};
      std::string_view tag {datagram.data() + rtcp_header_size + ciphertext_size,
                            SRTP_AEAD_TAG_LENGTH};
      std::vector<std::uint8_t> plaintext;

      if (rtcp_decrypt_cipher.decrypt(
            tag, ciphertext, plaintext, &iv,
            {reinterpret_cast<const char *>(aad.data()), aad.size()}) != 0) {
        return {};
      }

      {
        std::lock_guard lock {state_mutex};
        if (ended_ssrcs.contains(sender_ssrc) ||
            (have_ssrc && sender_ssrc != ssrc) ||
            (have_srtcp_index && index <= highest_srtcp_index)) {
          return {};
        }
        ssrc = sender_ssrc;
        have_ssrc = true;
        highest_srtcp_index = index;
        have_srtcp_index = true;
      }

      receive_result_t result;
      result.authenticated = true;
      result.ended = static_cast<std::uint8_t>(datagram[1]) == 203;
      if (result.ended) {
        {
          std::lock_guard lock {state_mutex};
          reset_source_state_locked(sender_ssrc);
        }
        packet_cv.notify_all();
        pcm_cv.notify_all();
      }
      return result;
    }

    void reset_source_state_locked(std::uint32_t retired_ssrc) {
      if (retired_ssrc != 0) {
        ended_ssrcs.insert(retired_ssrc);
      }
      packets.clear();
      pcm_frames.clear();
      have_ssrc = false;
      ssrc = 0;
      have_highest_index = false;
      highest_index = 0;
      base_index = 0;
      replay_bitmap = 0;
      have_srtcp_index = false;
      highest_srtcp_index = 0;
      playout_started = false;
      expected_index = 0;
      expected_timestamp = 0;
      received_packets = 0;
      prior_expected = 0;
      prior_received = 0;
      have_transit = false;
      previous_transit = 0;
      jitter = 0;
      last_report_time = {};
      stream_generation++;
    }

    std::uint64_t estimate_packet_index(std::uint16_t sequence) {
      std::lock_guard lock {state_mutex};
      if (!have_highest_index) {
        return sequence;
      }

      auto roc = static_cast<std::uint32_t>(highest_index >> 16);
      const auto highest_sequence = static_cast<std::uint16_t>(highest_index);
      if (highest_sequence < 0x8000 && sequence > highest_sequence + 0x8000) {
        if (roc > 0) {
          --roc;
        }
      } else if (highest_sequence >= 0x8000 &&
                 static_cast<std::uint16_t>(highest_sequence - sequence) > 0x8000) {
        ++roc;
      }
      return (static_cast<std::uint64_t>(roc) << 16) | sequence;
    }

    bool accept_replay_index(std::uint64_t index) {
      if (!have_highest_index) {
        have_highest_index = true;
        highest_index = index;
        base_index = index;
        replay_bitmap = 1;
        return true;
      }

      if (index > highest_index) {
        const auto shift = index - highest_index;
        replay_bitmap = shift >= replay_window_size ? 1 : (replay_bitmap << shift) | 1;
        highest_index = index;
        return true;
      }

      const auto age = highest_index - index;
      if (age >= replay_window_size || (replay_bitmap & (std::uint64_t {1} << age)) != 0) {
        return false;
      }
      replay_bitmap |= std::uint64_t {1} << age;
      return true;
    }

    void update_jitter(std::uint32_t timestamp) {
      const auto now = std::chrono::steady_clock::now().time_since_epoch();
      const auto arrival = std::chrono::duration_cast<std::chrono::microseconds>(now).count() *
                           ML_MICROPHONE_SAMPLE_RATE / 1000000;
      const auto transit = arrival - timestamp;
      if (have_transit) {
        const auto delta = std::llabs(transit - previous_transit);
        jitter += (static_cast<double>(delta) - jitter) / 16.0;
      }
      previous_transit = transit;
      have_transit = true;
    }

    std::vector<std::uint8_t> make_receiver_report() {
      std::array<std::uint8_t, 24> report {};
      std::uint32_t report_ssrc;
      std::uint32_t source_ssrc;
      std::uint32_t extended_highest;
      std::uint32_t jitter_value;
      std::uint8_t fraction_lost;
      std::int32_t cumulative_lost;

      {
        std::lock_guard lock {state_mutex};
        if (!have_ssrc || !have_highest_index) {
          return {};
        }

        const auto expected = highest_index - base_index + 1;
        const auto lost = static_cast<std::int64_t>(expected) -
                          static_cast<std::int64_t>(received_packets);
        cumulative_lost = static_cast<std::int32_t>(
          std::clamp<std::int64_t>(lost, -0x800000, 0x7FFFFF));

        const auto expected_interval = expected - prior_expected;
        const auto received_interval = received_packets - prior_received;
        const auto lost_interval = static_cast<std::int64_t>(expected_interval) -
                                   static_cast<std::int64_t>(received_interval);
        fraction_lost = expected_interval != 0 && lost_interval > 0 ?
                          static_cast<std::uint8_t>(
                            std::min<std::uint64_t>(255, (lost_interval << 8) / expected_interval)) :
                          0;
        prior_expected = expected;
        prior_received = received_packets;

        report_ssrc = receiver_ssrc;
        source_ssrc = ssrc;
        extended_highest = static_cast<std::uint32_t>(highest_index);
        jitter_value = static_cast<std::uint32_t>(
          std::min<double>(jitter, std::numeric_limits<std::uint32_t>::max()));
      }

      write_be32(report.data(), source_ssrc);
      report[4] = fraction_lost;
      report[5] = static_cast<std::uint8_t>(cumulative_lost >> 16);
      report[6] = static_cast<std::uint8_t>(cumulative_lost >> 8);
      report[7] = static_cast<std::uint8_t>(cumulative_lost);
      write_be32(report.data() + 8, extended_highest);
      write_be32(report.data() + 12, jitter_value);

      const auto index = srtcp_send_index++ & 0x7FFFFFFFU;
      const auto wire_index = index | 0x80000000U;
      std::vector<std::uint8_t> packet(
        rtcp_header_size + report.size() + SRTP_AEAD_TAG_LENGTH + 4);
      packet[0] = 0x81;
      packet[1] = 201;
      write_be16(packet.data() + 2, 7);
      write_be32(packet.data() + 4, report_ssrc);
      write_be32(packet.data() + packet.size() - 4, wire_index);

      std::array<std::uint8_t, rtcp_header_size + 4> aad {};
      std::copy_n(packet.begin(), rtcp_header_size, aad.begin());
      std::copy_n(packet.end() - 4, 4, aad.begin() + rtcp_header_size);
      auto iv = make_rtcp_iv(keys.rtcpSalt, report_ssrc, index);
      auto *ciphertext = packet.data() + rtcp_header_size;
      auto *tag = ciphertext + report.size();

      if (rtcp_encrypt_cipher.encrypt(
            {reinterpret_cast<const char *>(report.data()), report.size()},
            tag, ciphertext, &iv,
            {reinterpret_cast<const char *>(aad.data()), aad.size()}) !=
          static_cast<int>(report.size())) {
        return {};
      }
      return packet;
    }

    void decode_loop() {
      platf::set_thread_name("microphone::decode");
      platf::adjust_thread_priority(platf::thread_priority_e::high);

      auto next_playout = std::chrono::steady_clock::time_point {};
      auto last_packet_time = std::chrono::steady_clock::now();

      while (true) {
        packet_t packet {};
        bool have_packet = false;
        bool use_fec = false;
        bool should_decode = false;
        std::uint64_t decode_generation = 0;

        {
          std::unique_lock lock {state_mutex};
          packet_cv.wait(lock, [&]() {
            return stopping || !packets.empty() || playout_started;
          });
          if (stopping) {
            break;
          }

          if (!playout_started && !packets.empty()) {
            auto first = packets.begin();
            expected_index = first->first;
            expected_timestamp = first->second.timestamp;
            playout_started = true;
            next_playout = std::chrono::steady_clock::now() + initial_playout_delay;
            last_packet_time = std::chrono::steady_clock::now();
          }

          const auto active_generation = stream_generation;
          if (next_playout != std::chrono::steady_clock::time_point {}) {
            packet_cv.wait_until(lock, next_playout, [&]() {
              return stopping || stream_generation != active_generation;
            });
          }
          if (stopping) {
            break;
          }
          if (stream_generation != active_generation) {
            continue;
          }

          const auto now = std::chrono::steady_clock::now();
          bool advance_index = true;
          if (auto current = packets.find(expected_index); current != packets.end()) {
            if (current->second.timestamp > expected_timestamp) {
              should_decode = true;
              advance_index = false;
            } else {
              packet = std::move(current->second);
              packets.erase(current);
              have_packet = true;
              should_decode = true;
              last_packet_time = now;
            }
          } else if (auto next = packets.find(expected_index + 1); next != packets.end()) {
            packet = next->second;
            have_packet = true;
            use_fec = true;
            should_decode = true;
          } else if (now - last_packet_time <= idle_decode_timeout) {
            should_decode = true;
          } else {
            playout_started = false;
            next_playout = {};
            packets.clear();
          }

          if (should_decode) {
            decode_generation = stream_generation;
            if (advance_index) {
              ++expected_index;
            }
            expected_timestamp += ML_MICROPHONE_FRAME_SAMPLES;
            next_playout += frame_duration;
            if (next_playout < now - frame_duration) {
              next_playout = now;
            }
          }
        }

        if (!should_decode) {
          continue;
        }

        std::array<std::int16_t, ML_MICROPHONE_FRAME_SAMPLES> pcm {};
        if (have_packet && packet.marker) {
          opus_decoder_ctl(decoder.get(), OPUS_RESET_STATE);
        }
        const auto *opus_data = have_packet ? packet.opus.data() : nullptr;
        const auto opus_size = have_packet ? static_cast<opus_int32>(packet.opus.size()) : 0;
        const auto samples = opus_decode(
          decoder.get(), opus_data, opus_size, pcm.data(), pcm.size(), use_fec ? 1 : 0);
        if (samples < 0) {
          BOOST_LOG(warning) << "Microphone Opus decode failed: "sv << opus_strerror(samples);
          continue;
        }
        if (samples < static_cast<int>(pcm.size())) {
          std::fill(pcm.begin() + samples, pcm.end(), 0);
        }

        {
          std::lock_guard lock {state_mutex};
          if (decode_generation != stream_generation || stopping) {
            continue;
          }
          if (pcm_frames.size() >= pcm_queue_limit) {
            pcm_frames.pop_front();
            pcm_drops++;
          }
          pcm_frames.emplace_back(std::move(pcm));
        }
        pcm_cv.notify_one();
      }
    }

    void sink_loop() {
      platf::set_thread_name("microphone::sink");
      auto sink = platf::microphone_sink(config::audio.microphone_sink);
      if (!sink) {
        BOOST_LOG(error) << "No usable virtual microphone render endpoint was found"sv;
      }

      while (true) {
        std::array<std::int16_t, ML_MICROPHONE_FRAME_SAMPLES> pcm {};
        {
          std::unique_lock lock {state_mutex};
          pcm_cv.wait(lock, [&]() { return stopping || !pcm_frames.empty(); });
          if (stopping && pcm_frames.empty()) {
            break;
          }
          pcm = std::move(pcm_frames.front());
          pcm_frames.pop_front();
        }

        if (sink && !sink->write(pcm.data(), pcm.size())) {
          BOOST_LOG(error) << "Virtual microphone write failed; closing the sink"sv;
          sink.reset();
        }
      }

      if (sink) {
        sink->flush();
      }
    }

    SRTP_SESSION_KEYS keys {};
    bool keys_valid {};
    crypto::cipher::gcm_t rtp_cipher;
    crypto::cipher::gcm_t rtcp_encrypt_cipher;
    crypto::cipher::gcm_t rtcp_decrypt_cipher;

    mutable std::mutex state_mutex;
    std::condition_variable packet_cv;
    std::condition_variable pcm_cv;
    bool running {};
    bool stopping {};
    std::thread decode_thread;
    std::thread sink_thread;
    std::unique_ptr<OpusDecoder, opus_decoder_deleter> decoder;
    std::map<std::uint64_t, packet_t> packets;
    std::deque<std::array<std::int16_t, ML_MICROPHONE_FRAME_SAMPLES>> pcm_frames;

    bool have_ssrc {};
    std::uint32_t ssrc {};
    std::uint32_t receiver_ssrc {};
    std::set<std::uint32_t> ended_ssrcs;
    std::uint64_t stream_generation {};
    bool have_highest_index {};
    std::uint64_t highest_index {};
    std::uint64_t base_index {};
    std::uint64_t replay_bitmap {};
    bool have_srtcp_index {};
    std::uint32_t highest_srtcp_index {};
    std::uint32_t srtcp_send_index {};

    bool playout_started {};
    std::uint64_t expected_index {};
    std::uint32_t expected_timestamp {};
    std::uint64_t received_packets {};
    std::uint64_t total_received_packets {};
    std::uint64_t late_packets {};
    std::uint64_t queue_drops {};
    std::uint64_t pcm_drops {};
    std::uint64_t prior_expected {};
    std::uint64_t prior_received {};
    bool have_transit {};
    std::int64_t previous_transit {};
    double jitter {};
    std::chrono::steady_clock::time_point last_report_time {};
  };

  receiver_t::receiver_t(const crypto::aes_t &input_key, const crypto::aes_t &input_iv,
                         std::string_view audio_ping_payload):
      impl {std::make_unique<impl_t>(input_key, input_iv, audio_ping_payload)} {
  }

  receiver_t::~receiver_t() = default;

  bool receiver_t::start() {
    return impl->start();
  }

  void receiver_t::stop() {
    impl->stop();
  }

  receive_result_t receiver_t::receive(std::string_view datagram) {
    return impl->receive(datagram);
  }

  bool receiver_t::valid() const {
    return impl->valid();
  }
}  // namespace microphone
