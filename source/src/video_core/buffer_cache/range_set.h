// SPDX-FileCopyrightText: 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <boost/icl/discrete_interval.hpp>
#include <boost/icl/interval_map.hpp>
#include <boost/icl/split_interval_map.hpp>
#include <boost/icl/split_interval_set.hpp>
#include <boost/pool/pool.hpp>
#include <boost/pool/pool_alloc.hpp>
#include <boost/pool/poolfwd.hpp>
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "common/types.h"
#include "video_core/ds_ab.h"

namespace VideoCore {

template <class T>
using RangeSetsAllocator =
    boost::fast_pool_allocator<T, boost::default_user_allocator_new_delete,
                               boost::details::pool::default_mutex, 1024, 2048>;

struct RangeSet {
    using IntervalSet =
        boost::icl::interval_set<VAddr, std::less,
                                 ICL_INTERVAL_INSTANCE(ICL_INTERVAL_DEFAULT, VAddr, std::less),
                                 RangeSetsAllocator>;
    using IntervalType = typename IntervalSet::interval_type;

    explicit RangeSet() = default;
    ~RangeSet() = default;

    void Add(VAddr base_address, size_t size) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_set.add(interval);
    }

    /// VR_RANGESET_FAST (on by default; '0' plain Add; 'v' every skip checked with icl::contains,
    /// with how often each path decides; 'a' plain Add and this on alternate calls, timed):
    /// Add for a range that is usually in the set already (a buffer range written again, as the
    /// HLE copy's destinations every frame). The member that contained the last such range is
    /// remembered (its bounds, not an iterator) until a Subtract or Clear; otherwise one lookup of
    /// the member holding the first byte. Exact: adding a contained range changes nothing, and Add
    /// only grows or joins members, so a remembered member stays inside the set.
    void AddIfNew(VAddr base_address, size_t size) {
        static const char mode = [] {
            const char* v = std::getenv("VR_RANGESET_FAST");
            return v && (v[0] == '0' || v[0] == 'v' || v[0] == 'a') ? v[0] : '1';
        }();
        const VAddr end_address = base_address + size;
        if (mode == '0' || size == 0 || !DsAb::FixesOn()) {
            Add(base_address, size);
            return;
        }
        if (mode == 'a') {
            // Both leave the same set: plain Add on odd calls, this path on even ones, timed.
            static u64 calls = 0, add_ticks = 0, fast_ticks = 0;
            const bool fast = (++calls & 1) == 0;
            const u64 start = Common::FencedRDTSC();
            if (fast) {
                AddIfNewFast(base_address, end_address);
            } else {
                Add(base_address, size);
            }
            (fast ? fast_ticks : add_ticks) += Common::FencedRDTSC() - start;
            if ((calls & 0x3FFFF) == 0) {
                LOG_INFO(Render_Vulkan,
                         "VR_RANGESET_FAST timing: {} calls, Add {:.0f} cycles a call, AddIfNew "
                         "{:.0f}",
                         calls, double(add_ticks) / double(calls / 2),
                         double(fast_ticks) / double(calls / 2));
            }
            return;
        }
        if (mode == '1') {
            AddIfNewFast(base_address, end_address);
            return;
        }
        const bool by_memo = m_known_generation == m_generation && m_known_lo <= base_address &&
                             end_address <= m_known_hi;
        bool contained = by_memo || Known(base_address, end_address);
        if (mode == 'v') {
            // How often each path decides (memo, one lookup, the full add), logged every 2^16.
            static u64 memo_hits = 0, find_hits = 0, adds = 0;
            if (contained && !Contains(base_address, size)) {
                LOG_ERROR(Render_Vulkan, "VR_RANGESET_FAST verify: {:#x}+{:#x} taken as contained",
                          base_address, size);
                contained = false;
            }
            (contained ? (by_memo ? memo_hits : find_hits) : adds)++;
            if (((memo_hits + find_hits + adds) & 0xFFFF) == 0) {
                LOG_INFO(Render_Vulkan, "VR_RANGESET_FAST verify: memo {} lookup {} added {}",
                         memo_hits, find_hits, adds);
            }
        }
        if (!contained) {
            Add(base_address, size);
        }
    }

