#include "hakios-metal/gpu.hpp"
#include "hakios-metal/pm4.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <cstdlib>
#include <cstring>

namespace hakios::metal {

namespace {
constexpr uint64_t kPage = 0x4000;  // Darwin's page; the driver rounds to its own (4 KB) inside it

MsmTimespec deadline(Transport& t, int64_t ns) {
    // the driver takes a deadline, not a length: a time of the clock that goes on while the machine sleeps
    // (mach_continuous_time, which is Darwin's CLOCK_MONOTONIC), as Linux's msm takes one of CLOCK_MONOTONIC
    if (ns < 0) {
        ns = 0;
    }
    int64_t at = t.nowNs() + ns;
    return MsmTimespec{at / 1000000000, at % 1000000000};
}
}  // namespace

int Gpu::param(uint32_t which, uint64_t* value) {
    MsmParam p{};
    p.pipe = kPipe3D;
    p.param = which;
    int e = t_->call(kGetParam, &p, sizeof(p));
    if (e == 0) {
        *value = p.value;
    }
    return e;
}

uint32_t Gpu::chipId() {
    uint64_t v = 0;
    return param(kParamChipId, &v) == 0 ? uint32_t(v) : 0;
}

uint64_t Gpu::faults() {
    uint64_t v = 0;
    return param(kParamFaults, &v) == 0 ? v : 0;
}

std::unique_ptr<Bo> Gpu::newBo(uint64_t size, uint32_t flags, int* error) {
    *error = 0;
    if (size == 0) {
        *error = EINVAL;
        return nullptr;
    }
    MsmGemNew n{};
    n.size = (size + kPage - 1) & ~(kPage - 1);
    n.flags = flags;
    if ((*error = t_->call(kGemNew, &n, sizeof(n))) != 0) {
        return nullptr;
    }
    MsmGemInfo info{};
    info.handle = n.handle;
    info.info = kInfoGetIova;
    if ((*error = t_->call(kGemInfo, &info, sizeof(info))) != 0) {
        DrmGemClose c{n.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        return nullptr;
    }
    void* cpu = t_->map(n.handle, n.size);
    if (cpu == nullptr) {
        DrmGemClose c{n.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        *error = ENOMEM;
        return nullptr;
    }
    std::unique_ptr<Bo> bo(new Bo(*this, n.handle, n.size, info.value, cpu));
    bo->counted_ = true;
    bo->flags_ = flags;
    allocated_ += n.size;
    return bo;
}

std::unique_ptr<Bo> Gpu::surfaceBo(uint32_t surfaceId, SurfaceInfo* out, int* error) {
    HakiosSurface s{};
    s.surface = surfaceId;
    if ((*error = t_->call(kHakiosSurface, &s, sizeof(s))) != 0) {
        return nullptr;
    }
    MsmGemInfo info{};
    info.handle = s.handle;
    info.info = kInfoGetIova;
    if ((*error = t_->call(kGemInfo, &info, sizeof(info))) != 0) {
        DrmGemClose c{s.handle, 0};
        t_->call(kGemClose, &c, sizeof(c));
        return nullptr;
    }
    *out = SurfaceInfo{s.size, s.offset, s.rowBytes, s.width, s.height, s.pixelFormat, s.planes};
    return std::unique_ptr<Bo>(new Bo(*this, s.handle, s.size, info.value, t_->map(s.handle, s.size)));
}

void Gpu::disown(Bo& bo) {
    if (bo.cpu_ != nullptr) {
        t_->unmap(bo.handle_, bo.cpu_);
        bo.cpu_ = nullptr;
    }
    bo.owned_ = false;
    if (bo.counted_) {
        allocated_ -= bo.size_;
        bo.counted_ = false;
    }
}

void Gpu::disown(std::unique_ptr<Bo> bo) {
    if (bo != nullptr) disown(*bo);
}

Bo::~Bo() { gpu_.release(*this); }

void Gpu::release(Bo& bo) {
    if (!bo.owned_) {
        return;
    }
    if (bo.counted_) {
        allocated_ -= bo.size_;
        bo.counted_ = false;
    }
    if (bo.cpu_ != nullptr) {
        t_->unmap(bo.handle_, bo.cpu_);
    }
    DrmGemClose c{bo.handle_, 0};
    t_->call(kGemClose, &c, sizeof(c));
}

std::atomic<uint64_t> gRecycleMissesTotal{0};

bool memoryLow() {
#if defined(__APPLE__)
    // (the run 224603: at 212 s, with a scripted workload on, IOSurface allocations began to fail and processes to crash, and the kernel panicked at
    // 239 s: the plug-in's caches -- 256 MB of texture memory and 128 MB of slabs a queue -- are memory the system then wanted)
    static std::atomic<int64_t> checkedNs{0};
    static std::atomic<bool> low{false};
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - checkedNs.load() < 1000000000) return low.load();   // (one sysctl a second at most, whoever asks)
    checkedNs = now;
    unsigned int freePages = 0;
    size_t size = sizeof(freePages);
    uint64_t memsize = 0;
    size_t msize = sizeof(memsize);
    static const uint64_t total = sysctlbyname("hw.memsize", &memsize, &msize, nullptr, 0) == 0 ? memsize : 1;
    if (sysctlbyname("vm.page_free_count", &freePages, &size, nullptr, 0) != 0 || total == 0) return low.load();
    const uint64_t freeBytes = uint64_t(freePages) * 16384;   // (a page is 16 KB on this kernel; the threshold is generous for 4 KB ones)
    low = freeBytes < (64ull << 20);   // (really short: under 64 MB free. The run with 192 MB or 1/16 of the memory as the bar kept nothing and cost 4 fps)
    return low.load();
#else
    return false;
#endif
}

std::atomic<uint64_t> gRecycleHits{0}, gRecycleMisses{0}, gRecycleTrimmed{0}, gRecycleCached{0};   // (c_api.cpp's tally, all Gpus)

namespace {
uint64_t poolBytes() {
    static const uint64_t v = [] {
        const char* e = getenv("HAKIOS_METAL_TEXPOOL_MB");
        const long mb = e != nullptr && e[0] != 0 ? strtol(e, nullptr, 0) : 256;
        return uint64_t(mb < 0 ? 0 : mb) << 20;
    }();
    return v;
}
std::chrono::seconds poolSeconds() {
    static const long v = [] {
        const char* e = getenv("HAKIOS_METAL_TEXPOOL_SECONDS");
        return e != nullptr && e[0] != 0 ? strtol(e, nullptr, 0) : 4L;
    }();
    return std::chrono::seconds(v);
}
}  // namespace

// (under cacheLock_) what is older than the time, then the oldest until ROOM more bytes fit under the cap; the Bos freed
// here are closed under the lock, which is a kernel call each: few, and only when the cache is full or old
void Gpu::trimLocked(std::chrono::steady_clock::time_point now, uint64_t room) {
    for (auto it = cache_.begin(); it != cache_.end();) {
        auto& v = it->second;
        while (!v.empty() && now - v.front().since > poolSeconds()) {
            cachedBytes_ -= v.front().bo->size();
            v.erase(v.begin());
            cacheTrimmed_++;
            gRecycleTrimmed++;
        }
        it = v.empty() ? cache_.erase(it) : std::next(it);
    }
    while (cachedBytes_ + room > poolBytes() && !cache_.empty()) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.front().since < oldest->second.front().since) oldest = it;
        }
        cachedBytes_ -= oldest->second.front().bo->size();
        oldest->second.erase(oldest->second.begin());
        cacheTrimmed_++;
        gRecycleTrimmed++;
        if (oldest->second.empty()) cache_.erase(oldest);
    }
}

