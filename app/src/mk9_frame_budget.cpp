// mk9-lauta (2026-11-10): where the game thread's frame goes, every 10 s ("[frame] ..."): the engine's tick
// (FEngineLoop::Tick, sub_823621F8) split into its own work and its waits for UE3's render thread (sub_82741630, the
// tick's render command fence: the game thread may run one frame ahead of the render thread and no more), and the
// frame boundary before it (the pad read's hook in online/mk9_capture.cpp: an online session's work there, the loads
// finished at the frame's start). With percentiles: the online plan's gates read them (docs/online/baseline-0.3.0.md).

#include "mk9_frame_budget.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mk9_frame_budget_log, true, "MK9", "Log where the game thread's frames go every 10 s ([frame])");

namespace mk9::frame_budget {
namespace {

using Clock = std::chrono::steady_clock;

// Lengths in 0.1 ms buckets up to 100 ms (longer ones in the last).
struct Histogram {
  static constexpr size_t kBuckets = 1000;
  uint32_t counts[kBuckets] = {};
  uint64_t total = 0;
  double sum_ms = 0.0, max_ms = 0.0;

  void Add(double ms) {
    ++counts[std::min<size_t>(size_t(std::max(ms, 0.0) * 10.0), kBuckets - 1)];
    ++total;
    sum_ms += ms;
    max_ms = std::max(max_ms, ms);
  }
  double Mean() const { return total ? sum_ms / double(total) : 0.0; }
  double Percentile(double fraction) const {
    if (!total) return 0.0;
    const uint64_t wanted = uint64_t(double(total - 1) * fraction) + 1;
    uint64_t seen = 0;
    for (size_t i = 0; i < kBuckets; ++i) {
      seen += counts[i];
      if (seen >= wanted) return (double(i) + 0.5) / 10.0;
    }
    return double(kBuckets) / 10.0;
  }
  void Clear() { *this = Histogram{}; }
};

// The game thread's only.
struct State {
  std::thread::id game;
  Clock::time_point since{};
  Clock::time_point tick_start{};
  bool in_tick = false;
  double tick_wait_ms = 0.0;  // the tick's waits for the render thread so far
  Histogram work, wait, boundary;
};
State g;

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

void MaybeReport(Clock::time_point now) {
  if (g.since == Clock::time_point{}) g.since = now;
  if (now - g.since < std::chrono::seconds(10)) return;
  if (REXCVAR_GET(mk9_frame_budget_log) && g.work.total) {
    REXLOG_INFO("[frame] game thread, {:.1f} s: {} frames; its work {:.2f} ms a frame (p50 {:.1f}, p99 {:.1f}, most "
                "{:.1f}); waiting for the render thread {:.2f} ms (p99 {:.1f}, most {:.1f}); the frame boundary {:.2f} "
                "ms (p99 {:.1f}, most {:.1f})",
                Ms(now - g.since) / 1000.0, g.work.total, g.work.Mean(), g.work.Percentile(0.5),
                g.work.Percentile(0.99), g.work.max_ms, g.wait.Mean(), g.wait.Percentile(0.99), g.wait.max_ms,
                g.boundary.Mean(), g.boundary.Percentile(0.99), g.boundary.max_ms);
  }
  g.since = now;
  g.work.Clear();
  g.wait.Clear();
  g.boundary.Clear();
}

}  // namespace

void NoteBoundary(double ms) { g.boundary.Add(ms); }

}  // namespace mk9::frame_budget

using mk9::frame_budget::g;

extern "C" REX_FUNC(__imp__sub_823621F8);
REX_HOOK_RAW(sub_823621F8) {
  const auto start = mk9::frame_budget::Clock::now();
  g.game = std::this_thread::get_id();
  g.in_tick = true;
  g.tick_wait_ms = 0.0;
  __imp__sub_823621F8(ctx, base);
  g.in_tick = false;
  const auto end = mk9::frame_budget::Clock::now();
  const double total = mk9::frame_budget::Ms(end - start);
  g.work.Add(std::max(0.0, total - g.tick_wait_ms));
  g.wait.Add(g.tick_wait_ms);
  mk9::frame_budget::MaybeReport(end);
}

extern "C" REX_FUNC(__imp__sub_82741630);
REX_HOOK_RAW(sub_82741630) {
  if (!g.in_tick || std::this_thread::get_id() != g.game) {
    __imp__sub_82741630(ctx, base);
    return;
  }
  const auto start = mk9::frame_budget::Clock::now();
  __imp__sub_82741630(ctx, base);
  g.tick_wait_ms += mk9::frame_budget::Ms(mk9::frame_budget::Clock::now() - start);
}
