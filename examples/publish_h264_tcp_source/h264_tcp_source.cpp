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

#include "h264_tcp_source.h"

#include <chrono>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define INVALID_SOCKET_VALUE INVALID_SOCKET
#define close_socket closesocket
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int socket_t;
#define INVALID_SOCKET_VALUE (-1)
#define close_socket close
#endif

namespace publish_h264 {

namespace {

constexpr std::size_t kMaxNalSize = 4 * 1024 * 1024; // 4 MB
constexpr std::size_t kReadChunkSize = 64 * 1024;

// NAL types: 1 = non-IDR slice, 5 = IDR slice
static bool isVclNal(const std::uint8_t *data, std::size_t size) {
  if (size < 1)
    return false;
  std::uint8_t type = data[0] & 0x1f;
  return type == 1 || type == 5;
}
static bool isIdrNal(const std::uint8_t *data, std::size_t size) {
  if (size < 1)
    return false;
  return (data[0] & 0x1f) == 5;
}

struct StartCode {
  std::size_t offset;
  std::size_t size;
};

static bool findStartCode(const std::vector<std::uint8_t> &buf,
                          std::size_t start,
                          StartCode &out) {
  for (std::size_t i = start; i + 3 < buf.size(); ++i) {
    if (buf[i] == 0 && buf[i + 1] == 0) {
      if (buf[i + 2] == 1) {
        out = {i, 3};
        return true;
      }
      if (i + 4 < buf.size() && buf[i + 2] == 0 && buf[i + 3] == 1) {
        out = {i, 4};
        return true;
      }
    }
  }
  return false;
}

socket_t connectTcp(const std::string &host, std::uint16_t port) {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    return INVALID_SOCKET_VALUE;
#endif
  struct addrinfo hints = {}, *res = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  std::string portStr = std::to_string(port);
  if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
    std::cerr << "H264TcpSource: getaddrinfo failed for " << host << ":"
              << port << "\n";
    return INVALID_SOCKET_VALUE;
  }
  socket_t fd =
      socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (fd == INVALID_SOCKET_VALUE) {
    freeaddrinfo(res);
    return INVALID_SOCKET_VALUE;
  }
  if (connect(fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) != 0) {
    close_socket(fd);
    freeaddrinfo(res);
    return INVALID_SOCKET_VALUE;
  }
  freeaddrinfo(res);

  // Disable Nagle's algorithm to reduce TCP buffering latency.
  int flag = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char *>(&flag), sizeof(flag));

  return fd;
}

} // namespace

H264TcpSource::H264TcpSource(const std::string &host, std::uint16_t port,
                             H264AccessUnitCallback callback,
                             H264Framing framing)
    : host_(host),
      port_(port),
      callback_(std::move(callback)),
      framing_(framing) {}

H264TcpSource::~H264TcpSource() { stop(); }

void H264TcpSource::start() {
  if (running_.exchange(true))
    return;
  thread_ = std::thread(&H264TcpSource::loop, this);
}

void H264TcpSource::stop() {
  running_.store(false);
  if (thread_.joinable())
    thread_.join();
}

