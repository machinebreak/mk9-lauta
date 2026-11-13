// MK9 online: the two players' own path (ice_link.h).

#include "online/ice_link.h"

#include <juice/juice.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "online/lobby.h"

REXCVAR_DEFINE_BOOL(mk9_ice_relay_only, false, "MK9 Online", "Tests: the players' path only through the TURN relay");

namespace mk9::net {

using json = nlohmann::json;

namespace {

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool IsRelay(const std::string& candidate) { return candidate.find(" typ relay") != std::string::npos; }

void LogFromJuice(juice_log_level_t level, const char* message) {
  if (level >= JUICE_LOG_LEVEL_ERROR) {
    REXLOG_ERROR("[ice] {}", message);
  } else if (level >= JUICE_LOG_LEVEL_WARN) {
    REXLOG_WARN("[ice] {}", message);
  }
}

}  // namespace

struct IceLink::Impl {
  std::string room, id;  // id: the guest's (on the host's side, the one whose offer it answered)
  bool host = false;
  bool relay_only = false;
  // the servers, kept alive for libjuice's pointers
  std::string stun_host;
  uint16_t stun_port = 0;
  std::vector<std::string> turn_strings;
  std::vector<juice_turn_server_t> turn;

  // Three locks, so libjuice's callbacks (on its own thread, its agent locked) never wait for one held while calling it:
  std::mutex mutex;        // the agent and the handshake (held while calling libjuice; the callbacks never take it)
  juice_agent_t* agent = nullptr;
  bool remote_set = false;
  std::vector<std::string> early;  // the peer's candidates before its description
  std::string local_sdp;
  uint64_t offered_at = 0;
  bool answered = false;
  std::mutex sig_mutex;    // who the relayed messages go to (the callbacks send candidates)
  uint64_t session = 0;    // the exchange's number (the guest's offer picks it): a stale one is ignored
  std::mutex inbox_mutex;  // the datagrams in
  std::condition_variable readable;
  struct Datagram {
    uint64_t arrived_us = 0;  // libjuice's thread took it (LastArrivalUs)
    std::vector<uint8_t> bytes;
  };
  std::deque<Datagram> inbox;
  uint64_t last_arrival_us = 0;  // of the datagram Receive returned last (the game thread's)

  std::atomic<int> state{JUICE_STATE_DISCONNECTED};
  std::atomic<bool> failed{false}, stopping{false};
  std::thread pump;

  // Relays data with the exchange's number to the other player.
  void SendSig(json data) {
    std::string to;
    {
      std::lock_guard<std::mutex> lock(sig_mutex);
      data["s"] = session;
      to = id;
    }
    if (host) {
      if (!to.empty()) lobby::SigToGuest(room, to, data);
    } else {
      lobby::SigToHost(room, data);
    }
  }

  uint64_t Session() {
    std::lock_guard<std::mutex> lock(sig_mutex);
    return session;
  }

  // A new agent for this exchange (the mutex held). remote: the offer it answers (the host's side), set before the
  // local description is made: libjuice makes an agent whose own description comes first the controlling one, so a
  // host that made its answer first was controlling too ("ICE role conflict (both controlling)" in every session that
  // froze at its start, 2026-11-10); answering after the offer, it is the controlled one, as ICE wants.
  bool NewAgent(const std::string* remote = nullptr) {
    if (agent) juice_destroy(agent);
    agent = nullptr;
    remote_set = false;
    early.clear();
    {
      std::lock_guard<std::mutex> lock(inbox_mutex);
      inbox.clear();
    }
    state = JUICE_STATE_DISCONNECTED;
    juice_config_t config{};
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    config.stun_server_host = stun_host.empty() ? nullptr : stun_host.c_str();
    config.stun_server_port = stun_port;
    config.turn_servers = turn.empty() ? nullptr : turn.data();
    config.turn_servers_count = int(turn.size());
    config.cb_state_changed = [](juice_agent_t*, juice_state_t s, void* user) {
      auto* self = static_cast<Impl*>(user);
      self->state = int(s);
      REXLOG_INFO("[ice] room {}: {}", self->room, juice_state_to_string(s));
      if (s == JUICE_STATE_FAILED) self->failed = true;
      self->readable.notify_all();
    };
    config.cb_candidate = [](juice_agent_t*, const char* sdp, void* user) {
      auto* self = static_cast<Impl*>(user);
      if (self->relay_only && !IsRelay(sdp)) return;
      self->SendSig({{"ice", "cand"}, {"c", sdp}});
    };
    config.cb_gathering_done = [](juice_agent_t*, void* user) {
      auto* self = static_cast<Impl*>(user);
      self->SendSig({{"ice", "done"}});
    };
    config.cb_recv = [](juice_agent_t*, const char* data, size_t size, void* user) {
      auto* self = static_cast<Impl*>(user);
      {
        std::lock_guard<std::mutex> lock(self->inbox_mutex);
        if (self->inbox.size() < 4096) {
          self->inbox.push_back({NowUs(), std::vector<uint8_t>(data, data + size)});
        }
      }
      self->readable.notify_all();
    };
    config.user_ptr = this;
    agent = juice_create(&config);
    if (!agent) return false;
    if (remote) SetRemote(*remote);
    char sdp[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(agent, sdp, sizeof(sdp)) != JUICE_ERR_SUCCESS) return false;
    local_sdp = sdp;
    return true;
  }

