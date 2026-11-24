#include "eng/platform/FrameLimiter.h"

#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#endif

namespace eng {

FrameLimiter::FrameLimiter() {
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
}

FrameLimiter::~FrameLimiter() {
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
}

void FrameLimiter::wait(double maxFps) {
    Clock::time_point now = Clock::now();
    if (maxFps <= 0.0) {
        m_started = false;
        return;
    }
    auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / maxFps));
    if (!m_started || now > m_next + period) {  // first frame, or fell more than a frame behind: resync
        m_started = true;
        m_next = now + period;
        return;
    }
    // Sleep until ~1.5 ms before the deadline, then spin to it.
    constexpr auto kSpin = std::chrono::microseconds(1500);
    if (m_next - now > kSpin) std::this_thread::sleep_for(m_next - now - kSpin);
    while (Clock::now() < m_next) std::this_thread::yield();
    m_next += period;
}

}  // namespace eng
