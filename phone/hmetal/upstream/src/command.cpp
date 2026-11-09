#include "hakios-metal/command.hpp"
#include "hakios-metal/texture.hpp"
#include "hakios-metal/render.hpp"

#include <cstdio>
#include <sstream>
#include <string>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <map>
#include <mutex>
#include <cstdlib>
#include <cstring>

namespace hakios::metal {

namespace {
constexpr size_t kCommandBo = 64 * 1024;           // one command buffer's packets, at most (grown when needed)
constexpr int64_t kPatienceNs = 5000000000;        // a fence not passed in 5 s: the GPU is hung, the buffer is an error
constexpr uint32_t kFillHead = 64;                 // words written by CP_MEM_WRITE before the copies double them
// The most one CP_MEMCPY moves. On the phone (27 October) 125 000 words in one packet took 4.2 ms, and 4 Mi words (16 MB)
// in one packet hung the GPU ("no answer" from the kernel's driver, and the system after it). Cut into packets of this
// size, each waited for before the next, the same copy is a row of small ones.
constexpr uint32_t kMemcpyMax = 64 * 1024;
}  // namespace

void appendStream(pm4::Stream& to, const pm4::Stream& from);   // render.cpp's: a pass of its own onto the buffer's stream

namespace {
std::mutex gAddressLock;
std::map<uint64_t, std::weak_ptr<Buffer>> gByAddress;   // by gpuAddress, every Buffer newBuffer made and still alive

std::shared_ptr<Buffer> addressed(Buffer* raw) {
    const uint64_t at = raw->gpuAddress();
    std::shared_ptr<Buffer> b(raw, [at](Buffer* x) {
        {
            std::lock_guard<std::mutex> g(gAddressLock);
            auto it = gByAddress.find(at);
            if (it != gByAddress.end() && it->second.expired()) gByAddress.erase(it);
        }
        delete x;
    });
    std::lock_guard<std::mutex> g(gAddressLock);
    gByAddress[at] = b;
    return b;
}
}  // namespace

std::shared_ptr<Buffer> bufferAt(uint64_t address, uint64_t* offset) {
    std::lock_guard<std::mutex> g(gAddressLock);
    auto it = gByAddress.upper_bound(address);
    if (it == gByAddress.begin()) return nullptr;
    --it;
    std::shared_ptr<Buffer> b = it->second.lock();
    if (b == nullptr || address - it->first >= b->length()) return nullptr;
    *offset = address - it->first;
    return b;
}

std::atomic<uint32_t> gBuffersMade{0}, gBuffersReused{0};
std::atomic<uint64_t> gBytesMade{0};

std::shared_ptr<Buffer> newBuffer(Gpu& gpu, uint64_t length, std::string* why) {
    gBuffersMade++;
    gBytesMade += length;
    int error = 0;
    std::unique_ptr<Bo> bo = gpu.newBo(length, kBoWriteCombined, &error);
    if (bo == nullptr) {
        *why = "the driver gave no buffer of " + std::to_string(length) + " bytes: " + strerror(error);
        return nullptr;
    }
    return addressed(new Buffer(std::move(bo), length));
}

namespace {
// One slab is 32 draw tables. Pieces are cut at 256 bytes, which is more than anything in them wants (128 for a
// program's preload and the border colour, 64 for a texture descriptor), and a piece of a page or more starts on a
// page, so that a table lies in memory as it did in a buffer of its own.
constexpr uint64_t kSlabBytes = 256 * 1024;
constexpr uint64_t kPieceAlign = 256;
constexpr uint64_t kPageAlign = 4096;
}  // namespace

bool CommandBuffer::piece(uint64_t bytes, Piece* out, std::string* why) {
    bytes = (std::max<uint64_t>(bytes, 1) + kPieceAlign - 1) & ~(kPieceAlign - 1);
    const uint64_t align = bytes >= kPageAlign ? kPageAlign : kPieceAlign;
    if (slab_ != nullptr) {
        slabUsed_ = std::min(kSlabBytes, (slabUsed_ + align - 1) & ~(align - 1));
    }
    if (bytes > kSlabBytes / 4) {
        // (a piece this large in a slab would leave most of the slab unused when the next one did not fit)
        out->buffer = queue_.takeBuffer(bytes, why);
        out->offset = 0;
        if (out->buffer != nullptr) slabs_.push_back(out->buffer);
    } else {
        if (slab_ == nullptr || kSlabBytes - slabUsed_ < bytes) {
            slab_ = queue_.takeSlab(kSlabBytes, why);
            slabUsed_ = 0;
            if (slab_ != nullptr) slabs_.push_back(slab_);
        }
        out->buffer = slab_;
        out->offset = slabUsed_;
        if (slab_ != nullptr) {
            slabUsed_ += bytes;
        }
    }
    if (out->buffer == nullptr) {
        return false;
    }
    if (out->offset == 0) {
        use(*out->buffer, true);
        keep(out->buffer);
    }
    return true;
}

uint64_t CommandBuffer::programAddress(const Kernel& k, std::string* why) {
    const uint32_t n = std::max<uint32_t>(k.numWords, 1);
    auto same = [&](const Uploaded& u) {
        if (u.words.size() != k.numWords) return false;
        for (uint32_t i = 0; i < k.numWords; i += 64) {
            if (u.words[i] != k.words[i]) return false;
        }
        return k.numWords == 0 || u.words.back() == k.words[k.numWords - 1];
    };
    const auto [first, last] = uploaded_.equal_range(k.words);
    for (auto it = first; it != last; ++it) {
        if (same(it->second)) return it->second.gpuAddress;
    }
    (void)n;
    std::shared_ptr<Buffer> b = queue_.programBuffer(k, why);
    if (b == nullptr) return 0;
    use(*b, false);
    keep(b);
    uploaded_.emplace(k.words, Uploaded{std::vector<uint32_t>(k.words, k.words + k.numWords), b->gpuAddress()});
    return b->gpuAddress();
}

namespace {
uint64_t sizeClass(uint64_t bytes) {
    uint64_t c = 4096;
    while (c < bytes) c <<= 1;
    return c;
}
}  // namespace

std::shared_ptr<Buffer> CommandQueue::takeBuffer(uint64_t bytes, std::string* why) {
    const uint64_t size = sizeClass(bytes);
    std::shared_ptr<Buffer> b;
    {
        std::lock_guard<std::mutex> g(poolLock_);
        auto it = freeBuffers_.find(size);
        if (it != freeBuffers_.end() && !it->second.empty()) {
            b = std::move(it->second.back());
            it->second.pop_back();
            freeBytes_ -= size;
        }
    }
    if (b == nullptr) return newBuffer(gpu_, size, why);
    gBuffersReused++;
    memset(b->contents(), 0, size);   // (as a new one comes from the kernel)
    return b;
}

void CommandQueue::giveBuffer(std::shared_ptr<Buffer> b) {
    // (only one no one else holds, of a size takeBuffer makes: a piece's buffer kept elsewhere is still someone's)
    if (b == nullptr || b.use_count() != 1 || b->contents() == nullptr || b->length() != sizeClass(b->length())) return;
    std::lock_guard<std::mutex> g(poolLock_);
    if (freeBytes_ + b->length() > (32ull << 20)) return;
    freeBytes_ += b->length();
    freeBuffers_[b->length()].push_back(std::move(b));
}

std::shared_ptr<Buffer> CommandQueue::takeSlab(uint64_t bytes, std::string* why) { return takeBuffer(bytes, why); }

void CommandQueue::giveSlabs(std::vector<std::shared_ptr<Buffer>>* slabs) {
    for (auto& b : *slabs) giveBuffer(std::move(b));
    slabs->clear();
}

std::shared_ptr<Buffer> CommandQueue::programBuffer(const Kernel& k, std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    const auto [first, last] = resident_.equal_range(k.words);
    for (auto it = first; it != last;) {
        const Resident& r = it->second;
        if (r.words.size() == k.numWords && std::memcmp(r.words.data(), k.words, 4ull * k.numWords) == 0) return r.buffer;
        residentBytes_ -= 4ull * r.words.size();
        it = resident_.erase(it);   // (the pointer's words have changed: a freed program's)
    }
    // (past 64 MB of programs, the ones kept are let go: a command buffer in flight keeps its own)
    if (residentBytes_ > (64ull << 20)) {
        resident_.clear();
        residentBytes_ = 0;
    }
    std::shared_ptr<Buffer> b = newBuffer(gpu_, 4ull * std::max<uint32_t>(k.numWords, 1), why);
    if (b == nullptr || b->contents() == nullptr) return nullptr;
    memcpy(b->contents(), k.words, 4ull * k.numWords);
    resident_.emplace(k.words, Resident{std::vector<uint32_t>(k.words, k.words + k.numWords), b});
    residentBytes_ += 4ull * k.numWords;
    return b;
}

namespace {
// the live surface buffers, by (GPU, kernel handle): the kernel gives this client one handle per surface, so two
// Buffers over it would close it twice. The deleter takes the lock too, so that a handle the kernel hands out again
// cannot be met by a Buffer still being destroyed.
std::mutex gSurfaceLock;
std::map<std::pair<const Gpu*, uint32_t>, std::weak_ptr<Buffer>> gSurfaceBuffers;
std::atomic<uint64_t> gSurfaceHandovers{0};
// (redfin hmv6) HM_SURFACE_HANDOVER=1 (default): a dying surface Buffer whose handle a new Buffer took leaves it open (0: closes it, as before)
bool surfaceHandoverOn() {
    static const bool on = [] { const char* e = getenv("HM_SURFACE_HANDOVER"); return e == nullptr || e[0] != '0'; }();
    return on;
}
}  // namespace

void (*gSurfaceDeleterHook)() = nullptr;
uint64_t surfaceHandovers() { return gSurfaceHandovers.load(); }

std::shared_ptr<Buffer> surfaceBuffer(Gpu& gpu, uint32_t surfaceId, SurfaceInfo* info, std::string* why) {
    std::lock_guard<std::mutex> g(gSurfaceLock);
    int error = 0;
    std::unique_ptr<Bo> bo = gpu.surfaceBo(surfaceId, info, &error);
    if (bo == nullptr) {
        *why = "IOSurface " + std::to_string(surfaceId) + ": the driver gave no buffer for it: " +
               (error == ENOENT ? std::string("no such surface") : error == ENOTSUP ? std::string("a surface around a program's own memory is not one the GPU takes") : strerror(error));
        return nullptr;
    }
    const auto key = std::make_pair(static_cast<const Gpu*>(&gpu), bo->handle());
    auto live = gSurfaceBuffers.find(key);
    if (live != gSurfaceBuffers.end()) {
        if (std::shared_ptr<Buffer> b = live->second.lock()) {
            gpu.disown(std::move(bo));   // the same handle as the live Buffer's Bo: closed once, by that Bo
            return b;
        }
    }
    const uint64_t length = bo->size();
    Gpu* owner = &gpu;
    std::shared_ptr<Buffer> b(new Buffer(std::move(bo), length), [key, owner](Buffer* x) {
        if (gSurfaceDeleterHook != nullptr) gSurfaceDeleterHook();
        std::lock_guard<std::mutex> g(gSurfaceLock);
        auto it = gSurfaceBuffers.find(key);
        if (it != gSurfaceBuffers.end() && it->second.expired()) {
            gSurfaceBuffers.erase(it);
        } else if (it != gSurfaceBuffers.end() && surfaceHandoverOn()) {
            // (redfin hmv6) Between this Buffer's last reference going (often the waiter thread, a completed command
            // buffer letting go) and this lock, surfaceBuffer met the entry expired and made a NEW Buffer over the same
            // kernel handle (the transport answers the same handle while the BO is open). Closing it here closed the
            // new Buffer's handle: every later submission naming that surface was refused with ENOENT (hmt7: the
            // display surfaces 630/631 after turning back to portrait, 'the driver refused the submission: No such file
            // or directory' from frame 993 until lock/unlock made new ones). The handle is the new Buffer's now.
            gSurfaceHandovers.fetch_add(1);
            owner->disown(x->takeBo());
        }
        delete x;
    });
    gSurfaceBuffers[key] = b;
    return b;
}

// --- CommandBuffer ----------------------------------------------------------------------------------------------

bool CommandBuffer::fail(const std::string& why) {
    if (error_.empty()) {
        error_ = why;
    }
    return false;
}

void CommandBuffer::stamp(uint32_t slot) {
    if (profileStamps_ == nullptr) {
        std::string why;
        profileStamps_ = queue_.takeBuffer(16 * kMaxProfiled, &why);
        if (profileStamps_ == nullptr) {
            profile_ = tuning_ = false;
            return;
        }
        memset(profileStamps_->contents(), 0, 16 * kMaxProfiled);
        keep(profileStamps_);
    }
    const uint64_t at = profileStamps_->gpuAddress() + 8ull * slot;
    const uint32_t w[3] = {a6xx::CP_REG_TO_MEM_0_REG(0x0980) | a6xx::CP_REG_TO_MEM_0_CNT(2) | a6xx::CP_REG_TO_MEM_0_64B, uint32_t(at),
                           uint32_t(at >> 32)};
    stream_.packet(a6xx::CP_REG_TO_MEM, w, 3);
    use(*profileStamps_, true);
}

std::shared_ptr<Buffer> CommandQueue::vscBuffer(std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    if (vsc_ == nullptr) vsc_ = newBuffer(gpu_, render::vscBytes(), why);
    return vsc_;
}

void CommandQueue::checkVsc() {
    std::lock_guard<std::mutex> g(lock_);
    if (vsc_ == nullptr || vsc_->contents() == nullptr) return;
    const uint32_t word = *reinterpret_cast<const volatile uint32_t*>(static_cast<const uint8_t*>(vsc_->contents()) + 32ull * render::kVscDrawPitch + render::kVscOverflowAt);
    if (word != 0) vscOverflowed_ = true;
}

bool CommandQueue::preferGmem(uint64_t key) {
    std::lock_guard<std::mutex> g(lock_);
    Tune& t = tune_[key];
    t.passes++;
    // (sysmem first: a kind of pass seen once may not come again, and an untried GMEM pass is the dearer gamble -- 5 ms
    // against 0.13 for a light one)
    if (t.runs[0] == 0) return false;
    if (t.runs[1] == 0) return true;
    const bool better = t.us[1] < t.us[0];
    // (the other mode again now and then, what a pass costs drifting: every 16th pass, or every 64th when the one is
    // dearer by half -- a light pass of CoreAnimation took 5 ms in GMEM against 0.13 in sysmem)
    const double ratio = better ? t.us[0] / t.us[1] : t.us[1] / t.us[0];
    return t.passes % (ratio > 1.5 ? 64 : 16) == 0 ? !better : better;
}

namespace {
uint64_t kindKey(uint64_t kind, uint32_t ordinal) { return kind * 0x100000001b3ull ^ ordinal; }
}  // namespace

uint32_t CommandQueue::lastDraws(uint64_t kind, uint32_t ordinal) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = lastDraws_.find(kindKey(kind, ordinal));
    return it == lastDraws_.end() ? 0 : it->second;
}

