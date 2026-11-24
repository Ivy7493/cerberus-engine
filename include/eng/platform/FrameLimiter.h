#pragma once

#include <chrono>

namespace eng {

// Caps a loop's rate: call wait(maxFps) once per iteration (at the top, so
// early-continue paths are capped too). Sleeps most of the gap and spins the
// last bit, since OS sleeps overshoot; keeps a steady cadence (no drift) and
// resyncs after a frame that ran long instead of rushing to catch up.
// maxFps <= 0: no cap. On Windows it raises the system timer resolution to
// 1 ms while one exists (otherwise sleeps are ~15.6 ms granular).
class FrameLimiter {
public:
    FrameLimiter();
    ~FrameLimiter();
    FrameLimiter(const FrameLimiter&) = delete;
    FrameLimiter& operator=(const FrameLimiter&) = delete;

    void wait(double maxFps);

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_next{};
    bool m_started = false;
};

}  // namespace eng