    /// AddIfNew's decision and add, no counting: the memo, one lookup, else the full add.
    void AddIfNewFast(VAddr base_address, VAddr end_address) {
        if (!(m_known_generation == m_generation && m_known_lo <= base_address &&
              end_address <= m_known_hi) &&
            !Known(base_address, end_address)) {
            m_ranges_set.add(IntervalType{base_address, end_address});
        }
    }

    /// The member holding base_address contains [base_address, end_address): remembered.
    bool Known(VAddr base_address, VAddr end_address) {
        const auto it = m_ranges_set.find(base_address);
        if (it == m_ranges_set.end() || it->lower() > base_address || end_address > it->upper()) {
            return false;
        }
        m_known_lo = it->lower();
        m_known_hi = it->upper();
        m_known_generation = m_generation;
        return true;
    }

    void Subtract(VAddr base_address, size_t size) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_set.subtract(interval);
        ++m_generation; // AddIfNew's remembered member may be gone
    }

    void Clear() {
        m_ranges_set.clear();
        ++m_generation;
    }

    bool Contains(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::contains(m_ranges_set, interval);
    }

    bool Intersects(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::intersects(m_ranges_set, interval);
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (m_ranges_set.empty()) {
            return;
        }

        for (const auto& set : m_ranges_set) {
            const VAddr inter_addr_end = set.upper();
            const VAddr inter_addr = set.lower();
            func(inter_addr, inter_addr_end);
        }
    }

    template <typename Func>
    void ForEachInRange(VAddr base_addr, size_t size, Func&& func) const {
        if (m_ranges_set.empty()) {
            return;
        }
        const VAddr start_address = base_addr;
        const VAddr end_address = start_address + size;
        const IntervalType search_interval{start_address, end_address};
        auto it = m_ranges_set.lower_bound(search_interval);
        if (it == m_ranges_set.end()) {
            return;
        }
        auto end_it = m_ranges_set.upper_bound(search_interval);
        for (; it != end_it; it++) {
            VAddr inter_addr_end = it->upper();
            VAddr inter_addr = it->lower();
            if (inter_addr_end > end_address) {
                inter_addr_end = end_address;
            }
            if (inter_addr < start_address) {
                inter_addr = start_address;
            }
            func(inter_addr, inter_addr_end);
        }
    }

    template <typename Func>
    void ForEachNotInRange(VAddr base_addr, size_t size, Func&& func) const {
        const VAddr end_addr = base_addr + size;
        ForEachInRange(base_addr, size, [&](VAddr range_addr, VAddr range_end) {
            if (size_t gap_size = range_addr - base_addr; gap_size != 0) {
                func(base_addr, gap_size);
            }
            base_addr = range_end;
        });
        if (base_addr != end_addr) {
            func(base_addr, end_addr - base_addr);
        }
    }

    IntervalSet m_ranges_set;
    // AddIfNew: a member known to be in the set while m_known_generation == m_generation.
    VAddr m_known_lo{}, m_known_hi{};
    u64 m_generation{1}, m_known_generation{};
};

template <typename T>
class RangeMap {
public:
    using IntervalMap =
        boost::icl::interval_map<VAddr, T, boost::icl::total_absorber, std::less,
                                 boost::icl::inplace_identity, boost::icl::inter_section,
                                 ICL_INTERVAL_INSTANCE(ICL_INTERVAL_DEFAULT, VAddr, std::less),
                                 RangeSetsAllocator>;
    using IntervalType = typename IntervalMap::interval_type;

public:
    RangeMap() = default;
    ~RangeMap() = default;

    RangeMap(RangeMap const&) = delete;
    RangeMap& operator=(RangeMap const&) = delete;

    RangeMap(RangeMap&& other);
    RangeMap& operator=(RangeMap&& other);

