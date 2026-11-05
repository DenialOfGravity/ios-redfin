// hakios-metal's Transport (the msm DRM requests of hakios's HakiosGPU) on top of our kernel's GPU user client: the
// NativeAdreno ABI v1 (phone/turnip/NativeAdrenoABI.h) that SweetAccelerator's v2 client speaks to Turnip's darwin
// backend (xnu/patches/0115, 0904, 1038). It is what lets the vendored hakios-metal core (phone/hmetal/upstream) run
// on redfin's kernel without a HakiosGPU service.
//
// Conventions copied from phone/turnip/v2test.cpp and the darwin backend: the service is IOAcceleratorES, opened with
// type 0x4147; every method is tunnelled through selector 0 as {'NTNL', method, payload}; buffers are BoAlloc'd 4 KiB
// aligned (align_log2 12, not contiguous) and BoMap'd at an address this client chooses inside its own window
// (DeviceInfo va_start/va_size) with permissions read|write, writeback 0, shareability 2; the CPU sees a buffer
// through BoCpuMap's token and IOConnectMapMemory64(kIOMapAnywhere | kIOMapWriteCombineCache).
//
// Cache maintenance (our kernel does none, and the A620 does not snoop the CPU): imported pages (an IOSurface's, a
// caller's userptr) keep the caller's cacheable mapping while the GPU maps them non-cacheable. So every imported
// buffer a submission names is cleaned and invalidated (dc civac over its whole import) right before the submission,
// and every imported buffer a submission names as WRITTEN is cleaned and invalidated again once that submission's
// fence is seen to pass (WAIT_FENCE, CPU_PREP). Driver-made buffers are write-combined for the CPU and need none.
// phone/metal2 (NativeBridgeMetalFrontend.mm flushSurfaces) does the same with seeds to skip unchanged surfaces;
// this does every one every time, which is correct and slower.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "hakios-metal/gpu.hpp"

namespace hakios::metal {

// The GPU through our kernel (IOAcceleratorES, NativeAdreno ABI v1). nullptr with *why when there is no such service,
// it will not open, or it answers another ABI version.
std::unique_ptr<Transport> openNativeAdrenoGpu(std::string* why);

// The same over a connection already open (tests: an in-process fake service through the IOKit shim). The transport
// owns the connection and closes it.
std::unique_ptr<Transport> nativeAdrenoTransportOn(unsigned int connection, std::string* why);

namespace nativeadreno {
// The request upstream sends for a userptr buffer (hakios.gpu.cpp kHakiosUserMemory): the caller's own whole pages.
constexpr uint32_t kUserMemory = 0x7d;
struct UserMemory { uint64_t address, size; uint32_t handle, flags; };
static_assert(sizeof(UserMemory) == 24, "hakios.gpu.cpp struct hakios_user_memory");

// The highest GPU clock redfin's kernel programs (xnu/patches/0904 HFI: 275/400/500/625 MHz): GET_PARAM MAX_FREQ.
// Not read from the kernel, which has no such method in ABI v1.
constexpr uint64_t kMaxFrequencyHz = 625000000;

// Test hook: called with every range the transport cleans/invalidates (BEFORE: before a submission; else after a
// fence). On arm64 the dc civac is done as well; elsewhere only the hook runs.
extern void (*gCacheHook)(const void* address, size_t bytes, bool beforeSubmit);
}  // namespace nativeadreno

}  // namespace hakios::metal
