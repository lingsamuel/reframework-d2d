#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include "D3D12Renderer.hpp"

// The host owns swapchain synchronization. This class protects our CPU leases and
// retires our GPU resources without waiting for Lua from a Present/reset callback.
class RendererLifecycle {
public:
    struct Snapshot {
        std::shared_ptr<D3D12Renderer> renderer;
        uint64_t generation{};
    };

    Snapshot snapshot() const {
        std::scoped_lock lock{m_mutex};
        return {m_active, m_generation};
    }

    void publish(std::shared_ptr<D3D12Renderer> renderer) {
        std::scoped_lock lock{m_mutex};
        retire_active();
        m_active = std::move(renderer);
        ++m_generation;
    }

    void reset(const std::shared_ptr<D3D12Renderer>& expected = {}) {
        std::scoped_lock lock{m_mutex};
        if (expected && m_active != expected) return;
        retire_active();
        ++m_generation;
    }

    void collect() {
        std::list<std::shared_ptr<D3D12Renderer>> batch;
        {
            std::scoped_lock lock{m_mutex};
            batch.splice(batch.end(), m_retired);
        }
        // A tail marker is valid only after the last old CPU user can no longer Flush.
        batch.remove_if([](const auto& renderer) {
            return renderer.use_count() == 1 && renderer->ready_to_destroy();
        });
        {
            std::scoped_lock lock{m_mutex};
            m_retired.splice(m_retired.end(), batch);
        }
    }


private:
    void retire_active() {
        if (m_active) {
            m_retired.push_back(m_active);
            m_active.reset();
        }
    }
    mutable std::mutex m_mutex;
    std::shared_ptr<D3D12Renderer> m_active;
    std::list<std::shared_ptr<D3D12Renderer>> m_retired;
    uint64_t m_generation{};
};
