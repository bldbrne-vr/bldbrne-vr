// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_SLIM_PACKET's codec pieces (scratchpad/fast-renderer/packet_codec.h, used by
// Rasterizer::DrawPacket::EncodeSlim / DecodeSlim): PacketWriter::ArrayIf writes exactly the
// array Array writes of the kept values; PacketCursor reads every layout PacketReader reads, with
// and without its bounds checks, borrows aligned arrays, and finds where a second form starts;
// HeldValue sends a record again only when its bytes changed, so a consumer that keeps the last
// one sent always holds the producer's current record (and one that forgot to forget is caught).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "scratchpad/fast-renderer/packet_codec.h"

namespace {

using FastRender::HeldValue;
using FastRender::PacketAlignment;
using FastRender::PacketCursor;
using FastRender::PacketReader;
using FastRender::PacketWriter;

// A ConstantBinding stand-in: a pointer, a refusal (no ring buffer) and a probe.
struct Binding {
    const void* stage{};
    std::uint32_t binding{}, input{};
    std::uint64_t buffer{}, offset{};
    bool probe{};
};
bool Keeps(const Binding& b) {
    return b.buffer != 0 || b.probe;
}

struct alignas(16) Wide {
    std::uint64_t first, second;
};

struct Header {
    std::uint64_t sequence;
    std::uint32_t count, flags;
};

// The kind of record the camera is: padding included, compared byte for byte.
struct Record {
    std::uint64_t id{};
    bool driven{};
    float yaw{}, pitch{};
    std::array<float, 4> fov{};
};

alignas(16) std::array<std::byte, 4096> g_bytes;

template <class Exception, class Fn>
bool Throws(Fn&& fn) {
    try {
        fn();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

std::vector<Binding> MakeBindings(std::mt19937& rng, std::size_t n) {
    std::vector<Binding> bindings(n);
    for (auto& b : bindings) {
        b.stage = reinterpret_cast<const void*>(std::uintptr_t(rng() % 3 + 1) * 64);
        b.binding = rng() % 8;
        b.input = rng() % 3;
        b.buffer = rng() % 3 == 0 ? 0 : 0x1000 + rng() % 16;
        b.offset = rng();
        b.probe = b.buffer == 0 && rng() % 4 == 0;
    }
    return bindings;
}

TEST(SlimPacket, ArrayIfWritesTheKeptArray) {
    std::mt19937 rng{7};
    for (int round = 0; round < 200; ++round) {
        const auto bindings = MakeBindings(rng, rng() % 12);
        std::vector<Binding> kept;
        for (const auto& b : bindings)
            if (Keeps(b))
                kept.push_back(b);
        for (const bool aligned : {false, true}) {
            alignas(16) std::array<std::byte, 2048> expected{};
            alignas(16) std::array<std::byte, 2048> actual{};
            // An odd prefix, so the aligned layout pads.
            PacketWriter want{expected.data(), aligned};
            PacketWriter got{actual.data(), aligned};
            PacketWriter size{nullptr, aligned};
            want.Value(std::uint8_t{3});
            got.Value(std::uint8_t{3});
            size.Value(std::uint8_t{3});
            want.Array(kept);
            got.ArrayIf(bindings, Keeps);
            size.ArrayIf(bindings, Keeps);
            ASSERT_EQ(got.Size(), want.Size());
            ASSERT_EQ(size.Size(), want.Size());
            ASSERT_EQ(std::memcmp(actual.data(), expected.data(), want.Size()), 0);
        }
    }
}

TEST(SlimPacket, CursorReadsWhatTheReaderReads) {
    const std::vector<std::uint32_t> words{13, 21, 34};
    const std::array<Wide, 2> wide{{{55, 89}, {144, 233}}};
    const std::vector<std::uint32_t> empty;
    const Header header{0x1234567890ull, 3, 0x81};
    for (const bool aligned : {false, true}) {
        PacketWriter size{nullptr, aligned};
        PacketWriter writer{g_bytes.data(), aligned};
        for (auto* w : {&size, &writer}) {
            w->Value(header);
            w->Array(words);
            w->Array(wide);
            w->Array(empty);
            w->Value(std::uint8_t{9});
            w->Array(words);
        }
        ASSERT_EQ(size.Size(), writer.Size());
        const std::span<const std::byte> packet{g_bytes.data(), writer.Size()};
        for (const bool checked : {false, true}) {
            PacketCursor cursor{packet, checked};
            EXPECT_EQ(cursor.Checked(), checked);
            Header h{};
            cursor.Value(h);
            cursor.SetAligned(aligned);
            EXPECT_EQ(h.sequence, header.sequence);
            EXPECT_EQ(h.count, 3u);
            EXPECT_EQ(h.flags, 0x81u);
            if (aligned) {
                const auto view = cursor.View<std::uint32_t>();
                ASSERT_EQ(view.size(), 3u);
                EXPECT_EQ(view[2], 34u);
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(view.data()) % PacketAlignment, 0u);
                EXPECT_GE(reinterpret_cast<const std::byte*>(view.data()), g_bytes.data());
                const auto w = cursor.View<Wide>();
                ASSERT_EQ(w.size(), 2u);
                EXPECT_EQ(w[1].second, 233u);
                EXPECT_TRUE(cursor.View<std::uint32_t>().empty());
            } else {
                EXPECT_TRUE(Throws<std::logic_error>([&] { cursor.View<std::uint32_t>(); }));
                // The failed borrow took nothing: read the same array again.
                std::vector<std::uint32_t> copy;
                cursor.Copy(copy);
                EXPECT_EQ(copy, words);
                std::array<Wide, 2> result{};
                EXPECT_EQ(cursor.CopyPrefix(std::span{result}), 2u);
                EXPECT_EQ(result[0].first, 55u);
                cursor.Copy(copy);
                EXPECT_TRUE(copy.empty());
            }
            std::uint8_t nine{};
            cursor.Value(nine);
            EXPECT_EQ(nine, 9);
            std::vector<std::uint32_t> last;
            cursor.Copy(last);
            EXPECT_EQ(last, words);
            EXPECT_EQ(cursor.Position(), writer.Size());
            EXPECT_EQ(cursor.Remaining(), 0u);
        }
    }
}

TEST(SlimPacket, CheckedCursorRejectsTruncation) {
    const std::vector<std::uint32_t> words{1, 2, 3, 4, 5};
    for (const bool aligned : {false, true}) {
        PacketWriter writer{g_bytes.data(), aligned};
        writer.Value(std::uint32_t{77});
        writer.Array(words);
        for (std::size_t length = 0; length < writer.Size(); ++length) {
            EXPECT_TRUE(Throws<std::out_of_range>([&] {
                PacketCursor cursor{{g_bytes.data(), length}, true};
                std::uint32_t v;
                cursor.Value(v);
                cursor.SetAligned(aligned);
                std::vector<std::uint32_t> copy;
                cursor.Copy(copy);
            }));
        }
    }
    // A fixed array's capacity is checked without FAST_PIPE_VERIFY too.
    PacketWriter writer{g_bytes.data(), false};
    writer.Array(words);
    PacketCursor cursor{{g_bytes.data(), writer.Size()}, false};
    std::array<std::uint32_t, 2> small{};
    EXPECT_TRUE(Throws<std::out_of_range>([&] { cursor.CopyPrefix(std::span{small}); }));
    // So is a borrowed array's base.
    PacketCursor unaligned{{g_bytes.data() + 1, 32}, false};
    EXPECT_TRUE(Throws<std::invalid_argument>([&] { unaligned.SetAligned(true); }));
}

// FAST_CPU_SLIM_PACKET_VERIFY's packet: the slim form, then the full form at the next aligned
// offset, each read from its own start.
TEST(SlimPacket, SecondFormStartsAligned) {
    const std::vector<std::uint32_t> slim_words{4, 5};
    const std::vector<Wide> full_wide{{1, 2}, {3, 4}, {5, 6}};
    for (const bool aligned : {false, true}) {
        PacketWriter slim{g_bytes.data(), aligned};
        slim.Value(std::uint8_t{1});
        slim.Array(slim_words);
        const std::size_t full_at =
            (slim.Size() + PacketAlignment - 1) / PacketAlignment * PacketAlignment;
        PacketWriter full{g_bytes.data() + full_at, aligned};
        full.Value(std::uint8_t{2});
        full.Array(full_wide);
        PacketCursor cursor{{g_bytes.data(), full_at + full.Size()}, true};
        std::uint8_t tag{};
        cursor.Value(tag);
        cursor.SetAligned(aligned);
        std::vector<std::uint32_t> a;
        cursor.Copy(a);
        EXPECT_EQ(tag, 1);
        EXPECT_EQ(a, slim_words);
        ASSERT_EQ((cursor.Position() + PacketAlignment - 1) / PacketAlignment * PacketAlignment,
                  full_at);
        PacketReader reader{{g_bytes.data() + full_at, full.Size()}};
        reader.Value(tag);
        reader.SetAligned(aligned);
        std::vector<Wide> b;
        reader.Copy(b);
        EXPECT_EQ(tag, 2);
        ASSERT_EQ(b.size(), 3u);
        EXPECT_EQ(b[2].second, 6u);
        EXPECT_EQ(reader.Remaining(), 0u);
    }
}

TEST(SlimPacket, HeldValueSendsOnlyChanges) {
    HeldValue<Record> held;
    Record r{};
    r.id = 1;
    EXPECT_TRUE(held.Differs(r)); // nothing sent yet
    held.Sent(r);
    EXPECT_FALSE(held.Differs(r));
    Record copy;
    std::memcpy(&copy, &r, sizeof(r));
    EXPECT_FALSE(held.Differs(copy));
    copy.fov[3] = 0.5f; // a field written in place
    EXPECT_TRUE(held.Differs(copy));
    held.Forget(); // a new consumer
    EXPECT_TRUE(held.Differs(r));
}

// A producer that sends the record only when HeldValue says it changed, and a consumer that keeps
// the last one received: at every packet the consumer holds the producer's record, byte for byte.
// A consumer replaced without Forget is caught.
TEST(SlimPacket, KeptRecordFollowsTheProducer) {
    std::mt19937 rng{11};
    for (const bool forget : {true, false}) {
        HeldValue<Record> held;
        Record current{};
        current.id = 1;
        Record consumer{};
        std::size_t sends = 0, packets = 0, stale = 0;
        for (int i = 0; i < 20000; ++i) {
            switch (rng() % 64) {
            case 0: // a new camera update
                ++current.id;
                current.yaw = float(rng() % 1000);
                break;
            case 1: // the render-camera hook writes the same record later
                current.fov[rng() % 4] = float(rng() % 100);
                break;
            case 2: // a new decoder (the pipe made again)
                consumer = Record{};
                if (forget)
                    held.Forget();
                break;
            default:
                break;
            }
            const bool send = held.Differs(current);
            if (send) {
                std::memcpy(&consumer, &current, sizeof(current)); // the packet's bytes
                held.Sent(current);
                ++sends;
            }
            ++packets;
            stale += std::memcmp(&consumer, &current, sizeof(current)) != 0;
        }
        if (forget) {
            EXPECT_EQ(stale, 0u);
            EXPECT_LT(sends * 10, packets); // most packets carry no record
        } else {
            EXPECT_GT(stale, 0u);
        }
    }
}

} // namespace
