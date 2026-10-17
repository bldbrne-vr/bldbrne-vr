// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <new>
#include <gtest/gtest.h>
#include <malloc.h>
#include "quick_allocation_probe.h"
namespace QuickAllocationProbe {
thread_local bool tracking{};
thread_local std::size_t allocations{};
std::size_t updates{}, cancellations{};
Scope::Scope(const char* op) : operation(op), before(allocations), previous(tracking) {
    tracking = true;
}
Scope::~Scope() {
    tracking = previous;
    if (allocations != before) {
        std::fprintf(stderr, "Controller %s allocated %zu times\n", operation,
                     allocations - before);
        std::abort();
    }
}
} // namespace QuickAllocationProbe
static void Count() {
    if (QuickAllocationProbe::tracking)
        ++QuickAllocationProbe::allocations;
}
void* operator new(std::size_t size) {
    Count();
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void* operator new(std::size_t size, std::align_val_t align) {
    Count();
    if (void* p = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(align)))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return ::operator new(size, align);
}
void operator delete(void* p, std::align_val_t) noexcept {
    _aligned_free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    _aligned_free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    _aligned_free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    _aligned_free(p);
}
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    QuickAllocationProbe::tracking = true;
    void* probe = ::operator new(17);
    QuickAllocationProbe::tracking = false;
    ::operator delete(probe);
    if (QuickAllocationProbe::allocations != 1)
        return 2;
    QuickAllocationProbe::allocations = 0;
    const int result = RUN_ALL_TESTS();
    std::printf("Allocation probe: %zu updates, %zu cancellations, %zu C++ heap allocations inside "
                "controller calls\n",
                QuickAllocationProbe::updates, QuickAllocationProbe::cancellations,
                QuickAllocationProbe::allocations);
    return result;
}
