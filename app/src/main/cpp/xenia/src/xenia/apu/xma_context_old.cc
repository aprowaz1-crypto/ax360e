/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2024 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*/

#include "xenia/apu/xma_context_old.h"

#include <cstring>

#include "xenia/apu/xma_decoder.h"
#include "xenia/apu/xma_helpers.h"
#include "xenia/base/bit_stream.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/ring_buffer.h"

#if XE_PLATFORM_ANDROID
#include <android/log.h>
#endif

extern "C" {
#if XE_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "third_party/FFmpeg/libavcodec/avcodec.h"
#if XE_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

namespace xe {
namespace apu {

namespace {

bool HasUsablePacketFrameStart(const uint8_t* packet,
                               uint32_t packet_frame_offset) {
  if (!packet || packet_frame_offset < XmaContext::kBitsPerHeader ||
      packet_frame_offset >
          (XmaContext::kBitsPerPacket - XmaContext::kBitsPerHeader)) {
    return false;
  }

  BitStream stream(const_cast<uint8_t*>(packet), XmaContext::kBitsPerPacket);
  stream.SetOffset(packet_frame_offset);
  if (stream.BitsRemaining() < 15) {
    return false;
  }

  const uint64_t frame_size = stream.Read(15);
  if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
    return false;
  }

  return (frame_size - 15) <= stream.BitsRemaining();
}

}  // namespace

XmaContextOld::XmaContextOld() = default;

XmaContextOld::~XmaContextOld() {
  if (av_context_) {
    if (avcodec_is_open(av_context_)) {
      avcodec_close(av_context_);
    }
    av_free(av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
  // if (current_frame_) {
  //   delete[] current_frame_;
  //  }
}

int XmaContextOld::Setup(uint32_t id, Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  // chrispy: preallocate this buffer so that ffmpeg isn't reallocating it for
  // every packet, these allocations were causing RtlSubsegmentInitialize
  av_packet_->buf = av_buffer_alloc(128 * 1024);
  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    XELOGE("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    XELOGE("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    XELOGE("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

bool XmaContextOld::Work() {
  if (!is_enabled() || !is_allocated()) {
    return false;
  }
  {
    std::lock_guard<xe_mutex> lock(lock_);
    set_is_enabled(false);

    auto context_ptr = memory()->TranslateVirtual(guest_ptr());
    XMA_CONTEXT_DATA data(context_ptr);
    Decode(&data);
    data.Store(context_ptr);

#if XE_PLATFORM_ANDROID
    static uint32_t work_count = 0;
    if (++work_count % 500 == 1) {
      __android_log_print(ANDROID_LOG_INFO, "ax360e_xma",
          "XMA Work() called %u times (ctx %d) obv=%d in0v=%d in1v=%d "
          "rd_off=%u wr_off=%u blk=%u",
          work_count, id(),
          (int)data.output_buffer_valid,
          (int)data.input_buffer_0_valid, (int)data.input_buffer_1_valid,
          (uint32_t)data.output_buffer_read_offset,
          (uint32_t)data.output_buffer_write_offset,
          (uint32_t)data.output_buffer_block_count);
    }
#endif

    return true;
  }
}

void XmaContextOld::Enable() {
  std::lock_guard<xe_mutex> lock(lock_);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);

  data.Store(context_ptr);

  set_is_enabled(true);
}

bool XmaContextOld::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContextOld::Clear() {
  std::lock_guard<xe_mutex> lock(lock_);
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);

  data.input_buffer_0_valid = 0;
  data.input_buffer_1_valid = 0;
  data.output_buffer_valid = 0;

  data.input_buffer_read_offset = 0;
  data.output_buffer_read_offset = 0;
  data.output_buffer_write_offset = 0;

  xma_frame_.fill(0);
  split_frame_len_ = 0;
  split_frame_len_partial_ = 0;
  split_frame_padding_start_ = 0;

  data.Store(context_ptr);
}

void XmaContextOld::Disable() {
  std::lock_guard<xe_mutex> lock(lock_);
  set_is_enabled(false);
}

void XmaContextOld::Release() {
  // Lock it in case the decoder thread is working on it now.
  std::lock_guard<xe_mutex> lock(lock_);
  assert_true(is_allocated_ == true);

  set_is_allocated(false);
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));  // Zero it.
}

void XmaContextOld::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  // No more frames.
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = GetPacketFirstFrameOffset(data);
}

bool XmaContextOld::TrySetupNextLoop(XMA_CONTEXT_DATA* data,
                                     bool ignore_input_buffer_offset) {
  // Setup the input buffer offset if next loop exists.
  // TODO(Pseudo-Kernel): Need to handle loop in the following cases.
  // 1. loop_start == loop_end == 0
  // 2. loop_start > loop_end && loop_count > 0
  if (data->loop_count > 0 && data->loop_start < data->loop_end &&
      (ignore_input_buffer_offset ||
       data->input_buffer_read_offset >= data->loop_end)) {
    // Loop back to the beginning.
    data->input_buffer_read_offset = data->loop_start;
    if (data->loop_count < 255) {
      data->loop_count--;
    }
    return true;
  }
  return false;
}

/*
void XmaContext::NextPacket(
    uint8_t* input_buffer,
    uint32_t input_size,
    uint32_t input_buffer_read_offset) {
*/
void XmaContextOld::NextPacket(XMA_CONTEXT_DATA* data) {
  // auto packet_idx = GetFramePacketNumber(input_buffer, input_size,
  // input_buffer_read_offset);

  // packet_idx++;
  // if (packet_idx++ >= input_size)
}

int XmaContextOld::GetSampleRate(int id) {
  switch (id) {
    case 0:
      return 24000;
    case 1:
      return 32000;
    case 2:
      return 44100;
    case 3:
      return 48000;
  }
  assert_always();
  return 0;
}

bool XmaContextOld::ValidFrameOffset(uint8_t* block, size_t size_bytes,
                                     size_t frame_offset_bits) {
  uint32_t packet_num =
      GetFramePacketNumber(block, size_bytes, frame_offset_bits);
  if (packet_num == -1) {
    // Invalid packet number
    XELOGAPU("ValidFrameOffset: Invalid packet number");
    return false;
  }

  uint8_t* packet = block + (packet_num * kBytesPerPacket);
  size_t relative_offset_bits = frame_offset_bits % kBitsPerPacket;

  uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  if (first_frame_offset == -1 || first_frame_offset > kBitsPerPacket) {
    XELOGAPU("ValidFrameOffset: Invalid frame offset {}", first_frame_offset);
    // Packet only contains a partial frame, so no frames can start here.
    return false;
  }

  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);
  while (true) {
    if (stream.offset_bits() == relative_offset_bits) {
      return true;
    }

    if (stream.BitsRemaining() < 15) {
      XELOGAPU("ValidFrameOffset: No room for next frame header {}",
               first_frame_offset);
      // Not enough room for another frame header.
      return false;
    }

    uint64_t size = stream.Read(15);
    if ((size - 15) > stream.BitsRemaining()) {
      XELOGAPU("ValidFrameOffset: Last frame {} - {}", first_frame_offset,
               size);
      // Last frame.
      return false;
    } else if (size == 0x7FFF) {
      // Invalid frame (and last of this packet)
      return false;
    }

    stream.Advance(size - 16);

    // Read the trailing bit to see if frames follow
    if (stream.Read(1) == 0) {
      break;
    }
  }

  return false;
}

void XmaContextOld::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  // What I see:
  // XMA outputs 2 bytes per sample
  // 512 samples per frame (128 per subframe)
  // Max output size is data.output_buffer_block_count * 256

  // This decoder is fed packets (max 4095 per buffer)
  // Packets contain "some" frames
  // 32bit header (big endian)

  // Frames are the smallest thing the SPUs can decode.
  // They can and usually will span packets.

  // Sample rates (data.sample_rate):
  // 0 - 24 kHz
  // 1 - 32 kHz
  // 2 - 44.1 kHz
  // 3 - 48 kHz

  // SPUs also support stereo decoding. (data.is_stereo)

  // Check the output buffer - we cannot decode anything else if it's
  // unavailable.
  if (!data->output_buffer_valid) {
#if XE_PLATFORM_ANDROID
    static uint32_t obv_stall_count = 0;
    if (++obv_stall_count % 500 == 1) {
      __android_log_print(ANDROID_LOG_WARN, "ax360e_xma",
          "XMA ctx %d: output_buffer_valid=0 STALL (count=%u) "
          "in0_valid=%d in1_valid=%d rd_off=%u wr_off=%u blk_cnt=%u",
          id(), obv_stall_count,
          (int)data->input_buffer_0_valid, (int)data->input_buffer_1_valid,
          (uint32_t)data->output_buffer_read_offset,
          (uint32_t)data->output_buffer_write_offset,
          (uint32_t)data->output_buffer_block_count);
    }
#endif
    return;
  }

  // No available data.
  if (!data->input_buffer_0_valid && !data->input_buffer_1_valid) {
#if XE_PLATFORM_ANDROID
    static uint32_t no_input_count = 0;
    if (++no_input_count % 500 == 1) {
      __android_log_print(ANDROID_LOG_WARN, "ax360e_xma",
          "XMA ctx %d: no valid input buffers (count=%u)",
          id(), no_input_count);
    }
#endif
    return;
  }

  // XAudio Loops
  // loop_count:
  //  - XAUDIO2_MAX_LOOP_COUNT = 254
  //  - XAUDIO2_LOOP_INFINITE = 255
  // loop_start/loop_end are bit offsets to a specific frame

  // Translate pointers for future use.
  // Sometimes the game will use rolling input buffers. If they do, we cannot
  // assume they form a complete block! In addition, the buffers DO NOT have
  // to be contiguous!
  uint8_t* in0 = data->input_buffer_0_valid
                     ? memory()->TranslatePhysical(data->input_buffer_0_ptr)
                     : nullptr;
  uint8_t* in1 = data->input_buffer_1_valid
                     ? memory()->TranslatePhysical(data->input_buffer_1_ptr)
                     : nullptr;
  uint8_t* current_input_buffer = data->current_buffer ? in1 : in0;

  if (!current_input_buffer) {
    XELOGE("XmaContext {}: Error - input buffer pointer is invalid!", id());
    return;
  }

  if (!data->output_buffer_block_count) {
    XELOGE("XmaContext {}: Error - Received 0 for output_buffer_block_count!",
           id());
    return;
  }

  if (is_stream_done_) {
    is_stream_done_ = false;
    packets_skip_ = 0;
    SwapInputBuffer(data);
    return;
  }

  size_t input_buffer_0_size =
      data->input_buffer_0_packet_count * kBytesPerPacket;
  size_t input_buffer_1_size =
      data->input_buffer_1_packet_count * kBytesPerPacket;

  size_t current_input_size =
      data->current_buffer ? input_buffer_1_size : input_buffer_0_size;
  size_t current_input_packet_count = current_input_size / kBytesPerPacket;
  bool is_streaming = data->input_buffer_0_packet_count == 1 &&
                      data->input_buffer_1_packet_count == 1;

  // Output buffers are in raw PCM samples, 256 bytes per block.
  // Output buffer is a ring buffer. We need to write from the write offset
  // to the read offset.
  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);
  uint32_t output_capacity =
      data->output_buffer_block_count * kBytesPerSubframeChannel;
  uint32_t output_read_offset =
      data->output_buffer_read_offset * kBytesPerSubframeChannel;
  uint32_t output_write_offset =
      data->output_buffer_write_offset * kBytesPerSubframeChannel;

  RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);

  auto reset_split_frame_state = [&]() {
    split_frame_len_ = 0;
    split_frame_len_partial_ = 0;
    split_frame_padding_start_ = 0;
  };
  auto fail_parser = [&](const char* reason, size_t offset, size_t limit_bits) {
    XELOGE("XmaContext {}: {} (offset={} limit={})", id(), reason, offset,
           limit_bits);
    data->parser_error_status = 4;
    reset_split_frame_state();
    if (av_context_) {
      avcodec_flush_buffers(av_context_);
    }
    SwapInputBuffer(data);
  };

  // We can only decode an entire frame and write it out at a time, so
  // don't save any samples.
  // TODO(JoelLinn): subframes when looping
  size_t output_remaining_bytes = output_rb.write_count();
  output_remaining_bytes -=
      output_remaining_bytes % (kBytesPerFrameChannel << data->is_stereo);

  // is_dirty_ = true; // TODO
  // is_dirty_ = false;  // TODO
  assert_false(data->stop_when_done);
  assert_false(data->interrupt_when_done);
  static int total_samples = 0;
  // Decode until we can't write any more data.
  while (output_remaining_bytes > 0) {
    if (!data->input_buffer_0_valid && !data->input_buffer_1_valid) {
      // Out of data.
      break;
    }
    // Setup the input buffer if we are at loop_end.
    // The input buffer must not be swapped out until all loops are processed.
    bool reuse_input_buffer = TrySetupNextLoop(data, false);

    // assert_true(packets_skip_ == 0);
    // assert_true(split_frame_len_ == 0);
    // assert_true(split_frame_len_partial_ == 0);

    // Where are we in the buffer (in XMA jargon)
    int packet_idx, frame_idx, frame_count;
    uint8_t* packet;
    bool frame_last_split;

    const size_t current_input_size_bits = current_input_size * 8;
    if (data->input_buffer_read_offset > current_input_size_bits) {
      fail_parser("Provided input offset exceeds input buffer size",
                  data->input_buffer_read_offset, current_input_size_bits);
      return;
    }
    BitStream stream(current_input_buffer, current_input_size_bits);
    stream.SetOffset(data->input_buffer_read_offset);
    // if we had a buffer swap try to skip packets first
    if (packets_skip_ > 0) {
      packet_idx =
          GetFramePacketNumber(current_input_buffer, current_input_size,
                               data->input_buffer_read_offset);
      if (packet_idx < 0) {
        fail_parser("Invalid packet index while skipping packets",
                    data->input_buffer_read_offset, current_input_size_bits);
        return;
      }
      while (packets_skip_ > 0) {
        packets_skip_--;
        packet_idx++;
        if (packet_idx > current_input_packet_count) {
          if (!reuse_input_buffer) {
            // Last packet. Try setup once more.
            reuse_input_buffer = TrySetupNextLoop(data, true);
          }
          if (!reuse_input_buffer) {
            if (is_streaming) {
              SwapInputBuffer(data);
            } else {
              is_stream_done_ = true;
            }
          }
          return;
        }
      }
      // invalid frame pointer but needed for us
      data->input_buffer_read_offset = packet_idx * kBitsPerPacket;
      // continue;
    }

    if (split_frame_len_) {
      // handle a frame that was split over two packages
      packet_idx =
          GetFramePacketNumber(current_input_buffer, current_input_size,
                               data->input_buffer_read_offset);
      if (packet_idx < 0 ||
          packet_idx >= static_cast<int>(current_input_packet_count)) {
        fail_parser("Invalid packet index for split frame",
                    data->input_buffer_read_offset, current_input_size_bits);
        return;
      }
      packet = current_input_buffer + packet_idx * kBytesPerPacket;
      std::tie(frame_count, frame_last_split) = GetPacketFrameCount(packet);
      frame_idx = -1;

      const size_t packet_limit_bits = (packet_idx + 1) * kBitsPerPacket;
      const size_t packet_payload_offset =
          packet_idx * kBitsPerPacket + kBitsPerHeader;
      if (packet_payload_offset > packet_limit_bits) {
        fail_parser("Split frame packet payload offset exceeds packet bounds",
                    packet_payload_offset, packet_limit_bits);
        return;
      }
      stream = BitStream(current_input_buffer, packet_limit_bits);
      stream.SetOffset(packet_payload_offset);

      if (split_frame_len_ > xma::kMaxFrameLength) {
        // TODO write CopyPeekMethod
        auto offset = stream.offset_bits();
        stream.Copy(
            xma_frame_.data() + 1 +
                ((split_frame_len_partial_ + split_frame_padding_start_) / 8),
            15 - split_frame_len_partial_);
        stream.SetOffset(offset);
        BitStream slen(xma_frame_.data() + 1, 15 + split_frame_padding_start_);
        slen.Advance(split_frame_padding_start_);
        split_frame_len_ = static_cast<int>(slen.Read(15));
      }

      if (frame_count > 0) {
        // assert_true(xma::GetPacketFrameOffset(packet) - 32 ==
        //             split_frame_len_ - split_frame_len_partial_);
      }

      auto offset = stream.Copy(
          xma_frame_.data() + 1 +
              ((split_frame_len_partial_ + split_frame_padding_start_) / 8),
          split_frame_len_ - split_frame_len_partial_);
      assert_true(offset ==
                  (split_frame_padding_start_ + split_frame_len_partial_) % 8);
    } else {
      if (data->input_buffer_read_offset % kBitsPerPacket == 0) {
        // Invalid offset. Go ahead and set it.
        int packet_number =
            GetFramePacketNumber(current_input_buffer, current_input_size,
                                 data->input_buffer_read_offset);

        if (packet_number == -1) {
          return;
        }

        auto offset = FindNextPacketWithFrameOffset(current_input_buffer,
                                                    current_input_size,
                                                    packet_number);
        if (offset == 0) {
          XELOGE("XmaContext {}: Failed to resolve next frame offset at packet "
                 "boundary",
                 id());
          fail_parser("Failed to resolve next frame offset at packet boundary",
                      data->input_buffer_read_offset, current_input_size_bits);
          return;
        }

        const size_t packet_start_offset =
            static_cast<size_t>(packet_number) * kBitsPerPacket;
        const size_t header_only_offset = packet_start_offset + kBitsPerHeader;
        if (offset == header_only_offset &&
            !ValidFrameOffset(current_input_buffer, current_input_size,
                              offset)) {
          auto next_offset = FindNextPacketWithFrameOffset(
              current_input_buffer, current_input_size, packet_number + 1);
          if (next_offset != 0) {
            XELOGAPU(
                "XmaContext {}: Skipping invalid packet-start frame offset {} "
                "at packet {}, advancing to {}",
                id(), offset, packet_number, next_offset);
            offset = next_offset;
          }
        }

        data->input_buffer_read_offset = static_cast<uint32_t>(offset);
      }

      if (!ValidFrameOffset(current_input_buffer, current_input_size,
                            data->input_buffer_read_offset)) {
        const size_t packet_number =
            data->input_buffer_read_offset / kBitsPerPacket;
        // Try finding a valid frame in the current packet first
        auto next_offset = FindNextPacketWithFrameOffset(
            current_input_buffer, current_input_size, packet_number);
        if (next_offset != 0 &&
            next_offset > data->input_buffer_read_offset &&
            ValidFrameOffset(current_input_buffer, current_input_size,
                             next_offset)) {
          XELOGAPU(
              "XmaContext {}: Recovered from invalid read "
              "offset {} by advancing to {}",
              id(), data->input_buffer_read_offset, next_offset);
          data->input_buffer_read_offset = static_cast<uint32_t>(next_offset);
        } else {
          // Current packet exhausted, try from next packet onward
          next_offset = FindNextPacketWithFrameOffset(
              current_input_buffer, current_input_size, packet_number + 1);
          if (next_offset != 0 &&
              ValidFrameOffset(current_input_buffer, current_input_size,
                               next_offset)) {
            XELOGAPU(
                "XmaContext {}: Recovered from invalid read "
                "offset {} by advancing to next packet at {}",
                id(), data->input_buffer_read_offset, next_offset);
            data->input_buffer_read_offset =
                static_cast<uint32_t>(next_offset);
          }
        }
      }

      if (!ValidFrameOffset(current_input_buffer, current_input_size,
                            data->input_buffer_read_offset)) {
        XELOGAPU("XmaContext {}: Error - Invalid read offset {}!", id(),
                 data->input_buffer_read_offset);
        SwapInputBuffer(data);
        return;
      }

      // Where are we in the buffer (in XMA jargon)
      std::tie(packet_idx, frame_idx) =
          GetFrameNumber(current_input_buffer, current_input_size,
                         data->input_buffer_read_offset);
      if (packet_idx < 0 || frame_idx < 0) {
        fail_parser("Invalid frame location", data->input_buffer_read_offset,
                    current_input_size_bits);
        return;
      }
      packet = current_input_buffer + packet_idx * kBytesPerPacket;
      // frames that belong to this packet
      std::tie(frame_count, frame_last_split) = GetPacketFrameCount(packet);
      assert_true(frame_count >= 0);  // TODO end

      PrepareDecoder(packet, data->sample_rate, bool(data->is_stereo));

      // Current frame is split to next packet:
      bool frame_is_split = frame_last_split && (frame_idx >= frame_count - 1);

      stream =
          BitStream(current_input_buffer, (packet_idx + 1) * kBitsPerPacket);
      if (data->input_buffer_read_offset > stream.size_bits()) {
        fail_parser("Frame offset exceeds current packet bounds",
                    data->input_buffer_read_offset, stream.size_bits());
        return;
      }
      stream.SetOffset(data->input_buffer_read_offset);
      // int frame_len;
      // int frame_len_partial
      split_frame_len_partial_ = static_cast<int>(stream.BitsRemaining());
      if (split_frame_len_partial_ >= 15) {
        split_frame_len_ = static_cast<int>(stream.Peek(15));
      } else {
        // assert_always();
        split_frame_len_ = xma::kMaxFrameLength + 1;
      }
      if (frame_is_split !=
          (split_frame_len_ > split_frame_len_partial_)) {
        XELOGE(
            "XmaContext {}: Split-frame invariant mismatch "
            "(frame_is_split={}, split_len={}, split_partial={})",
            id(), frame_is_split, split_frame_len_, split_frame_len_partial_);
        fail_parser("Split-frame invariant mismatch",
                    data->input_buffer_read_offset, current_input_size_bits);
        return;
      }

      // TODO fix bitstream copy
      std::memset(xma_frame_.data(), 0, xma_frame_.size());

      {
        int32_t bits_to_copy =
            std::min(split_frame_len_, split_frame_len_partial_);

        if (!stream.IsOffsetValid(bits_to_copy)) {
          XELOGAPU(
              "XmaContext {}: Error - Invalid amount of bits to copy! "
              "split_frame_len: {}, split_partial: {}, offset_bits: {}",
              id(), split_frame_len_, split_frame_len_partial_,
              stream.offset_bits());
          fail_parser("Invalid amount of bits to copy in split frame",
                      stream.offset_bits(), stream.size_bits());
          return;
        }
        auto offset = stream.Copy(xma_frame_.data() + 1, bits_to_copy);
        assert_true(offset < 8);
        split_frame_padding_start_ = static_cast<uint8_t>(offset);
      }

      if (frame_is_split) {
        // go to next xma packet of this stream
        packets_skip_ = xma::GetPacketSkipCount(packet) + 1;
        while (packets_skip_ > 0) {
          packets_skip_--;
          packet += kBytesPerPacket;
          packet_idx++;
          if (packet_idx >= current_input_packet_count) {
            if (!reuse_input_buffer) {
              // Last packet. Try setup once more.
              reuse_input_buffer = TrySetupNextLoop(data, true);
            }
            if (!reuse_input_buffer) {
              if (is_streaming) {
                SwapInputBuffer(data);
              } else {
                is_stream_done_ = true;
              }
            }
            return;
          }
        }
        // TODO guest might read this:
        data->input_buffer_read_offset = packet_idx * kBitsPerPacket;
        continue;
      }
    }

    av_packet_->data = xma_frame_.data();
    av_packet_->size = static_cast<int>(
        1 + ((split_frame_padding_start_ + split_frame_len_) / 8) +
        (((split_frame_padding_start_ + split_frame_len_) % 8) ? 1 : 0));

    auto padding_end = av_packet_->size * 8 -
                       (8 + split_frame_padding_start_ + split_frame_len_);
    assert_true(padding_end < 8);
    xma_frame_[0] =
        ((split_frame_padding_start_ & 7) << 5) | ((padding_end & 7) << 2);

    split_frame_len_ = 0;
    split_frame_len_partial_ = 0;
    split_frame_padding_start_ = 0;

    auto ret = avcodec_send_packet(av_context_, av_packet_);
    if (ret < 0) {
      XELOGE(
          "XmaContext {}: Error - Sending packet for decoding failed ({})",
          id(), ret);
      fail_parser("Sending packet for decoding failed",
                  data->input_buffer_read_offset, current_input_size_bits);
      return;
    }
    ret = avcodec_receive_frame(av_context_, av_frame_);
    bool frame_decoded = false;
    if (ret == AVERROR(EAGAIN)) {
      // The decoder accepted this frame but needs more packets before emitting
      // PCM. Advance the bitstream so we don't keep resubmitting the same packet
      // start on the next kick.
    } else if (ret == AVERROR_EOF) {
      break;
    } else if (ret < 0) {
      XELOGE("XmaContext {}: Error - Decoding failed ({})", id(), ret);
      fail_parser("Decoding failed", data->input_buffer_read_offset,
                  current_input_size_bits);
      return;
    } else {
      frame_decoded = true;
    }
    if (frame_decoded) {
      // copy over 1 frame
      // update input buffer read offset

      // assert(decoded_consumed_samples_ + kSamplesPerFrame <=
      //       current_frame_.size());
      assert_true(av_context_->sample_fmt == AV_SAMPLE_FMT_FLTP);
      // assert_true(frame_is_split == (frame_idx == -1));

      //			dump_raw(av_frame_, id());
      ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data),
                   bool(av_frame_->channels > 1), raw_frame_.data());
      // decoded_consumed_samples_ += kSamplesPerFrame;

      auto byte_count = kBytesPerFrameChannel << data->is_stereo;
      assert_true(output_remaining_bytes >= byte_count);
      output_rb.Write(raw_frame_.data(), byte_count);
      output_remaining_bytes -= byte_count;
      data->output_buffer_write_offset = output_rb.write_offset() / 256;

      total_samples += id_ == 0 ? kSamplesPerFrame : 0;
    }

    uint32_t offset =
        std::max(kBitsPerHeader, data->input_buffer_read_offset);
    offset = static_cast<uint32_t>(
        GetNextFrame(current_input_buffer, current_input_size, offset));

    if (offset == 0 && frame_idx == -1) {
      // A split frame can resume right at packet payload start. If that resume
      // packet contains no additional complete frame starts, jumping back to
      // its first-frame offset (often raw header + 32 bits) just bounces the
      // parser on the same packet start forever. Move on to the next packet
      // instead.
      packets_skip_ = xma::GetPacketSkipCount(packet) + 1;
      while (packets_skip_ > 0) {
        packets_skip_--;
        packet_idx++;
        if (packet_idx >= current_input_packet_count) {
          if (!reuse_input_buffer) {
            reuse_input_buffer = TrySetupNextLoop(data, true);
          }
          if (!reuse_input_buffer) {
            if (is_streaming) {
              SwapInputBuffer(data);
              data->input_buffer_read_offset = GetPacketFirstFrameOffset(data);
            } else {
              is_stream_done_ = true;
            }
            if (output_rb.write_offset() == output_rb.read_offset()) {
#if XE_PLATFORM_ANDROID
              __android_log_print(ANDROID_LOG_WARN, "ax360e_xma",
                  "XMA ctx %d: obv->0 at split-frame skip (rd=%u wr=%u)",
                  id(), (uint32_t)output_rb.read_offset(), (uint32_t)output_rb.write_offset());
#endif
              data->output_buffer_valid = 0;
            }
          }
          return;
        }
      }
      packet = current_input_buffer + packet_idx * kBytesPerPacket;
      offset = static_cast<uint32_t>(FindNextPacketWithFrameOffset(
          current_input_buffer, current_input_size, packet_idx));
    } else if (frame_idx + 1 >= frame_count) {
      // Skip to next packet (no split frame)
      packets_skip_ = xma::GetPacketSkipCount(packet) + 1;
      while (packets_skip_ > 0) {
        packets_skip_--;
        packet_idx++;
        if (packet_idx >= current_input_packet_count) {
          if (!reuse_input_buffer) {
            // Last packet. Try setup once more.
            reuse_input_buffer = TrySetupNextLoop(data, true);
          }
          if (!reuse_input_buffer) {
            if (is_streaming) {
              SwapInputBuffer(data);
              data->input_buffer_read_offset =
                  GetPacketFirstFrameOffset(data);
            } else {
              is_stream_done_ = true;
            }
            if (output_rb.write_offset() == output_rb.read_offset()) {
#if XE_PLATFORM_ANDROID
              __android_log_print(ANDROID_LOG_WARN, "ax360e_xma",
                  "XMA ctx %d: obv->0 at packet-skip (rd=%u wr=%u)",
                  id(), (uint32_t)output_rb.read_offset(), (uint32_t)output_rb.write_offset());
#endif
              data->output_buffer_valid = 0;
            }
          }
          return;
        }
      }
      packet = current_input_buffer + packet_idx * kBytesPerPacket;
      // TODO(Gliniak): There might be an edge-case when we're in packet 26/27
      // and GetPacketFrameOffset returns that there is no data in this packet
      // aka. FrameOffset is set to more than 0x7FFF-0x20
      offset = static_cast<uint32_t>(FindNextPacketWithFrameOffset(
          current_input_buffer, current_input_size, packet_idx));
    }
    if (offset == 0) {
      // Next packet but we already skipped to it
      if (packet_idx >= current_input_packet_count) {
        // Buffer is fully used
        if (!reuse_input_buffer) {
          // Last packet. Try setup once more.
          reuse_input_buffer = TrySetupNextLoop(data, true);
        }
        if (!reuse_input_buffer) {
          if (is_streaming) {
            SwapInputBuffer(data);
          } else {
            is_stream_done_ = true;
          }
        }
        break;
      }
      offset = static_cast<uint32_t>(FindNextPacketWithFrameOffset(
          current_input_buffer, current_input_size, packet_idx));
    }
    if (offset <= data->input_buffer_read_offset ||
        offset > current_input_size_bits) {
      fail_parser("Invalid next frame offset", offset, current_input_size_bits);
      return;
    }
    data->input_buffer_read_offset = offset;

    if (!frame_decoded) {
      continue;
    }
  }

  // assert_true((split_frame_len_ != 0) == (data->input_buffer_read_offset ==
  // 0));

  // The game will kick us again with a new output buffer later.
  // It's important that we only invalidate this if we actually wrote to it!!
  if (output_rb.write_offset() == output_rb.read_offset()) {
#if XE_PLATFORM_ANDROID
    __android_log_print(ANDROID_LOG_WARN, "ax360e_xma",
        "XMA ctx %d: output_buffer_valid -> 0 (ringbuf full/empty, "
        "rd=%u wr=%u cap=%u)",
        id(), (uint32_t)output_read_offset, (uint32_t)output_write_offset,
        (uint32_t)output_capacity);
#endif
    data->output_buffer_valid = 0;
  }
}