void H264TcpSource::loop() {
  socket_t fd = connectTcp(host_, port_);
  if (fd == INVALID_SOCKET_VALUE) {
    std::cerr << "H264TcpSource: failed to connect to " << host_ << ":"
              << port_ << "\n";
    running_.store(false);
    return;
  }

  if (framing_ == H264Framing::AnnexB) {
    std::cout << "H264TcpSource: connected to " << host_ << ":" << port_
              << " (Annex-B byte-stream)\n";
  } else {
    std::cout << "H264TcpSource: connected to " << host_ << ":" << port_
              << " (length-prefixed AVC)\n";
  }

  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::uint8_t> access_unit;
  bool has_idr = false;

  auto emit_access_unit = [&](const std::uint8_t *nal_data,
                              std::size_t nal_size,
                              bool is_vcl) {
    static const std::uint8_t kStartCode[] = {0, 0, 0, 1};
    access_unit.insert(access_unit.end(), kStartCode, kStartCode + 4);
    access_unit.insert(access_unit.end(), nal_data, nal_data + nal_size);
    if (isIdrNal(nal_data, nal_size))
      has_idr = true;

    if (!is_vcl)
      return;

    std::int64_t ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    H264AccessUnit au;
    au.data = std::move(access_unit);
    au.timestamp_us = ts_us;
    au.is_keyframe = has_idr;
    access_unit.clear();
    has_idr = false;
    if (callback_)
      callback_(std::move(au));
  };

  if (framing_ == H264Framing::AnnexB) {
    std::vector<std::uint8_t> stream_buf;
    stream_buf.reserve(kReadChunkSize * 2);

    while (running_.load()) {
      std::uint8_t chunk[kReadChunkSize];
#ifdef _WIN32
      int n = recv(fd, reinterpret_cast<char *>(chunk),
                   static_cast<int>(kReadChunkSize), 0);
#else
      ssize_t n = recv(fd, chunk, kReadChunkSize, 0);
#endif
      if (n <= 0) {
        running_.store(false);
        break;
      }
      stream_buf.insert(stream_buf.end(), chunk, chunk + n);

      std::size_t parse_pos = 0;
      StartCode sc;
      while (findStartCode(stream_buf, parse_pos, sc)) {
        if (parse_pos == 0 && sc.offset > 0) {
          // Drop any leading bytes before the first start code.
          stream_buf.erase(stream_buf.begin(), stream_buf.begin() + sc.offset);
          parse_pos = 0;
          continue;
        }
        StartCode next_sc;
        std::size_t next_start = sc.offset + sc.size;
        if (!findStartCode(stream_buf, next_start, next_sc)) {
          // Need more data for the next NAL.
          break;
        }

        std::size_t nal_start = sc.offset + sc.size;
        std::size_t nal_size = next_sc.offset - nal_start;
        if (nal_size > 0) {
          const std::uint8_t *nal_data = stream_buf.data() + nal_start;
          bool is_vcl = isVclNal(nal_data, nal_size);
          emit_access_unit(nal_data, nal_size, is_vcl);
        }
        parse_pos = next_sc.offset;
      }

      if (parse_pos > 0) {
        stream_buf.erase(stream_buf.begin(), stream_buf.begin() + parse_pos);
      }
    }
  } else {
    while (running_.load()) {
      // Read 4-byte big-endian length
      std::uint8_t lenBuf[4];
      std::size_t filled = 0;
      while (filled < 4 && running_.load()) {
#ifdef _WIN32
        int n = recv(fd, reinterpret_cast<char *>(lenBuf + filled),
                     static_cast<int>(4 - filled), 0);
#else
        ssize_t n = recv(fd, lenBuf + filled, 4 - filled, 0);
#endif
        if (n <= 0) {
          running_.store(false);
          break;
        }
        filled += static_cast<std::size_t>(n);
      }
      if (!running_.load() || filled < 4)
        break;

      std::uint32_t nalLen = (static_cast<std::uint32_t>(lenBuf[0]) << 24) |
                             (static_cast<std::uint32_t>(lenBuf[1]) << 16) |
                             (static_cast<std::uint32_t>(lenBuf[2]) << 8) |
                             static_cast<std::uint32_t>(lenBuf[3]);
      if (nalLen == 0 || nalLen > kMaxNalSize) {
        std::cerr << "H264TcpSource: invalid NAL length " << nalLen << "\n";
        running_.store(false);
        break;
      }

      std::vector<std::uint8_t> nal(nalLen);
      filled = 0;
      while (filled < nalLen && running_.load()) {
#ifdef _WIN32
        int n = recv(fd, reinterpret_cast<char *>(nal.data() + filled),
                     static_cast<int>(nalLen - filled), 0);
#else
        ssize_t n = recv(fd, nal.data() + filled, nalLen - filled, 0);
#endif
        if (n <= 0) {
          running_.store(false);
          break;
        }
        filled += static_cast<std::size_t>(n);
      }
      if (!running_.load() || filled < nalLen)
        break;

      bool is_vcl = isVclNal(nal.data(), nal.size());
      emit_access_unit(nal.data(), nal.size(), is_vcl);
    }
  }

  close_socket(fd);
  running_.store(false);
}

} // namespace publish_h264
