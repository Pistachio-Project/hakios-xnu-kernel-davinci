// The Adreno command processor's packets (PM4), as the A6xx family reads them (here the Adreno 7c+ Gen 3 of the
// SM7325, chip id 0x06030500, an A635-family part to Mesa).
//
// Two kinds are written here, the same two freedreno writes on these GPUs:
//   type 4   "write COUNT registers from REG on":  0x4 << 28 | count | parity(count) << 7 | reg << 8 | parity(reg) << 27
//   type 7   "run OPCODE with COUNT words":        0x7 << 28 | count | parity(count) << 15 | opcode << 16 | parity(opcode) << 23
// The parity bits are odd parity of the field they follow. The command processor checks them and stops on a wrong one
// (a hang of the GPU, not an error the program sees), so they are worked out here and checked by the tests against
// numbers taken from Mesa (src/freedreno/common/freedreno_pm4.h, pm4_pkt4_hdr and pm4_pkt7_hdr).
//
// The opcodes are Mesa's src/freedreno/registers/adreno/adreno_pm4.xml, checked one by one against it.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace hakios::metal::pm4 {

enum Opcode : uint8_t {
    kNop = 0x10,              // CP_NOP: the words that follow are skipped (markers, padding)
    kWaitMemWrites = 0x12,    // CP_WAIT_MEM_WRITES: the memory writes before it have landed
    kWaitForMe = 0x13,        // CP_WAIT_FOR_ME: the micro engine has caught up with the prefetch parser
    kWaitForIdle = 0x26,      // CP_WAIT_FOR_IDLE: everything before it is done
    kMemWrite = 0x3d,         // CP_MEM_WRITE: address (2 words), then the words to write there
    kIndirectBuffer = 0x3f,   // CP_INDIRECT_BUFFER: address (2 words), size in words
    kMemToMem = 0x73,         // CP_MEM_TO_MEM
    kMemcpy = 0x75,           // CP_MEMCPY (A6xx and later): words, source (2 words), destination (2 words)
};

// odd parity of the low 32 bits: 1 when the number of ones is even, so that the field and its bit have an odd count
constexpr uint32_t oddParity(uint32_t v) {
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v &= 0xf;
    return (~0x6996u >> v) & 1u;
}

constexpr uint32_t kType4 = 0x40000000u;
constexpr uint32_t kType7 = 0x70000000u;
constexpr uint32_t kMaxType4Count = 0x7e;     // a type-4 count has 7 bits, and Mesa keeps it below 0x7f
constexpr uint32_t kMaxType7Count = 0x3fff;   // a type-7 count has 14 bits
// CP_INDIRECT_BUFFER's IB_SIZE is 20 bits of words (adreno_pm4.xml, A5XX-): the command processor runs a longer
// buffer's size modulo 2^20 -- the rest of it never, or the cut at a packet's middle read on as packets
constexpr uint32_t kMaxIbWords = 0xfffff;

constexpr uint32_t type4(uint32_t reg, uint32_t count) {
    return kType4 | count | oddParity(count) << 7 | (reg & 0x3ffffu) << 8 | oddParity(reg) << 27;
}

constexpr uint32_t type7(uint32_t opcode, uint32_t count) {
    return kType7 | count | oddParity(count) << 15 | (opcode & 0x7fu) << 16 | oddParity(opcode) << 23;
}

// What a header says, and whether its parity holds: for the tests and for reading a stream back (the fake GPU).
struct Header {
    enum Kind { kRegisters, kCommand, kBad } kind = kBad;
    uint32_t reg = 0;         // type 4: the first register
    uint32_t opcode = 0;      // type 7
    uint32_t count = 0;       // the words after the header
};

// The same for a stream this process wrote itself (appendStream, appendDeduped copy one packet at a time, a hundred
// and more a draw): the header's kind by its top nibble, no parity checked, as the encoder put the headers there
inline Header decodeOwn(uint32_t word) {
    Header h;
    switch (word >> 28) {
    case 4:
        h.count = word & 0x7f;
        h.reg = (word >> 8) & 0x3ffff;
        h.kind = Header::kRegisters;
        break;
    case 7:
        h.count = word & 0x3fff;
        h.opcode = (word >> 16) & 0x7f;
        h.kind = Header::kCommand;
        break;
    default:
        break;
    }
    return h;
}

