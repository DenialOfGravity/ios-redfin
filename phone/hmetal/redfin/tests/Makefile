# phone/hmetal/redfin on the host: the NativeAdreno transport against an in-process fake of our kernel's v2 client,
# through a tiny IOKit/IOSurface shim (shim/). No iOS SDK needed.
#
#   make test      transport_test (the transport's own cases), as the A635 and as the A620
#   make e2e       upstream's command, compute and render suites with every Gpu on the transport -> fake service ->
#                  upstream's FakeGpu packet interpreter (HMETAL_TEST_TRANSPORT), as both chips
#   make all       both
CXX      ?= c++
HERE     := $(abspath .)
UP        = $(abspath ../../upstream)
CXXFLAGS ?= -O1 -g
override CXXFLAGS += -std=c++17 -Wall -Wextra -Werror -pthread -I$(HERE)/shim -I$(HERE)/.. -I$(HERE) -I$(UP)/include -I$(UP)/tests -I$(abspath ../../../turnip)
CHIP     ?= a635 a620
OUT       = out
CORE      = $(addprefix $(UP)/,src/pm4.cpp src/gpu.cpp src/command.cpp src/compute.cpp src/texture.cpp src/render.cpp \
	src/link.cpp src/compiler.cpp src/metallib.cpp src/c_api.cpp src/fake_gpu.cpp)
OURS      = ../native_adreno_transport.cpp fake_native_adreno.cpp
DEPS      = $(wildcard ../*.hpp ../*.cpp *.hpp *.cpp shim/*/*.h $(UP)/include/hakios-metal/*.hpp $(UP)/src/*.cpp $(UP)/tests/*.cpp $(UP)/tests/*.hpp)

.PHONY: all test e2e clean
all: test e2e

$(OUT)/core.a: $(CORE) $(wildcard $(UP)/include/hakios-metal/*.hpp)
	@mkdir -p $(OUT)/core
	cd $(OUT)/core && for f in $(CORE); do $(CXX) $(CXXFLAGS) -c $$f || exit 1; done
	rm -f $@ && ar rcs $@ $(OUT)/core/*.o

$(OUT)/transport_test: transport_test.cpp $(OURS) $(UP)/tests/main.cpp $(OUT)/core.a $(DEPS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ transport_test.cpp $(OURS) $(UP)/tests/main.cpp $(OUT)/core.a

$(OUT)/e2e: $(OURS) $(OUT)/core.a $(DEPS)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -DHMETAL_TEST_TRANSPORT=hmetalViaNativeAdreno -o $@ $(OURS) $(addprefix $(UP)/tests/,command_test.cpp \
		compute_test.cpp render_test.cpp main.cpp) $(OUT)/core.a

test: $(OUT)/transport_test
	@for c in $(CHIP); do echo "== transport $$c"; $(OUT)/transport_test --chip $$c > $(OUT)/transport-$$c.log 2>&1; r=$$?; \
		grep -E "^chip:|FAIL|tests, " $(OUT)/transport-$$c.log; [ $$r -eq 0 ] || exit $$r; done

# The surface cases make their surfaces with FakeGpu::addSurface (the hakios kernel's table, request 0x7e answered by
# the fake itself) and read them back with surfaceBytes; through our transport 0x7e is IOSurfaceLookup + BoImport, which
# that table is not behind. transport_test covers the transport's surface path (import, describe, clean, invalidate).
E2E_SKIP  = iosurface,two_plane_surface,a_surface_,surface_reads,reads_a_surface
e2e: $(OUT)/e2e
	@for c in $(CHIP); do echo "== e2e $$c"; HMETAL_TEST_SKIP=$(E2E_SKIP) $(OUT)/e2e --chip $$c > $(OUT)/e2e-$$c.log 2>&1; r=$$?; \
		grep -E "^chip:|FAIL|tests, " $(OUT)/e2e-$$c.log; [ $$r -eq 0 ] || exit $$r; done

clean:
	rm -rf $(OUT)
