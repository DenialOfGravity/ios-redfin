// (redfin hm-cache) hm-seed: the stage cache's host side.
//
//   hm-seed build CACHE SEED [--chip ID]
//       Every stage the phone compiled into CACHE (its src/*.src manifests and bc/*.bc bitcode, written by compileStage
//       with HM_CACHE_MANIFEST=1: pull /var/local/hmetal/cache/{src,bc}) compiled again here into SEED, filed under the
//       names this build's key gives them (HAKIOS_METAL_CACHE_FORMAT + the Mesa id). SEED goes to the phone as
//       /var/local/hmetal/cache-seed (read-only: HAKIOS_METAL_CACHE_SEED, after the writable cache). A manifest whose
//       stage name this host computes differently under the same key (a layout or hash difference between this host
//       and the phone) is counted and said: those seeds would never be found.
//   hm-seed time LIBRARY.metallib FUNCTION [--chip ID] [-c INDEX=VALUE]... [--known BINDING:HEX] [-n N]
//       One function compiled N times (default 3) with no cache, then (with --known) specialised for the bytes: the
//       wall and CPU ms of each and how much of it the front end and the folding took.
//
// Nothing of Apple's is written by this tool outside SEED; the bitcode is read where it lies (the PC's project folder).
//
// SPDX-License-Identifier: MIT

#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "hakios-metal/a6xx.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/metallib.hpp"

using namespace hakios::metal;