  void AddRemoteCandidate(const std::string& candidate) {
    if (relay_only && !IsRelay(candidate)) return;
    if (!remote_set) {
      early.push_back(candidate);
      return;
    }
    juice_add_remote_candidate(agent, candidate.c_str());
  }

  void SetRemote(const std::string& sdp) {
    juice_set_remote_description(agent, sdp.c_str());
    remote_set = true;
    for (const std::string& c : early) juice_add_remote_candidate(agent, c.c_str());
    early.clear();
  }

  void OnSig(const json& m) {
    const std::string kind = m.value("ice", "");
    const uint64_t s = m.value("s", uint64_t(0));
    std::lock_guard<std::mutex> lock(mutex);
    if (host && kind == "offer") {
      const std::string from = m.value("from", "");
      if (s == Session() && agent) {
        SendSig({{"ice", "answer"}, {"sdp", local_sdp}});  // the guest missed the answer
        return;
      }
      // a new exchange: the guest (re)started
      {
        std::lock_guard<std::mutex> sig(sig_mutex);
        id = from;
        session = s;
      }
      const std::string offer = m.value("sdp", "");
      if (!NewAgent(&offer)) {
        failed = true;
        return;
      }
      SendSig({{"ice", "answer"}, {"sdp", local_sdp}});
      juice_gather_candidates(agent);
      REXLOG_INFO("[ice] room {}: the guest {} offered; answered", room, from);
      return;
    }
    if (s != Session() || !agent) return;  // another exchange's
    if (!host && kind == "answer" && !answered) {
      answered = true;
      SetRemote(m.value("sdp", ""));
      juice_gather_candidates(agent);
      REXLOG_INFO("[ice] room {}: the host answered", room);
    } else if (kind == "cand") {
      AddRemoteCandidate(m.value("c", ""));
    } else if (kind == "done") {
      if (remote_set) juice_set_remote_gathering_done(agent);
    }
  }

