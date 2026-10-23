#include "eng/net/LagTransport.h"

#include <algorithm>

namespace eng {

LagTransport::LagTransport(std::unique_ptr<Transport> inner, int rttMs, int jitterMs, float lossPercent)
    : m_inner(std::move(inner)), m_rttMs(std::max(0, rttMs)), m_jitterMs(std::max(0, jitterMs)), m_loss(lossPercent) {}

LagTransport::Clock::time_point LagTransport::delayFrom(Clock::time_point now, Delivery delivery,
                                                       Clock::time_point& lastReliable) {
    int jitter = m_jitterMs ? std::uniform_int_distribution<int>(-m_jitterMs, m_jitterMs)(m_rng) : 0;
    auto due = now + std::chrono::milliseconds(std::max(0, m_rttMs / 2 + jitter));
    if (delivery == Delivery::Reliable) {  // keep reliable traffic in order
        due = std::max(due, lastReliable);
        lastReliable = due;
    }
    return due;
}

bool LagTransport::dropped(Delivery delivery) {
    if (delivery == Delivery::Reliable || m_loss <= 0.0f) return false;
    return std::uniform_real_distribution<float>(0.0f, 100.0f)(m_rng) < m_loss;
}

void LagTransport::send(PeerId peer, const void* data, size_t size, Delivery delivery) {
    if (dropped(delivery)) return;
    const auto* p = static_cast<const uint8_t*>(data);
    m_out.push_back({delayFrom(Clock::now(), delivery, m_lastReliableOut), peer, std::vector<uint8_t>(p, p + size), delivery});
}

void LagTransport::poll(std::vector<TransportEvent>& out) {
    auto now = Clock::now();
    // Release due sends (any order; reliable ones were stamped monotonic).
    for (auto it = m_out.begin(); it != m_out.end();) {
        if (it->due <= now) {
            m_inner->send(it->peer, it->data, it->delivery);
            it = m_out.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<TransportEvent> fresh;
    m_inner->poll(fresh);
    for (auto& e : fresh) {
        // Incoming data has no delivery tag here; treat it as unreliable for
        // loss only when it's short-lived game state (the inner transport
        // already resent anything reliable), so just delay everything.
        m_in.push_back({delayFrom(now, Delivery::Reliable, m_lastReliableIn), std::move(e)});
    }
    while (!m_in.empty() && m_in.front().due <= now) {
        out.push_back(std::move(m_in.front().event));
        m_in.pop_front();
    }
}

}  // namespace eng
