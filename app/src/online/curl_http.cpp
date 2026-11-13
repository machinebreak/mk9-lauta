// mk9-lauta (2026-11-13): curl_http.h, the Linux build's HTTP(S) and WebSockets.

#include "online/curl_http.h"

#include <dlfcn.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <random>

#include <curl/curl.h>

#include <rex/logging.h>

namespace mk9::net {
namespace {

// libcurl's entry points, from the library loaded at run time (the build links no libcurl: a player without it still
// plays, offline).
struct Curl {
  bool ready = false;
  std::string why;
  CURLcode (*global_init)(long) = nullptr;
  CURL* (*easy_init)() = nullptr;
  CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
  CURLcode (*easy_perform)(CURL*) = nullptr;
  void (*easy_cleanup)(CURL*) = nullptr;
  CURLcode (*easy_getinfo)(CURL*, CURLINFO, ...) = nullptr;
  const char* (*easy_strerror)(CURLcode) = nullptr;
  CURLcode (*easy_send)(CURL*, const void*, size_t, size_t*) = nullptr;
  CURLcode (*easy_recv)(CURL*, void*, size_t, size_t*) = nullptr;
  curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
  void (*slist_free_all)(curl_slist*) = nullptr;
  curl_version_info_data* (*version_info)(CURLversion) = nullptr;
};

Curl& Lib() {
  static Curl curl = [] {
    Curl c;
    void* lib = nullptr;
    for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"}) {
      if ((lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)) != nullptr) break;
    }
    if (!lib) {
      c.why = "libcurl is not installed (libcurl.so.4): the online needs it";
      REXLOG_ERROR("[net] {}", c.why);
      return c;
    }
    auto get = [&](auto& fn, const char* symbol) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, symbol));
      return fn != nullptr;
    };
    const bool all = get(c.global_init, "curl_global_init") && get(c.easy_init, "curl_easy_init") &&
                     get(c.easy_setopt, "curl_easy_setopt") && get(c.easy_perform, "curl_easy_perform") &&
                     get(c.easy_cleanup, "curl_easy_cleanup") && get(c.easy_getinfo, "curl_easy_getinfo") &&
                     get(c.easy_strerror, "curl_easy_strerror") && get(c.easy_send, "curl_easy_send") &&
                     get(c.easy_recv, "curl_easy_recv") && get(c.slist_append, "curl_slist_append") &&
                     get(c.slist_free_all, "curl_slist_free_all") && get(c.version_info, "curl_version_info");
    if (!all) {
      c.why = "this libcurl lacks a function the game uses";
      REXLOG_ERROR("[net] {}", c.why);
      return c;
    }
    if (c.global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      c.why = "libcurl did not start";
      REXLOG_ERROR("[net] {}", c.why);
      return c;
    }
    const curl_version_info_data* v = c.version_info(CURLVERSION_NOW);
    REXLOG_INFO("[net] libcurl {} ({})", v && v->version ? v->version : "?",
                v && v->ssl_version ? v->ssl_version : "no TLS");
    c.ready = true;
    return c;
  }();
  return curl;
}

// What a failure was, in lobby.cpp's words (its screens explain each one).
std::string Kind(CURLcode code, long status) {
  switch (code) {
    case CURLE_OK: break;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY: return "dns";
    case CURLE_COULDNT_CONNECT:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING: return "connect";
    case CURLE_OPERATION_TIMEDOUT: return "timeout";
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_ENGINE_INITFAILED:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS: return "tls";
    default: return "other";
  }
  if (status && (status < 200 || status >= 300) && status != 101) return "http";
  return "";
}

struct Parsed {
  bool ok = false, secure = false;
  std::string host, path;  // path with its query
  long port = 0;
};