std::unique_ptr<Bo> Gpu::newBoRecycled(uint64_t size, uint32_t flags, int* error) {
    const uint64_t rounded = (size + kPage - 1) & ~(kPage - 1);
    if (poolBytes() != 0) {
        std::lock_guard<std::mutex> g(cacheLock_);
        auto it = cache_.find({rounded, flags});
        if (it != cache_.end() && !it->second.empty()) {
            std::unique_ptr<Bo> bo = std::move(it->second.back().bo);   // (the last given: warmest in the caches)
            it->second.pop_back();
            if (it->second.empty()) cache_.erase(it);
            cachedBytes_ -= bo->size();
            cacheHits_++;
            gRecycleHits++;
            gRecycleCached = cachedBytes_;
            allocated_ += bo->size_;
            bo->counted_ = true;
            *error = 0;
            return bo;
        }
        cacheMisses_++;
        gRecycleMisses++;
        gRecycleMissesTotal++;
    }
    return newBo(size, flags, error);
}

void Gpu::recycleBo(std::unique_ptr<Bo> bo) {
    if (bo == nullptr) return;
    // (only what this driver made and still owns, of a size worth keeping: not a surface's memory, nor a handle another owns)
    if (poolBytes() == 0 || memoryLow() || !bo->owned_ || !bo->counted_ || bo->cpu_ == nullptr || bo->size_ < (64u << 10) || bo->size_ > poolBytes() / 4) return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> g(cacheLock_);
    trimLocked(now, bo->size_);
    allocated_ -= bo->size_;
    bo->counted_ = false;
    cachedBytes_ += bo->size_;
    const std::pair<uint64_t, uint32_t> key{bo->size_, bo->flags_};
    cache_[key].push_back(Cached{std::move(bo), now});
    gRecycleCached = cachedBytes_;
}

Gpu::RecycleStats Gpu::recycleStats(bool reset) {
    std::lock_guard<std::mutex> g(cacheLock_);
    trimLocked(std::chrono::steady_clock::now(), memoryLow() ? poolBytes() + 1 : 0);   // (the time's trim, and everything when memory is short, as the report comes round: an idle process lets go too)
    gRecycleCached = cachedBytes_;
    RecycleStats s{cacheHits_, cacheMisses_, cacheTrimmed_, cachedBytes_};
    if (reset) cacheHits_ = cacheMisses_ = cacheTrimmed_ = 0;
    return s;
}