  // The handshake's thread: the relayed messages in, the guest's offer again until it is answered.
  void Pump() {
    while (!stopping) {
      lobby::Event event;
      while (lobby::PollIce(&event)) {
        if (event.room == room && event.data.is_object()) OnSig(event.data);
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!host && agent && !answered && NowUs() - offered_at > 1000000) {
          offered_at = NowUs();
          SendSig({{"ice", "offer"}, {"from", id}, {"sdp", local_sdp}});
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  // STUN and TURN from the lobby's /ice (the WebRTC form): libjuice takes one STUN server and TURN over UDP.
  void TakeServers(const json& servers) {
    for (const json& server : servers) {
      if (!server.is_object()) continue;
      std::vector<std::string> urls;
      const json& u = server.value("urls", json());
      if (u.is_string()) urls.push_back(u.get<std::string>());
      if (u.is_array()) {
        for (const json& x : u) {
          if (x.is_string()) urls.push_back(x.get<std::string>());
        }
      }
      for (const std::string& url : urls) {
        const bool stun = url.rfind("stun:", 0) == 0, turn_url = url.rfind("turn:", 0) == 0;
        if (!stun && !turn_url) continue;  // turns: (TLS) is not libjuice's
        if (turn_url && url.find("transport=") != std::string::npos && url.find("transport=udp") == std::string::npos)
          continue;
        std::string rest = url.substr(5);
        rest = rest.substr(0, rest.find('?'));
        const size_t colon = rest.rfind(':');
        const std::string address = colon == std::string::npos ? rest : rest.substr(0, colon);
        const uint16_t port = uint16_t(colon == std::string::npos ? 3478 : std::atoi(rest.c_str() + colon + 1));
        if (stun && stun_host.empty()) {
          stun_host = address;
          stun_port = port;
        } else if (turn_url && turn.size() < 4) {
          turn_strings.push_back(address);
          turn_strings.push_back(server.value("username", ""));
          turn_strings.push_back(server.value("credential", ""));
          turn.push_back({nullptr, nullptr, nullptr, port});
        }
      }
    }
    // the strings stay put now: point at them
    for (size_t i = 0; i < turn.size(); ++i) {
      turn[i].host = turn_strings[i * 3].c_str();
      turn[i].username = turn_strings[i * 3 + 1].c_str();
      turn[i].password = turn_strings[i * 3 + 2].c_str();
    }
  }
};

std::unique_ptr<IceLink> IceLink::Create(const std::string& room, bool host, const std::string& id) {
  static const bool logging = [] {
    juice_set_log_handler(LogFromJuice);
    juice_set_log_level(JUICE_LOG_LEVEL_WARN);
    return true;
  }();
  (void)logging;
  auto impl = std::make_unique<Impl>();
  impl->room = room;
  impl->host = host;
  impl->id = id;
  impl->relay_only = REXCVAR_GET(mk9_ice_relay_only);
  impl->TakeServers(lobby::IceServers());
  if (impl->turn.empty()) {
    REXLOG_WARN("[ice] room {} as the {}: STUN {}, no TURN server: behind some providers' networks the two games "
                "will not find each other", room, host ? "host" : "guest",
                impl->stun_host.empty() ? "none" : impl->stun_host);
  } else {
    REXLOG_INFO("[ice] room {} as the {}: STUN {}, {} TURN server(s){}", room, host ? "host" : "guest",
                impl->stun_host.empty() ? "none" : impl->stun_host, impl->turn.size(),
                impl->relay_only ? " (relay only: test)" : "");
  }
  if (!host) {
    std::random_device rd;
    impl->session = (uint64_t(rd()) << 32 | rd()) & 0x1FFFFFFFFFFFFFull;  // JSON-safe
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->NewAgent()) return nullptr;
  }
  Impl* raw = impl.get();
  raw->pump = std::thread([raw] { raw->Pump(); });
  return std::unique_ptr<IceLink>(new IceLink(std::move(impl)));
}

IceLink::IceLink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

IceLink::~IceLink() {
  impl_->stopping = true;
  if (impl_->pump.joinable()) impl_->pump.join();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->agent) juice_destroy(impl_->agent);
  impl_->agent = nullptr;
}

bool IceLink::HasPeer() const {
  const int s = impl_->state.load();
  return s == JUICE_STATE_CONNECTED || s == JUICE_STATE_COMPLETED;
}

bool IceLink::Failed() const { return impl_->failed.load(); }

bool IceLink::Restart() {
  if (impl_->host) return false;  // the host follows the guest's new offer
  {
    std::lock_guard<std::mutex> sig(impl_->sig_mutex);
    std::random_device rd;
    impl_->session = (uint64_t(rd()) << 32 | rd()) & 0x1FFFFFFFFFFFFFull;  // JSON-safe
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->answered = false;
  impl_->offered_at = 0;  // the pump offers at once
  impl_->failed = false;
  if (!impl_->NewAgent()) {
    impl_->failed = true;
    return false;
  }
  REXLOG_INFO("[ice] room {}: a new exchange (the path was up but carried nothing)", impl_->room);
  return true;
}

std::string IceLink::PeerText() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->agent) return "nobody yet";
  char local[JUICE_MAX_ADDRESS_STRING_LEN] = {}, remote[JUICE_MAX_ADDRESS_STRING_LEN] = {};
  char local_c[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {}, remote_c[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
  if (juice_get_selected_addresses(impl_->agent, local, sizeof(local), remote, sizeof(remote)) != JUICE_ERR_SUCCESS)
    return "no path yet";
  juice_get_selected_candidates(impl_->agent, local_c, sizeof(local_c), remote_c, sizeof(remote_c));
  const bool relayed = IsRelay(local_c) || IsRelay(remote_c);
  return fmt::format("{} ({}, from {})", remote, relayed ? "relayed by TURN" : "direct", local);
}

bool IceLink::Relayed() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->agent) return false;
  char local_c[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {}, remote_c[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = {};
  if (juice_get_selected_candidates(impl_->agent, local_c, sizeof(local_c), remote_c, sizeof(remote_c)) !=
      JUICE_ERR_SUCCESS) {
    return false;
  }
  return IsRelay(local_c) || IsRelay(remote_c);
}

bool IceLink::Send(const void* data, size_t size) {
  if (!HasPeer()) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->agent && juice_send(impl_->agent, static_cast<const char*>(data), size) == JUICE_ERR_SUCCESS;
}

int IceLink::Receive(void* buffer, size_t capacity) {
  std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
  if (impl_->inbox.empty()) return -1;
  Impl::Datagram datagram = std::move(impl_->inbox.front());
  impl_->inbox.pop_front();
  impl_->last_arrival_us = datagram.arrived_us;
  const size_t n = std::min(capacity, datagram.bytes.size());
  std::memcpy(buffer, datagram.bytes.data(), n);
  return int(n);
}

uint64_t IceLink::LastArrivalUs() const {
  std::lock_guard<std::mutex> lock(impl_->inbox_mutex);
  return impl_->last_arrival_us;
}

bool IceLink::WaitReadable(int timeout_ms) {
  std::unique_lock<std::mutex> lock(impl_->inbox_mutex);
  return impl_->readable.wait_for(lock, std::chrono::milliseconds(std::max(0, timeout_ms)),
                                  [this] { return !impl_->inbox.empty(); });
}

}  // namespace mk9::net
