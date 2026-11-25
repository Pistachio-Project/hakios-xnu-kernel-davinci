#include "hakios-metal/command.hpp"
#include "hakios-metal/texture.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <map>
#include <mutex>
#include <cstring>

namespace hakios::metal {

namespace {
constexpr size_t kCommandBo = 64 * 1024;           // one command buffer's packets, at most (grown when needed)
constexpr size_t kSpareStreams = 4;                 // command buffers' stream memory a queue keeps for the next ones
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
// the pool's tally for the plug-in's buffers line: buffers given back, refused as still held by another owner, refused
// because the pool was full, and the made ones of 1 MB or more (a GMEM pass's bodies, the binning streams)
std::atomic<uint64_t> gBuffersMadeTotal{0}, gBytesMadeTotal{0};   // (never reset: the hitch lines' before and after)
std::atomic<uint32_t> gPrefilled{0}, gRefilled{0};
std::atomic<uint32_t> gPoolGiven{0}, gPoolShared{0}, gPoolFull{0}, gBigMade{0};

std::shared_ptr<Buffer> newBuffer(Gpu& gpu, uint64_t length, std::string* why, bool recycle) {
    gBuffersMade++;
    gBytesMade += length;
    gBuffersMadeTotal++;
    gBytesMadeTotal += length;
    if (length >= (1u << 20)) gBigMade++;
    int error = 0;
    std::unique_ptr<Bo> bo = recycle ? gpu.newBoRecycled(length, kBoWriteCombined, &error) : gpu.newBo(length, kBoWriteCombined, &error);
    if (bo == nullptr) {
        *why = "the driver gave no buffer of " + std::to_string(length) + " bytes: " + strerror(error);
        return nullptr;
    }
    Buffer* raw = new Buffer(std::move(bo), length);
    if (recycle) raw->setRecyclable();
    return addressed(raw);
}

namespace {
// One slab is 32 draw tables. Pieces are cut at 256 bytes, which is more than anything in them wants (128 for a
// program's preload and the border colour, 64 for a texture descriptor), and a piece of a page or more starts on a
// page, so that a table lies in memory as it did in a buffer of its own.
constexpr uint64_t kSlabBytes = 256 * 1024;
constexpr uint64_t kPieceAlign = 256;
constexpr uint64_t kPageAlign = 4096;
}  // namespace

bool CommandBuffer::piece(uint64_t bytes, Piece* out, std::string* why, bool zero) {
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
            slab_ = queue_.takeSlab(kSlabBytes, why, &slabFresh_);
            slabUsed_ = 0;
            if (slab_ != nullptr) slabs_.push_back(slab_);
        }
        out->buffer = slab_;
        out->offset = slabUsed_;
        if (slab_ != nullptr) {
            // (a slab from the pool is not zeroed whole when it is taken, as it was -- 256 KB of write-combined memory
            // written for the 31 draws it holds -- but a piece of it when it is handed out, unless its user writes every
            // word it reads: a draw's table does, and was 8 KB of zeros a draw that nothing read)
            if (zero && !slabFresh_) memset(static_cast<uint8_t*>(slab_->contents()) + slabUsed_, 0, bytes);
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

bool envFullState() {
    static const bool on = [] { const char* e = getenv("HAKIOS_METAL_FULL_STATE"); return e != nullptr && e[0] != '0' && e[0] != 0; }();
    return on;
}

uint32_t envDirtyGroups() {
    static const uint32_t mask = [] {
        const char* e = getenv("HAKIOS_METAL_DIRTY_GROUPS");
        return e == nullptr || e[0] == 0 ? render::kAllGroups : uint32_t(strtoul(e, nullptr, 0)) & (render::kAllGroups | render::kTrackBinned);
    }();
    return mask;
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

std::shared_ptr<Buffer> CommandQueue::takeBuffer(uint64_t bytes, std::string* why, bool zero, bool* fresh) {
    const uint64_t size = sizeClass(bytes);
    std::shared_ptr<Buffer> b;
    bool low = false;
    {
        std::lock_guard<std::mutex> g(poolLock_);
        auto it = freeBuffers_.find(size);
        if (it != freeBuffers_.end() && !it->second.empty()) {
            b = std::move(it->second.back());
            it->second.pop_back();
            freeBytes_ -= size;
            low = size == kSlabBytes && it->second.size() < 64;
        }
    }
    if (low) {
        gRefilled += 64;
        wantSlabs(64);
    }
    if (fresh != nullptr) *fresh = b == nullptr;
    if (b == nullptr) return newBuffer(gpu_, size, why);
    gBuffersReused++;
    if (zero) memset(b->contents(), 0, size);   // (as a new one comes from the kernel)
    return b;
}

void CommandQueue::giveBuffer(std::shared_ptr<Buffer> b) {
    // (only one no one else holds, of a size takeBuffer makes: a piece's buffer kept elsewhere is still someone's)
    if (b == nullptr || b->contents() == nullptr || b->length() != sizeClass(b->length())) return;
    if (b.use_count() != 1) {
        gPoolShared++;
        return;
    }
    // (the pool's size, HAKIOS_METAL_POOL_MB, 128 when not given: a heavy scene made 191 buffers, 116 MB, in 2 s against
    // the 32 MB this was, each fresh one a kernel call and first-touch page faults inside a draw call)
    static const uint64_t cap = [] {
        const char* e = getenv("HAKIOS_METAL_POOL_MB");
        const long mb = e != nullptr && e[0] != 0 ? strtol(e, nullptr, 0) : 128;
        return uint64_t(mb < 0 ? 0 : mb) << 20;
    }();
    std::lock_guard<std::mutex> g(poolLock_);
    if (freeBytes_ + b->length() > cap) {
        gPoolFull++;
        return;
    }
    gPoolGiven++;
    freeBytes_ += b->length();
    freeBuffers_[b->length()].push_back(std::move(b));
}

std::shared_ptr<Buffer> CommandQueue::takeSlab(uint64_t bytes, std::string* why, bool* fresh) { return takeBuffer(bytes, why, false, fresh); }

size_t CommandQueue::quarantinedCount() {
    std::lock_guard<std::mutex> g(lock_);
    return quarantined_.size();
}

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
// the weak reference, and a raw pointer good only while that Buffer still exists: when the weak one is expired but the
// entry is still here (under gSurfaceLock), the Buffer's deleter is blocked on this lock and has not run yet, so raw
// still points to it and its Bo still owns the handle -- enough to take the handle over before it closes it
struct SurfaceWeak {
    std::weak_ptr<Buffer> weak;
    Buffer* raw = nullptr;
};
std::map<std::pair<const Gpu*, uint32_t>, SurfaceWeak> gSurfaceBuffers;
// ... and by (GPU, surface ID), with what the kernel said of the surface: a surface wrapped again while its buffer lives
// is answered from here, no call to the kernel. The window server wraps its display's page in a texture anew for every
// frame and lets go of it after: each frame opened the surface's buffer in the kernel and closed it again, the close
// waiting for the GPU's lock beside the threads that wait for the GPU, and the next frame's wrap waiting on this lock
// for the close (waitstack=1 on the phone: 1051 samples of the render thread here, logs/072837-run.log). So the last
// kRecentSurfaces buffers wrapped are kept past their last user. A kept buffer holds its surface in the kernel, whose
// ID is not given to another surface meanwhile (MiniIOSurface: the slot frees with the surface's last holder).
struct SurfaceEntry {
    std::weak_ptr<Buffer> buffer;
    SurfaceInfo info;
};
std::map<std::pair<const Gpu*, uint32_t>, SurfaceEntry> gSurfaceIds;
constexpr size_t kRecentSurfaces = 4;
std::vector<std::pair<const Gpu*, std::shared_ptr<Buffer>>> gRecentSurfaces;   // (the last one wrapped at the back)

// with the lock held: B wrapped just now; what falls out of the recent ones goes to *DROPPED, let go by the caller after
// the lock (its deleter takes the lock)
void keepRecent(const Gpu* gpu, const std::shared_ptr<Buffer>& b, std::vector<std::shared_ptr<Buffer>>* dropped) {
    for (auto it = gRecentSurfaces.begin(); it != gRecentSurfaces.end(); ++it) {
        if (it->second == b) {
            gRecentSurfaces.erase(it);
            break;
        }
    }
    gRecentSurfaces.emplace_back(gpu, b);
    while (gRecentSurfaces.size() > kRecentSurfaces) {
        dropped->push_back(std::move(gRecentSurfaces.front().second));
        gRecentSurfaces.erase(gRecentSurfaces.begin());
    }
}
}  // namespace

void forgetSurfaceBuffers(const Gpu* gpu) {
    std::vector<std::shared_ptr<Buffer>> dropped;
    {
        std::lock_guard<std::mutex> g(gSurfaceLock);
        for (auto it = gRecentSurfaces.begin(); it != gRecentSurfaces.end();) {
            if (it->first == gpu) {
                dropped.push_back(std::move(it->second));
                it = gRecentSurfaces.erase(it);
            } else {
                ++it;
            }
        }
    }
    dropped.clear();   // (the deleters, each under the lock, after it)
    std::lock_guard<std::mutex> g(gSurfaceLock);
    for (auto it = gSurfaceIds.begin(); it != gSurfaceIds.end();) it = it->first.first == gpu ? gSurfaceIds.erase(it) : std::next(it);
}

void forgetSurfaceBuffer(const Gpu* gpu, const std::shared_ptr<Buffer>& b) {
    std::shared_ptr<Buffer> dropped;   // (let go after the lock)
    std::lock_guard<std::mutex> g(gSurfaceLock);
    for (auto it = gRecentSurfaces.begin(); it != gRecentSurfaces.end(); ++it) {
        if (it->first == gpu && it->second == b) {
            dropped = std::move(it->second);
            gRecentSurfaces.erase(it);
            break;
        }
    }
}

std::shared_ptr<Buffer> surfaceBuffer(Gpu& gpu, uint32_t surfaceId, SurfaceInfo* info, std::string* why) {
    std::vector<std::shared_ptr<Buffer>> dropped;   // (let go after the lock: declared before it)
    std::lock_guard<std::mutex> g(gSurfaceLock);
    const auto id = std::make_pair(static_cast<const Gpu*>(&gpu), surfaceId);
    auto known = gSurfaceIds.find(id);
    if (known != gSurfaceIds.end()) {
        if (std::shared_ptr<Buffer> b = known->second.buffer.lock()) {
            *info = known->second.info;
            keepRecent(&gpu, b, &dropped);
            return b;
        }
        gSurfaceIds.erase(known);
    }
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
        if (std::shared_ptr<Buffer> b = live->second.weak.lock()) {
            gpu.disown(std::move(bo));   // the same handle as the live Buffer's Bo: closed once, by that Bo
            gSurfaceIds[id] = SurfaceEntry{b, *info};
            keepRecent(&gpu, b, &dropped);
            return b;
        }
        // the entry is here but its Buffer is being destroyed (its deleter waits on this lock, its Bo still owns the
        // kernel handle the driver just gave us again). Neuter that Bo so its ~Bo closes nothing: our new Bo takes the
        // handle over as its one owner, instead of a second Bo being built over it and the handle closed twice.
        if (live->second.raw != nullptr) {
            gpu.disown(live->second.raw->bo());
        }
    }
    const uint64_t length = bo->size();
    std::shared_ptr<Buffer> b(new Buffer(std::move(bo), length), [key, id](Buffer* x) {
        std::lock_guard<std::mutex> g(gSurfaceLock);
        auto it = gSurfaceBuffers.find(key);
        if (it != gSurfaceBuffers.end() && it->second.weak.expired()) gSurfaceBuffers.erase(it);
        auto known = gSurfaceIds.find(id);
        if (known != gSurfaceIds.end() && known->second.buffer.expired()) gSurfaceIds.erase(known);
        delete x;
    });
    gSurfaceBuffers[key] = SurfaceWeak{b, b.get()};
    gSurfaceIds[id] = SurfaceEntry{b, *info};
    keepRecent(&gpu, b, &dropped);
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

uint32_t CommandQueue::likelyTouched(uint64_t kind, uint32_t ordinal) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = touched_.find(kindKey(kind, ordinal));
    if (it == touched_.end() || it->second.times == 0) return ~0u;
    uint32_t mask = 0;
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        if (it->second.at[i] != 0 && it->second.times - it->second.at[i] < kLikelyTimes) mask |= 1u << i;
    }
    return mask;
}

void CommandQueue::noteTouched(uint64_t kind, uint32_t ordinal, uint32_t touched) {
    std::lock_guard<std::mutex> g(lock_);
    Touched& t = touched_[kindKey(kind, ordinal)];
    t.times++;
    for (uint32_t i = 0; i < kMaxColorOutputs; i++) {
        if ((touched >> i) & 1) t.at[i] = t.times;
    }
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
    if (drawStamps_ == nullptr) return out;   // (none timed: every one drawn in tiles, its time 0)
    const auto* t = static_cast<const uint64_t*>(drawStamps_->contents());
    for (size_t i = 0; i < out.size(); i++) {
        const int32_t s = out[i].slot;
        out[i].us = s >= 0 && t[2 * s + 1] > t[2 * s] ? double(t[2 * s + 1] - t[2 * s]) / 19.2 : 0.0;
    }
    return out;
}

std::string CommandBuffer::passDetail(size_t i) const {
    if (i >= profiled_.size()) return {};
    const std::vector<PassTime> timed = passTimes();
    const PassTime& t = timed.size() > i ? timed[i] : profiled_[i];
    std::string out = t.head;
    if (t.gapUs != 0) out += " [" + std::to_string(int(t.gapUs)) + " us after the pass before]";
    out += " " + std::to_string(t.draws) + " draws";
    if (t.blended != 0) out += " (" + std::to_string(t.blended) + " blended)";
    out += ", fs up to " + std::to_string(t.maxFsWords) + " words";
    if (t.sampled != 0) {
        out += ", " + std::to_string(t.sampled) + " texture reads of " + std::to_string(t.sampledPixels / 1000000) + " Mpx in all (largest " +
               std::to_string(t.maxSampledW) + "x" + std::to_string(t.maxSampledH) + ", " + std::to_string(t.sampledMips) + " mipped)";
    }
    out += ":";
    for (const auto& n : t.names) out += " " + n.first + "x" + std::to_string(n.second);
    return out;
}

std::vector<CommandBuffer::PassTime> CommandBuffer::passTimes() const {
    std::vector<PassTime> out = profiled_;
    if (profileStamps_ == nullptr) return {};
    const auto* t = static_cast<const uint64_t*>(profileStamps_->contents());
    for (size_t i = 0; i < out.size(); i++) {
        out[i].us = t[2 * i + 1] > t[2 * i] ? double(t[2 * i + 1] - t[2 * i]) / 19.2 : 0.0;
        out[i].gapUs = i > 0 && t[2 * i] > t[2 * i - 1] && t[2 * i - 1] != 0 ? double(t[2 * i] - t[2 * i - 1]) / 19.2 : 0.0;
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
    // given back; one that completed has had its readers. Nor are a quarantined one's, which has let go of its queue:
    // the GPU may still write them, and the queue may be gone)
    if (finished_ && owner_ != nullptr) {
        queue_.giveBuffer(std::move(profileStamps_));
        queue_.giveBuffer(std::move(drawStamps_));
    }
}

void CommandBuffer::finish(Status s, bool recycle) {
    // (the queue kept alive to the end of this, whatever the handlers let go of: on the waiter's thread the waiter
    // holds it too, but a refused commit finishes on the committing thread, a deferred one on the Deferrer's)
    const std::shared_ptr<CommandQueue> queue = owner_;
    status_ = s;
    if (s == kCompleted && counts_.binnedPasses != 0) queue_.checkVsc();
    if (s == kCompleted && tuning_) {
        for (const PassTime& t : passTimes()) {
            if (t.tuneKey != 0) queue_.tuned(t.tuneKey, t.tiles != 0, t.us);
        }
    }
    if (recycle) {
        // the GPU has finished with this buffer, or never had it: its slabs and the memory it kept go back, before the
        // handlers, which touch nothing of them (the stamps they may read are held apart) and may let go of the queue
        kept_.clear();
        forgetRecent();
        slab_.reset();
        queue_.giveSlabs(&slabs_);
        keptAny_.clear();
    }
    // (when !recycle -- a wait that timed out with no reset -- kept_, slab_, slabs_ and keptAny_ stay referenced so none
    // of their memory is freed or pooled while the GPU may still be reading it; the queue quarantines the whole buffer)
    handlersAt_ = queue_.gpuClockNs();
    for (auto& h : handlers_) {
        h(*this);
    }
    handlers_.clear();
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

std::shared_ptr<CommandQueue> CommandQueue::make(Gpu& gpu, std::string* why) {
    uint32_t id = 0;
    int e = gpu.newQueue(0, &id);
    if (e != 0) {
        *why = std::string("the driver gave no submission queue: ") + strerror(e);
        return nullptr;
    }
    std::shared_ptr<CommandQueue> q(new CommandQueue(gpu, id));
    q->thread_ = std::thread([p = q.get(), orphaned = q->orphaned_] { p->waiter(orphaned); });
    const char* pre = getenv("HAKIOS_METAL_PREFILL");
    // (on only when asked for -- the plug-in asks, unless boot-arg metal_pool_prefill=0: the host tests compare draws by the
    // addresses of their memory, which a thread making slabs of its own would change from run to run)
    if (pre != nullptr && pre[0] != '0' && pre[0] != 0) {
        const char* mb = getenv("HAKIOS_METAL_PREFILL_MB");
        const long want = mb != nullptr && mb[0] != 0 ? strtol(mb, nullptr, 0) : 24;
        q->warm_ = std::thread([p = q.get()] { p->warmer(); });
        q->wantSlabs(unsigned(want <= 0 ? 0 : want * 4));   // (256 KB slabs: 4 to the MB)
    }
    return q;
}

void CommandQueue::wantSlabs(unsigned n) {
    if (!warm_.joinable() || n == 0) return;
    {
        std::lock_guard<std::mutex> g(warmLock_);
        warmWant_ += n;
    }
    warmCv_.notify_one();
}

void CommandQueue::warmer() {
    for (;;) {
        unsigned n;
        {
            std::unique_lock<std::mutex> g(warmLock_);
            warmCv_.wait(g, [this] { return warmQuit_ || warmWant_ != 0; });
            if (warmQuit_) return;
            n = warmWant_;
            warmWant_ = 0;
        }
        for (unsigned i = 0; i < n; i++) {
            {
                std::lock_guard<std::mutex> g(warmLock_);
                if (warmQuit_) return;
            }
            {
                std::lock_guard<std::mutex> g(poolLock_);
                if (freeBytes_ + kSlabBytes > (32ull << 20) * 4) break;   // (the pool's own cap is giveBuffer's)
            }
            int error = 0;
            std::unique_ptr<Bo> bo = gpu_.newBo(kSlabBytes, kBoWriteCombined, &error);
            if (bo == nullptr) break;
            std::memset(bo->cpu(), 0, kSlabBytes);   // (the first touch, here: a page fault each 4 KB, off the frame)
            std::shared_ptr<Buffer> b = addressed(new Buffer(std::move(bo), kSlabBytes));
            std::lock_guard<std::mutex> g(poolLock_);
            freeBuffers_[kSlabBytes].push_back(std::move(b));
            freeBytes_ += kSlabBytes;
            gPrefilled++;
        }
    }
}

CommandQueue::~CommandQueue() {
    {
        std::lock_guard<std::mutex> g(warmLock_);
        warmQuit_ = true;
    }
    warmCv_.notify_all();
    if (warm_.joinable()) warm_.join();
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
    }
    changed_.notify_all();
    if (thread_.get_id() == std::this_thread::get_id()) {
        // the last reference went on the waiter's own thread, after a completed handler let go of the caller's: the
        // thread cannot join itself (std::terminate). Nothing is in flight (each one in flight holds the queue), and the
        // waiter returns as soon as this does, touching nothing of the queue.
        *orphaned_ = true;
        thread_.detach();
    } else if (thread_.joinable()) {
        thread_.join();
    }
    idle_.clear();
    gpu_.closeQueue(id_);
}

std::shared_ptr<CommandBuffer> CommandQueue::commandBuffer() {
    std::shared_ptr<CommandBuffer> cb(new CommandBuffer(shared_from_this()));
    std::lock_guard<std::mutex> g(lock_);
    if (!spareStreams_.empty()) {
        cb->stream_.adopt(std::move(spareStreams_.back()));
        spareStreams_.pop_back();
    }
    return cb;
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
// the A635's SP cores and fibers an SP (freedreno_devices.py, the 7c+ Gen 3 entry: num_ccu 2, fibers_per_sp 128 * 2 * 16)
constexpr uint32_t kSpCores = 2;
constexpr uint32_t kFibersPerSp = 128 * 2 * 16;
}  // namespace

std::shared_ptr<Buffer> CommandQueue::privateMemory(uint32_t bytes, bool perWave, PrivateMemory* out, std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    const int l = perWave ? 1 : 0;
    if (privatePerFiber_[l] < bytes) {
        uint32_t perFiber = 512;
        while (perFiber < bytes) perFiber *= 2;
        const uint64_t perSp = (uint64_t(perFiber) * kFibersPerSp + 4095) & ~uint64_t(4095);
        std::shared_ptr<Buffer> b = newBuffer(gpu_, perSp * kSpCores, why);
        if (b == nullptr) return nullptr;
        private_[l].push_back(std::move(b));
        privatePerFiber_[l] = perFiber;
    }
    const uint32_t perFiber = privatePerFiber_[l];
    out->address = private_[l].back()->gpuAddress();
    out->perFiber = perFiber;
    out->perSp = uint32_t((uint64_t(perFiber) * kFibersPerSp + 4095) & ~uint64_t(4095));
    out->perWave = perWave;
    return private_[l].back();
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
    const std::vector<uint32_t>& w = cb->stream_.words();
    // the stream as runs the GPU's IB_SIZE can count, cut between packets: one IB1 a run, run in order (a stream of
    // more than 2^20 words -- some thousands of draws in one command buffer -- was handed over as one, and the command
    // processor ran its size modulo 2^20)
    std::vector<std::pair<uint32_t, uint32_t>> runs = pm4::ibRuns(w);
    if (runs.size() > Gpu::kMaxSubmitCmds) {
        cb->error_ = "a command stream of " + std::to_string(w.size()) + " words: more than the " + std::to_string(Gpu::kMaxSubmitCmds) +
                     " indirect buffers of " + std::to_string(pm4::kMaxIbWords) + " words one submission takes";
        cb->finish(CommandBuffer::kError);
        return;
    }
    for (auto& r : runs) r.first *= 4;   // (in bytes into the command buffer's memory)
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
    // (its words are in the GPU's buffer now: the memory goes to the next command buffer, a few kept at most)
    if (spareStreams_.size() < kSpareStreams) spareStreams_.push_back(cb->stream_.release());
    cb->status_ = CommandBuffer::kCommitted;
    cb->faultsAtCommit_ = gpu_.faults();
    cb->submitCallAt_ = gpu_.transport().nowNs();
    int e = gpu_.submit(id_, *cb->commands_, runs, cb->uses_, &cb->fence_);
    cb->submitReturnAt_ = gpu_.transport().nowNs();
    if (e != 0) {
        cb->error_ = std::string("the driver refused the submission: ") + strerror(e);
        idle_.push_back(std::move(cb->commands_));
        g.unlock();                         // (the handlers may commit more: not under the queue's lock)
        cb->finish(CommandBuffer::kError);
        return;
    }
    cb->status_ = CommandBuffer::kScheduled;
    inFlight_.push_back(std::move(cb));
    changed_.notify_all();
}

void CommandQueue::waiter(const std::shared_ptr<bool>& orphaned) {
    std::unique_lock<std::mutex> g(lock_);
    for (;;) {
        changed_.wait(g, [this] { return quit_ || !inFlight_.empty(); });
        if (inFlight_.empty()) {
            return;  // quitting, and nothing left in flight
        }
        std::shared_ptr<CommandBuffer> cb = inFlight_.front();
        // (the queue held while this one is finished and put away: its handlers may let go of every other reference)
        std::shared_ptr<CommandQueue> self = cb->owner_;
        g.unlock();
        int e = gpu_.wait(id_, cb->fence_, kPatienceNs);
        cb->completedAt_ = gpu_.transport().nowNs();
        if (cb->completedAt_ - cb->committedAt_ > 50000000 && e == 0) {
            // (a slow one: the kernel's account of where the time went, asked for here, off the handlers' path)
            cb->haveKernelTimes_ = gpu_.fenceTimes(cb->fence_, &cb->kernelTimes_) == 0;
        }
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
        // a timeout with no reset means the GPU is still able to be running this submission: its command BO, slabs and
        // stamp buffers must not be reused or freed under it
        const bool poisoned = (e == ETIMEDOUT && !givenUp);
        if (poisoned) {
            // held for the queue's life; its commands_ and buffers are never touched again. Put there before its
            // handlers run and its waiters wake, so that what they ask of the queue then says so (quarantinedCount
            // read after waitUntilCompleted found it not there yet, now and then)
            g.lock();
            quarantined_.push_back(cb);
            g.unlock();
        }
        cb->finish(e == 0 ? CommandBuffer::kCompleted : CommandBuffer::kError, !poisoned);
        g.lock();
        inFlight_.pop_front();
        if (poisoned) {
            cb->owner_.reset();           // (self holds the queue to the end of this turn)
        } else if (cb->commands_ != nullptr) {
            idle_.push_back(std::move(cb->commands_));
        }
        g.unlock();
        // The buffer let go of first (its stamps go back to the queue's pool, which SELF keeps), then the queue: SELF
        // may be the last reference, and the queue's destructor then runs here, on this thread, and says so through
        // ORPHANED -- after which nothing of the queue may be touched.
        cb.reset();
        self.reset();
        if (*orphaned) return;
        g.lock();
    }
}

bool CommandBuffer::pathHops(double out[10]) const {
    auto ms = [](int64_t a, int64_t b) { return a != 0 && b != 0 ? double(b - a) / 1e6 : -1.0; };
    const HakiosFenceTimes& k = kernelTimes_;
    auto kms = [](uint64_t a, uint64_t b) { return a != 0 && b != 0 && b >= a ? double(b - a) / 1e6 : -1.0; };
    out[0] = ms(commitCalledAt_, committedAt_);
    out[1] = ms(committedAt_, submitCallAt_);
    out[2] = ms(submitCallAt_, submitReturnAt_);
    out[3] = haveKernelTimes_ ? kms(k.enterNs, k.lockedNs) : -1.0;
    out[4] = haveKernelTimes_ ? kms(k.lockedNs, k.ringNs) : -1.0;
    out[5] = haveKernelTimes_ ? kms(k.ringNs, k.startNs) : -1.0;
    out[6] = haveKernelTimes_ ? kms(k.startNs, k.endNs) : -1.0;
    out[7] = haveKernelTimes_ ? kms(k.endNs, k.irqNs) : -1.0;
    // (the kernel's clock and this one are the same while the machine has not slept)
    out[8] = haveKernelTimes_ && k.irqNs != 0 ? ms(int64_t(k.irqNs), completedAt_) : -1.0;
    out[9] = ms(completedAt_, handlersAt_);
    return haveKernelTimes_;
}

int64_t CommandBuffer::nowNs() const { return queue_.gpuClockNs(); }

}  // namespace hakios::metal
