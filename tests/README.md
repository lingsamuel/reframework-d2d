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

Composition tests inspect actual scene-resource ownership: SDR/scRGB allocate no
scene texture, while HDR10 lazily creates one stable texture shared across frames.
Native D3D12 pixel readbacks cover colored backgrounds, transparent HUD pixels,
negative and extended scRGB values, and 80/200/1000-nit paper white. A queue gate
keeps six composition submissions in flight across SDR/HDR10 switches; each result
is captured before the next frame overwrites the target. The test calls the same
composition recording function as Present, so HDR10 coverage does not require an
HDR monitor. Expected HDR10 values are calculated by a double-precision CPU PQ
reference, including the scene's R10 quantization.

When the D3D12 debug layer is installed it is enabled before device creation, and
the composition tests reject error/corruption messages. Only the existing
`REFLECTSHAREDPROPERTIES_INVALIDOBJECT` probe from successful D3D11On12 wrapping
is excluded during fixture initialization; that ID is still an error during drawing.
These checks validate resource reduction and pixel semantics, not game frame-rate gains.


Frame handoff ownership tests run as the separate `frame-mailbox-tests` CTest target.
They check held-slot stability, latest publication, zero frame copies, producer-side
resource recycling, and 100,000 concurrent publications. The WARP suite also checks
that the command vector and long string keep the same addresses across handoff, and
that a half-built Lua batch never reaches Present.

The mailbox is independent of DirectX and can also be checked under Linux:

```sh
g++ -std=c++20 -O1 -g -fsanitize=thread -pthread -Isrc tests/FrameMailboxTests.cpp -o /tmp/refd2d-mailbox-tests
/tmp/refd2d-mailbox-tests
```

Optional CPU handoff microbenchmark (not a game frame-rate benchmark):

```powershell
cmake --build build/tests --config Release --target drawlist-handoff-benchmark
./build/tests/Release/drawlist-handoff-benchmark.exe
```
