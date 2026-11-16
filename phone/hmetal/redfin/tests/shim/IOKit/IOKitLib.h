// A tiny stand-in for IOKit's user side, enough for native_adreno_transport.cpp to compile and run on Linux against an
// in-process fake service (tests/fake_native_adreno.cpp). Only the calls the transport makes; the types as Darwin's.
#pragma once
#include <cstddef>
#include <cstdint>

#include <mach/mach.h>

typedef int IOReturn;
typedef uint32_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef uint32_t CFMutableDictionaryRef_;
typedef void* CFMutableDictionaryRef;

constexpr mach_port_t kIOMainPortDefault = 0;
constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnError = int(0xe00002bc);
constexpr IOReturn kIOReturnNoMemory = int(0xe00002bd);
constexpr IOReturn kIOReturnNoResources = int(0xe00002be);
constexpr IOReturn kIOReturnBadArgument = int(0xe00002c2);
constexpr IOReturn kIOReturnUnsupported = int(0xe00002c7);
constexpr IOReturn kIOReturnVMError = int(0xe00002c8);
constexpr IOReturn kIOReturnBusy = int(0xe00002d5);
constexpr IOReturn kIOReturnTimeout = int(0xe00002d6);
constexpr IOReturn kIOReturnNotReady = int(0xe00002d8);
constexpr IOReturn kIOReturnNotFound = int(0xe00002f0);

enum : uint32_t {
    kIOMapAnywhere = 0x00000001,
    kIOMapWriteCombineCache = 0x00000400,
};

CFMutableDictionaryRef IOServiceMatching(const char* name);
io_service_t IOServiceGetMatchingService(mach_port_t main, CFMutableDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t* connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void* input, size_t inputSize, void* output,
                                        size_t* outputSize);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask, mach_vm_address_t* atAddress,
                                   mach_vm_size_t* ofSize, uint32_t options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask, mach_vm_address_t atAddress);
