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

#include "livekit/encoded_video_source.h"

#include <stdexcept>

#include "ffi.pb.h"
#include "ffi_client.h"
#include "video_frame.pb.h"

namespace livekit {

EncodedVideoSource::EncodedVideoSource(int width, int height)
    : width_(width), height_(height) {

  proto::FfiRequest req;
  auto *msg = req.mutable_new_video_source();
  msg->set_type(proto::VideoSourceType::VIDEO_SOURCE_ENCODED_H264);
  msg->mutable_resolution()->set_width(static_cast<std::uint32_t>(width_));
  msg->mutable_resolution()->set_height(static_cast<std::uint32_t>(height_));

  auto resp = FfiClient::instance().sendRequest(req);
  if (!resp.has_new_video_source()) {
    throw std::runtime_error("EncodedVideoSource: missing new_video_source");
  }

  handle_ = FfiHandle(resp.new_video_source().source().handle().id());
}

bool EncodedVideoSource::captureEncodedFrame(
    const std::vector<std::uint8_t> &encoded_data, std::int64_t timestamp_us,
    bool is_keyframe) {
  return captureEncodedFrame(encoded_data.data(), encoded_data.size(),
                             timestamp_us, is_keyframe);
}

bool EncodedVideoSource::captureEncodedFrame(const std::uint8_t *data,
                                             std::size_t size,
                                             std::int64_t timestamp_us,
                                             bool is_keyframe) {
  if (!handle_) {
    return false;
  }
  if (!data || size == 0) {
    return false;
  }
  if (!capture_supported_.load()) {
    return false;
  }

  proto::FfiRequest req;
  auto *msg = req.mutable_capture_encoded_frame();
  msg->set_source_handle(handle_.get());
  msg->set_encoded_data(data, size);
  msg->set_timestamp_us(timestamp_us);
  msg->set_is_keyframe(is_keyframe);

  try {
    auto resp = FfiClient::instance().sendRequest(req);
    return true;
  } catch (const std::exception &e) {
    capture_supported_.store(false);
    return false;
  }
}

} // namespace livekit
