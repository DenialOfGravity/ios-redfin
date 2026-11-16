// (redfin) Which Transport a test's Gpu gets over its FakeGpu: the fake itself (upstream's way), or, built with
// -DHMETAL_TEST_TRANSPORT=fn, fn(fake): phone/hmetal/redfin's NativeAdreno transport in front of the same fake, so
// that the suites run end to end through it (phone/hmetal/redfin/tests/Makefile, `make e2e`).
#pragma once
#include <memory>

#include "hakios-metal/fake_gpu.hpp"

#ifdef HMETAL_TEST_TRANSPORT
std::unique_ptr<hakios::metal::Transport> HMETAL_TEST_TRANSPORT(std::unique_ptr<hakios::metal::FakeGpu> fake);
#define HMETAL_TRANSPORT_OF(f) HMETAL_TEST_TRANSPORT(std::move(f))
#else
#define HMETAL_TRANSPORT_OF(f) std::move(f)
#endif
