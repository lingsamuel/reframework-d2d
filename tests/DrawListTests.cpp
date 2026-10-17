#include <stdexcept>
#include <string>
#include "DrawList.hpp"

void test_drawlist_batch_ownership() {
    DrawList list;
    auto empty = list.acquire();
    if (empty.changed || empty.frame.generation || !empty.frame.commands.empty())
        throw std::runtime_error("Initial command batch was not empty");
    auto writer = list.begin();
    std::shared_ptr<D2DFont> font;
    writer.text(font, std::string(4096, 'a'), 1, 2, 0xFFFFFFFF);
    const auto* command_address = writer.commands.data();
    const auto* string_address = writer.commands.front().str.data();
    list.publish(7);
    auto held = list.acquire();
    if (!held.changed || held.frame.generation != 7 || held.frame.commands.data() != command_address ||
        held.frame.commands.front().str.data() != string_address)
        throw std::runtime_error("Command publication copied the batch or its text");
    for (int i = 0; i < 100; ++i) {
        auto next = list.begin();
        next.fill_rect(0, 0, 2, 2, 0xFFFFFFFF);
        list.publish(8);
        if (held.frame.commands.front().str != std::string(4096, 'a'))
            throw std::runtime_error("Recording a new batch changed the held commands");
    }
    auto newest = list.acquire();
    if (!newest.changed || newest.frame.generation != 8 || newest.frame.commands.size() != 1 ||
        newest.frame.commands.front().type != DrawList::CommandType::FILL_RECT)
        throw std::runtime_error("Consumer did not receive the newest complete command batch");
    list.clear();
    auto cleared = list.acquire();
    if (!cleared.changed || cleared.frame.generation || !cleared.frame.commands.empty())
        throw std::runtime_error("Clearing Lua commands did not publish an invalid empty batch");
}
