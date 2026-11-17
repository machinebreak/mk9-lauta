// MK9 online: the two players' own path (ice_link.h).

#include "online/ice_link.h"

#include <juice/juice.h>

#include <algorithm>
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

// A lobby id for the logs ("the host" for the empty one).
std::string Who(const std::string& id) { return id.empty() ? std::string("the host") : id; }

}  // namespace

struct IceLink::Impl {
  std::string room;
  // mk9-lauta (2026-11-17): the two sides' lobby ids (the host's: empty). A session of two's host learns its guest's
  // from the offer (learn_remote); the side that offers is a guest (of two guests, the later one in the room).
  std::string local_id, remote_id;
  bool learn_remote = false;
  bool offerer = false;
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
  uint64_t session = 0;    // the exchange's number (the offer picks it): a stale one is ignored
  std::mutex inbox_mutex;  // the datagrams in
  std::condition_variable readable;
  struct Datagram {
    uint64_t arrived_us = 0;  // libjuice's thread took it (LastArrivalUs)
    std::vector<uint8_t> bytes;
  };
  std::deque<Datagram> inbox;
  uint64_t last_arrival_us = 0;  // of the datagram Receive returned last (the game thread's)

  std::atomic<int> state{JUICE_STATE_DISCONNECTED};
  std::atomic<bool> failed{false};

  // Relays data with the exchange's number and this side's id to the other side.
  void SendSig(json data) {
    std::string to;
    bool unknown = false;
    {
      std::lock_guard<std::mutex> lock(sig_mutex);
      data["s"] = session;
      to = remote_id;
      unknown = learn_remote && remote_id.empty();
    }
    data["from"] = local_id;
    if (unknown) return;  // a session of two's host before its guest offered: nobody to send to yet
    if (to.empty()) {
      lobby::SigToHost(room, data);
    } else {
      lobby::SigToGuest(room, to, data);
    }
  }

  uint64_t Session() {
    std::lock_guard<std::mutex> lock(sig_mutex);
    return session;
  }

  // This link takes a message from that sender (the router): its other side's, or an offer the host of a session of
  // two waits for from its guest, whoever it is.
  bool Takes(const std::string& from, const std::string& kind) {
    std::lock_guard<std::mutex> lock(sig_mutex);
    if (learn_remote && remote_id.empty()) return kind == "offer";
    return from == remote_id;
  }

