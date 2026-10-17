#pragma once

#include <array>
#include <atomic>

// One logical producer and one logical consumer; each side may migrate threads only
// under its own external serialization. No payload is copied or destroyed here.
template <typename Frame> class FrameMailbox {
public:
    struct View {
        const Frame& frame;
        bool changed;
    };

    // Producer only. The returned slot is exclusive until publish().
    Frame& back() noexcept { return m_frames[m_back]; }

    void publish() noexcept {
        // Release publishes all writes; acquire finishes the consumer's reads before
        // an old held slot can become writable again.
        m_back = m_middle.exchange(m_back | changed_bit, std::memory_order_acq_rel) & index_mask;
    }

    // Consumer only. The returned frame stays immutable until the next acquire().
    View acquire() noexcept {
        const bool changed = (m_middle.load(std::memory_order_acquire) & changed_bit) != 0;
        if (changed) {
            m_front = m_middle.exchange(m_front, std::memory_order_acq_rel) & index_mask;
        }
        return {m_frames[m_front], changed};
    }

private:
    static constexpr unsigned changed_bit = 4;
    static constexpr unsigned index_mask = 3;
    std::array<Frame, 3> m_frames{};
    std::atomic<unsigned> m_middle{1};
    unsigned m_back = 0;  // producer only
    unsigned m_front = 2; // consumer only
};