namespace {
bool readAll(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

double ms(const timespec& a, const timespec& b) { return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6; }

bool selectChipArg(const char* arg) {
    const a6xx::ChipInfo* c = a6xx::chipFor(uint32_t(std::strtoul(arg, nullptr, 0)));
    if (c == nullptr) {
        std::fprintf(stderr, "hm-seed: no chip %s\n", arg);
        return false;
    }
    a6xx::selectChip(*c);
    return true;
}

int build(const std::string& cache, const std::string& seed) {
    setenv("HAKIOS_METAL_CACHE", seed.c_str(), 1);
    unsetenv("HAKIOS_METAL_CACHE_SEED");
    setenv("HM_CACHE_MANIFEST", "0", 1);
    setenv("HM_STAGE_MEMO", "0", 1);
    mkdir(seed.c_str(), 0755);
    DIR* d = opendir((cache + "/src").c_str());
    if (d == nullptr) {
        std::fprintf(stderr, "hm-seed: no %s/src (pull the phone's /var/local/hmetal/cache/src and bc)\n", cache.c_str());
        return 2;
    }
    unsigned seen = 0, made = 0, refused = 0, unreadable = 0, otherKey = 0, otherName = 0;
    timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (dirent* e; (e = readdir(d)) != nullptr;) {
        const std::string n = e->d_name;
        if (n.size() < 5 || n.compare(n.size() - 4, 4, ".src") != 0) continue;
        seen++;
        StageManifest m;
        std::string bc;
        if (!stageManifestRead(cache + "/src/" + n, &m) || !readAll(cache + "/bc/" + m.bitcodeName, &bc) || bc.size() != m.bitcodeSize) {
            unreadable++;
            continue;
        }
        const std::string& key = m.key;
        const std::string& stageName = m.stageName;
        const FunctionConstants& constants = m.constants;
        const uint32_t iboBase = m.iboBase;
        const bool hasKnown = m.hasKnown;
        const KnownBuffer& known = m.known;
        const KnownBuffer* kp = hasKnown ? &known : nullptr;
        const std::string here = stageCacheFileName(bc.data(), bc.size(), constants, iboBase, kp);
        if (key != stageCacheKeyText()) {
            otherKey++;   // made under another format or Mesa: compiled anew under ours (a new name)
        } else if (here != "/" + stageName) {
            if (otherName++ < 5) std::printf("hm-seed: %s: the phone filed it as %s, this host as %s (same key %s): layout or hash differ\n",
                                              n.c_str(), stageName.c_str(), here.c_str() + 1, key.c_str());
        }
        std::string why;
        if (compileStage(bc.data(), bc.size(), &why, constants, iboBase, kp) != nullptr) {
            made++;
        } else {
            refused++;
            std::printf("hm-seed: %s refused: %s\n", n.c_str(), why.c_str());
        }
    }
    closedir(d);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    std::printf("hm-seed: key \"%s\": %u manifests, %u stages made into %s, %u refused, %u unreadable, %u from another key, "
                "%u named otherwise by this host; %.0f s\n", stageCacheKeyText(), seen, made, seed.c_str(), refused, unreadable, otherKey,
                otherName, ms(t0, t1) / 1e3);
    return refused == 0 && unreadable == 0 && otherName == 0 ? 0 : 1;
}

int timeOne(int argc, char** argv) {
    std::string libPath, fn;
    FunctionConstants constants;
    KnownBuffer known;
    bool hasKnown = false;
    int n = 3;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--chip") && i + 1 < argc) {
            if (!selectChipArg(argv[++i])) return 2;
        } else if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            unsigned index = 0;
            unsigned long long value = 0;
            if (sscanf(argv[++i], "%u=%llu", &index, &value) != 2) return 2;
            constants.push_back(FunctionConstant{index, value});
        } else if (!strcmp(argv[i], "--known") && i + 1 < argc) {
            const char* a = argv[++i];
            known.binding = unsigned(strtoul(a, nullptr, 0));
            const char* hex = strchr(a, ':');
            for (hex = hex ? hex + 1 : ""; hex[0] && hex[1]; hex += 2) {
                char b[3] = {hex[0], hex[1], 0};
                known.bytes.push_back(uint8_t(strtoul(b, nullptr, 16)));
            }
            hasKnown = true;
        } else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            n = atoi(argv[++i]);
        } else if (libPath.empty()) {
            libPath = argv[i];
        } else {
            fn = argv[i];
        }
    }
    const bool useCache = getenv("HM_SEED_USE_CACHE") != nullptr;   // (hm-seed compile: the cache as the environment sets it)
    if (!useCache) {
        unsetenv("HAKIOS_METAL_CACHE");
        unsetenv("HAKIOS_METAL_CACHE_SEED");
        setenv("HM_STAGE_MEMO", "0", 1);
    }
    std::string data;
    MetalLibrary lib;
    if (!readAll(libPath, &data) || !lib.parse(reinterpret_cast<const uint8_t*>(data.data()), data.size())) {
        std::fprintf(stderr, "hm-seed: %s: not a metallib\n", libPath.c_str());
        return 2;
    }
    const MetalFunction* f = lib.find(fn);
    const uint8_t* bc = nullptr;
    uint64_t len = 0;
    if (f == nullptr || !lib.bitcode(*f, &bc, &len)) {
        std::fprintf(stderr, "hm-seed: no function %s\n", fn.c_str());
        return 2;
    }
    std::printf("hm-seed: %s: %llu bytes of bitcode, %zu constants\n", fn.c_str(), (unsigned long long)len, constants.size());
    for (int pass = 0; pass < (hasKnown ? 2 : 1); pass++) {
        for (int i = 0; i < n; i++) {
            const CompileStats a = compileStats();
            timespec w0, w1, c0, c1;
            clock_gettime(CLOCK_MONOTONIC, &w0);
            clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
            std::string why;
            auto s = compileStage(bc, len, &why, constants, 0, pass ? &known : nullptr);
            clock_gettime(CLOCK_MONOTONIC, &w1);
            clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
            const CompileStats b = compileStats();
            if (useCache) std::printf("  hits %u (memo %u, seed %u), misses %u\n", b.hits - a.hits, b.memoHits - a.memoHits, b.seedHits - a.seedHits, b.misses - a.misses);
            std::printf("  %s #%d: %s %zu words, wall %.1f ms, cpu %.1f ms; front end %.1f, folding %.1f, rest %.1f ms\n",
                        pass ? "specialised" : "whole", i, s ? "ok" : why.c_str(), s ? s->words.size() : size_t(0), ms(w0, w1), ms(c0, c1),
                        (b.translateNs - a.translateNs) / 1e6, (b.foldNs - a.foldNs) / 1e6,
                        ms(w0, w1) - (b.translateNs - a.translateNs) / 1e6 - (b.foldNs - a.foldNs) / 1e6);
        }
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && !strcmp(argv[1], "build")) {
        for (int i = 4; i + 1 < argc; i++) {
            if (!strcmp(argv[i], "--chip") && !selectChipArg(argv[i + 1])) return 2;
        }
        if (!hasCompiler()) return std::fprintf(stderr, "hm-seed: no compiler in this build (MESA=1)\n"), 2;
        return build(argv[2], argv[3]);
    }
    if (argc >= 3 && !strcmp(argv[1], "rewarm")) {   // the phone's pre-warm, here: HAKIOS_METAL_CACHE=DIR hm-seed rewarm [--chip ID]
        if (argc >= 4 && !strcmp(argv[2], "--chip") && !selectChipArg(argv[3])) return 2;
        if (!hasCompiler()) return std::fprintf(stderr, "hm-seed: no compiler in this build (MESA=1)\n"), 2;
        const RewarmCounts n = stageCacheRewarm(nullptr, nullptr);
        std::printf("hm-seed: rewarm under \"%s\": %u manifests, %u present, %u made, %u failed, %u unreadable\n", stageCacheKeyText(),
                    n.manifests, n.present, n.made, n.failed, n.unreadable);
        return n.failed == 0 && n.unreadable == 0 ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "compile")) {   // as time, through the cache the environment names (HM_CACHE_VERIFY etc.)
        if (!hasCompiler()) return std::fprintf(stderr, "hm-seed: no compiler in this build (MESA=1)\n"), 2;
        setenv("HM_SEED_USE_CACHE", "1", 1);
        const int r = timeOne(argc - 2, argv + 2);
        sleep(getenv("HM_CACHE_VERIFY") != nullptr ? 3 : 0);   // (the verify threads)
        return r;
    }
    if (argc >= 4 && !strcmp(argv[1], "time")) {
        if (!hasCompiler()) return std::fprintf(stderr, "hm-seed: no compiler in this build (MESA=1)\n"), 2;
        return timeOne(argc - 2, argv + 2);
    }
    std::fprintf(stderr, "usage: hm-seed build CACHE SEED [--chip ID] | hm-seed time LIBRARY FUNCTION [--chip ID] [-c I=V]... [--known B:HEX] [-n N]\n");
    return 2;
}