// scheme://host[:port][/path?query], http(s) or ws(s)
Parsed ParseUrl(const std::string& url) {
  Parsed p;
  const size_t colon = url.find("://");
  if (colon == std::string::npos) return p;
  const std::string scheme = url.substr(0, colon);
  if (scheme == "https" || scheme == "wss") {
    p.secure = true;
  } else if (scheme != "http" && scheme != "ws") {
    return p;
  }
  const size_t host_at = colon + 3;
  size_t path_at = url.find_first_of("/?", host_at);
  if (path_at == std::string::npos) path_at = url.size();
  std::string authority = url.substr(host_at, path_at - host_at);
  p.path = path_at < url.size() ? url.substr(path_at) : "/";
  if (p.path[0] == '?') p.path = "/" + p.path;
  p.port = p.secure ? 443 : 80;
  if (const size_t c = authority.rfind(':'); c != std::string::npos && authority.find(']') == std::string::npos) {
    p.port = std::strtol(authority.c_str() + c + 1, nullptr, 10);
    authority.resize(c);
  }
  p.host = authority;
  p.ok = !p.host.empty() && p.port > 0 && p.port < 65536;
  return p;
}

// The common options of a request; *resolve: the list given to CURLOPT_RESOLVE (the caller frees it).
void Setup(Curl& c, CURL* h, const std::string& url, const HttpOptions& o, curl_slist** resolve,
           curl_slist** headers) {
  c.easy_setopt(h, CURLOPT_URL, url.c_str());
  c.easy_setopt(h, CURLOPT_NOSIGNAL, 1L);  // the game's threads: no SIGALRM for the timeouts
  c.easy_setopt(h, CURLOPT_USERAGENT, "mk9-online/1.0");
  c.easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, o.connect_timeout_ms);
  if (o.timeout_ms > 0) {
    c.easy_setopt(h, CURLOPT_TIMEOUT_MS, o.timeout_ms);
  } else {
    // a long download: no whole-transfer limit, but one that stalls for 30 s ends
    c.easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    c.easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
  }
  c.easy_setopt(h, CURLOPT_FOLLOWLOCATION, o.follow_redirects ? 1L : 0L);
  c.easy_setopt(h, CURLOPT_MAXREDIRS, 8L);
  c.easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // any encoding libcurl decodes
  if (!o.resolve_address.empty()) {
    const Parsed p = ParseUrl(url);
    const std::string entry = p.host + ":" + std::to_string(p.port) + ":" + o.resolve_address;
    *resolve = c.slist_append(*resolve, entry.c_str());
    c.easy_setopt(h, CURLOPT_RESOLVE, *resolve);
  }
  for (const std::string& header : o.headers) *headers = c.slist_append(*headers, header.c_str());
  if (*headers) c.easy_setopt(h, CURLOPT_HTTPHEADER, *headers);
  if (o.range_from) {
    const std::string range = std::to_string(o.range_from) + "-";
    c.easy_setopt(h, CURLOPT_RANGE, range.c_str());  // libcurl copies it
  }
}

struct SinkState {
  const std::function<bool(const char*, size_t)>* sink;
  CURL* handle;
  Curl* curl;
  HttpResult* result;
  bool sized = false;
  bool stopped = false;
};

size_t WriteToSink(char* data, size_t size, size_t count, void* user) {
  auto* s = static_cast<SinkState*>(user);
  if (!s->sized) {
    s->sized = true;
    curl_off_t length = -1;
    if (s->curl->easy_getinfo(s->handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length) == CURLE_OK && length > 0) {
      s->result->total = uint64_t(length);
    }
    long status = 0;
    s->curl->easy_getinfo(s->handle, CURLINFO_RESPONSE_CODE, &status);
    // an error page is not the body asked for: not given to the sink
    if (status && (status < 200 || status >= 300)) {
      s->stopped = true;
      return 0;
    }
  }
  if (!(*s->sink)(data, size * count)) {
    s->stopped = true;
    return 0;
  }
  return size * count;
}

