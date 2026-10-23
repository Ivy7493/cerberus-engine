#pragma once

#include "eng/net/Transport.h"

#include <chrono>
#include <deque>
#include <memory>
#include <random>

namespace eng {

// Test decorator: wraps any Transport and adds one-way latency (half of
// `rttMs` each direction), jitter, and loss of unreliable messages. Reliable
// messages are delayed but never dropped or reordered, matching what the
// real transports guarantee. For trying netcode under realistic conditions
// on one machine (Delve's --lag/--jitter/--loss).
class LagTransport : public Transport {
public:
    LagTransport(std::unique_ptr<Transport> inner, int rttMs, int jitterMs, float lossPercent);

    void send(PeerId peer, const void* data, size_t size, Delivery delivery) override;
    using Transport::send;
    void poll(std::vector<TransportEvent>& out) override;
    void disconnect(PeerId peer) override { m_inner->disconnect(peer); }
    int pingMs(PeerId peer) const override { return m_inner->pingMs(peer) + m_rttMs; }

private:
    using Clock = std::chrono::steady_clock;
    struct Outgoing {
        Clock::time_point due;
        PeerId peer;
        std::vector<uint8_t> data;
        Delivery delivery;
    };
    struct Incoming {
        Clock::time_point due;
        TransportEvent event;
    };

    Clock::time_point delayFrom(Clock::time_point now, Delivery delivery, Clock::time_point& lastReliable);
    bool dropped(Delivery delivery);

    std::unique_ptr<Transport> m_inner;
    int m_rttMs, m_jitterMs;
    float m_loss;
    std::mt19937 m_rng{1234};
    std::deque<Outgoing> m_out;
    std::deque<Incoming> m_in;
    Clock::time_point m_lastReliableOut{}, m_lastReliableIn{};
};

}  // namespace eng
