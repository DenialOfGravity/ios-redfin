#include <cstdlib>
#include <cstring>
#include <string>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "test.hpp"

// (redfin) the chip the fake GPU is: --chip a620 / a635 first on the command line, or HMETAL_CHIP; the A635 by default.
// The driver's chip follows the fake's (Gpu's constructor selects it), so the whole suite runs as that chip.
int main(int argc, char** argv) {
    const char* want = getenv("HMETAL_CHIP");
    if (argc > 2 && strcmp(argv[1], "--chip") == 0) {
        want = argv[2];
        argv += 2;
        argc -= 2;
    }
    using namespace hakios::metal;
    if (want != nullptr && want[0] != 0) {
        if (strcmp(want, "a620") == 0) FakeGpu::defaultChip() = &a6xx::kChipA620;
        else if (strcmp(want, "a635") == 0) FakeGpu::defaultChip() = &a6xx::kChipA635;
        else {
            std::fprintf(stderr, "unknown chip %s (a620, a635)\n", want);
            return 2;
        }
    }
    a6xx::selectChip(*FakeGpu::defaultChip());
    std::printf("chip: %s\n", FakeGpu::defaultChip()->name);
    int ran = 0, skipped = 0;
    for (const test::Case& c : test::cases()) {
        if (argc > 1 && strstr(c.name, argv[1]) == nullptr) {
            continue;
        }
        // (redfin) HMETAL_TEST_SKIP=a,b: cases whose names hold one of these are not run (counted as skipped)
        if (const char* skip = getenv("HMETAL_TEST_SKIP")) {
            bool out = false;
            for (const char* p = skip; *p != 0;) {
                const char* e = strchr(p, ',');
                const std::string part(p, e ? size_t(e - p) : strlen(p));
                if (!part.empty() && strstr(c.name, part.c_str()) != nullptr) out = true;
                p = e ? e + 1 : p + part.size();
            }
            if (out) {
                std::printf("skip %s\n", c.name);
                skipped++;
                continue;
            }
        }
        int before = test::failures();
        c.run();
        std::printf("%s %s\n", test::failures() == before ? "ok  " : "FAIL", c.name);
        ran++;
    }
    std::printf("%d tests, %d failed checks%s\n", ran, test::failures(), skipped ? (", " + std::to_string(skipped) + " skipped").c_str() : "");
    return test::failures() == 0 ? 0 : 1;
}
