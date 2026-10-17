#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "FrameMailbox.hpp"

namespace {
void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Frame {
    uint64_t sequence{};
    std::vector<uint64_t> values;
    std::string text;
    std::shared_ptr<int> resource;
    inline static unsigned copies{};
    Frame() = default;
    Frame(const Frame& other) : sequence{other.sequence}, values{other.values}, text{other.text}, resource{other.resource} { ++copies; }
    Frame& operator=(const Frame& other) {
        ++copies;
        sequence = other.sequence; values = other.values; text = other.text; resource = other.resource;
        return *this;
    }
};

void test_latest_and_held() {
    FrameMailbox<Frame> mailbox;
    auto empty = mailbox.acquire();
    expect(!empty.changed && empty.frame.sequence == 0, "Initial frame must be empty");
    auto* published = &mailbox.back();
    mailbox.back().sequence = 1;
    mailbox.back().text = std::string(4096, 'a');
    mailbox.publish();
    auto first = mailbox.acquire();
    expect(first.changed && &first.frame == published, "Handoff copied the frame instead of transferring its slot");
    for (uint64_t i = 2; i <= 100; ++i) {
        mailbox.back().sequence = i;
        mailbox.back().text = std::string(4096, 'b');
        mailbox.publish();
        expect(first.frame.sequence == 1 && first.frame.text == std::string(4096, 'a'), "Producer overwrote a held frame");
    }
    auto newest = mailbox.acquire();
    expect(newest.changed && newest.frame.sequence == 100, "Consumer did not acquire the latest complete frame");
    auto again = mailbox.acquire();
    expect(!again.changed && &again.frame == &newest.frame, "An unpublished frame replaced the held frame");
    expect(Frame::copies == 0, "Publishing or acquiring copied frame contents");
}

void test_resource_recycling() {
    FrameMailbox<Frame> mailbox;
    auto resource = std::make_shared<int>(7);
    std::weak_ptr<int> alive = resource;
    mailbox.back().resource = std::move(resource);
    mailbox.publish();
    auto held = mailbox.acquire();
    for (int i = 0; i < 100; ++i) {
        mailbox.back().resource.reset();
        mailbox.publish();
    }
    expect(!alive.expired() && *held.frame.resource == 7, "Held resource was reclaimed by producer");
    auto next = mailbox.acquire();
    expect(next.changed && !next.frame.resource && !alive.expired(), "Consumer destroyed the previous frame resource");
    for (int i = 0; i < 3; ++i) {
        mailbox.back().resource.reset();
        mailbox.publish();
    }
    expect(alive.expired(), "Producer could not recycle the released slot");
}

void test_concurrent_publication() {
    FrameMailbox<Frame> mailbox;
    constexpr uint64_t count = 100000;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint64_t seq = 1; seq <= count; ++seq) {
            auto& frame = mailbox.back();
            frame.sequence = seq;
            frame.values.assign(96, seq);
            frame.text.assign(256, static_cast<char>('a' + seq % 26));
            mailbox.publish();
        }
        done.store(true, std::memory_order_release);
    });
    bool intact = true;
    uint64_t previous{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (previous != count && std::chrono::steady_clock::now() < deadline) {
        const auto view = mailbox.acquire();
        const auto seq = view.frame.sequence;
        if (!seq) continue;
        intact = intact && seq >= previous;
        for (auto value : view.frame.values) intact = intact && value == seq;
        for (char value : view.frame.text) intact = intact && value == static_cast<char>('a' + seq % 26);
        std::this_thread::yield();
        intact = intact && view.frame.sequence == seq;
        previous = seq;
    }
    producer.join();
    expect(done.load(std::memory_order_acquire) && previous == count, "Latest frame was lost");
    expect(intact, "Concurrent publication tore a held frame");
}
}

int main() {
    try {
        test_latest_and_held();
        test_resource_recycling();
        test_concurrent_publication();
        std::cout << "PASS: mailbox ownership, latest frame, resource recycling and 100000 concurrent publications\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