void CommandQueue::noteDraws(uint64_t kind, uint32_t ordinal, uint32_t draws) {
    std::lock_guard<std::mutex> g(lock_);
    lastDraws_[kindKey(kind, ordinal)] = draws;
    Peak& p = peakDraws_[kindKey(kind, ordinal)];
    if (draws >= p.draws || ++p.below >= 16) p.draws = draws, p.below = 0;
}

uint32_t CommandQueue::peakDraws(uint64_t kind, uint32_t ordinal) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = peakDraws_.find(kindKey(kind, ordinal));
    return it == peakDraws_.end() ? 0 : it->second.draws;
}

void CommandQueue::tuned(uint64_t key, bool gmem, double us) {
    if (!(us > 0)) return;
    std::lock_guard<std::mutex> g(lock_);
    Tune& t = tune_[key];
    double& avg = t.us[gmem ? 1 : 0];
    avg = t.runs[gmem ? 1 : 0]++ == 0 ? us : 0.75 * avg + 0.25 * us;
}

void CommandBuffer::stampStart(PassKind kind, uint32_t width, uint32_t height) {
    if (!(profile_ || tuning_) || profiled_.size() >= kMaxProfiled) return;
    { PassTime t{}; t.kind = kind, t.width = width, t.height = height; profiled_.push_back(std::move(t)); }
    stamp(uint32_t(2 * (profiled_.size() - 1)));
}