std::string Base64(const uint8_t* data, size_t size) {
  static const char* kDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t v = uint32_t(data[i]) << 16 | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0) |
                       (i + 2 < size ? uint32_t(data[i + 2]) : 0);
    out += kDigits[(v >> 18) & 63];
    out += kDigits[(v >> 12) & 63];
    out += i + 1 < size ? kDigits[(v >> 6) & 63] : '=';
    out += i + 2 < size ? kDigits[v & 63] : '=';
  }
  return out;
}

std::string Lower(std::string s) {
  for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

}  // namespace

bool CurlReady(std::string* why) {
  Curl& c = Lib();
  if (!c.ready && why) *why = c.why;
  return c.ready;
}

HttpResult HttpGet(const std::string& url, const HttpOptions& options,
                   const std::function<bool(const char*, size_t)>& sink) {
  HttpResult r;
  Curl& c = Lib();
  if (!c.ready) {
    r.kind = "other";
    r.text = c.why;
    return r;
  }
  CURL* h = c.easy_init();
  if (!h) {
    r.kind = "other";
    r.text = "curl_easy_init";
    return r;
  }
  curl_slist* resolve = nullptr;
  curl_slist* headers = nullptr;
  Setup(c, h, url, options, &resolve, &headers);
  SinkState state{&sink, h, &c, &r};
  c.easy_setopt(h, CURLOPT_WRITEFUNCTION, &WriteToSink);
  c.easy_setopt(h, CURLOPT_WRITEDATA, &state);
  const CURLcode code = c.easy_perform(h);
  c.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.status);
  c.easy_cleanup(h);
  if (resolve) c.slist_free_all(resolve);
  if (headers) c.slist_free_all(headers);
  r.code = state.stopped && code == CURLE_WRITE_ERROR ? 0 : int(code);
  r.kind = Kind(state.stopped ? CURLE_OK : code, r.status);
  r.text = code == CURLE_OK ? std::string() : c.easy_strerror(code);
  r.ok = code == CURLE_OK && r.status >= 200 && r.status < 300 && !state.stopped;
  if (state.stopped && r.kind.empty()) r.kind = r.status >= 200 && r.status < 300 ? "other" : "http";
  return r;
}

HttpResult HttpGetText(const std::string& url, const HttpOptions& options, std::string* body, size_t max_bytes) {
  body->clear();
  return HttpGet(url, options, [&](const char* data, size_t size) {
    if (body->size() + size > max_bytes) return false;
    body->append(data, size);
    return true;
  });
}

// --- WebSocket ------------------------------------------------------------------------------------------------------

