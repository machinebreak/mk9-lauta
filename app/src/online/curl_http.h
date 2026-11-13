// mk9-lauta (2026-11-13): HTTP(S) and WebSockets on Linux, with the system's libcurl (the Windows build uses WinHTTP).
//
// libcurl is loaded at run time (libcurl.so.4, or Debian's libcurl-gnutls.so.4): every desktop distribution and
// SteamOS have it, its TLS and certificates are the system's, and without it the game still runs (no online: the log
// and the screens say so). The WebSocket is this file's own over libcurl's TLS connection (CONNECT_ONLY), so any
// libcurl does, not only one built with its WebSocket support. "By address" (the lobby's: some providers' DNS does not
// know its name) is CURLOPT_RESOLVE: the name's address given, the name kept for TLS (SNI, the certificate) and Host.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mk9::net {

// libcurl is there (loaded once); why not otherwise.
bool CurlReady(std::string* why = nullptr);

struct HttpOptions {
  std::string resolve_address;  // connect to this address with the URL's name kept ("" = ask the DNS)
  long timeout_ms = 15000;      // the whole transfer (0: none; a download then ends when it stalls 30 s)
  long connect_timeout_ms = 8000;
  uint64_t range_from = 0;      // a download that goes on from this byte
  std::vector<std::string> headers;
  bool follow_redirects = true;
};

struct HttpResult {
  bool ok = false;     // an answer of 2xx (or 101 for a WebSocket), its body all given
  long status = 0;     // the HTTP status
  int code = 0;        // libcurl's error (0: none)
  std::string kind;    // what failed, as lobby.cpp says it: dns, connect, timeout, tls, http, other ("" when ok)
  std::string text;    // libcurl's words for it
  uint64_t total = 0;  // the body's length when the server said it (with range_from: the part after it)
};

// GET: the body to `sink` as it comes (false from it stops the transfer).
HttpResult HttpGet(const std::string& url, const HttpOptions& options,
                   const std::function<bool(const char*, size_t)>& sink);
// GET of a small text: the body, up to max_bytes.
HttpResult HttpGetText(const std::string& url, const HttpOptions& options, std::string* body,
                       size_t max_bytes = 4u << 20);

// A WebSocket (ws:// or wss://). Receive and SendText may run on two threads at once; Abort from any thread.
class WebSocket {
 public:
  static std::shared_ptr<WebSocket> Connect(const std::string& url, const HttpOptions& options, HttpResult* result);
  ~WebSocket();
  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;

  // One text message (a single frame, masked as a client's must be).
  bool SendText(const std::string& text);
  // The next whole message (text or binary; fragments joined; the server's pings answered): false once the link
  // closed, failed or was aborted.
  bool Receive(std::string* message, size_t max_bytes = 256 * 1024);
  // Ends it: a Receive waiting returns false at once.
  void Abort();

 private:
  WebSocket() = default;
  bool SendRaw(const uint8_t* data, size_t size);
  bool SendFrame(uint8_t opcode, const uint8_t* payload, size_t size);
  // More bytes into rx_ (waits for them): false when the link is gone.
  bool Fill();

  void* curl_ = nullptr;   // CURL*
  int socket_ = -1;
  std::mutex io_;           // libcurl's handle: one call at a time (receiving and sending threads)
  std::mutex send_;         // one frame at a time, whole (the receiving thread answers pings too)
  std::atomic<bool> aborted_{false};
  std::vector<uint8_t> rx_;  // received bytes not read yet
};

}  // namespace mk9::net
