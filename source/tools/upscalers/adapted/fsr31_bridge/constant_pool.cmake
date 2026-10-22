# Generated-source patch: pinned upstream sources remain untouched.
set(pool_old [==[    uint64_t frameId = 0u;
};

struct BridgeDescriptorSet]==])
set(pool_new [==[    uint64_t frameId = 0u;
    uint32_t memoryTypeIndex = 0;
};

struct BridgeDescriptorSet]==])
string(FIND "${bridge_text}" "${pool_old}" pool_found)
if(pool_found EQUAL -1)
 message(FATAL_ERROR "Constant pool patch 0 no longer matches upstream")
endif()
string(REPLACE "${pool_old}" "${pool_new}" bridge_text "${bridge_text}")
set(pool_old [==[    std::vector<std::unique_ptr<BridgeConstantBuffer>> constantBuffers;
    /*]==])
set(pool_new [==[    std::vector<std::unique_ptr<BridgeConstantBuffer>> constantBuffers;
    std::vector<std::unique_ptr<BridgeConstantBuffer>> freeConstantBuffers;
    /*]==])
string(FIND "${bridge_text}" "${pool_old}" pool_found)
if(pool_found EQUAL -1)
 message(FATAL_ERROR "Constant pool patch 1 no longer matches upstream")
endif()
string(REPLACE "${pool_old}" "${pool_new}" bridge_text "${bridge_text}")
set(pool_old [==[    for (const std::unique_ptr<BridgeConstantBuffer>& constantBuffer : bridge->constantBuffers)
        destroy_constant_buffer(bridge, constantBuffer.get());]==])
set(pool_new [==[    for (const auto& constantBuffer : bridge->freeConstantBuffers)
        destroy_constant_buffer(bridge, constantBuffer.get());
    for (const std::unique_ptr<BridgeConstantBuffer>& constantBuffer : bridge->constantBuffers)
        destroy_constant_buffer(bridge, constantBuffer.get());]==])
string(FIND "${bridge_text}" "${pool_old}" pool_found)
if(pool_found EQUAL -1)
 message(FATAL_ERROR "Constant pool patch 2 no longer matches upstream")
endif()
string(REPLACE "${pool_old}" "${pool_new}" bridge_text "${bridge_text}")
set(pool_old [==[    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;]==])
set(pool_new [==[    // Only completed submissions enter this bounded cache. Exact sizes preserve
    // descriptor ranges, and the existing frame ownership remains unchanged.
    {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        auto it = std::find_if(bridge->freeConstantBuffers.begin(),
            bridge->freeConstantBuffers.end(), [size](const auto& item) { return item->size == size; });
        if (it != bridge->freeConstantBuffers.end()) {
            constantBuffer = std::move(*it);
            bridge->freeConstantBuffers.erase(it);
        }
    }
    allocationInfo.memoryTypeIndex = constantBuffer->memoryTypeIndex;
    if (constantBuffer->buffer == VK_NULL_HANDLE) {
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;]==])
string(FIND "${bridge_text}" "${pool_old}" pool_found)
if(pool_found EQUAL -1)
 message(FATAL_ERROR "Constant pool patch 3 no longer matches upstream")
endif()
string(REPLACE "${pool_old}" "${pool_new}" bridge_text "${bridge_text}")
set(pool_old [==[    void* mapped = nullptr;
    if (vkMapMemory(bridge->device, constantBuffer->memory, 0u, size, 0u, &mapped)]==])
set(pool_new [==[    constantBuffer->memoryTypeIndex = allocationInfo.memoryTypeIndex;
    }
    void* mapped = nullptr;
    if (vkMapMemory(bridge->device, constantBuffer->memory, 0u, VK_WHOLE_SIZE, 0u, &mapped)]==])
string(FIND "${bridge_text}" "${pool_old}" pool_found)
if(pool_found EQUAL -1)
 message(FATAL_ERROR "Constant pool patch 4 no longer matches upstream")
endif()
string(REPLACE "${pool_old}" "${pool_new}" bridge_text "${bridge_text}")
set(pool_old [==[            range.size = size;]==])
set(pool_new [==[            range.size = VK_WHOLE_SIZE;]==])
string(FIND "${bridge_text}" "${pool_old}" pool_found)
if(pool_found EQUAL -1)
 message(FATAL_ERROR "Constant pool patch 5 no longer matches upstream")
endif()
string(REPLACE "${pool_old}" "${pool_new}" bridge_text "${bridge_text}")
set(pool_old [==[    for (const std::unique_ptr<BridgeConstantBuffer>& item : constantBuffers)
        destroy_constant_buffer(bridge, item.get());]==])
set(pool_new [==[    for (auto& item : constantBuffers) {
        std::lock_guard<std::mutex> lock(bridge->mutex);
        if (item && bridge->freeConstantBuffers.size() < 64) {
            item->frameId = 0;
            bridge->freeConstantBuffers.emplace_back(std::move(item));
        } else {
            destroy_constant_buffer(bridge, item.get());
        }
    }]==])
string(FIND "${bridge_text}" "${pool_old}" pool_found)
if(pool_found EQUAL -1)
 message(FATAL_ERROR "Constant pool patch 6 no longer matches upstream")
endif()
string(REPLACE "${pool_old}" "${pool_new}" bridge_text "${bridge_text}")