    void Add(VAddr base_address, size_t size, const T& value) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_map.add({interval, value});
    }

    void Subtract(VAddr base_address, size_t size) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_map -= interval;
    }

    void Clear() {
        m_ranges_map.clear();
    }

    bool Contains(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::contains(m_ranges_map, interval);
    }

    bool Intersects(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::intersects(m_ranges_map, interval);
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (m_ranges_map.empty()) {
            return;
        }

        for (const auto& [interval, value] : m_ranges_map) {
            const VAddr inter_addr_end = interval.upper();
            const VAddr inter_addr = interval.lower();
            func(inter_addr, inter_addr_end, value);
        }
    }

    template <typename Func>
    void ForEachInRange(VAddr base_addr, size_t size, Func&& func) const {
        if (m_ranges_map.empty()) {
            return;
        }
        const VAddr start_address = base_addr;
        const VAddr end_address = start_address + size;
        const IntervalType search_interval{start_address, end_address};
        auto it = m_ranges_map.lower_bound(search_interval);
        if (it == m_ranges_map.end()) {
            return;
        }
        auto end_it = m_ranges_map.upper_bound(search_interval);
        for (; it != end_it; it++) {
            VAddr inter_addr_end = it->first.upper();
            VAddr inter_addr = it->first.lower();
            if (inter_addr_end > end_address) {
                inter_addr_end = end_address;
            }
            if (inter_addr < start_address) {
                inter_addr = start_address;
            }
            func(inter_addr, inter_addr_end, it->second);
        }
    }

    template <typename Func>
    void ForEachNotInRange(VAddr base_addr, size_t size, Func&& func) const {
        const VAddr end_addr = base_addr + size;
        ForEachInRange(base_addr, size, [&](VAddr range_addr, VAddr range_end, const T&) {
            if (size_t gap_size = range_addr - base_addr; gap_size != 0) {
                func(base_addr, gap_size);
            }
            base_addr = range_end;
        });
        if (base_addr != end_addr) {
            func(base_addr, end_addr - base_addr);
        }
    }

private:
    IntervalMap m_ranges_map;
};

template <typename T>
class SplitRangeMap {
public:
    using IntervalMap = boost::icl::split_interval_map<
        VAddr, T, boost::icl::total_absorber, std::less, boost::icl::inplace_identity,
        boost::icl::inter_section, ICL_INTERVAL_INSTANCE(ICL_INTERVAL_DEFAULT, VAddr, std::less),
        RangeSetsAllocator>;
    using IntervalType = typename IntervalMap::interval_type;

public:
    SplitRangeMap() = default;
    ~SplitRangeMap() = default;

    SplitRangeMap(SplitRangeMap const&) = delete;
    SplitRangeMap& operator=(SplitRangeMap const&) = delete;

    SplitRangeMap(SplitRangeMap&& other);
    SplitRangeMap& operator=(SplitRangeMap&& other);

    void Add(VAddr base_address, size_t size, const T& value) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_map.add({interval, value});
    }

    void Subtract(VAddr base_address, size_t size) {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        m_ranges_map -= interval;
    }

    void Clear() {
        m_ranges_map.clear();
    }

    bool Contains(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::contains(m_ranges_map, interval);
    }

    bool Intersects(VAddr base_address, size_t size) const {
        const VAddr end_address = base_address + size;
        IntervalType interval{base_address, end_address};
        return boost::icl::intersects(m_ranges_map, interval);
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        if (m_ranges_map.empty()) {
            return;
        }

        for (const auto& [interval, value] : m_ranges_map) {
            const VAddr inter_addr_end = interval.upper();
            const VAddr inter_addr = interval.lower();
            func(inter_addr, inter_addr_end, value);
        }
    }

    template <typename Func>
    void ForEachInRange(VAddr base_addr, size_t size, Func&& func) const {
        if (m_ranges_map.empty()) {
            return;
        }
        const VAddr start_address = base_addr;
        const VAddr end_address = start_address + size;
        const IntervalType search_interval{start_address, end_address};
        auto it = m_ranges_map.lower_bound(search_interval);
        if (it == m_ranges_map.end()) {
            return;
        }
        auto end_it = m_ranges_map.upper_bound(search_interval);
        for (; it != end_it; it++) {
            VAddr inter_addr_end = it->first.upper();
            VAddr inter_addr = it->first.lower();
            if (inter_addr_end > end_address) {
                inter_addr_end = end_address;
            }
            if (inter_addr < start_address) {
                inter_addr = start_address;
            }
            func(inter_addr, inter_addr_end, it->second);
        }
    }

    template <typename Func>
    void ForEachNotInRange(VAddr base_addr, size_t size, Func&& func) const {
        const VAddr end_addr = base_addr + size;
        ForEachInRange(base_addr, size, [&](VAddr range_addr, VAddr range_end, const T&) {
            if (size_t gap_size = range_addr - base_addr; gap_size != 0) {
                func(base_addr, gap_size);
            }
            base_addr = range_end;
        });
        if (base_addr != end_addr) {
            func(base_addr, end_addr - base_addr);
        }
    }

private:
    IntervalMap m_ranges_map;
};

} // namespace VideoCore