Gpu::~Gpu() {
    {
        std::lock_guard<std::mutex> g(cacheLock_);
        cache_.clear();
        cachedBytes_ = 0;
    }
    {
        std::unique_lock<std::mutex> g(queuesLock_);
        queuesGone_.wait(g, [this] { return queues_ == 0; });
    }
    forgetSurfaceBuffers(this);
}

Gpu::QueueHold::QueueHold(Gpu& gpu) : gpu_(gpu) {
    std::lock_guard<std::mutex> g(gpu_.queuesLock_);
    gpu_.queues_++;
}

Gpu::QueueHold::~QueueHold() {
    // (told under the lock: once it is let go the Gpu may be destroyed, and nothing of it is touched after)
    std::lock_guard<std::mutex> g(gpu_.queuesLock_);
    if (--gpu_.queues_ == 0) gpu_.queuesGone_.notify_all();
}

int Gpu::newQueue(uint32_t priority, uint32_t* id) {
    MsmSubmitQueue q{0, priority, 0};
    int e = t_->call(kSubmitQueueNew, &q, sizeof(q));
    if (e == 0) {
        *id = q.id;
    }
    return e;
}

void Gpu::closeQueue(uint32_t id) {
    if (id != 0) {
        uint32_t q = id;
        t_->call(kSubmitQueueClose, &q, sizeof(q));
    }
}

int Gpu::submit(uint32_t queue, const Bo& commands, uint32_t offsetBytes, uint32_t words, const std::vector<BoUse>& uses,
                uint32_t* fence) {
    return submit(queue, commands, std::vector<std::pair<uint32_t, uint32_t>>{{offsetBytes, words}}, uses, fence);
}

int Gpu::submit(uint32_t queue, const Bo& commands, const std::vector<std::pair<uint32_t, uint32_t>>& runs, const std::vector<BoUse>& uses,
                uint32_t* fence) {
    // (each run is a CP_INDIRECT_BUFFER of its own in the kernel's ring, whose IB_SIZE holds 20 bits: a longer one would
    // run cut short, so it is refused here, as is a count the kernel would refuse)
    if (runs.empty() || runs.size() > kMaxSubmitCmds) {
        return EINVAL;
    }
    for (const auto& [offsetBytes, words] : runs) {
        if (words == 0 || words > pm4::kMaxIbWords || offsetBytes % 4 != 0 || offsetBytes + 4ull * words > commands.size()) {
            return EINVAL;
        }
    }
    // the command buffer is the first of the buffers; the others follow it once each, read or written
    std::vector<MsmSubmitBo> bos;
    bos.push_back(MsmSubmitBo{kSubmitBoRead, commands.handle(), commands.gpuAddress()});
    for (const BoUse& u : uses) {
        if (u.bo == &commands) {
            continue;
        }
        bool seen = false;
        for (MsmSubmitBo& b : bos) {
            if (b.handle == u.bo->handle()) {
                b.flags |= u.write ? kSubmitBoWrite : kSubmitBoRead;
                seen = true;
            }
        }
        if (!seen) {
            bos.push_back(MsmSubmitBo{u.write ? kSubmitBoWrite : kSubmitBoRead, u.bo->handle(), u.bo->gpuAddress()});
        }
    }
    std::vector<MsmSubmitCmd> cmds(runs.size());
    for (size_t i = 0; i < runs.size(); i++) {
        cmds[i].type = kSubmitCmdBuf;
        cmds[i].submit_idx = 0;
        cmds[i].submit_offset = runs[i].first;
        cmds[i].size = runs[i].second * 4;
    }
    MsmGemSubmit s{};
    s.flags = kPipe3D;
    s.nr_bos = uint32_t(bos.size());
    s.nr_cmds = uint32_t(cmds.size());
    s.bos = uint64_t(uintptr_t(bos.data()));
    s.cmds = uint64_t(uintptr_t(cmds.data()));
    s.fence_fd = -1;
    s.queueid = queue;
    int e = t_->call(kGemSubmit, &s, sizeof(s));
    if (e == 0) {
        *fence = s.fence;
    }
    return e;
}

int Gpu::wait(uint32_t queue, uint32_t fence, int64_t timeoutNs) {
    MsmWaitFence w{};
    w.fence = fence;
    w.timeout = deadline(*t_, timeoutNs);
    w.queueid = queue;
    return t_->call(kWaitFence, &w, sizeof(w));
}

int Gpu::fenceTimes(uint32_t fence, HakiosFenceTimes* out) {
    *out = HakiosFenceTimes{};
    out->fence = fence;
    return t_->call(kHakiosFenceTimes, out, sizeof(*out));
}

int Gpu::cpuPrep(const Bo& bo, uint32_t op, int64_t timeoutNs) {
    MsmGemCpuPrep p{bo.handle(), op, deadline(*t_, timeoutNs)};
    return t_->call(kGemCpuPrep, &p, sizeof(p));
}

void Gpu::cpuFini(const Bo& bo) {
    MsmGemCpuFini f{bo.handle()};
    t_->call(kGemCpuFini, &f, sizeof(f));
}

}  // namespace hakios::metal
