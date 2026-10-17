#pragma once

#include <array>
#include "common/types.h"

namespace Vulkan::VrPouchTable {

struct Key {
    u32 count;
    u64 hash;
};

inline constexpr std::array<Key, 12> Keys{{
    {183, 0x111a013acf0a8f86ull},
    {183, 0xa00326df6afd3b19ull},
    {183, 0xba54d040e2f74f10ull},
    {186, 0x982430eddd4b33c8ull},
    {558, 0xd1988d39d83f829full},
    {561, 0x42f5b44e5e9f9d8aull},
    {561, 0x7b7a4914336a1599ull},
    {561, 0xf16d6e309a88a3eeull},
    {936, 0x42e5fb2253fcf9b7ull},
    {936, 0xb132ee85486afcf1ull},
    {936, 0xb80fbf6a8c52932cull},
    {936, 0xecc16aebfff21882ull},
}};

}
