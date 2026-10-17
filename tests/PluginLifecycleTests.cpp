#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include "D3D12TestFixture.hpp"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "reframework/API.h"

extern "C" bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam*);
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID);

namespace {
std::recursive_mutex lua_mutex;
REFLuaStateCreatedCb created;
REFLuaStateDestroyedCb destroyed;
REFOnPresentCb present;
REFOnDeviceResetCb reset;
REFOnPreApplicationEntryCb begin_rendering;
std::promise<void>* entered{};
std::atomic<bool> has_entered{false};
std::shared_future<void> release_draw;

void log_message(const char*, ...) {}

int block_draw(lua_State*) {
    has_entered.store(true);
    entered->set_value();
    release_draw.wait();
    return 0;
}

void run(lua_State* state, const char* script) {
    std::scoped_lock lock{lua_mutex};
    if (luaL_dostring(state, script) != LUA_OK) {
        const std::string error = lua_tostring(state, -1);
        lua_pop(state, 1);
        throw std::runtime_error(error);
    }
}
}

void test_plugin_lifecycle() {
    using namespace std::chrono_literals;
    test::D3D12TestFixture f;
    f.renderer->wait_for_idle();
    f.renderer.reset();
    REFrameworkPluginFunctions functions{};
    functions.on_lua_state_created = [](REFLuaStateCreatedCb cb) { created = cb; return true; };
    functions.on_lua_state_destroyed = [](REFLuaStateDestroyedCb cb) { destroyed = cb; return true; };
    functions.on_present = [](REFOnPresentCb cb) { present = cb; return true; };
    functions.on_device_reset = [](REFOnDeviceResetCb cb) { reset = cb; return true; };
    functions.on_pre_application_entry = [](const char*, REFOnPreApplicationEntryCb cb) { begin_rendering = cb; return true; };
    functions.lock_lua = [] { lua_mutex.lock(); };
    functions.unlock_lua = [] { lua_mutex.unlock(); };
    functions.log_error = log_message;
    functions.log_info = log_message;
    REFrameworkRendererData renderer_data{REFRAMEWORK_RENDERER_D3D12, f.device.Get(), f.swapchain.Get(), f.queue.Get()};
    REFrameworkPluginInitializeParam param{};
    param.functions = &functions;
    param.renderer_data = &renderer_data;
    DllMain(GetModuleHandleW(nullptr), DLL_PROCESS_ATTACH, nullptr);
    if (!reframework_plugin_initialize(&param)) throw std::runtime_error("Plugin host initialization failed");
    lua_State* state = luaL_newstate();
    luaL_openlibs(state);
    created(state);
    lua_pushcfunction(state, block_draw);
    lua_setglobal(state, "block_draw");
    run(state, R"(
        assert(not pcall(d2d.surface_size))
        assert(not pcall(d2d.create_font, "Arial", 14))
        assert(not pcall(d2d.fill_rect, 0, 0, 1, 1, 0xffffffff))
        init_count, draw_count = 0, 0
        d2d.register(function()
            init_count = init_count + 1
            font = font or d2d.create_font("Arial", 14)
        end, function()
            draw_count = draw_count + 1
            if should_block then block_draw() end
            assert(not pcall(d2d.text, nil, "bad", 0, 0, 0xffffffff))
            assert(not pcall(d2d.image, nil, 0, 0))
            d2d.fill_rect(0, 0, 64, 64, 0xffffffff)
        end)
    )");
    present();
    begin_rendering();
    present();
    run(state, "assert(init_count == 1 and draw_count == 1)");

    std::promise<void> entered_draw, release;
    entered = &entered_draw;
    auto entered_future = entered_draw.get_future();
    release_draw = release.get_future().share();
    run(state, "should_block = true; d2d.detail.set_max_updaterate(100000)");
    auto drawing = std::async(std::launch::async, [] {
        for (int i = 0; i < 1000; ++i) {
            begin_rendering();
            if (has_entered.load()) break;
            std::this_thread::sleep_for(1ms);
        }
    });
    if (entered_future.wait_for(3s) != std::future_status::ready) {
        release.set_value();
        drawing.get();
        throw std::runtime_error("The test draw callback was not entered");
    }
    bool cached_overlay_stable = true;
    // The game clears/renders a fresh scene every frame. A previously painted HUD
    // must still be composited while Lua is busy building the next command list.
    for (int i = 0; i < 4; ++i) {
        f.clear_scene();
        present();
        if (f.read_red() < 0.99f) cached_overlay_stable = false;
        test::check_hr(f.swapchain->Present(0, 0));
    }
    const auto start = std::chrono::steady_clock::now();
    reset();
    const bool reset_returned = std::chrono::steady_clock::now() - start < 100ms;
    // A replacement renderer must not pin the previous swapchain buffers, even while
    // an old Lua draw still owns a CPU lease. The host drains the actual game queue.
    D3D12CommandContext drain(f.device.Get());
    drain.signal(f.queue.Get());
    drain.wait(10000);
    const HRESULT resize_hr = f.swapchain->ResizeBuffers(0, 8, 8, DXGI_FORMAT_UNKNOWN, 0);
    release.set_value();
    drawing.get();
    test::check_hr(resize_hr);
    if (!cached_overlay_stable) throw std::runtime_error("Cached overlay disappeared while Lua was recording the next frame");
    if (!reset_returned) throw std::runtime_error("Device reset waited for a Lua draw callback");
    run(state, "should_block = false");
    present();
    begin_rendering();
    present();
    run(state, "local w,h=d2d.surface_size(); assert(w==8 and h==8); assert(init_count==2)");
    if (f.read_red() < 0.99f) throw std::runtime_error("Plugin did not recover after concurrent reset");
    Microsoft::WRL::ComPtr<ID3D12Fence> gate;
    test::check_hr(f.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    test::check_hr(f.queue->Wait(gate.Get(), 1));
    for (int i = 0; i < 5; ++i) { begin_rendering(); present(); }
    test::check_hr(gate->Signal(1));
    drain.signal(f.queue.Get());
    drain.wait(10000);
    begin_rendering();
    present();
    run(state, "assert(init_count == 2, 'GPU lag rebuilt the renderer')");
    for (UINT size : {4u, 16u}) {
        drain.signal(f.queue.Get());
        drain.wait(10000);
        reset();
        DXGI_SWAP_CHAIN_DESC1 desc{};
        test::check_hr(f.swapchain->GetDesc1(&desc));
        desc.Width = desc.Height = size;
        renderer_data.swapchain = nullptr;
        f.swapchain.Reset();
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> replacement;
        test::check_hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        // No intervening Present/collection is available until creation succeeds.
        test::check_hr(factory->CreateSwapChainForHwnd(f.queue.Get(), f.window, &desc, nullptr, nullptr, &replacement));
        test::check_hr(replacement.As(&f.swapchain));
        renderer_data.swapchain = f.swapchain.Get();
        present();
        begin_rendering();
        present();
        if (f.read_red() < 0.99f) throw std::runtime_error("Plugin did not draw into the replacement swapchain");
    }
    run(state, "local w,h=d2d.surface_size(); assert(w==16 and h==16); assert(init_count==4)");
    destroyed(state);
    lua_close(state);
    reset();
    for (int i = 0; i < 20; ++i) { present(); std::this_thread::sleep_for(1ms); }
}