// WORDS, whole packets, cut into runs of at most MAX words each, at packet boundaries: (first word, words) of each, in
// order -- what one CP_INDIRECT_BUFFER can take (kMaxIbWords), a run apiece
std::vector<std::pair<uint32_t, uint32_t>> ibRuns(const std::vector<uint32_t>& words, uint32_t max = kMaxIbWords);

// (inline: appendStream and appendDeduped decode every word of every draw they copy, and the encoder writes each
// register packet through registers(): calls through the library's boundary, one per packet, were a draw's cost)
inline Header decode(uint32_t word) {
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

// A stream of packets being written. Addresses are the GPU's (the IOVA of a buffer plus an offset).
class Stream {
public:
    void registers(uint32_t reg, const uint32_t* values, uint32_t count) {
        // more than a header can count goes as several packets, each starting where the one before stopped
        ready();
        while (count > 0) {
            const uint32_t n = count < kMaxType4Count ? count : kMaxType4Count;
            uint32_t* at = grow(1 + n);
            at[0] = type4(reg, n);
            for (uint32_t i = 0; i < n; i++) at[1 + i] = values[i];
            reg += n;
            values += n;
            count -= n;
        }
    }
    // one packet of PREFIX (N words, the load's header and addresses) then DATA (M words) then PAD zeros: written in place
    void packetOf(uint32_t opcode, const uint32_t* prefix, uint32_t n, const uint32_t* data, uint32_t m, uint32_t pad) {
        assert(n + m + pad <= kMaxType7Count);
        ready();
        uint32_t* at = grow(1 + n + m + pad);
        at[0] = type7(opcode, n + m + pad);
        for (uint32_t i = 0; i < n; i++) at[1 + i] = prefix[i];
        for (uint32_t i = 0; i < m; i++) at[1 + n + i] = data[i];
        for (uint32_t i = 0; i < pad; i++) at[1 + n + m + i] = 0;
    }
    void nop(uint32_t words = 0);
    void waitForIdle();
    void waitMemWrites();
    void waitForMe();
    void memWrite(uint64_t address, const uint32_t* values, uint32_t count);
    void memcpy(uint64_t destination, uint64_t source, uint32_t words);
    void indirect(uint64_t address, uint32_t words);
    // any type-7 packet: OPCODE with WORDS after its header (the compute dispatch's packets, compute.cpp)
    void packet(uint32_t opcode, const uint32_t* words, uint32_t count) {
        assert(count <= kMaxType7Count);
        ready();
        uint32_t* at = grow(1 + count);
        at[0] = type7(opcode, count);
        for (uint32_t i = 0; i < count; i++) at[1 + i] = words[i];
    }

    // packets already encoded (another stream's words, or what the encoder wrote earlier), as they are
    void append(const uint32_t* words, size_t count) {
        ready();
        words_.insert(words_.end(), words, words + count);
    }
    const std::vector<uint32_t>& words() const { return words_; }
    size_t size() const { return words_.size(); }
    size_t capacity() const { return words_.capacity(); }
    void clear() { words_.clear(); }   // (the memory stays: a stream used again grows no more)
    void reserve(size_t words) { words_.reserve(words); }
    // the words' memory handed on, the stream left empty; and memory taken over, emptied: a command buffer's stream goes
    // back to its queue once its words are in the GPU's buffer, so that the next command buffer does not grow its own
    // from 512 words again, every doubling a copy and every large free a madvise (CommandQueue::submit)
    std::vector<uint32_t> release() {
        std::vector<uint32_t> w = std::move(words_);
        words_.clear();
        return w;
    }
    void adopt(std::vector<uint32_t>&& w) {
        w.clear();
        words_ = std::move(w);
    }

private:
    void command(uint32_t opcode, uint32_t count) {
        assert(count <= kMaxType7Count);
        ready();
        words_.push_back(type7(opcode, count));
    }
    // room for N more words, which the caller writes: where they start (the vector's size grows by N, zeros past the old end
    // until written -- one resize rather than a push_back a word, each with its own look at the capacity)
    uint32_t* grow(size_t n) {
        const size_t at = words_.size();
        words_.resize(at + n);
        return words_.data() + at;
    }
    // a stream's first words: room for a draw's at once (most streams are a draw's or a pass's head), not a word at a time
    void ready() {
        if (words_.capacity() == 0) words_.reserve(512);
    }
    void address(uint64_t a) {
        words_.push_back(uint32_t(a));
        words_.push_back(uint32_t(a >> 32));
    }
    std::vector<uint32_t> words_;
};

}  // namespace hakios::metal::pm4
