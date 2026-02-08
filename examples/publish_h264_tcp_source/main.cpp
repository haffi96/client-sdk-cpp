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

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "livekit/encoded_video_source.h"
#include "livekit/livekit.h"
#include "livekit/room.h"
#include "h264_tcp_source.h"

using namespace livekit;

namespace {

std::atomic<bool> g_running{true};

void printUsage(const char *prog) {
  std::cerr << "Usage: " << prog
            << " --url <ws-url> --token <token> --h264-tcp <host:port> [options]\n\n"
            << "  --url <url>              LiveKit WebSocket URL\n"
            << "  --token <token>          JWT token\n"
            << "  --enable_e2ee            Enable E2EE\n"
            << "  --e2ee_key <key>         E2EE shared key\n\n"
            << "  --h264-tcp <host:port>   TCP server for H.264 (default 127.0.0.1:5004)\n"
            << "  --h264-framing <mode>    avcc (length-prefixed) or annexb (byte-stream)\n"
            << "  --width <w>              Frame width (default: 1280)\n"
            << "  --height <h>             Frame height (default: 720)\n"
            << "  --max-bitrate <bps>      Max video bitrate hint (default: 0 = auto)\n"
            << "  --max-framerate <fps>    Max framerate hint (default: 0 = auto)\n\n"
            << "Env: LIVEKIT_URL, LIVEKIT_TOKEN, LIVEKIT_E2EE_KEY\n";
}

void handleSignal(int) { g_running.store(false); }

struct H264TcpArgs {
  std::string host = "127.0.0.1";
  std::uint16_t port = 5004;
  int width = 1280;
  int height = 720;
  publish_h264::H264Framing framing = publish_h264::H264Framing::Avcc;
  int max_bitrate = 0;       // 0 = auto
  int max_framerate = 0;     // 0 = auto
};

bool parseArgs(int argc, char *argv[], std::string &url, std::string &token,
               bool &enable_e2ee, std::string &e2ee_key, H264TcpArgs &h264) {
  enable_e2ee = false;
  h264 = H264TcpArgs{};
  auto getFlag = [&](const std::string &name, int &i) -> std::string {
    std::string arg = argv[i];
    std::string eq = name + "=";
    if (arg.size() >= name.size() && arg.compare(0, name.size(), name) == 0) {
      if (arg.size() > name.size() && arg[name.size()] == '=')
        return arg.substr(eq.size());
      if (i + 1 < argc)
        return std::string(argv[++i]);
    }
    return {};
  };

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help")
      return false;
    if (a == "--enable_e2ee") {
      enable_e2ee = true;
      continue;
    }
    if (a.compare(0, 10, "--h264-tcp") == 0) {
      std::string v = getFlag("--h264-tcp", i);
      if (v.empty())
        v = "127.0.0.1:5004";
      size_t colon = v.find(':');
      if (colon != std::string::npos) {
        h264.host = v.substr(0, colon);
        try {
          h264.port = static_cast<std::uint16_t>(std::stoul(v.substr(colon + 1)));
        } catch (...) {
          h264.port = 5004;
        }
      } else {
        h264.host = v;
      }
      continue;
    }
    if (a.compare(0, 14, "--h264-framing") == 0) {
      std::string v = getFlag("--h264-framing", i);
      if (v == "annexb") {
        h264.framing = publish_h264::H264Framing::AnnexB;
      } else if (v == "avcc" || v.empty()) {
        h264.framing = publish_h264::H264Framing::Avcc;
      } else {
        std::cerr << "Unknown --h264-framing value: " << v << "\n";
        return false;
      }
      continue;
    }
    if (a.compare(0, 8, "--width") == 0) {
      std::string v = getFlag("--width", i);
      if (!v.empty())
        try {
          h264.width = std::stoi(v);
        } catch (...) {}
      continue;
    }
    if (a.compare(0, 9, "--height") == 0) {
      std::string v = getFlag("--height", i);
      if (!v.empty())
        try {
          h264.height = std::stoi(v);
        } catch (...) {}
      continue;
    }
    if (a.compare(0, 13, "--max-bitrate") == 0) {
      std::string v = getFlag("--max-bitrate", i);
      if (!v.empty())
        try {
          h264.max_bitrate = std::stoi(v);
        } catch (...) {}
      continue;
    }
    if (a.compare(0, 15, "--max-framerate") == 0) {
      std::string v = getFlag("--max-framerate", i);
      if (!v.empty())
        try {
          h264.max_framerate = std::stoi(v);
        } catch (...) {}
      continue;
    }
    if (a.compare(0, 5, "--url") == 0) {
      std::string v = getFlag("--url", i);
      if (!v.empty())
        url = v;
      continue;
    }
    if (a.compare(0, 7, "--token") == 0) {
      std::string v = getFlag("--token", i);
      if (!v.empty())
        token = v;
      continue;
    }
    if (a.compare(0, 10, "--e2ee_key") == 0) {
      std::string v = getFlag("--e2ee_key", i);
      if (!v.empty())
        e2ee_key = v;
    }
  }