uint32_t XmaContextOld::GetPacketFirstFrameOffset(
    const XMA_CONTEXT_DATA* data) {
  uint32_t first_frame_offset = 0;

  uint8_t* in0 = data->input_buffer_0_valid
                     ? memory()->TranslatePhysical(data->input_buffer_0_ptr)
                     : nullptr;
  uint8_t* in1 = data->input_buffer_1_valid
                     ? memory()->TranslatePhysical(data->input_buffer_1_ptr)
                     : nullptr;
  uint8_t* current_input_buffer = data->current_buffer ? in1 : in0;
  size_t current_input_size =
      data->current_buffer ? data->input_buffer_1_packet_count * kBytesPerPacket
                           : data->input_buffer_0_packet_count * kBytesPerPacket;

  if (current_input_buffer) {
    size_t resolved_offset =
        FindNextPacketWithFrameOffset(current_input_buffer, current_input_size,
                                      0);
    if (resolved_offset != 0) {
      first_frame_offset = static_cast<uint32_t>(resolved_offset);
    } else {
      uint32_t packet_frame_offset =
          xma::GetPacketFrameOffset(current_input_buffer);
      XELOGE(
          "XmaContext {}: No usable first frame offset in next packet "
          "(header offset={})",
          id(), packet_frame_offset);
    }
  }
  return first_frame_offset;
}

