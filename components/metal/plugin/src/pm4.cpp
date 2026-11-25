#include "hakios-metal/pm4.hpp"

#include <cassert>

namespace hakios::metal::pm4 {

std::vector<std::pair<uint32_t, uint32_t>> ibRuns(const std::vector<uint32_t>& words, uint32_t max) {
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    uint32_t start = 0;
    for (size_t i = 0; i < words.size();) {
        const size_t next = i + 1 + decode(words[i]).count;   // (a packet is at most 1 + kMaxType7Count words, far below MAX)
        if (next - start > max) {
            runs.emplace_back(start, uint32_t(i - start));
            start = uint32_t(i);
        }
        i = next;
    }
    if (start < words.size()) runs.emplace_back(start, uint32_t(words.size() - start));
    return runs;
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

}  // namespace hakios::metal::pm4
