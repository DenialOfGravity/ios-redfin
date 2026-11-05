// An in-process stand-in for our kernel's NativeAdreno v2 user client (xnu/patches/0115 + 0904 + 1038, the v2Method
// rules: struct sizes, 4 KiB alignment, the client's window, BoMap busy, imports without a CPU token), behind the IOKit
// shim of tests/shim. The GPU behind it is upstream's FakeGpu (its packet interpreter), with each BO placed at the
// address the client chose (FakeGpu::placeAt).
//
// Imported pages are modelled as the A620 sees them: the GPU reads and writes its own copy (a FakeGpu buffer), the
// CPU its cacheable pages, and the two meet only through the transport's cache maintenance (nativeadreno::gCacheHook:
// a clean copies CPU -> GPU, an invalidate after the fence GPU -> CPU). A transport that forgot either gets stale data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "hakios-metal/fake_gpu.hpp"

namespace fakena {

struct Counts {
    uint32_t allocs = 0, imports = 0, maps = 0, frees = 0, submits = 0, waits = 0, cpuMaps = 0, queuesMade = 0;
    uint32_t cleans = 0, invalidates = 0, unsupported = 0;
    uint64_t lastSubmitIova = 0, lastSubmitBytes = 0;
};

// Install a service (IOAcceleratorES) over FAKE: the next IOServiceOpen connects to it (several may live at once, one
// connection each); uninstall(FAKE) removes it, uninstall() all. counts() are the newest service's.
void install(hakios::metal::FakeGpu* fake);
void uninstall(hakios::metal::FakeGpu* fake = nullptr);
Counts counts();
uint64_t windowStart();
uint64_t windowSize();

// A fake IOSurface for IOSurfaceLookup: page-aligned memory of the caller's (the CPU's view)
struct Surface {
    uint32_t id, width, height, rowBytes, pixelFormat;
    std::vector<uint8_t>* storage;
    uint8_t* base;
    size_t allocSize;
};
uint8_t* addSurface(uint32_t id, uint32_t width, uint32_t height, uint32_t rowBytes, uint32_t pixelFormat);
void removeSurfaces();
int surfaceRefs(uint32_t id);   // references the transport holds (lookups not yet released)

// What the GPU's copy of an import holds now (its FakeGpu buffer), for a test to tell a clean from none
std::vector<uint8_t> gpuViewOfImport(const void* address, size_t bytes);

}  // namespace fakena

// For upstream's tests built with -DHMETAL_TEST_TRANSPORT=hmetalViaNativeAdreno: the FakeGpu behind our transport
std::unique_ptr<hakios::metal::Transport> hmetalViaNativeAdreno(std::unique_ptr<hakios::metal::FakeGpu> fake);
