// mk9-nx (2026-11-07, rollback's boundary): parking the threads that run guest code.
//
// A host thread is busy while it runs guest code (a guest thread's entry, a host-to-guest call: an interrupt, an audio
// callback, an APC, a deferred completion) and at rest while it waits in the kernel (XObject::Wait / SignalAndWait /
// WaitMultiple, XThread::Delay) or runs host code of its own. While a park is on, a thread about to turn busy (its
// wait returning, a guest call from the host) stays where it is until the park ends; so once every other thread is at
// rest (Begin), the parking thread has the guest memory to itself: what a rollback needs to keep and put back the
// game's memory whole. A thread's rest inside a WorkScope (its loop running one task: the caller marks it, as it knows
// the loop) is halfway through that task, not at rest: Begin waits for such a thread to finish the task, and a thread
// leaving a wait inside one runs on, so a parked thread is never in the middle of a marked task. Where each thread goes
// to rest (its guest stack pointer at the wait) is counted too, for diagnostics.
//
// The parking thread must not take the kernel's global lock, call guest code or wait in the kernel while the park is
// on: a parked thread may hold what those need.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace rex::system::park {

// The calling thread at rest for the scope (the kernel's waits).
class RestScope {
 public:
  RestScope();
  ~RestScope();
  RestScope(const RestScope&) = delete;
  RestScope& operator=(const RestScope&) = delete;

 private:
  uint8_t previous_ = 0;  // the rest it is inside of (an APC's wait within an alertable wait)
};

// The calling thread in the middle of a task for the scope (its rests inside do not count as at rest).
class WorkScope {
 public:
  WorkScope();
  ~WorkScope();
  WorkScope(const WorkScope&) = delete;
  WorkScope& operator=(const WorkScope&) = delete;
};

// The calling thread busy for the scope (guest code called from the host), once no park holds it.
class BusyScope {
 public:
  BusyScope();
  ~BusyScope();
  BusyScope(const BusyScope&) = delete;
  BusyScope& operator=(const BusyScope&) = delete;
};

// The calling thread goes away (XThread::Exit, which does not return): never busy again.
void ThreadExiting();
// The calling thread runs on through parks (and Begin does not wait for it): for threads whose guest code touches no
// memory a parking thread keeps or puts back (a library given memory of its own, mk9's sound).
void SetExempt(bool exempt);

// An exempt thread running code that does touch that memory for the scope (a library's callback into the game): a
// park on now is waited out first, and Begin waits for the thread to leave the scope. Nothing for other threads.
class GuardScope {
 public:
  GuardScope();
  ~GuardScope();
  GuardScope(const GuardScope&) = delete;
  GuardScope& operator=(const GuardScope&) = delete;

 private:
  bool was_exempt_ = false;
};

struct ThreadView {
  uint32_t host_id = 0;     // the host thread's id
  std::string name;         // its guest thread's name when it first rested or called guest code
  bool busy = false;
  bool exempt = false;      // runs on through parks (SetExempt)
  uint32_t rests = 0;       // its rests so far (wraps)
  uint32_t sp = 0;          // where it last went to rest: its guest stack pointer (0: no guest context)
  uint32_t arg = 0;         // ... and the wait's first argument (a handle, an object)
  bool idle = false;        // at rest outside a task (or in host code)
  uint32_t sites[8] = {};   // the places it rests at (stack pointers) and how often: counts[i]
  uint32_t counts[8] = {};
};

// Starts a park: true once every other thread is at rest (the park stays on until End), false after the timeout (the
// park is off again; *busy, when given: the threads still busy then).
bool Begin(std::chrono::microseconds timeout, std::vector<ThreadView>* busy = nullptr);
void End();
bool Parked();
// Every live thread seen so far.
std::vector<ThreadView> Threads();

}  // namespace rex::system::park
