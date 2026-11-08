// MK9 online: the session's datagram transport (transport.h).

#include "online/transport.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(mk9_net_lag_ms, 0, "MK9 Online", "Tests: added one-way delay of every datagram sent, ms");
REXCVAR_DEFINE_INT32(mk9_net_jitter_ms, 0, "MK9 Online", "Tests: a random 0..this ms on top of mk9_net_lag_ms");
REXCVAR_DEFINE_INT32(mk9_net_loss_pct, 0, "MK9 Online", "Tests: the share of datagrams sent that are dropped, %");

namespace mk9::net {
namespace {

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

}  // namespace

LaggedTransport::LaggedTransport(std::unique_ptr<Transport> inner, int lag_ms, int jitter_ms, int loss_pct)
    : inner_(std::move(inner)),
      lag_ms_(std::max(0, lag_ms)),
      jitter_ms_(std::max(0, jitter_ms)),
      loss_pct_(std::clamp(loss_pct, 0, 100)) {}

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
  if (loss_pct_ && int(rng_() % 100) < loss_pct_) return true;  // lost on the way
  uint64_t due = NowUs() + uint64_t(lag_ms_) * 1000;
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
            loss = REXCVAR_GET(mk9_net_loss_pct);
  if (lag <= 0 && jitter <= 0 && loss <= 0) return transport;
  REXLOG_WARN("[net] emulated network conditions: +{} ms one way, jitter {} ms, loss {}% (tests)", lag, jitter, loss);
  return std::make_unique<LaggedTransport>(std::move(transport), lag, jitter, loss);
}

}  // namespace mk9::net