void CommandBuffer::stampEnd() {
    if (!(profile_ || tuning_) || profiled_.empty()) return;
    stamp(uint32_t(2 * (profiled_.size() - 1) + 1));
}

void CommandBuffer::drawStamp(uint32_t slot) {
    if (drawStamps_ == nullptr) {
        std::string why;
        drawStamps_ = queue_.takeBuffer(16 * kMaxProfiledDraws, &why);
        if (drawStamps_ == nullptr) {
            profileDraws_ = false;
            return;
        }
        memset(drawStamps_->contents(), 0, 16 * kMaxProfiledDraws);
        keep(drawStamps_);
    }
    const uint64_t at = drawStamps_->gpuAddress() + 8ull * slot;
    stream_.waitForIdle();
    const uint32_t w[3] = {a6xx::CP_REG_TO_MEM_0_REG(0x0980) | a6xx::CP_REG_TO_MEM_0_CNT(2) | a6xx::CP_REG_TO_MEM_0_64B, uint32_t(at),
                           uint32_t(at >> 32)};
    stream_.packet(a6xx::CP_REG_TO_MEM, w, 3);
    use(*drawStamps_, true);
}

std::vector<CommandBuffer::DrawTime> CommandBuffer::drawTimes() const {
    std::vector<DrawTime> out = drawsProfiled_;
    if (drawStamps_ == nullptr) return {};
    const auto* t = static_cast<const uint64_t*>(drawStamps_->contents());
    for (size_t i = 0; i < out.size(); i++) {
        out[i].us = t[2 * i + 1] > t[2 * i] ? double(t[2 * i + 1] - t[2 * i]) / 19.2 : 0.0;
    }
    return out;
}

