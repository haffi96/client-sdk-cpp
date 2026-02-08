/*
 * Copyright 2025 LiveKit, Inc.
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
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace publish_h264 {

enum class H264Framing {
  Avcc,
  AnnexB,
};

struct H264AccessUnit {
  std::vector<std::uint8_t> data;
  std::int64_t timestamp_us{0};
  bool is_keyframe{false};
};

using H264AccessUnitCallback = std::function<void(H264AccessUnit)>;

/**
 * Reads length-prefixed AVC (4-byte big-endian NAL length + payload) from a
 * TCP server and yields complete access units. Keyframes are detected by
 * NAL type (IDR = 5). Runs a background thread; call stop() to disconnect.
 */
class H264TcpSource {
public:
  H264TcpSource(const std::string &host, std::uint16_t port,
                H264AccessUnitCallback callback, H264Framing framing);
  ~H264TcpSource();

  void start();
  void stop();
  bool running() const { return running_.load(); }

private:
  void loop();

  std::string host_;
  std::uint16_t port_;
  H264AccessUnitCallback callback_;
  H264Framing framing_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

} // namespace publish_h264
