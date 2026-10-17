// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);
    // `name`: the blob's name (its file name without the extension), as Save/Load/Remove take it.
    void ForEachBlob(BlobType type,
                     const std::function<void(const std::string& name, std::vector<u8>&& data)>& func);

    // The type and name of every blob, without reading it (a file whose extension is no blob
    // type's, or whose name has a dot of its own, is skipped).
    void ForEachName(const std::function<void(BlobType type, const std::string& name)>& func);

    // Deletes one blob (not in the archived mode). True when a file was removed.
    bool Remove(BlobType type, const std::string& name);
    // Deletes every blob, the profile included (not in the archived mode): a cache written for
    // another GPU / driver profile starts over instead of being ignored for good.
    bool Clear();

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    bool opened{};
};

} // namespace Storage