std::vector<CommandBuffer::PassTime> CommandBuffer::passTimes() const {
    std::vector<PassTime> out = profiled_;
    if (profileStamps_ == nullptr) return {};
    const auto* t = static_cast<const uint64_t*>(profileStamps_->contents());
    for (size_t i = 0; i < out.size(); i++) {
        out[i].us = t[2 * i + 1] > t[2 * i] ? double(t[2 * i + 1] - t[2 * i]) / 19.2 : 0.0;
    }
    return out;
}

bool CommandBuffer::refuseWork(const std::string& why) {
    if (!skipRefused_) {
        return fail(why);
    }
    skipped_++;
    lastSkipped_ = why;
    return false;
}

void CommandBuffer::addCompletedHandler(std::function<void(CommandBuffer&)> handler) {
    if (status_ <= kEnqueued) {
        handlers_.push_back(std::move(handler));
    }
}

void CommandBuffer::enqueue() {
    Status expected = kNotEnqueued;
    status_.compare_exchange_strong(expected, kEnqueued);
}

void CommandBuffer::commit() {
    if (status_ > kEnqueued) {
        return;  // Metal asserts here; committing twice does nothing at all in this driver
    }
    queue_.submit(shared_from_this());
}

void CommandBuffer::waitUntilCompleted() {
    std::unique_lock<std::mutex> g(lock_);
    // (a buffer never committed is not waited for: Metal would wait forever, which helps nobody)
    done_.wait(g, [this] { return finished_ || status_ <= kEnqueued; });
}

CommandBuffer::~CommandBuffer() {
    // (a buffer never completed may be destroyed with the GPU still on it -- an error -- and its stamps are then not
    // given back; one that completed has had its readers)
    if (finished_) {
        queue_.giveBuffer(std::move(profileStamps_));
        queue_.giveBuffer(std::move(drawStamps_));
    }
}

void CommandBuffer::finish(Status s) {
    status_ = s;
    if (s == kCompleted && counts_.binnedPasses != 0) queue_.checkVsc();
    if (s == kCompleted && tuning_) {
        for (const PassTime& t : passTimes()) {
            if (t.tuneKey != 0) queue_.tuned(t.tuneKey, t.tiles != 0, t.us);
        }
    }
    for (auto& h : handlers_) {
        h(*this);
    }
    handlers_.clear();
    kept_.clear();
    slab_.reset();
    queue_.giveSlabs(&slabs_);   // (done with: the GPU has finished with this buffer, or never had it)
    keptAny_.clear();
    {
        std::lock_guard<std::mutex> g(lock_);
        finished_ = true;
    }
    done_.notify_all();
}

bool CommandBuffer::copy(const Buffer& from, uint64_t fromOffset, const Buffer& to, uint64_t toOffset, uint64_t length) {
    closePass();
    if (status_ > kEnqueued) {
        return fail("copy after commit");
    }
    if (hangDumpOn()) mark(Mark{uint32_t(stream_.words().size()), kMarkCopy, uint8_t(0), 0, 0, 0, uint32_t(length), 0, 0, nullptr, nullptr});   // (redfin hmv6)
    if (fromOffset > from.length() || length > from.length() - fromOffset || toOffset > to.length() ||
        length > to.length() - toOffset) {
        return fail("copy outside a buffer");
    }
    if (length == 0) {
        return true;
    }
    if (fromOffset % 4 || toOffset % 4 || length % 4) {
        // the command processor moves words; a copy of bytes goes through the 2D engine instead (a pass of its own).
        // MapKit's snapshot service copies rows of 2 bytes into its textures.
        std::string why;
        Piece table;
        if (!piece(render::kTableBytes, &table, &why)) {
            return fail(std::string("no memory for the copy's table: ") + why);
        }
        pm4::Stream added;
        render::encodeBytes(added, from.gpuAddress() + fromOffset, to.gpuAddress() + toOffset, length, table.gpuAddress(), table.contents());
        appendStream(stream_, added);
        use(from, false);
        use(to, true);
        return true;
    }
    uint64_t words = length / 4, done = 0;
    while (done < words) {
        uint32_t n = uint32_t(std::min<uint64_t>(words - done, kMemcpyMax));
        stream_.memcpy(to.gpuAddress() + toOffset + 4 * done, from.gpuAddress() + fromOffset + 4 * done, n);
        // what comes after may read what this wrote: the copy lands before the next packet is taken
        stream_.waitMemWrites();
        stream_.waitForMe();
        done += n;
    }
    use(from, false);
    use(to, true);
    return true;
}

