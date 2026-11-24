#pragma once

#include "eng/net/Transport.h"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace eng {

// Counts what goes through another Transport — message payloads, not the
// inner transport's own headers, resends or keepalives — for the dev
// overlay and logs. Wrap outermost (around a LagTransport too, so the
// numbers are what the game asked for). The counters are shared, so a game
// can keep one set across sessions while each session owns its transport:
//
//   auto counters = std::make_shared<MeteredTransport::Counters>();   // the game keeps this
//   session = Session(std::make_unique<MeteredTransport>(std::move(transport), counters));
//
// Counters are cumulative; take differences over time for rates.
class MeteredTransport : public Transport {
public:
    struct Counters {
        uint64_t bytesOut = 0, bytesIn = 0;
        uint64_t messagesOut = 0, messagesIn = 0;
        uint64_t reliableOut = 0;    // of messagesOut
        uint32_t largestOut = 0;     // biggest single message sent (vs. the path MTU)
        uint32_t disconnects = 0;
    };

    explicit MeteredTransport(std::unique_ptr<Transport> inner, std::shared_ptr<Counters> counters = nullptr)
        : m_inner(std::move(inner)), m_counters(counters ? std::move(counters) : std::make_shared<Counters>()) {}

    void send(PeerId peer, const void* data, size_t size, Delivery delivery) override {
        m_counters->bytesOut += size;
        ++m_counters->messagesOut;
        if (delivery == Delivery::Reliable) ++m_counters->reliableOut;
        m_counters->largestOut = std::max(m_counters->largestOut, static_cast<uint32_t>(size));
        m_inner->send(peer, data, size, delivery);
    }
    using Transport::send;

    void poll(std::vector<TransportEvent>& out) override {
        size_t first = out.size();
        m_inner->poll(out);
        for (size_t i = first; i < out.size(); ++i) {
            if (out[i].type == TransportEvent::Type::Data) {
                m_counters->bytesIn += out[i].data.size();
                ++m_counters->messagesIn;
            } else {
                ++m_counters->disconnects;
            }
        }
    }
    void disconnect(PeerId peer) override { m_inner->disconnect(peer); }
    int pingMs(PeerId peer) const override { return m_inner->pingMs(peer); }

    const Counters& counters() const { return *m_counters; }
    Transport& inner() { return *m_inner; }

private:
    std::unique_ptr<Transport> m_inner;
    std::shared_ptr<Counters> m_counters;
};

}  // namespace eng
