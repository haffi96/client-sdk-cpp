/*
 * Copyright 2025 LiveKit
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "livekit/ffi_handle.h"

namespace livekit {

/**
 * Video source that accepts pre-encoded H.264 access units and publishes
 * them over WebRTC without decoding or re-encoding (passthrough).
 *
 * Create with width/height (resolution of the encoded stream). Push
 * Annex-B framed access units via captureEncodedFrame(). The SDK injects
 * them directly into WebRTC's RTP packetizer.
 *
 * See docs/H264_FRAMING.md for the expected data format.
 */
class EncodedVideoSource {
public:
  /**
   * Create an encoded H.264 video source.
   *
   * @param width   Width in pixels.
   * @param height  Height in pixels.
   */
  EncodedVideoSource(int width, int height);
  virtual ~EncodedVideoSource() = default;

  EncodedVideoSource(const EncodedVideoSource &) = delete;
  EncodedVideoSource &operator=(const EncodedVideoSource &) = delete;
  EncodedVideoSource(EncodedVideoSource &&) noexcept = default;
  EncodedVideoSource &operator=(EncodedVideoSource &&) noexcept = default;

  int width() const noexcept { return width_; }
  int height() const noexcept { return height_; }
  std::uint64_t ffi_handle_id() const noexcept { return handle_.get(); }

  /**
   * Push one H.264 access unit (e.g. SPS/PPS/SEI/slice NALs in order).
   *
   * @param encoded_data  Annex-B framed bytes (NALs separated by
   *                      00 00 00 01 start codes).
   * @param timestamp_us  Timestamp in microseconds.
   * @param is_keyframe   True if the frame contains an IDR slice.
   */
  bool captureEncodedFrame(const std::vector<std::uint8_t> &encoded_data,
                           std::int64_t timestamp_us,
                           bool is_keyframe);

  bool captureEncodedFrame(const std::uint8_t *data, std::size_t size,
                           std::int64_t timestamp_us, bool is_keyframe);

  /// Returns false after the first capture failure.
  bool captureSupported() const noexcept { return capture_supported_.load(); }

private:
  FfiHandle handle_;
  int width_{0};
  int height_{0};
  std::atomic<bool> capture_supported_{true};
};

} // namespace livekit