bool CommandBuffer::fill(const Buffer& b, uint64_t offset, uint64_t length, uint8_t value) {
    closePass();
    if (status_ > kEnqueued) {
        return fail("fill after commit");
    }
    if (hangDumpOn()) mark(Mark{uint32_t(stream_.words().size()), kMarkCopy, uint8_t(1), 0, 0, 0, uint32_t(length), 0, 0, nullptr, nullptr});   // (redfin hmv6)
    if (offset % 4 || length % 4) {
        return fail("fill of " + std::to_string(length) + " bytes at " + std::to_string(offset) +
                    ": the command processor writes words, so both must be multiples of 4");
    }
    if (offset > b.length() || length > b.length() - offset) {
        return fail("fill outside the buffer");
    }
    if (length == 0) {
        return true;
    }
    uint32_t word = uint32_t(value) * 0x01010101u;
    uint64_t words = length / 4, base = b.gpuAddress() + offset;
    // the first words written as they are, ...
    uint32_t head = uint32_t(std::min<uint64_t>(words, kFillHead));
    std::vector<uint32_t> pattern(head, word);
    stream_.memWrite(base, pattern.data(), head);
    stream_.waitMemWrites();
    stream_.waitForMe();
    // ... and the rest copied from what is already filled, twice as much each time
    uint64_t have = head;
    while (have < words) {
        uint32_t n = uint32_t(std::min<uint64_t>(std::min<uint64_t>(have, words - have), kMemcpyMax));
        stream_.memcpy(base + 4 * have, base, n);
        stream_.waitMemWrites();
        stream_.waitForMe();
        have += n;
    }
    use(b, true);
    return true;
}

// --- CommandQueue -----------------------------------------------------------------------------------------------

std::unique_ptr<CommandQueue> CommandQueue::make(Gpu& gpu, std::string* why) {
    uint32_t id = 0;
    int e = gpu.newQueue(0, &id);
    if (e != 0) {
        *why = std::string("the driver gave no submission queue: ") + strerror(e);
        return nullptr;
    }
    std::unique_ptr<CommandQueue> q(new CommandQueue(gpu, id));
    q->thread_ = std::thread([p = q.get()] { p->waiter(); });
    return q;
}

CommandQueue::~CommandQueue() {
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    idle_.clear();
    gpu_.closeQueue(id_);
}

std::shared_ptr<CommandBuffer> CommandQueue::commandBuffer() {
    return std::shared_ptr<CommandBuffer>(new CommandBuffer(*this));
}

std::shared_ptr<Texture> CommandQueue::nullTexture(std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    if (null_ == nullptr) {
        TextureDesc d;
        d.format = PixelFormat::kRGBA8Unorm;
        std::shared_ptr<Texture> t = Texture::make(gpu_, d, why);
        // (written, not taken as the kernel's fresh zeros: the zero is the claim, so it is put there in so many words)
        const uint32_t zero = 0;
        if (t == nullptr || !t->replace(0, 0, 1, 1, &zero, 4, why)) return nullptr;
        null_ = std::move(t);
    }
    return null_;
}

namespace {
// (redfin) the chip's SP cores and fibers an SP (freedreno_devices.py: the A635 2 x 128 * 2 * 16, the A620 1 x 128 * 16)
uint32_t spCores() { return a6xx::chip().numSpCores; }
uint32_t fibersPerSp() { return a6xx::chip().fibersPerSp; }
// (redfin) the wave layout's extra (ChipInfo::pvtPerWaveFiberScale: 2 on the A620)
uint32_t waveScale() { return std::max(a6xx::chip().pvtPerWaveFiberScale, 1u); }
}  // namespace

std::shared_ptr<Buffer> CommandQueue::privateMemory(uint32_t bytes, bool perWave, PrivateMemory* out, std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    const int l = perWave ? 1 : 0;
    if (privatePerFiber_[l] < bytes) {
        uint32_t perFiber = 512;
        while (perFiber < bytes) perFiber *= 2;
        const uint64_t perSp = (uint64_t(perFiber) * fibersPerSp() * (perWave ? waveScale() : 1u) + 4095) & ~uint64_t(4095);
        std::shared_ptr<Buffer> b = newBuffer(gpu_, perSp * spCores(), why);
        if (b == nullptr) return nullptr;
        private_[l].push_back(std::move(b));
        privatePerFiber_[l] = perFiber;
    }
    const uint32_t perFiber = privatePerFiber_[l];
    out->address = private_[l].back()->gpuAddress();
    out->perFiber = perFiber;
    out->perSp = uint32_t((uint64_t(perFiber) * fibersPerSp() * (perWave ? waveScale() : 1u) + 4095) & ~uint64_t(4095));
    out->perWave = perWave;
    return private_[l].back();
}

std::shared_ptr<Buffer> CommandQueue::privateMemoryScaled(uint32_t bytes, bool perWave, uint32_t fiberScale, uint32_t bufferScale, PrivateMemory* out,
                                                          std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    uint32_t perFiber = 512;
    while (perFiber < bytes) perFiber *= 2;
    const uint64_t perSp = (uint64_t(perFiber) * fibersPerSp() * std::max(fiberScale, 1u) + 4095) & ~uint64_t(4095);
    std::shared_ptr<Buffer> b = newBuffer(gpu_, perSp * spCores() * std::max(bufferScale, 1u), why);
    if (b == nullptr) return nullptr;
    privateScaled_.push_back(b);
    out->address = b->gpuAddress();
    out->perFiber = perFiber;
    out->perSp = uint32_t(perSp);
    out->perWave = perWave;
    return b;
}

