// A host benchmark of the per-draw encoding a CoreAnimation-like pass pays (CommandBuffer::draw): ~100 draws a pass,
// several pipelines (flat, textured, vertex-buffer, blended), buffers and textures bound per draw, a scissor each, in
// a GMEM pass with binning and the plug-in's queue settings, through the fake GPU. Prints the best ns per draw.
//   make bench   (HAKIOS_METAL_BENCH=N rounds; BENCH_MODE=gmem|sys|dual)
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hakios-metal/command.hpp"
#include "hakios-metal/fake_gpu.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/texture.hpp"
#include "../tests/fixtures/tri_attr_vs_air.hpp"
#include "../tests/fixtures/tri_color_fs_air.hpp"
#include "../tests/fixtures/tri_color_vs_air.hpp"
#include "../tests/fixtures/tri_flat_fs_air.hpp"
#include "../tests/fixtures/tri_flat_vs_air.hpp"
#include "../tests/fixtures/tri_tex_fs_air.hpp"

#ifdef PROF
extern volatile int g_prof_on;   // (a sampling profiler's switch: on only inside the draw loop)
#define PROF_ON(v) (g_prof_on = (v))
#else
#define PROF_ON(v) ((void)0)
#endif

using namespace hakios::metal;
namespace kernels = hakios::metal::kernels;

