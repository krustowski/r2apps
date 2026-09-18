#ifndef _R2CXX_TIME_HPP_
#define _R2CXX_TIME_HPP_

/*
 *  time.hpp — the clock and the calendar.
 *
 *  ticks() counts milliseconds since boot, but the PIT runs at 100 Hz, so the
 *  value moves in steps of 10 ms.  sleep() rounds up to the next tick.
 */

#include "optional.hpp"
#include "string.hpp"
#include "syscall.hpp"

namespace r2 {

/*  Milliseconds since boot, in 10 ms steps (syscall 0x04).  */
inline uint64_t ticks() noexcept { return (uint64_t)raw_syscall(Sys::GetTicks, 0, 0); }

/*
 *  Asks the kernel to block the process for `ms` milliseconds (syscall 0x05),
 *  and yields the CPU --- which is how a polling loop stops starving every
 *  other process on the system.
 *
 *  It is one syscall and it is best-effort: the kernel marks the process
 *  blocked and the scheduler wakes it on a PIT tick, but the scheduler takes
 *  its lock with try_lock, so a sleep requested at a contended moment can
 *  return early.  For pacing a loop that is fine.  When the delay actually
 *  matters, use sleep_at_least().
 */
inline void sleep(uint64_t ms) noexcept { raw_syscall(Sys::Sleep, (int64_t)ms, 0); }

/*
 *  Sleeps until at least `ms` milliseconds have passed, re-entering the
 *  syscall if it came back early.  Costs one more read of the clock per
 *  attempt and gives up after `max_attempts` so that a kernel which never
 *  blocks cannot turn this into a spin.
 */
inline void sleep_at_least(uint64_t ms, int max_attempts = 8) noexcept {
    uint64_t deadline = (uint64_t)raw_syscall(Sys::GetTicks, 0, 0) + ms;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        uint64_t now = (uint64_t)raw_syscall(Sys::GetTicks, 0, 0);
        if (now >= deadline)
            return;
        raw_syscall(Sys::Sleep, (int64_t)(deadline - now), 0);
    }
}

/*  Reads the RTC (syscall 0x02).  */
optional<RtcTime> clock_now();

/*  "14:03:27" and "2026-09-18".  */
string format_time(const RtcTime &t);
string format_date(const RtcTime &t);

/*
 *  Elapsed-time helper:
 *
 *      Stopwatch sw;
 *      ... work ...
 *      println("took ", sw.elapsed_ms(), " ms");
 */
class Stopwatch {
  public:
    Stopwatch() noexcept : start_(ticks()) {}

    void restart() noexcept { start_ = ticks(); }
    uint64_t elapsed_ms() const noexcept { return ticks() - start_; }

  private:
    uint64_t start_;
};

/*
 *  Frame pacing for an animation loop: sleeps for whatever is left of the
 *  frame, and does not sleep at all when the frame already overran.
 *
 *      FrameTimer frame(20);   // 50 frames a second
 *      while (running) { draw(); frame.wait(); }
 */
class FrameTimer {
  public:
    explicit FrameTimer(uint64_t frame_ms) noexcept : frame_ms_(frame_ms), next_(ticks()) {}

    void wait() noexcept {
        next_ += frame_ms_;
        uint64_t now = ticks();
        if (next_ > now)
            sleep(next_ - now);
        else
            next_ = now; /*  overran: resynchronise instead of catching up  */
    }

  private:
    uint64_t frame_ms_;
    uint64_t next_;
};

} // namespace r2

#endif