  if (url.empty()) {
    const char *e = std::getenv("LIVEKIT_URL");
    if (e)
      url = e;
  }
  if (token.empty()) {
    const char *e = std::getenv("LIVEKIT_TOKEN");
    if (e)
      token = e;
  }
  if (e2ee_key.empty()) {
    const char *e = std::getenv("LIVEKIT_E2EE_KEY");
    if (e)
      e2ee_key = e;
  }
  return !(url.empty() || token.empty());
}

static std::vector<std::uint8_t> toBytes(const std::string &s) {
  return std::vector<std::uint8_t>(s.begin(), s.end());
}

class LoggingDelegate : public livekit::RoomDelegate {
public:
  void onParticipantConnected(livekit::Room &,
                               const livekit::ParticipantConnectedEvent &ev) override {
    std::cout << "[Room] participant connected: " << ev.participant->identity() << "\n";
  }
  void onTrackSubscribed(livekit::Room &,
                         const livekit::TrackSubscribedEvent &ev) override {
    std::cout << "[Room] track subscribed: "
              << (ev.publication ? ev.publication->name() : "?") << "\n";
  }
};

} // namespace

int main(int argc, char *argv[]) {
  std::string url, token, e2ee_key;
  bool enable_e2ee = false;
  H264TcpArgs h264;
  if (!parseArgs(argc, argv, url, token, enable_e2ee, e2ee_key, h264)) {
    printUsage(argv[0]);
    return 1;
  }
  if (url.empty() || token.empty()) {
    std::cerr << "LIVEKIT_URL and LIVEKIT_TOKEN (or --url/--token) are required\n";
    return 1;
  }

  std::signal(SIGINT, handleSignal);
  livekit::initialize(livekit::LogSink::kConsole);

  auto room = std::make_unique<livekit::Room>();
  LoggingDelegate delegate;
  room->setDelegate(&delegate);

  RoomOptions options;
  options.auto_subscribe = true;
  options.dynacast = false;
  if (enable_e2ee) {
    livekit::E2EEOptions enc;
    enc.encryption_type = livekit::EncryptionType::GCM;
    if (!e2ee_key.empty())
      enc.key_provider_options.shared_key = toBytes(e2ee_key);
    options.encryption = enc;
  }

  if (!room->Connect(url, token, options)) {
    std::cerr << "Failed to connect\n";
    livekit::shutdown();
    return 1;
  }
  std::cout << "Connected to room: " << room->room_info().name << "\n";

  const int width = h264.width;
  const int height = h264.height;

  auto encodedSource = std::make_shared<EncodedVideoSource>(width, height);
  auto videoTrack =
      LocalVideoTrack::createLocalVideoTrack("h264_tcp", encodedSource);
  TrackPublishOptions videoOpts;
  videoOpts.source = TrackSource::SOURCE_CAMERA;
  videoOpts.dtx = false;
  videoOpts.video_codec = VideoCodec::H264;
  videoOpts.simulcast = false;  // passthrough: single encoded layer only

  // Optional encoding constraints to guide WebRTC rate control.
  if (h264.max_bitrate > 0 || h264.max_framerate > 0) {
    VideoEncodingOptions enc;
    enc.max_bitrate = static_cast<std::uint64_t>(h264.max_bitrate);
    enc.max_framerate = static_cast<double>(h264.max_framerate);
    videoOpts.video_encoding = enc;
    std::cout << "Video encoding constraints: max_bitrate=" << enc.max_bitrate
              << " bps, max_framerate=" << enc.max_framerate << " fps\n";
  }

  std::shared_ptr<LocalTrackPublication> videoPub;
  try {
    videoPub = room->localParticipant()->publishTrack(videoTrack, videoOpts);
    std::cout << "Published video track: SID=" << videoPub->sid()
              << " name=" << videoPub->name() << "\n";
  } catch (const std::exception &e) {
    std::cerr << "Failed to publish track: " << e.what() << "\n";
    livekit::shutdown();
    return 1;
  }

  auto h264Source = std::make_unique<publish_h264::H264TcpSource>(
      h264.host, h264.port,
      [encodedSource](publish_h264::H264AccessUnit au) {
        bool ok = encodedSource->captureEncodedFrame(
            au.data, au.timestamp_us, au.is_keyframe);
        if (!ok || !encodedSource->captureSupported()) {
          std::cerr << "Encoded capture failed; stopping.\n";
          g_running.store(false);
        }
      },
      h264.framing);
  h264Source->start();

  while (g_running.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  h264Source->stop();
  room->setDelegate(nullptr);
  if (videoPub)
    room->localParticipant()->unpublishTrack(videoPub->sid());
  room.reset();
  livekit::shutdown();
  std::cout << "Exiting.\n";
  return 0;
}
