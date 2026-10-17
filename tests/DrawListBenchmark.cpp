#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "DrawList.hpp"

// REFD2D_LEGACY_HANDOFF is for compiling against the pre-refactor c9b3560 headers.
int main() {
    using Clock = std::chrono::steady_clock;
    for (size_t count : {1024u, 4096u}) {
        DrawList list;
        DrawList::Command command{};
        command.type = DrawList::CommandType::TEXT;
        command.str.assign(128, 'x');
        // Exercise reference counting without creating any graphics resources.
        command.font_resource = std::shared_ptr<D2DFont>(std::make_shared<int>(1), nullptr);
        constexpr unsigned iterations = 2000;
        std::chrono::nanoseconds total{};
        size_t checksum{};
#if defined(REFD2D_LEGACY_HANDOFF)
        std::vector<DrawList::Command> replay;
#endif
        for (unsigned i = 0; i < iterations + 10; ++i) {
            {
#if defined(REFD2D_LEGACY_HANDOFF)
                auto writer = list.acquire();
#else
                auto writer = list.begin();
#endif
                writer.commands.assign(count, command);
            }
            const auto start = Clock::now();
#if defined(REFD2D_LEGACY_HANDOFF)
            {
                auto reader = list.acquire();
                replay = reader.commands;
                checksum += replay.size();
            }
#else
            list.publish(1);
            auto reader = list.acquire();
            checksum += reader.frame.commands.size();
#endif
            const auto elapsed = Clock::now() - start;
            if (i >= 10) total += std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
        }
#if defined(REFD2D_LEGACY_HANDOFF)
        std::cout << "legacy-copy";
#else
        std::cout << "slot-exchange";
#endif
        std::cout << " commands=" << count << " text_bytes=128 handoff_us="
                  << static_cast<double>(total.count()) / iterations / 1000.0 << " checksum=" << checksum << '\n';
    }
}
