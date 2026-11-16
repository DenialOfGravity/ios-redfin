#include "hakios-metal/pm4.hpp"

#include <cassert>
#include <cstring>
#include <mutex>

namespace hakios::metal::pm4 {

Header decode(uint32_t word) {
    Header h;
    switch (word >> 28) {
    case 4:
        h.count = word & 0x7f;
        h.reg = (word >> 8) & 0x3ffff;
        if (((word >> 7) & 1) == oddParity(h.count) && ((word >> 27) & 1) == oddParity(h.reg)) {
            h.kind = Header::kRegisters;
        }
        break;
    case 7:
        h.count = word & 0x3fff;
        h.opcode = (word >> 16) & 0x7f;
        if (((word >> 15) & 1) == oddParity(h.count) && ((word >> 23) & 1) == oddParity(h.opcode)) {
            h.kind = Header::kCommand;
        }
        break;
    default:
        break;
    }
    return h;
}

void Stream::command(uint32_t opcode, uint32_t count) {
    assert(count <= kMaxType7Count);
    words_.push_back(type7(opcode, count));
}

void Stream::registers(uint32_t reg, const uint32_t* values, uint32_t count) {
    // more than a header can count goes as several packets, each starting where the one before stopped
    while (count > 0) {
        uint32_t n = count < kMaxType4Count ? count : kMaxType4Count;
        uint32_t* w = grow(1 + n);   // (ws-hm-int13: was a push_back and an insert)
        w[0] = type4(reg, n);
        if (n != 0) std::memcpy(w + 1, values, size_t(n) * 4);
        reg += n;
        values += n;
        count -= n;
    }
}

void Stream::nop(uint32_t words) {
    command(kNop, words);
    words_.insert(words_.end(), words, 0u);
}

void Stream::waitForIdle() { command(kWaitForIdle, 0); }
void Stream::waitMemWrites() { command(kWaitMemWrites, 0); }
void Stream::waitForMe() { command(kWaitForMe, 0); }

void Stream::memWrite(uint64_t a, const uint32_t* values, uint32_t count) {
    while (count > 0) {
        uint32_t n = count < kMaxType7Count - 2 ? count : kMaxType7Count - 2;
        command(kMemWrite, 2 + n);
        address(a);
        words_.insert(words_.end(), values, values + n);
        a += 4ull * n;
        values += n;
        count -= n;
    }
}

void Stream::memcpy(uint64_t destination, uint64_t source, uint32_t words) {
    command(kMemcpy, 5);
    words_.push_back(words);
    address(source);
    address(destination);
}

void Stream::indirect(uint64_t a, uint32_t words) {
    command(kIndirectBuffer, 3);
    address(a);
    words_.push_back(words);
}

void Stream::packet(uint32_t opcode, const uint32_t* words, uint32_t count) {
    assert(count <= kMaxType7Count);
    uint32_t* w = grow(1 + size_t(count));   // (ws-hm-int13: was a push_back and an insert)
    w[0] = type7(opcode, count);
    if (count != 0) std::memcpy(w + 1, words, size_t(count) * 4);
}

// (ws-hm-int13) at most four streams' storage kept, the largest; none bigger than 4 M words
namespace {
std::mutex gRecycleLock;
std::vector<std::vector<uint32_t>> gRecycled;
}  // namespace
void Stream::recycle(Stream&& s) {
    std::vector<uint32_t> v = std::move(s.words_);
    if (v.capacity() < 1024 || v.capacity() > (size_t(4) << 20)) return;
    v.clear();
    std::lock_guard<std::mutex> g(gRecycleLock);
    if (gRecycled.size() < 4) gRecycled.push_back(std::move(v));
}
void Stream::adoptRecycled() {
    std::vector<uint32_t> v;
    {
        std::lock_guard<std::mutex> g(gRecycleLock);
        if (gRecycled.empty()) return;
        v = std::move(gRecycled.back());
        gRecycled.pop_back();
    }
    if (v.capacity() <= words_.capacity()) return;
    v.assign(words_.begin(), words_.end());
    words_.swap(v);
}

}  // namespace hakios::metal::pm4