  // A new agent for this exchange (the mutex held). remote: the offer it answers (the answering side), set before the
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
      REXLOG_INFO("[ice] room {} ({}): {}", self->room, Who(self->remote_id), juice_state_to_string(s));
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
    if (!offerer && kind == "offer") {
      const std::string from = m.value("from", "");
      if (s == Session() && agent) {
        SendSig({{"ice", "answer"}, {"sdp", local_sdp}});  // the offering side missed the answer
        return;
      }
      // a new exchange: the offering side (re)started
      {
        std::lock_guard<std::mutex> sig(sig_mutex);
        if (learn_remote) remote_id = from;
        session = s;
      }
      const std::string offer = m.value("sdp", "");
      if (!NewAgent(&offer)) {
        failed = true;
        return;
      }
      SendSig({{"ice", "answer"}, {"sdp", local_sdp}});
      juice_gather_candidates(agent);
      REXLOG_INFO("[ice] room {}: {} offered; answered", room, Who(from));
      return;
    }
    if (s != Session() || !agent) return;  // another exchange's
    if (offerer && kind == "answer" && !answered) {
      answered = true;
      SetRemote(m.value("sdp", ""));
      juice_gather_candidates(agent);
      REXLOG_INFO("[ice] room {}: {} answered", room, Who(remote_id));
    } else if (kind == "cand") {
      AddRemoteCandidate(m.value("c", ""));
    } else if (kind == "done") {
      if (remote_set) juice_set_remote_gathering_done(agent);
    }
  }

  // The offering side's offer again, until it is answered (both sides may be starting up).
  void Tick() {
    std::lock_guard<std::mutex> lock(mutex);
    if (offerer && agent && !answered && NowUs() - offered_at > 1000000) {
      offered_at = NowUs();
      SendSig({{"ice", "offer"}, {"from", local_id}, {"sdp", local_sdp}});
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

namespace {

// mk9-lauta (2026-11-17): the relayed ICE messages come in one queue (lobby.h PollIce) for every path of this game: a
// session of four machines has three. One thread for all of them takes each message to the path it is for (by the
// room and the sender's id) and makes the offering sides offer again until answered (what each link's own thread did
// while there was one link).
std::mutex g_links_mutex;
std::vector<IceLink::Impl*> g_links;

void Route() {
  for (;;) {
    lobby::Event event;
    while (lobby::PollIce(&event)) {
      if (!event.data.is_object()) continue;
      const std::string from = event.data.value("from", "");
      const std::string kind = event.data.value("ice", "");
      std::lock_guard<std::mutex> lock(g_links_mutex);
      IceLink::Impl* target = nullptr;
      // a link whose other side sent it first; then a session of two's host waiting for its guest's offer
      for (IceLink::Impl* link : g_links) {
        if (link->room == event.room && !link->learn_remote && link->Takes(from, kind)) target = link;
      }
      if (!target) {
        for (IceLink::Impl* link : g_links) {
          if (link->room == event.room && link->learn_remote && link->Takes(from, kind)) target = link;
        }
      }
      if (target) target->OnSig(event.data);
    }
    {
      std::lock_guard<std::mutex> lock(g_links_mutex);
      for (IceLink::Impl* link : g_links) link->Tick();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void Register(IceLink::Impl* link) {
  static const bool started = [] {
    std::thread(Route).detach();
    return true;
  }();
  (void)started;
  std::lock_guard<std::mutex> lock(g_links_mutex);
  g_links.push_back(link);
}

void Unregister(IceLink::Impl* link) {
  std::lock_guard<std::mutex> lock(g_links_mutex);
  g_links.erase(std::remove(g_links.begin(), g_links.end(), link), g_links.end());
}

// The link's servers and, on the offering side, its first agent.
std::unique_ptr<IceLink::Impl> NewImpl(const std::string& room, const std::string& local_id,
                                       const std::string& remote_id, bool learn_remote, bool offerer) {
  static const bool logging = [] {
    juice_set_log_handler(LogFromJuice);
    juice_set_log_level(JUICE_LOG_LEVEL_WARN);
    return true;
  }();
  (void)logging;
  auto impl = std::make_unique<IceLink::Impl>();
  impl->room = room;
  impl->local_id = local_id;
  impl->remote_id = remote_id;
  impl->learn_remote = learn_remote;
  impl->offerer = offerer;
  impl->relay_only = REXCVAR_GET(mk9_ice_relay_only);
  impl->TakeServers(lobby::IceServers());
  const std::string other = learn_remote ? std::string("its guest") : Who(remote_id);
  if (impl->turn.empty()) {
    REXLOG_WARN("[ice] room {}, the path to {} ({}): STUN {}, no TURN server: behind some providers' networks the "
                "two games will not find each other",
                room, other, offerer ? "offering" : "answering",
                impl->stun_host.empty() ? "none" : impl->stun_host);
  } else {
    REXLOG_INFO("[ice] room {}, the path to {} ({}): STUN {}, {} TURN server(s){}", room, other,
                offerer ? "offering" : "answering", impl->stun_host.empty() ? "none" : impl->stun_host,
                impl->turn.size(), impl->relay_only ? " (relay only: test)" : "");
  }
  if (offerer) {
    std::random_device rd;
    impl->session = (uint64_t(rd()) << 32 | rd()) & 0x1FFFFFFFFFFFFFull;  // JSON-safe
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (!impl->NewAgent()) return nullptr;
  }
  return impl;
}

}  // namespace

std::unique_ptr<IceLink> IceLink::Create(const std::string& room, bool host, const std::string& id) {
  // a session of two: the host answers whichever guest offers, the guest offers to the host
  auto impl = host ? NewImpl(room, "", "", true, false) : NewImpl(room, id, "", false, true);
  if (!impl) return nullptr;
  Register(impl.get());
  return std::unique_ptr<IceLink>(new IceLink(std::move(impl)));
}

std::unique_ptr<IceLink> IceLink::CreatePair(const std::string& room, const std::string& local_id,
                                             const std::string& remote_id, bool offers) {
  auto impl = NewImpl(room, local_id, remote_id, false, offers);
  if (!impl) return nullptr;
  Register(impl.get());
  return std::unique_ptr<IceLink>(new IceLink(std::move(impl)));
}

IceLink::IceLink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

IceLink::~IceLink() {
  Unregister(impl_.get());
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
  if (!impl_->offerer) return false;  // the answering side follows the other's new offer
  {
    std::lock_guard<std::mutex> sig(impl_->sig_mutex);
    std::random_device rd;
    impl_->session = (uint64_t(rd()) << 32 | rd()) & 0x1FFFFFFFFFFFFFull;  // JSON-safe
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->answered = false;
  impl_->offered_at = 0;  // the router offers at once
  impl_->failed = false;
  if (!impl_->NewAgent()) {
    impl_->failed = true;
    return false;
  }
  REXLOG_INFO("[ice] room {}: a new exchange with {} (the path was up but carried nothing)", impl_->room,
              Who(impl_->remote_id));
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
