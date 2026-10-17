# Rendering and reset regression tests

These Windows tests use real WARP D3D11/D3D12 devices, hidden swapchains and a small
REFramework callback host with real Lua 5.4.3. No game, game files or injected DLL is needed.

From a Windows developer shell with CMake and the Windows SDK installed:

```powershell
powershell -ExecutionPolicy Bypass -File tests/run-reset-tests.ps1
```

The script configures and builds the test target, runs it with a 60-second watchdog,
and propagates failures. Existing configured builds can be reused:

```powershell
powershell -ExecutionPolicy Bypass -File tests/run-reset-tests.ps1 -BuildDirectory build/tests -Filter "GPU pending"
```

Equivalent CMake commands:

```powershell
cmake -S . -B build/tests -DREFD2D_BUILD_TESTS=ON
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release --output-on-failure
```

Coverage includes SDR/HDR pixel values, repeated resolution changes, rendering after
callback exceptions, image loading inside a draw batch, image reuse across devices,
concurrent font-cache access, six swapchain buffers, device removal,
stable cached HUD composition while Lua records a frame or loads graphics resources, reset while a Lua draw is paused, and replacement of a flip-model swapchain
for the same window while an old renderer is retained. A real queue wait creates pending GPU work; a delegating test queue
injects frame/tail Signal failures while forwarding actual GPU commands.

The reset tests drain the host queue before ResizeBuffers, as a graphics application
must. They verify the plugin's resource ownership and synchronization; they do not
claim to reproduce an unspecified game crash. Continuity tests clear the scene and
rotate swapchain buffers before checking pixels, so old backbuffer contents cannot
hide a skipped HUD composition.