size_t CommandQueue::idleCommandBuffers() {
    std::lock_guard<std::mutex> g(lock_);
    return idle_.size();
}

std::unique_ptr<Bo> CommandQueue::takeCommandBo(size_t bytes, int* error) {
    {
        std::lock_guard<std::mutex> g(lock_);
        for (auto it = idle_.begin(); it != idle_.end(); ++it) {
            if ((*it)->size() >= bytes) {
                std::unique_ptr<Bo> bo = std::move(*it);
                idle_.erase(it);
                return bo;
            }
        }
    }
    return gpu_.newBo(std::max(bytes, kCommandBo), kBoGpuReadOnly | kBoWriteCombined, error);
}

// (redfin hmv8) HM_XR_SYNC: the kernel has one ring for every client and runs it in order, so the fence of an empty
// submission made now passes only once everything any process submitted before it has run -- the GPU writes another
// process (WebKit's GPU process, a Metal client on SweetMetal2) made into an IOSurface this process then reads on the
// processor. Its IB (one CP_NOP) goes back to the pool only once passed; one the GPU has not passed in time is kept aside.
int CommandQueue::ringBarrier(int64_t timeoutNs) {
    int error = 0;
    std::unique_ptr<Bo> bo = takeCommandBo(4, &error);
    if (bo == nullptr || bo->cpu() == nullptr) return error != 0 ? error : ENOMEM;
    const uint32_t nop = pm4::type7(pm4::kNop, 0);
    memcpy(bo->cpu(), &nop, 4);
    uint32_t fence = 0;
    int e;
    {
        std::lock_guard<std::mutex> g(lock_);
        e = gpu_.submit(id_, *bo, 0, 1, {}, &fence);
    }
    if (e == 0) e = gpu_.wait(id_, fence, timeoutNs);
    std::lock_guard<std::mutex> g(lock_);
    if (e == 0 || fence == 0) idle_.push_back(std::move(bo));
    else barrierLate_.push_back(std::move(bo));
    return e;
}

void CommandQueue::submit(std::shared_ptr<CommandBuffer> cb) {
    cb->committedAt_ = gpu_.transport().nowNs();
    if (!cb->error_.empty()) {
        cb->finish(CommandBuffer::kError);   // a blit that was refused: nothing of it reaches the GPU
        return;
    }
    cb->closePass();   // the last pass's tail
    // every command buffer ends with the GPU idle: what Metal's completion promises, that the work is done
    cb->stream_.waitMemWrites();
    cb->stream_.waitForIdle();
    // (redfin hmv10) HM_XR_LATE: the extended-range surfaces' copies, now (a ring barrier first); (redfin hmv11) HM_XR_GPU:
    // or a buffer of GPU decodes, sent first so the ring runs it between WebKit's writes and this buffer's draws
    if (std::shared_ptr<CommandBuffer> pre = cb->xrDecodeDeferred()) submit(std::move(pre));
    const std::vector<uint32_t>& w = cb->stream_.words();
    int error = 0;
    cb->commands_ = takeCommandBo(4 * w.size(), &error);
    if (cb->commands_ == nullptr) {
        cb->error_ = std::string("no memory for the command stream: ") + strerror(error);
        cb->finish(CommandBuffer::kError);
        return;
    }
    // the stream is written in order, as write-combined memory wants it, and read back by nobody
    memcpy(cb->commands_->cpu(), w.data(), 4 * w.size());
    std::unique_lock<std::mutex> g(lock_);  // the fences of one queue are handed out in commit order
    cb->status_ = CommandBuffer::kCommitted;
    cb->faultsAtCommit_ = gpu_.faults();
    int e = gpu_.submit(id_, *cb->commands_, 0, uint32_t(w.size()), cb->uses_, &cb->fence_);
    if (e != 0) {
        cb->error_ = std::string("the driver refused the submission: ") + strerror(e);
        idle_.push_back(std::move(cb->commands_));
        g.unlock();                         // (the handlers may commit more: not under the queue's lock)
        cb->finish(CommandBuffer::kError);
        return;
    }
    cb->status_ = CommandBuffer::kScheduled;
    cb->submittedWords_ = uint32_t(w.size());   // (redfin hmv6) for the hang report and a send again after a reset
    cb->submittedIova_ = cb->commands_->gpuAddress();
    inFlight_.push_back(std::move(cb));
    changed_.notify_all();
}

// (redfin hmv6) HM_RESET_RECOVER=1 (default): after a reset (the kernel's GPU reset gives up everything it had been
// given: the hung submission and every one behind it in the ring, their fences set as passed), the buffer the waiter
// was on fails (it hung, or the GPU was reset under it) and the buffers of this queue given up behind it are sent again,
// once each, when nothing committed after the reset has gone to the GPU yet (else they would run after newer frames:
// they fail as given up, as before). Our kernel keeps every BO, mapping and queue over a reset (xnu 0108 a6_recover:
// soft reset + CP init, page tables and SMMU banks kept; 0115: CreateQueue is the one ring, the BO table is the
// client's), so the same command IB, at the same address, is still good. 0: as before (every one of them fails).
namespace {
bool resetRecoverOn() {
    static const bool on = [] { const char* e = getenv("HM_RESET_RECOVER"); return e == nullptr || e[0] != '0'; }();
    return on;
}
}  // namespace