std::shared_ptr<WebSocket> WebSocket::Connect(const std::string& url, const HttpOptions& options,
                                              HttpResult* result) {
  HttpResult r;
  Curl& c = Lib();
  if (!c.ready) {
    r.kind = "other";
    r.text = c.why;
    if (result) *result = r;
    return nullptr;
  }
  const Parsed p = ParseUrl(url);
  if (!p.ok) {
    r.kind = "other";
    r.text = "not a ws:// or wss:// URL";
    if (result) *result = r;
    return nullptr;
  }
  // libcurl makes the (TLS) connection and stops there; the upgrade and the frames are this class's
  const std::string connect_url = std::string(p.secure ? "https://" : "http://") + p.host + ":" +
                                  std::to_string(p.port) + p.path;
  CURL* h = c.easy_init();
  if (!h) {
    r.kind = "other";
    r.text = "curl_easy_init";
    if (result) *result = r;
    return nullptr;
  }
  curl_slist* resolve = nullptr;
  curl_slist* headers = nullptr;
  HttpOptions o = options;
  o.headers.clear();
  Setup(c, h, connect_url, o, &resolve, &headers);
  c.easy_setopt(h, CURLOPT_TIMEOUT_MS, 0L);  // the link lasts; the handshake below has its own limit
  c.easy_setopt(h, CURLOPT_CONNECT_ONLY, 1L);
  const CURLcode code = c.easy_perform(h);
  if (resolve) c.slist_free_all(resolve);
  if (headers) c.slist_free_all(headers);
  if (code != CURLE_OK) {
    r.code = int(code);
    r.kind = Kind(code, 0);
    r.text = c.easy_strerror(code);
    c.easy_cleanup(h);
    if (result) *result = r;
    return nullptr;
  }
  std::shared_ptr<WebSocket> ws(new WebSocket());
  ws->curl_ = h;
  curl_socket_t socket = CURL_SOCKET_BAD;
  c.easy_getinfo(h, CURLINFO_ACTIVESOCKET, &socket);
  ws->socket_ = int(socket);

  // the upgrade
  std::random_device random;
  uint8_t nonce[16];
  for (uint8_t& b : nonce) b = uint8_t(random());
  const std::string key = Base64(nonce, sizeof(nonce));
  const bool default_port = p.port == (p.secure ? 443 : 80);
  std::string request = "GET " + p.path + " HTTP/1.1\r\nHost: " + p.host +
                        (default_port ? std::string() : ":" + std::to_string(p.port)) +
                        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                        "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: mk9-online/1.0\r\n\r\n";
  if (!ws->SendRaw(reinterpret_cast<const uint8_t*>(request.data()), request.size())) {
    r.kind = "connect";
    r.text = "the upgrade request was not sent";
    if (result) *result = r;
    return nullptr;
  }
  // the answer's head (up to the empty line; what comes after it is already the first frames)
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(o.connect_timeout_ms);
  size_t end = std::string::npos;
  for (;;) {
    const std::string seen(ws->rx_.begin(), ws->rx_.end());
    end = seen.find("\r\n\r\n");
    if (end != std::string::npos) break;
    if (ws->rx_.size() > 16384 || std::chrono::steady_clock::now() > deadline || !ws->Fill()) {
      r.kind = std::chrono::steady_clock::now() > deadline ? "timeout" : "connect";
      r.text = "no answer to the upgrade";
      if (result) *result = r;
      return nullptr;
    }
  }
  const std::string head = Lower(std::string(ws->rx_.begin(), ws->rx_.begin() + long(end)));
  ws->rx_.erase(ws->rx_.begin(), ws->rx_.begin() + long(end + 4));
  // "http/1.1 101 switching protocols"
  const size_t space = head.find(' ');
  r.status = space != std::string::npos ? std::strtol(head.c_str() + space + 1, nullptr, 10) : 0;
  if (r.status != 101 || head.find("upgrade: websocket") == std::string::npos) {
    r.kind = "http";
    r.text = "the server did not take the WebSocket (" + head.substr(0, head.find('\r')) + ")";
    if (result) *result = r;
    return nullptr;
  }
  r.ok = true;
  if (result) *result = r;
  return ws;
}

WebSocket::~WebSocket() {
  if (curl_) Lib().easy_cleanup(static_cast<CURL*>(curl_));
}

void WebSocket::Abort() {
  aborted_.store(true);
  // the socket shut down under libcurl: a poll waiting on it returns at once (the handle is cleaned up later)
  if (socket_ >= 0) ::shutdown(socket_, SHUT_RDWR);
}

bool WebSocket::SendRaw(const uint8_t* data, size_t size) {
  Curl& c = Lib();
  size_t done = 0;
  while (done < size) {
    if (aborted_.load()) return false;
    size_t sent = 0;
    CURLcode code;
    {
      std::lock_guard<std::mutex> lock(io_);
      code = c.easy_send(static_cast<CURL*>(curl_), data + done, size - done, &sent);
    }
    if (code == CURLE_AGAIN) {
      pollfd pfd{socket_, POLLOUT, 0};
      ::poll(&pfd, 1, 200);
      continue;
    }
    if (code != CURLE_OK) return false;
    done += sent;
  }
  return true;
}

