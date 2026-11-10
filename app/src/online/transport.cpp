// MK9 online: the session's datagram transport (transport.h).

#include "online/transport.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <chrono>
#include <thread>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(mk9_net_lag_ms, 0, "MK9 Online", "Tests: added one-way delay of every datagram sent, ms");
REXCVAR_DEFINE_INT32(mk9_net_jitter_ms, 0, "MK9 Online", "Tests: a random 0..this ms on top of mk9_net_lag_ms");
REXCVAR_DEFINE_INT32(mk9_net_loss_pct, 0, "MK9 Online", "Tests: the share of datagrams sent that are dropped, %");
REXCVAR_DEFINE_INT32(mk9_net_mute_ms, 0, "MK9 Online",
                     "Tests: for this long after the session's link is made every datagram sent is lost though the "
                     "path is up (the ICE sessions that froze: connected, and nothing came)");
REXCVAR_DEFINE_STRING(mk9_net_lag_steps, "", "MK9 Online",
                      "Tests: the one-way lag changes during the session: \"s:ms,s:ms\" (from s seconds after the "
                      "link is made, ms instead of mk9_net_lag_ms), as a connection whose round trip grows mid-fight");
REXCVAR_DEFINE_INT32(mk9_net_late_path_ms, 0, "MK9 Online",
                     "Tests: for this long after the session's link is made there is no path to send on (sends "
                     "dropped) while datagrams still come in, as an ICE link before its pair is chosen");

namespace mk9::net {
namespace {

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

}  // namespace

LaggedTransport::LaggedTransport(std::unique_ptr<Transport> inner, int lag_ms, int jitter_ms, int loss_pct,
                                 int late_path_ms, int mute_ms, std::vector<std::pair<int, int>> lag_steps)
    : inner_(std::move(inner)),
      lag_ms_(std::max(0, lag_ms)),
      jitter_ms_(std::max(0, jitter_ms)),
      loss_pct_(std::clamp(loss_pct, 0, 100)),
      path_from_us_(NowUs() + uint64_t(std::max(0, late_path_ms)) * 1000),
      mute_until_us_(NowUs() + uint64_t(std::max(0, mute_ms)) * 1000),
      made_us_(NowUs()),
      lag_steps_(std::move(lag_steps)) {}

bool LaggedTransport::HasPeer() const { return NowUs() >= path_from_us_ && inner_->HasPeer(); }

std::string LaggedTransport::PeerText() const {
  return fmt::format("{} (emulated: +{} ms, jitter {} ms, loss {}%)", inner_->PeerText(), lag_ms_, jitter_ms_,
                     loss_pct_);
}

void LaggedTransport::Pump() {
  const uint64_t now = NowUs();
  while (!held_.empty() && held_.front().due_us <= now) {
    inner_->Send(held_.front().bytes.data(), held_.front().bytes.size());
    held_.pop_front();
  }
}

bool LaggedTransport::Send(const void* data, size_t size) {
  Pump();
  if (NowUs() < path_from_us_) return false;  // no path yet (mk9_net_late_path_ms)
  if (NowUs() < mute_until_us_) return true;  // lost on a path that seems up (mk9_net_mute_ms)
  if (loss_pct_ && int(rng_() % 100) < loss_pct_) return true;  // lost on the way
  int lag = lag_ms_;
  for (const auto& [at_s, ms] : lag_steps_) {
    if (NowUs() >= made_us_ + uint64_t(at_s) * 1000000) lag = ms;
  }
  uint64_t due = NowUs() + uint64_t(std::max(0, lag)) * 1000;
  if (jitter_ms_) due += uint64_t(rng_() % uint32_t(jitter_ms_ * 1000 + 1));
  due = std::max(due, last_due_us_);  // in order, as a real path mostly keeps them
  last_due_us_ = due;
  const auto* bytes = static_cast<const uint8_t*>(data);
  held_.push_back({due, std::vector<uint8_t>(bytes, bytes + size)});
  Pump();
  return true;
}

int LaggedTransport::Receive(void* buffer, size_t capacity) {
  Pump();
  return inner_->Receive(buffer, capacity);
}

bool LaggedTransport::WaitReadable(int timeout_ms) {
  Pump();
  if (!held_.empty()) {
    const int64_t until_due = int64_t(held_.front().due_us) - int64_t(NowUs());
    timeout_ms = std::min<int64_t>(timeout_ms, std::max<int64_t>(0, until_due / 1000));
  }
  const bool readable = inner_->WaitReadable(timeout_ms);
  Pump();
  return readable;
}

std::unique_ptr<Transport> WithTestConditions(std::unique_ptr<Transport> transport) {
  const int lag = REXCVAR_GET(mk9_net_lag_ms), jitter = REXCVAR_GET(mk9_net_jitter_ms),
            loss = REXCVAR_GET(mk9_net_loss_pct), late = REXCVAR_GET(mk9_net_late_path_ms),
            mute = REXCVAR_GET(mk9_net_mute_ms);
  std::vector<std::pair<int, int>> steps;
  const std::string& text = REXCVAR_GET(mk9_net_lag_steps);
  for (size_t at = 0; at < text.size();) {
    const size_t end = std::min(text.find(',', at), text.size());
    int s = 0, ms = 0;
    if (std::sscanf(text.substr(at, end - at).c_str(), "%d:%d", &s, &ms) == 2) steps.emplace_back(s, ms);
    at = end + 1;
  }
  if (lag <= 0 && jitter <= 0 && loss <= 0 && late <= 0 && mute <= 0 && steps.empty()) return transport;
  REXLOG_WARN("[net] emulated network conditions: +{} ms one way, jitter {} ms, loss {}%, no path for {} ms{} (tests)",
              lag, jitter, loss, late, text.empty() ? "" : ", lag steps " + text);
  return std::make_unique<LaggedTransport>(std::move(transport), lag, jitter, loss, late, mute, std::move(steps));
}

}  // namespace mk9::net