size_t XmaContextOld::FindNextPacketWithFrameOffset(uint8_t* block, size_t size,
                                                    size_t packet_idx) {
  if (!block || size < kBytesPerPacket) {
    return 0;
  }

  const size_t packet_count = size / kBytesPerPacket;
  for (size_t i = packet_idx; i < packet_count; ++i) {
    uint8_t* packet = block + (i * kBytesPerPacket);
    const uint32_t packet_frame_offset = xma::GetPacketFrameOffset(packet);
    if (HasUsablePacketFrameStart(packet, packet_frame_offset)) {
      return (i * kBitsPerPacket) + packet_frame_offset;
    }
  }

  return 0;
}

size_t XmaContextOld::GetNextFrame(uint8_t* block, size_t size,
                                   size_t bit_offset) {
  // offset = xma::GetPacketFrameOffset(packet);
  // TODO meh
  // auto next_packet = bit_offset - bit_offset % kBitsPerPacket +
  // kBitsPerPacket;
  const size_t size_bits = size * 8;
  if (bit_offset >= size_bits) {
    XELOGAPU("GetNextFrame: Invalid bit offset {} / {}", bit_offset, size_bits);
    return 0;
  }

  auto packet_idx = GetFramePacketNumber(block, size, bit_offset);
  if (packet_idx < 0) {
    return 0;
  }

  BitStream stream(block, size_bits);
  stream.SetOffset(bit_offset);

  if (stream.BitsRemaining() < 15) {
    return 0;
  }

  uint64_t len = stream.Read(15);
  if ((len - 15) > stream.BitsRemaining()) {
    // assert_always("TODO");
    // *bit_offset = next_packet;
    // return false;
    // return next_packet;
    return 0;
  } else if (len >= xma::kMaxFrameLength) {
    XELOGAPU("GetNextFrame: Invalid frame length {}", len);
    // *bit_offset = next_packet;
    // return false;
    return 0;
    // return next_packet;
  }

  stream.Advance(len - (15 + 1));
  // Read the trailing bit to see if frames follow
  if (stream.Read(1) == 0) {
    return 0;
  }

  bit_offset += len;
  if (packet_idx < GetFramePacketNumber(block, size, bit_offset)) {
    return 0;
  }
  return bit_offset;
}