bool WebSocket::Fill() {
  Curl& c = Lib();
  uint8_t buffer[16384];
  for (;;) {
    if (aborted_.load()) return false;
    size_t got = 0;
    CURLcode code;
    {
      std::lock_guard<std::mutex> lock(io_);
      code = c.easy_recv(static_cast<CURL*>(curl_), buffer, sizeof(buffer), &got);
    }
    if (code == CURLE_AGAIN) {
      pollfd pfd{socket_, POLLIN, 0};
      ::poll(&pfd, 1, 200);
      continue;
    }
    if (code != CURLE_OK || got == 0) return false;  // an error, or the server closed the connection
    rx_.insert(rx_.end(), buffer, buffer + got);
    return true;
  }
}

bool WebSocket::SendFrame(uint8_t opcode, const uint8_t* payload, size_t size) {
  std::vector<uint8_t> frame;
  frame.reserve(size + 14);
  frame.push_back(uint8_t(0x80 | opcode));  // FIN: one frame
  if (size < 126) {
    frame.push_back(uint8_t(0x80 | size));
  } else if (size < 65536) {
    frame.push_back(0x80 | 126);
    frame.push_back(uint8_t(size >> 8));
    frame.push_back(uint8_t(size));
  } else {
    frame.push_back(0x80 | 127);
    for (int i = 7; i >= 0; --i) frame.push_back(uint8_t(uint64_t(size) >> (8 * i)));
  }
  static thread_local std::mt19937 random{std::random_device{}()};
  uint8_t mask[4];
  for (uint8_t& b : mask) b = uint8_t(random());
  frame.insert(frame.end(), mask, mask + 4);
  for (size_t i = 0; i < size; ++i) frame.push_back(uint8_t(payload[i] ^ mask[i & 3]));
  std::lock_guard<std::mutex> lock(send_);
  return SendRaw(frame.data(), frame.size());
}

bool WebSocket::SendText(const std::string& text) {
  return SendFrame(0x1, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool WebSocket::Receive(std::string* message, size_t max_bytes) {
  message->clear();
  bool started = false;
  for (;;) {
    // a whole frame's head and payload in rx_
    while (rx_.size() < 2) {
      if (!Fill()) return false;
    }
    const uint8_t b0 = rx_[0], b1 = rx_[1];
    const bool fin = (b0 & 0x80) != 0;
    const uint8_t opcode = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t length = b1 & 0x7F;
    size_t at = 2;
    if (length == 126 || length == 127) {
      const size_t extra = length == 126 ? 2 : 8;
      while (rx_.size() < at + extra) {
        if (!Fill()) return false;
      }
      length = 0;
      for (size_t i = 0; i < extra; ++i) length = length << 8 | rx_[at + i];
      at += extra;
    }
    if (length > max_bytes) return false;
    uint8_t mask[4] = {};
    if (masked) {
      while (rx_.size() < at + 4) {
        if (!Fill()) return false;
      }
      std::memcpy(mask, &rx_[at], 4);
      at += 4;
    }
    while (rx_.size() < at + length) {
      if (!Fill()) return false;
    }
    std::vector<uint8_t> payload(rx_.begin() + long(at), rx_.begin() + long(at + length));
    rx_.erase(rx_.begin(), rx_.begin() + long(at + length));
    if (masked) {
      for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];
    }
    switch (opcode) {
      case 0x8:  // close: answered, and the link is over
        SendFrame(0x8, payload.data(), std::min<size_t>(payload.size(), 2));
        return false;
      case 0x9:  // ping
        if (!SendFrame(0xA, payload.data(), payload.size())) return false;
        continue;
      case 0xA:  // pong
        continue;
      case 0x1:
      case 0x2:
        message->assign(payload.begin(), payload.end());
        started = true;
        break;
      case 0x0:  // continuation
        if (!started) return false;
        message->append(payload.begin(), payload.end());
        break;
      default:
        return false;
    }
    if (message->size() > max_bytes) return false;
    if (fin) return true;
  }
}

}  // namespace mk9::net
