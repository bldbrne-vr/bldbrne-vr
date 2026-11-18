// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <cstring>
#include <xxhash.h>

#include "video_core/cache_pack.h"

namespace VideoCore::CachePack {
namespace {

// Header: magic, version, entries, absorbed files, the blobs' bytes, the index's place, size and
// hash, then the hash of everything before it.
struct Header {
    u32 magic;
    u32 version;
    u32 entries;
    u32 absorbed;
    u64 data_bytes;
    u64 index_offset;
    u64 index_bytes;
    u64 index_hash;
    u64 reserved;
    u64 header_hash;
};
static_assert(sizeof(Header) == HeaderSize);

// The largest index record: an entry's type, name length, name, place, size and hash.
constexpr u64 MaxIndexRecord = 1 + 2 + MaxName + 3 * 8;

u64 HeaderHash(const Header& h) {
    return XXH3_64bits(&h, offsetof(Header, header_hash));
}

struct Out {
    std::vector<u8> bytes;
    template <typename T>
    void Put(T v) {
        const auto* p = reinterpret_cast<const u8*>(&v);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    void PutName(const std::string& s) {
        Put(u16(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
};

// Every read is bounds-checked; a failed one leaves `ok` false for good.
struct In {
    std::span<const u8> bytes;
    size_t at{};
    bool ok{true};
    template <typename T>
    T Get() {
        T v{};
        if (!ok || bytes.size() - at < sizeof(T)) {
            ok = false;
            return v;
        }
        std::memcpy(&v, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    std::string GetName() {
        const u16 n = Get<u16>();
        if (!ok || n == 0 || n > MaxName || bytes.size() - at < n) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(bytes.data() + at), n);
        at += n;
        return s;
    }
};

} // namespace

bool Reader::Open(const std::filesystem::path& path) {
    std::scoped_lock lock{mutex};
    entries.clear();
    by_name.clear();
    absorbed.clear();
    data_bytes = 0;
    file = std::ifstream(path, std::ios::binary);
    std::error_code error;
    const u64 file_size = std::filesystem::file_size(path, error);
    Header h{};
    if (!file || error || file_size < HeaderSize ||
        !file.read(reinterpret_cast<char*>(&h), sizeof(h)) || h.magic != Magic ||
        h.version != Version || h.header_hash != HeaderHash(h) || h.entries > MaxEntries ||
        h.absorbed > MaxEntries || h.data_bytes > file_size - HeaderSize ||
        h.index_offset != HeaderSize + h.data_bytes || h.index_bytes != file_size - h.index_offset ||
        h.index_bytes > u64(h.entries + h.absorbed) * MaxIndexRecord) {
        file = {};
        return false;
    }
    std::vector<u8> index(h.index_bytes);
    if (!file.seekg(std::streamoff(h.index_offset)) ||
        !file.read(reinterpret_cast<char*>(index.data()), std::streamsize(index.size())) ||
        XXH3_64bits(index.data(), index.size()) != h.index_hash) {
        file = {};
        return false;
    }
    In in{index};
    entries.reserve(h.entries);
    for (u32 i = 0; i < h.entries && in.ok; ++i) {
        Entry e;
        e.type = in.Get<u8>();
        e.name = in.GetName();
        e.offset = in.Get<u64>();
        e.size = in.Get<u64>();
        e.hash = in.Get<u64>();
        if (!in.ok || e.size > MaxBlob || e.offset > h.data_bytes ||
            e.size > h.data_bytes - e.offset || !by_name.emplace(e.name, entries.size()).second) {
            in.ok = false;
            break;
        }
        entries.push_back(std::move(e));
    }
    for (u32 i = 0; i < h.absorbed && in.ok; ++i) {
        Absorbed a;
        a.name = in.GetName();
        a.size = in.Get<u64>();
        a.time = in.Get<s64>();
        absorbed.push_back(std::move(a));
    }
    if (!in.ok || in.at != index.size()) {
        entries.clear();
        by_name.clear();
        absorbed.clear();
        file = {};
        return false;
    }
    data_bytes = h.data_bytes;
    return true;
}

const Entry* Reader::Find(const std::string& name) const {
    const auto it = by_name.find(name);
    return it == by_name.end() ? nullptr : &entries[it->second];
}

bool Reader::Read(const Entry& entry, std::vector<u8>& out) const {
    out.resize(entry.size);
    {
        std::scoped_lock lock{mutex};
        file.clear();
        if (!file.is_open() || !file.seekg(std::streamoff(HeaderSize + entry.offset)) ||
            !file.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()))) {
            out.clear();
            return false;
        }
    }
    if (XXH3_64bits(out.data(), out.size()) != entry.hash) {
        out.clear();
        return false;
    }
    return true;
}

bool Writer::Begin(const std::filesystem::path& path) {
    entries.clear();
    by_name.clear();
    absorbed.clear();
    data_bytes = 0;
    file = std::ofstream(path, std::ios::binary | std::ios::trunc);
    const Header placeholder{};
    ok = file && file.write(reinterpret_cast<const char*>(&placeholder), sizeof(placeholder));
    return ok;
}

bool Writer::Add(u8 type, const std::string& name, std::span<const u8> data) {
    if (!ok || name.empty() || name.size() > MaxName || data.size() > MaxBlob ||
        entries.size() >= MaxEntries) {
        return false;
    }
    if (by_name.contains(name)) {
        return true;
    }
    if (!file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
        ok = false;
        return false;
    }
    by_name.emplace(name, entries.size());
    entries.push_back({type, name, data_bytes, data.size(), XXH3_64bits(data.data(), data.size())});
    data_bytes += data.size();
    return true;
}

void Writer::Absorb(Absorbed file_) {
    if (!file_.name.empty() && file_.name.size() <= MaxName && absorbed.size() < MaxEntries) {
        absorbed.push_back(std::move(file_));
    }
}

bool Writer::Finish() {
    if (!ok) {
        file = {};
        return false;
    }
    Out index;
    for (const auto& e : entries) {
        index.Put(e.type);
        index.PutName(e.name);
        index.Put(e.offset);
        index.Put(e.size);
        index.Put(e.hash);
    }
    for (const auto& a : absorbed) {
        index.PutName(a.name);
        index.Put(a.size);
        index.Put(a.time);
    }
    Header h{};
    h.magic = Magic;
    h.version = Version;
    h.entries = u32(entries.size());
    h.absorbed = u32(absorbed.size());
    h.data_bytes = data_bytes;
    h.index_offset = HeaderSize + data_bytes;
    h.index_bytes = index.bytes.size();
    h.index_hash = XXH3_64bits(index.bytes.data(), index.bytes.size());
    h.header_hash = HeaderHash(h);
    ok = file.write(reinterpret_cast<const char*>(index.bytes.data()),
                    std::streamsize(index.bytes.size())) &&
         file.seekp(0) && file.write(reinterpret_cast<const char*>(&h), sizeof(h)) &&
         file.flush();
    file.close();
    return ok && !file.fail();
}

} // namespace VideoCore::CachePack
