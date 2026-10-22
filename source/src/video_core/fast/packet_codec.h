// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Owned queue payloads: aligned arrays can be borrowed until the handler returns.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace FastRender {
inline constexpr std::size_t PacketAlignment = 16;
class PacketWriter {
    std::byte* output;
    std::size_t position{};
    bool aligned;
public:
    PacketWriter(std::byte* output_, bool aligned_) : output(output_), aligned(aligned_) {}
    template<class T> void Value(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (output) std::memcpy(output + position, &value, sizeof(T));
        position += sizeof(T);
    }
    template<class Values> void Array(const Values& values) {
        using T = std::remove_cvref_t<decltype(values[0])>;
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= PacketAlignment);
        if (values.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("packet array count");
        Value(static_cast<std::uint32_t>(values.size()));
        if (aligned) {
            const auto padding = (PacketAlignment - position % PacketAlignment) % PacketAlignment;
            if (output && padding) std::memset(output + position, 0, padding);
            position += padding;
        }
        const auto bytes = values.size() * sizeof(T);
        if (output && bytes) std::memcpy(output + position, values.data(), bytes);
        position += bytes;
    }
    // FAST_CPU_SLIM_PACKET: Array's layout for the values `keep` accepts, in their order (read
    // back as one array).
    template<class Values, class Keep> void ArrayIf(const Values& values, Keep&& keep) {
        using T = std::remove_cvref_t<decltype(values[0])>;
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= PacketAlignment);
        std::uint32_t kept = 0;
        for (const auto& item : values) kept += keep(item) ? 1u : 0u;
        Value(kept);
        if (aligned) {
            const auto padding = (PacketAlignment - position % PacketAlignment) % PacketAlignment;
            if (output && padding) std::memset(output + position, 0, padding);
            position += padding;
        }
        for (const auto& item : values) {
            if (!keep(item)) continue;
            if (output) std::memcpy(output + position, &item, sizeof(T));
            position += sizeof(T);
        }
    }
    std::size_t Size() const { return position; }
};
// FAST_CPU_SLIM_PACKET: PacketReader's layout without its per-field bounds checks (the producer
// sized the packet with the same writer); `checked` (FAST_PIPE_VERIFY) keeps them. Checked
// always: the aligned base, a borrowed array in packed mode and a fixed array's capacity.
class PacketCursor {
    const std::byte* base;
    std::size_t size;
    std::size_t position{};
    bool aligned{};
    bool checked;
    void Need(std::size_t bytes) const {
        if (checked && bytes > size - position) throw std::out_of_range("truncated draw packet");
    }
    template<class T> std::span<const std::byte> ArrayBytes() {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= PacketAlignment);
        std::uint32_t count;
        Value(count);
        if (aligned) {
            const auto padding = (PacketAlignment - position % PacketAlignment) % PacketAlignment;
            Need(padding);
            position += padding;
        }
        if (checked && count > (size - position) / sizeof(T))
            throw std::out_of_range("packet array bounds");
        const std::span<const std::byte> result{base + position, std::size_t(count) * sizeof(T)};
        position += result.size();
        return result;
    }
public:
    PacketCursor(std::span<const std::byte> payload, bool checked_)
        : base(payload.data()), size(payload.size()), checked(checked_) {}
    void SetAligned(bool value) {
        if (value && reinterpret_cast<std::uintptr_t>(base) % PacketAlignment)
            throw std::invalid_argument("unaligned draw packet base");
        aligned = value;
    }
    bool Checked() const { return checked; }
    template<class T> void Value(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        Need(sizeof(T));
        std::memcpy(&value, base + position, sizeof(T));
        position += sizeof(T);
    }
    template<class T> std::span<const T> View() {
        if (!aligned) throw std::logic_error("packed arrays cannot be borrowed");
        const auto bytes = ArrayBytes<T>();
        return {reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T)};
    }
    template<class Values> void Copy(Values& values) {
        using T = std::remove_cvref_t<decltype(values[0])>;
        const auto bytes = ArrayBytes<T>();
        values.resize(bytes.size() / sizeof(T));
        if (!bytes.empty()) std::memcpy(values.data(), bytes.data(), bytes.size());
    }
    template<class T, std::size_t Extent> std::size_t CopyPrefix(std::span<T, Extent> values) {
        const auto bytes = ArrayBytes<T>();
        if (bytes.size() / sizeof(T) > values.size())
            throw std::out_of_range("fixed packet array bounds");
        if (!bytes.empty()) std::memcpy(values.data(), bytes.data(), bytes.size());
        return bytes.size() / sizeof(T);
    }
    // Where the next field starts (a packet that carries a second form after this one).
    std::size_t Position() const { return position; }
    std::size_t Remaining() const { return size - position; }
};
// FAST_CPU_SLIM_PACKET: a record the consumer keeps between packets, sent again only when its
// bytes differ from the last one sent (the producer's byte copy of it). Forget: a new consumer.
template<class T> class HeldValue {
    static_assert(std::is_trivially_copyable_v<T>);
    T held{};
    bool known{};
public:
    bool Differs(const T& value) const {
        return !known || std::memcmp(&held, &value, sizeof(T)) != 0;
    }
    void Sent(const T& value) {
        std::memcpy(&held, &value, sizeof(T));
        known = true;
    }
    void Forget() { known = false; }
};
class PacketReader {
    std::span<const std::byte> payload;
    std::size_t position{};
    bool aligned{};
    std::span<const std::byte> Take(std::size_t bytes) {
        if (bytes > Remaining()) throw std::out_of_range("truncated draw packet");
        auto result = payload.subspan(position, bytes);
        position += bytes;
        return result;
    }
    template<class T> std::span<const std::byte> ArrayBytes() {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= PacketAlignment);
        std::uint32_t count;
        Value(count);
        if (aligned) Take((PacketAlignment - position % PacketAlignment) % PacketAlignment);
        if (count > Remaining() / sizeof(T)) throw std::out_of_range("packet array bounds");
        return Take(std::size_t(count) * sizeof(T));
    }
public:
    explicit PacketReader(std::span<const std::byte> payload_) : payload(payload_) {}
    void SetAligned(bool value) {
        if (value && reinterpret_cast<std::uintptr_t>(payload.data()) % PacketAlignment)
            throw std::invalid_argument("unaligned draw packet base");
        aligned = value;
    }
    template<class T> void Value(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto data = Take(sizeof(T));
        std::memcpy(&value, data.data(), sizeof(T));
    }
    template<class T> std::span<const T> View() {
        if (!aligned) throw std::logic_error("packed arrays cannot be borrowed");
        const auto bytes = ArrayBytes<T>();
        return {reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T)};
    }
    template<class Values> void Copy(Values& values) {
        using T = std::remove_cvref_t<decltype(values[0])>;
        const auto bytes = ArrayBytes<T>();
        values.resize(bytes.size() / sizeof(T));
        if (!bytes.empty()) std::memcpy(values.data(), bytes.data(), bytes.size());
    }
    template<class T, std::size_t Extent> std::size_t CopyPrefix(std::span<T, Extent> values) {
        const auto bytes = ArrayBytes<T>();
        if (bytes.size() / sizeof(T) > values.size())
            throw std::out_of_range("fixed packet array bounds");
        if (!bytes.empty()) std::memcpy(values.data(), bytes.data(), bytes.size());
        return bytes.size() / sizeof(T);
    }
    std::size_t Remaining() const { return payload.size() - position; }
};
} // namespace FastRender
