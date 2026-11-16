// (tests) IOSurface's getters over a fake surface table (tests/fake_native_adreno.cpp addFakeSurface)
#pragma once
#include <cstddef>
#include <cstdint>
typedef struct FakeIOSurface* IOSurfaceRef;
typedef uint32_t IOSurfaceID;
typedef const void* CFTypeRef;
IOSurfaceRef IOSurfaceLookup(IOSurfaceID id);   // +1 reference
void CFRelease(CFTypeRef ref);
IOSurfaceID IOSurfaceGetID(IOSurfaceRef s);
void* IOSurfaceGetBaseAddress(IOSurfaceRef s);
size_t IOSurfaceGetAllocSize(IOSurfaceRef s);
size_t IOSurfaceGetWidth(IOSurfaceRef s);
size_t IOSurfaceGetHeight(IOSurfaceRef s);
size_t IOSurfaceGetBytesPerRow(IOSurfaceRef s);
uint32_t IOSurfaceGetPixelFormat(IOSurfaceRef s);
size_t IOSurfaceGetPlaneCount(IOSurfaceRef s);
void* IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s, size_t plane);
size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s, size_t plane);
uint32_t IOSurfaceGetSeed(IOSurfaceRef s);   // (fakena::setSurfaceSeed; else a new value every call)