int main() {
    auto f = std::make_unique<FakeGpu>();
    FakeGpu* fake = f.get();
    Gpu gpu(std::move(f));
    std::string why;
    auto queue = CommandQueue::make(gpu, &why);
    if (!queue) { std::printf("no queue: %s\n", why.c_str()); return 1; }
    const char* mode = getenv("BENCH_MODE") ? getenv("BENCH_MODE") : "gmem";
    auto tex = [&](uint32_t w, uint32_t h, uint32_t usage) {
        TextureDesc d;
        d.format = PixelFormat::kRGBA8Unorm;
        d.width = w; d.height = h; d.usage = usage;
        auto t = Texture::make(gpu, d, &why);
        if (!t) { std::printf("texture: %s\n", why.c_str()); exit(1); }
        return t;
    };
    auto target = tex(828, 1792, kTextureUsageRenderTarget | kTextureUsageRead);
    std::vector<std::shared_ptr<Texture>> src;
    for (int i = 0; i < 8; i++) src.push_back(tex(64 + 32 * i, 64 + 16 * i, kTextureUsageRead));
    Sampler smp;
    if (!makeSampler(SamplerDesc{}, &smp, &why)) return 1;
    auto vb = newBuffer(gpu, 1 << 16, &why);
    auto ub = newBuffer(gpu, 1 << 16, &why);
    if (!vb || !ub) return 1;

    Program flat{kernels::tri_flat_vs_air, kernels::tri_flat_fs_air, Linkage{}, "tri_flat"};
    Program tx{kernels::tri_color_vs_air, kernels::tri_tex_fs_air, Linkage{}, "tri_tex"};
    Program attr{kernels::tri_attr_vs_air, kernels::tri_color_fs_air, Linkage{}, "tri_attr"};
    attr.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat2, 0, 0};
    attr.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kUChar4Normalized, 8, 0};
    attr.vertexDescriptor.layouts[0] = VertexLayout{12, VertexStep::kPerVertex, 1};
    Program txBlend = tx;
    txBlend.blend = ColorBlend{true, 0, 0, 1, 7, 1, 7, 0xf};   // One, OneMinusSrcAlpha
    // BENCH_SYN=1: what an uber shader's draw adds to these small kernels' -- immediates uploaded each draw, a constant
    // range from each stage's buffer (at 8 mod 16, as CoreAnimation binds its uniforms: copied by the command processor)
    static uint32_t imm[128];
    for (uint32_t i = 0; i < 128; i++) imm[i] = 0x3f800000u + i;
    const bool syn = getenv("BENCH_SYN") != nullptr;
    if (syn) {
        for (Program* p : {&flat, &tx, &attr, &txBlend}) {
            for (Stage* s : {&p->vs, &p->fs}) {
                Kernel& k = s->k;
                k.immediates = imm;
                k.numImmediates = 128;
                k.immediatesOffsetVec4 = 0;
                k.constlen = std::max<uint32_t>(k.constlen, 64);
                k.uboRanges[0] = KernelUboRange{0, 36, 0, 64};
                k.numUboRanges = 1;
            }
        }
    }
    const uint64_t skew = syn ? 8 : 0;
    Program* progs[4] = {&flat, &tx, &attr, &txBlend};

    RenderPass pass;
    pass.color0 = target.get();
    pass.load = LoadAction::kLoad;
    render::Options o;
    o.gmem = strcmp(mode, "sys") != 0;
    o.gmemBinning = true;
    // BENCH_OFF=rast,vfd,...: those Options groups off, to see what each costs a draw
    if (const char* off = getenv("BENCH_OFF")) {
        const std::string s = off;
        auto has = [&](const char* k) { return s.find(k) != std::string::npos; };
        if (has("rast")) o.rast = false;
        if (has("blendZsa")) o.blendZsa = false;
        if (has("vfd")) o.vfd = false;
        if (has("programConfig")) o.programConfig = false;
        if (has("programVs")) o.programVs = false;
        if (has("programFs")) o.programFs = false;
        if (has("fsTextures")) o.fsTextures = false;
        if (has("vsTextures")) o.vsTextures = false;
        if (has("viewportScissor")) o.viewportScissor = false;
        if (has("zsNone")) o.zsNone = false;
        if (has("mrt")) o.mrt = false;
        if (has("msaaOff")) o.msaaOff = false;
        if (has("renderCntl")) o.renderCntl = false;
        if (has("lrzOff")) o.lrzOff = false;
        if (has("vpc")) o.vpc = false;
        if (has("fsInputs")) o.fsInputs = false;
        if (has("fsOutputs")) o.fsOutputs = false;
    }
    const int draws = 100;
    const char* env = getenv("HAKIOS_METAL_BENCH");
    const int rounds = env ? atoi(env) : 200;
    double best = 1e30, sum = 0;
    for (int round = 0; round < rounds + 3; round++) {
        auto cb = queue->commandBuffer();
        cb->setGmem(o.gmem);
        cb->setGmemMinDraws(strcmp(mode, "dual") == 0 ? 56 : 0);
        cb->setGmemBinning(true);
        cb->setGmemTuned(true);
        PROF_ON(1);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < draws; i++) {
            Program& p = *progs[i % 4];
            Draw d;
            d.vertexCount = 6;
            d.viewport = Viewport{0, 0, 828, 1792, 0, 1};
            d.scissor = Scissor{uint32_t(i * 7 % 300), uint32_t(i * 13 % 900), 200 + uint32_t(i % 50), 100 + uint32_t(i % 70)};
            Bindings vsb, fsb;
            if (&p == &attr) vsb.buffers = {Binding{vb.get(), uint64_t(i % 16) * 256 + skew}};
            else vsb.buffers = {Binding{ub.get(), uint64_t(i % 16) * 256 + skew}, Binding{ub.get(), 4096 + skew}};
            fsb.buffers = {Binding{ub.get(), uint64_t(i % 8) * 256 + skew}};
            if (&p == &tx || &p == &txBlend) {
                fsb.textures = {src[i % 8].get()};
                fsb.samplers = {&smp};
            }
            if (!cb->draw(p, pass, d, vsb, fsb, o)) { std::printf("draw %d: %s\n", i, cb->error().c_str()); return 1; }
        }
        PROF_ON(0);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / draws;
        if (round >= 3) { best = std::min(best, us); sum += us; }
        if (getenv("BENCH_NOCOMMIT")) continue;   // (for a profile of the encoding alone: the fake GPU's run is not ours)
        cb->commit();
        cb->waitUntilCompleted();
        if (cb->status() != CommandBuffer::kCompleted) { std::printf("not completed: %s | %s\n", cb->error().c_str(), fake->stats().lastFault.c_str()); return 1; }
    }
    std::printf("bench_draw[%s]: %.0f ns a draw at best, %.0f on average (%d draws a buffer, %d rounds)\n", mode, best * 1e3, sum * 1e3 / rounds, draws, rounds);
    return 0;
}