void CommandQueue::recoverAfterReset(CommandBuffer& hung, uint64_t faults) {
    // (lock_ held) inFlight_.front() is HUNG
    size_t behind = 0, after = 0;
    for (size_t i = 1; i < inFlight_.size(); i++) {
        if (inFlight_[i]->faultsAtCommit_ < faults) behind++; else after++;
    }
    render::notify("reset recovery: 1/3 GPU reset " + std::to_string(faults) + ": the buffer on fence " + std::to_string(hung.fence_) +
                   " failed (it hung, or the GPU was reset under it); " + std::to_string(behind) + " of this queue's buffers given up behind it, " +
                   std::to_string(after) + " committed since the reset");
    if (behind == 0) {
        render::notify("reset recovery: 2/3 nothing else to send again; 3/3 BOs, mappings and the queue are the kernel's still (nothing to make again)");
        return;
    }
    if (after != 0) {
        render::notify("reset recovery: 2/3 not sent again: " + std::to_string(after) + " newer buffers already went to the GPU (the old ones would run after them); " +
                       std::to_string(behind) + " fail as given up");
        return;
    }
    uint32_t sent = 0, refused = 0;
    for (size_t i = 1; i < inFlight_.size(); i++) {
        CommandBuffer& cb = *inFlight_[i];
        if (cb.commands_ == nullptr || cb.submittedWords_ == 0 || cb.resubmits_ != 0) {
            refused++;
            continue;
        }
        const uint32_t old = cb.fence_;
        uint32_t fence = 0;
        const int e = gpu_.submit(id_, *cb.commands_, 0, cb.submittedWords_, cb.uses_, &fence);
        if (e != 0) {
            render::notify("reset recovery: 2/3 buffer on fence " + std::to_string(old) + " not sent again: " + strerror(e));
            refused++;
            continue;
        }
        cb.fence_ = fence;
        cb.faultsAtCommit_ = faults;
        cb.resubmits_++;
        sent++;
        render::notify("reset recovery: 2/3 buffer on fence " + std::to_string(old) + " sent again as fence " + std::to_string(fence) + " (" +
                       std::to_string(cb.submittedWords_) + " words at 0x" + [&] { char b[24]; snprintf(b, sizeof b, "%llx", (unsigned long long)cb.submittedIova_); return std::string(b); }() + ")");
    }
    render::notify("reset recovery: 3/3 " + std::to_string(sent) + " sent again, " + std::to_string(refused) + " left to fail; BOs, mappings and the queue kept (the kernel's)");
}

// (redfin hmv7) HM_HANG_BLAME's tally: program (the core Program a draw mark names) -> hangs it was in
namespace {
std::mutex gBlameLock;
std::map<const void*, uint32_t> gBlame;
}  // namespace

uint32_t CommandBuffer::blameWords() {
    static const uint32_t w = [] {
        const char* e = getenv("HM_HANG_BLAME_WORDS");
        return e != nullptr && e[0] != '\0' ? uint32_t(strtoul(e, nullptr, 0)) : 1024u;
    }();
    return w;
}

uint32_t CommandBuffer::hangsOf(const void* program) {
    std::lock_guard<std::mutex> g(gBlameLock);
    auto it = gBlame.find(program);
    return it == gBlame.end() ? 0 : it->second;
}

std::vector<std::string> CommandBuffer::blame() const {
    std::vector<std::string> out;
    std::vector<const Mark*> seen;
    for (const Mark& m : marks_) {
        if (m.kind != kMarkDraw || m.what == nullptr) continue;
        bool dup = false;
        for (const Mark* o : seen) dup = dup || o->what == m.what;
        if (dup) continue;
        seen.push_back(&m);
    }
    std::lock_guard<std::mutex> g(gBlameLock);
    for (const Mark* m : seen) {
        if (m->words < blameWords()) continue;
        const uint32_t n = ++gBlame[m->what];
        char b[200];
        snprintf(b, sizeof b, "blame: %s(%p) fsw %u: in %u hung buffer%s now", m->name != nullptr ? m->name : "?", m->what, m->words, n, n == 1 ? "" : "s");
        out.push_back(b);
    }
    if (out.empty()) {
        out.push_back("blame: no program of " + std::to_string(blameWords()) + " fragment words or more in it (" + std::to_string(seen.size()) +
                      " programs): nothing blamed");
    }
    return out;
}

bool CommandBuffer::hangDumpOn() {
    static const bool on = [] { const char* e = getenv("HM_HANG_DUMP"); return e == nullptr || e[0] != '0'; }();
    return on;
}