int XmaContextOld::GetFramePacketNumber(uint8_t* block, size_t size,
                                        size_t bit_offset) {
  size *= 8;
  if (bit_offset >= size) {
    XELOGAPU("GetFramePacketNumber: Invalid bit offset {} / {}", bit_offset,
             size);
    return -1;
  }

  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;

  return (uint32_t)packet_number;
}

std::tuple<int, int> XmaContextOld::GetFrameNumber(uint8_t* block, size_t size,
                                                   size_t bit_offset) {
  auto packet_idx = GetFramePacketNumber(block, size, bit_offset);

  if (packet_idx < 0 || (packet_idx + 1) * kBytesPerPacket > size) {
    XELOGAPU("GetFrameNumber: Invalid packet index {} for size {}", packet_idx,
             size);
    return {packet_idx, -2};
  }

  if (bit_offset == 0) {
    return {packet_idx, -1};
  }

  uint8_t* packet = block + (packet_idx * kBytesPerPacket);
  auto first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(block, size * 8);
  stream.SetOffset(packet_idx * kBitsPerPacket + first_frame_offset);

  int frame_idx = 0;
  while (true) {
    if (stream.BitsRemaining() < 15) {
      break;
    }

    if (stream.offset_bits() == bit_offset) {
      break;
    }

    uint64_t size = stream.Read(15);
    if ((size - 15) > stream.BitsRemaining()) {
      // Last frame.
      break;
    } else if (size == 0x7FFF) {
      // Invalid frame (and last of this packet)
      break;
    }

    stream.Advance(size - (15 + 1));

    // Read the trailing bit to see if frames follow
    if (stream.Read(1) == 0) {
      break;
    }
    frame_idx++;
  }
  return {packet_idx, frame_idx};
}

