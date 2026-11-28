// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include "common/memory_write_trace.h"
#include "common/types.h"

namespace Core::FileSys {

// Read first so EOF, errors and short reads cannot discard other GPU-owned bytes.
// The staging array is reused on this thread and its bytes need no initialization.
class StagedRead {
public:
    std::optional<s64> Copy(void* dst, size_t requested, auto&& read, auto&& invalidate) {
        if (requested > capacity) {
            if (requested > static_cast<size_t>(std::numeric_limits<s64>::max())) {
                return std::nullopt;
            }
            std::unique_ptr<u8[]> next{new (std::nothrow) u8[requested]};
            if (!next) {
                return std::nullopt;
            }
            data = std::move(next);
            capacity = requested;
        }
        if (requested == 0) {
            return 0;
        }
        const s64 bytes = read(data.get(), requested);
        if (bytes <= 0) {
            return bytes;
        }
        if (static_cast<u64>(bytes) > requested) {
            return -1;
        }
        const auto address = reinterpret_cast<VAddr>(dst);
        invalidate(address, bytes);
        Common::MemoryWriteTrace::Event("file.copy.begin", address, bytes,
                                        Common::MemoryWriteTrace::SourceWord(data.get(), bytes));
        std::memcpy(dst, data.get(), bytes);
        Common::MemoryWriteTrace::Event("file.copy.end", address, bytes);
        return bytes;
    }

private:
    std::unique_ptr<u8[]> data;
    size_t capacity{};
};

inline StagedRead& ThreadReadBuffer() {
    static thread_local StagedRead buffer;
    return buffer;
}

} // namespace Core::FileSys
