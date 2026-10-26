/**
 * @file src/stream_fec_status.h
 * @brief Selective client FEC block feedback, without loss-policy inference.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace stream::fec_status {
  // Client -> host only. Host -> client 0x5502 remains Set RGB LED.
  inline constexpr std::uint16_t packet_type = 0x5502;
  inline constexpr std::uint32_t client_feature_flag = 0x01;
  inline constexpr std::size_t payload_size = 21;

  enum class refusal_e {
    none,
    capability_unavailable,
    invalid_size,
    invalid_data_count,
    invalid_received_count,
    invalid_block_identity,
    invalid_parity_count,
    protected_block_too_large,
    invalid_missing_count,
    generation_unavailable,
    generation_mismatch,
    duplicate_block,
    older_frame,
    ambiguous_frame,
    block_count_changed,
    reversed_arrival_time
  };

  /**
   * Raw facts from a validated report about one block, not a stream census.
   * Received counts exclude synthesized shards. Neither derived count below
   * claims actual RS success, decoded-frame loss or counterfactual repairability.
   */
  struct block_t {
    std::uint32_t frame_index;
    std::uint16_t highest_received_sequence_number;
    std::uint16_t next_contiguous_sequence_number;
    std::uint16_t missing_packets_before_highest_received;
    std::uint16_t total_data_packets;
    std::uint16_t total_parity_packets;
    std::uint16_t received_data_packets;
    std::uint16_t received_parity_packets;
    std::uint8_t fec_percentage;
    std::uint8_t block_index;
    std::uint8_t block_count;

    std::uint32_t absent_data_shards() const {
      return std::uint32_t {total_data_packets} - received_data_packets;
    }

    std::uint32_t rank_deficit_shards() const {
      const auto received = std::uint32_t {received_data_packets} + received_parity_packets;
      return received < total_data_packets ? total_data_packets - received : 0;
    }
  };

  struct parse_result_t {
    refusal_e refusal;
    std::optional<block_t> block;
  };

  /**
   * Parse only the decrypted payload of inbound 0x5502, excluding its LE control
   * header. Gate per session, never when installing a shared callback map.
   *
   * Contract: moonlight-common-c c999436858471dfefa7617af3b7dc03ec1644ce4,
   * src/Video.h:53-70 and src/RtpVideoQueue.c:92-107, 696-708, 733-764.
   * The legacy 16-byte 0x5501 format is deliberately unsupported.
   */
  inline parse_result_t parse(std::string_view payload, std::uint32_t client_features) {
    if (!(client_features & client_feature_flag)) {
      return {refusal_e::capability_unavailable, std::nullopt};
    }
    if (payload.size() != payload_size) {
      return {refusal_e::invalid_size, std::nullopt};
    }

    const auto byte = [&](std::size_t offset) {
      return static_cast<std::uint32_t>(static_cast<unsigned char>(payload[offset]));
    };
    const auto be16 = [&](std::size_t offset) {
      return static_cast<std::uint16_t>((byte(offset) << 8) | byte(offset + 1));
    };
    const block_t block {
      .frame_index = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3),
      .highest_received_sequence_number = be16(4),
      .next_contiguous_sequence_number = be16(6),
      .missing_packets_before_highest_received = be16(8),
      .total_data_packets = be16(10),
      .total_parity_packets = be16(12),
      .received_data_packets = be16(14),
      .received_parity_packets = be16(16),
      .fec_percentage = static_cast<std::uint8_t>(byte(18)),
      .block_index = static_cast<std::uint8_t>(byte(19)),
      .block_count = static_cast<std::uint8_t>(byte(20))
    };

    if (block.total_data_packets == 0 || block.total_data_packets > 1023) {
      return {refusal_e::invalid_data_count, std::nullopt};
    }
    if (block.received_data_packets > block.total_data_packets ||
        block.received_parity_packets > block.total_parity_packets) {
      return {refusal_e::invalid_received_count, std::nullopt};
    }
    if (block.block_count == 0 || block.block_count > 4 || block.block_index >= block.block_count) {
      return {refusal_e::invalid_block_identity, std::nullopt};
    }

    // This is the CLIENT's reconstruction of P from the effective header F,
    // not the sender's pre-header minimum-parity rounding. RtpVideoQueue.c:704
    // computes this relation even when the sender has raised F (e.g. to 200).
    const auto expected_parity = (std::uint32_t {block.total_data_packets} * block.fec_percentage + 99) / 100;
    if (block.total_parity_packets != expected_parity) {
      return {refusal_e::invalid_parity_count, std::nullopt};
    }
    const auto total = std::uint32_t {block.total_data_packets} + block.total_parity_packets;
    if (block.total_parity_packets != 0 && total > 255) {
      return {refusal_e::protected_block_too_large, std::nullopt};
    }
    const auto received = std::uint32_t {block.received_data_packets} + block.received_parity_packets;
    if (block.missing_packets_before_highest_received > total - received) {
      return {refusal_e::invalid_missing_count, std::nullopt};
    }

    // Do not constrain the RTP cursors against one another: the contiguous
    // fast-path cursor can lag after reordering. Missing-before-highest also
    // excludes unseen trailing packets; it is not D - received data.
    return {refusal_e::none, block};
  }

  using clock = std::chrono::steady_clock;

  struct sample_t {
    block_t block;
    clock::time_point received_at;
  };

  enum class availability_e {
    fresh_report,
    generation_unavailable,
    generation_mismatch,
    capability_unavailable,
    no_report,
    invalid_max_age,
    reversed_clock,
    stale_report
  };

  struct snapshot_t {
    availability_e availability;
    // Present only for a fresh accepted report, never an invented clean window.
    std::optional<sample_t> sample;
  };

  /**
   * One instance belongs to one session and is externally serialized by its
   * owner. State is bounded to the newest frame and a four-bit block mask.
   * Unseen blocks of that frame can arrive in any order on unsequenced ENet;
   * older frames and the ambiguous half-range distance are refused.
   *
   * Reports are selective and unreliable. Freshness describes host arrival
   * only; neither silence nor expiration establishes zero loss or video state.
   */
  class tracker_t {
  public:
    // Call from trusted session allocation/reset, never to follow an incoming
    // packet's generation. Zero denotes an unbound/unavailable session.
    void reset(std::uint64_t generation, std::uint32_t client_features) {
      generation_ = generation;
      client_features_ = client_features;
      latest_.reset();
      seen_blocks_ = 0;
    }

    refusal_e observe(std::uint64_t generation, std::string_view payload, clock::time_point received_at) {
      if (generation_ == 0) {
        return refusal_e::generation_unavailable;
      }
      if (generation != generation_) {
        return refusal_e::generation_mismatch;
      }
      const auto parsed = parse(payload, client_features_);
      if (!parsed.block) {
        return parsed.refusal;
      }

      const auto &block = *parsed.block;
      const auto block_bit = static_cast<std::uint8_t>(1u << block.block_index);
      bool new_frame = !latest_;
      if (latest_) {
        if (received_at < latest_->received_at) {
          return refusal_e::reversed_arrival_time;
        }
        const auto distance = std::uint32_t {block.frame_index - latest_->block.frame_index};
        if (distance == 0) {
          if (block.block_count != latest_->block.block_count) {
            return refusal_e::block_count_changed;
          }
          if (seen_blocks_ & block_bit) {
            return refusal_e::duplicate_block;
          }
        } else {
          if (distance == 0x80000000u) {
            return refusal_e::ambiguous_frame;
          }
          if (distance > 0x80000000u) {
            return refusal_e::older_frame;
          }
          new_frame = true;
        }
      }

      if (new_frame) {
        seen_blocks_ = 0;
      }
      seen_blocks_ |= block_bit;
      latest_ = sample_t {block, received_at};
      return refusal_e::none;
    }

    snapshot_t snapshot(std::uint64_t generation, clock::time_point now, clock::duration max_age) const {
      if (generation_ == 0) {
        return {availability_e::generation_unavailable, std::nullopt};
      }
      if (generation != generation_) {
        return {availability_e::generation_mismatch, std::nullopt};
      }
      if (!(client_features_ & client_feature_flag)) {
        return {availability_e::capability_unavailable, std::nullopt};
      }
      if (!latest_) {
        return {availability_e::no_report, std::nullopt};
      }
      if (max_age < clock::duration::zero()) {
        return {availability_e::invalid_max_age, std::nullopt};
      }
      if (now < latest_->received_at) {
        return {availability_e::reversed_clock, std::nullopt};
      }

      // After checking order, unsigned tick subtraction also handles extreme
      // time_points without overflowing a signed chrono duration.
      using unsigned_ticks_t = std::make_unsigned_t<clock::rep>;
      const auto age = static_cast<unsigned_ticks_t>(now.time_since_epoch().count()) -
                       static_cast<unsigned_ticks_t>(latest_->received_at.time_since_epoch().count());
      if (age > static_cast<unsigned_ticks_t>(max_age.count())) {
        return {availability_e::stale_report, std::nullopt};
      }
      return {availability_e::fresh_report, latest_};
    }

  private:
    std::uint64_t generation_ = 0;
    std::uint32_t client_features_ = 0;
    std::optional<sample_t> latest_;
    std::uint8_t seen_blocks_ = 0;
  };
}  // namespace stream::fec_status