std::tuple<int, bool> XmaContextOld::GetPacketFrameCount(uint8_t* packet) {
  auto first_frame_offset = xma::GetPacketFrameOffset(packet);
  if (first_frame_offset > kBitsPerPacket - kBitsPerHeader) {
    // frame offset is beyond packet end
    return {0, false};
  }

  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);
  int frame_count = 0;

  while (true) {
    if (stream.BitsRemaining() < 15) {
      return {frame_count, false};
    }

    frame_count++;
    uint64_t size = stream.Read(15);
    if ((size - 15) > stream.BitsRemaining()) {
      return {frame_count, true};
    } else if (size == 0x7FFF) {
      XELOGAPU("GetPacketFrameCount: Invalid frame length marker");
      return {frame_count, true};
    }

    stream.Advance(size - (15 + 1));

    if (stream.Read(1) == 0) {
      return {frame_count, false};
    }
    // There is a case when frame ends EXACTLY at the end of packet.
    // In such case we shouldn't increase frame count by additional not existing
    // frame and don't mark it as splitted, but as a normal frame
    if (!stream.BitsRemaining()) {
      return {frame_count, false};
    }
  }
}

int XmaContextOld::PrepareDecoder(uint8_t* packet, int sample_rate,
                                  bool is_two_channel) {
  // Sanity check: Packet metadata is always 1 for XMA2/0 for XMA
  assert_true((packet[2] & 0x7) == 1 || (packet[2] & 0x7) == 0);

  sample_rate = GetSampleRate(sample_rate);

  // Re-initialize the context with new sample rate and channels.
  uint32_t channels = is_two_channel ? 2 : 1;
  if (av_context_->sample_rate != sample_rate ||
      av_context_->channels != channels) {
    // We have to reopen the codec so it'll realloc whatever data it needs.
    // TODO(DrChat): Find a better way.
    avcodec_close(av_context_);

    av_context_->sample_rate = sample_rate;
    av_context_->channels = channels;

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      XELOGE("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    return 1;
  }
  return 0;
}

}  // namespace apu
}  // namespace xe