std::string CommandBuffer::hangReport() const {
    if (!hangDumpOn()) return "";
    std::ostringstream o;
    char b[160];
    uint32_t passes = 0, draws = 0, dispatches = 0, copies = 0, blits = 0, clears = 0;
    for (const Mark& m : marks_) {
        switch (m.kind) {
        case kMarkDraw: draws++; break;
        case kMarkPassEnd: passes++; break;
        case kMarkDispatch: dispatches++; break;
        case kMarkCopy: copies++; break;
        case kMarkBlit: blits++; break;
        case kMarkClear: clears++; break;
        }
    }
    snprintf(b, sizeof b, "IB at 0x%llx, %u words (klog 'ib1 <base> rem N': the CP had fetched to word %u-N, less its prefetch)",
             (unsigned long long)submittedIova_, submittedWords_, submittedWords_);
    o << b << "\n";
    o << passes << " passes, " << draws << " draws, " << dispatches << " dispatches, " << copies << " copies/fills, " << blits << " blits, " << clears
      << " clears; XR companions drawn " << xrDrawn_.size() << ", XR copies decoded " << xrDecoded_.size() << "; sent again " << resubmits_
      << (marksLost_ ? "; marks lost " + std::to_string(marksLost_) : std::string()) << "\n";
    // each pass: its words, target, mode, draws, and its programs (name, address, fragment words, private bytes)
    struct Prog { const void* what; const char* name; uint32_t words, pvt, draws; uint64_t elements; };
    std::vector<Prog> progs;
    uint32_t start = 0, firstDraw = ~0u, printed = 0;
    const uint32_t maxPasses = 24;
    uint32_t pass = 0;
    auto flushProgs = [&]() {
        std::string line = "   programs:";
        size_t n = 0;
        for (const Prog& p : progs) {
            if (n++ == 8) { line += " ..."; break; }
            snprintf(b, sizeof b, " %s(%p) x%u el %llu fsw %u pvt %u%s;", p.name != nullptr ? p.name : "?", p.what, p.draws, (unsigned long long)p.elements,
                     p.words, p.pvt & 0x7fffffffu, (p.pvt & 0x80000000u) ? "/wave" : "");
            line += b;
        }
        if (!progs.empty()) o << line << "\n";
        progs.clear();
    };
    for (const Mark& m : marks_) {
        if (m.kind == kMarkDraw) {
            if (firstDraw == ~0u) firstDraw = m.word;
            bool found = false;
            for (Prog& p : progs) {
                if (p.what == m.what) { p.draws++; p.elements += m.count; found = true; }
            }
            if (!found) progs.push_back(Prog{m.what, m.name, m.words, m.pvt, 1, m.count});
            continue;
        }
        const bool show = passes <= maxPasses || pass + maxPasses >= passes;   // (the last ones when there are many)
        if (m.kind == kMarkPassEnd) {
            if (show) {
                snprintf(b, sizeof b, " pass %u: words %u-%u, %ux%u format %u, %s, %u draws", pass, start, m.word, m.w, m.h, m.fmt, m.gmem ? "GMEM" : "sysmem", m.count);
                o << b << "\n";
                flushProgs();
                printed++;
            } else {
                progs.clear();
            }
            pass++;
            start = m.word;
            firstDraw = ~0u;
            continue;
        }
        if (show) {
            switch (m.kind) {
            case kMarkDispatch:
                snprintf(b, sizeof b, " dispatch at word %u: %s(%p) groups %ux%ux%u local %u, %u words, pvt %u", m.word, m.name != nullptr ? m.name : "?", m.what,
                         m.w, m.h, m.fmt, m.count, m.words, m.pvt & 0x7fffffffu);
                break;
            case kMarkCopy: snprintf(b, sizeof b, " %s at word %u: %u bytes", m.gmem ? "fill" : "copy", m.word, m.count); break;
            case kMarkBlit:
                snprintf(b, sizeof b, " blit at word %u: %ux%u format %u into %ux%u (%p)", m.word, m.w, m.h, m.fmt, m.count >> 16, m.count & 0xffff, m.what);
                break;
            case kMarkClear: snprintf(b, sizeof b, " clear at word %u: %ux%u format %u", m.word, m.w, m.h, m.fmt); break;
            default: b[0] = 0; break;
            }
            o << b << "\n";
        }
        start = m.word;
    }
    (void)printed;
    return o.str();
}

void CommandQueue::waiter() {
    std::unique_lock<std::mutex> g(lock_);
    for (;;) {
        changed_.wait(g, [this] { return quit_ || !inFlight_.empty(); });
        if (inFlight_.empty()) {
            return;  // quitting, and nothing left in flight
        }
        std::shared_ptr<CommandBuffer> cb = inFlight_.front();
        g.unlock();
        int e = gpu_.wait(id_, cb->fence_, kPatienceNs);
        cb->completedAt_ = gpu_.transport().nowNs();
        // A fence that passed is not yet a submission that ran. When the kernel resets a hung GPU (gpu_recover=1) it
        // gives up everything the GPU had been given, the hung submission and those behind it in the ring, by setting
        // the fence word to the last submission's number -- so their waits end as if they had run (hakios_gpu_recover:
        // "submissions up to N are given up as done"). FAULTS counts the resets; one more since the commit says so.
        uint64_t faults = gpu_.faults();
        bool givenUp = faults > cb->faultsAtCommit_;
        if (e == 0 && givenUp) {
            cb->error_ = "given up by a reset of the GPU (it hung on this submission or on one before it): not run";
            e = EIO;
        } else if (e == ETIMEDOUT) {
            cb->error_ = givenUp ? "the GPU did not finish it in 5 s: it hung, and the kernel has reset it (reset " +
                                       std::to_string(faults) + " of this run)"
                                 : "the GPU did not finish it in 5 s and is not answering; without gpu_recover=1 the "
                                   "kernel waits for nothing again in this run";
        } else if (e != 0) {
            cb->error_ = std::string("waiting for it failed: ") + strerror(e);
        }
        // (redfin hmv6) HM_HANG_DUMP: what the hung (or given-up-first) buffer held; HM_RESET_RECOVER: the others again
        const bool front = givenUp && (e == ETIMEDOUT || e == EIO) && cb->faultsAtCommit_ < faults && faults > hangsReported_;
        if (front && CommandBuffer::hangDumpOn()) {
            std::istringstream lines(cb->hangReport());
            std::string line;
            while (std::getline(lines, line)) render::notify("hang: fence " + std::to_string(cb->fence_) + ": " + line);
        }
        if (front && CommandBuffer::hangDumpOn()) {   // (redfin hmv7) HM_HANG_BLAME's tally (the marks are HM_HANG_DUMP's)
            for (const std::string& line : cb->blame()) render::notify("hang: fence " + std::to_string(cb->fence_) + ": " + line);
        }
        if (front) {
            hangsReported_ = faults;
            if (resetRecoverOn()) {
                g.lock();
                recoverAfterReset(*cb, faults);
                g.unlock();
            }
        }
        cb->finish(e == 0 ? CommandBuffer::kCompleted : CommandBuffer::kError);
        g.lock();
        inFlight_.pop_front();
        if (cb->commands_ != nullptr) {
            idle_.push_back(std::move(cb->commands_));
        }
    }
}

}  // namespace hakios::metal
