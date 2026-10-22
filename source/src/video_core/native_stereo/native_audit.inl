// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Isolated native-stereo integration capture: one full guest frame, never a timing run.
struct NativeAudit {
    std::filesystem::path dir;
    bool active{};
    u64 frame{};
    u64 projection_bytes{};
    nlohmann::json calls = nlohmann::json::array();
    nlohmann::json constants = nlohmann::json::object();
};

// Read once at start-up: VrNativeAudit runs per draw, and a function-local static's guard is a
// thread-local read per call.
static const char* const g_native_audit_dir = std::getenv("VR_NATIVE_AUDIT_DIR");

void Rasterizer::VrNativeAudit(const Pipeline* pipeline, const RenderState* state) {
    const char* const dir = g_native_audit_dir;
    if (!dir) return;
    if (!native_audit) {
        native_audit = std::make_unique<NativeAudit>();
        native_audit->dir = dir;
    }
    auto& audit = *native_audit;
    if (!audit.active) return;
    // Explicit diagnostic capture only. Never enable this for timing runs.
    static const bool projection_audit = std::getenv("VR_NATIVE_PROJECTION_AUDIT") != nullptr;
    constexpr u64 projection_limit = 16 * 1024 * 1024;
    nlohmann::json op;
    op["kind"] = pipeline->IsCompute() ? "compute" : "graphics";
    if (state) {
        op["extent"] = {state->width, state->height};
        op["view_mask"] = state->probe_view_mask;
        op["targets"] = nlohmann::json::array();
        for (u32 i=0; i<state->num_color_attachments; ++i) {
            if (!cb_descs[i].first) continue;
            const auto& image = texture_cache.GetImage(cb_descs[i].first);
            op["targets"].push_back({{"address",image.info.guest_address}, {"uid",image.image_uid},
                {"format",vk::to_string(image.info.pixel_format)}, {"layers",image.info.resources.layers},
                {"mips",image.info.resources.levels}, {"samples",image.info.num_samples}});
        }
        if (db_desc.first) {
            const auto& image = texture_cache.GetImage(db_desc.first);
            op["depth"] = {{"address",image.info.guest_address},{"format",vk::to_string(image.info.pixel_format)},
                {"width",image.info.size.width},{"height",image.info.size.height},
                {"native_paired",image.native_stereo},{"shared_shadow",NativeStereo::SharedShadowAtlas(image.info)}};
        }
    } else {
        const auto& cs = std::as_const(ScratchDraw::ComputeRegisters(liverpool));
        op["dispatch"] = {cs.dim_x,cs.dim_y,cs.dim_z};
    }
    if (state) {
        const auto& regs=std::as_const(ScratchDraw::Registers(liverpool));
        const auto& vp=regs.viewports[0];
        op["viewport"]={vp.xoffset,vp.yoffset,vp.xscale,vp.yscale};
        op["clip_disabled"]=regs.IsClipDisabled();
        op["screen_scissor"]={int(regs.screen_scissor.top_left_x),int(regs.screen_scissor.top_left_y),
                              int(regs.screen_scissor.bottom_right_x),int(regs.screen_scissor.bottom_right_y)};
        op["generic_scissor"]={int(regs.generic_scissor.top_left_x),int(regs.generic_scissor.top_left_y),
                               int(regs.generic_scissor.bottom_right_x),int(regs.generic_scissor.bottom_right_y)};
        op["window_scissor"]={int(regs.window_scissor.top_left_x),int(regs.window_scissor.top_left_y),
                              int(regs.window_scissor.bottom_right_x),int(regs.window_scissor.bottom_right_y)};
        op["window_offset"]={int(regs.window_offset.window_x_offset),int(regs.window_offset.window_y_offset)};
        op["window_offset_disabled"]=bool(regs.window_scissor.window_offset_disable);
        op["viewport_scissor_enabled"]=bool(regs.mode_control.vport_scissor_enable);
        op["viewport_scissor"]={int(regs.viewport_scissors[0].top_left_x),int(regs.viewport_scissors[0].top_left_y),
                                int(regs.viewport_scissors[0].bottom_right_x),int(regs.viewport_scissors[0].bottom_right_y)};

    }
    op["stages"] = nlohmann::json::array();
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) continue;
        nlohmann::json st={{"hash",stage->pgm_hash},{"stage",u32(stage->l_stage)},{"dma",stage->uses_dma}};
        st["buffers"]=nlohmann::json::array();
        st["images"]=nlohmann::json::array();
        for (const auto& desc : stage->buffers) {
            if (desc.IsSpecial()) continue;
            const auto sharp=desc.GetSharp(*stage);
            const u64 address=sharp.base_address;
            const u64 size=sharp.GetSize();
            nlohmann::json buffer={{"address",address},{"size",size},{"write",desc.is_written}};
            // Keep reconstruction constants with their stage/binding, including compute.
            // A size is a capture candidate, never sufficient evidence to remap it.
            const bool known_constants = size==208 || size==272 || size==336 || size==416;
            const bool extra_constants = projection_audit && size>=16 && size<=4096 && size%4==0;
            const bool readable = !desc.is_written && (known_constants || extra_constants) &&
                memory->IsValidGpuMapping(address,size) &&
                !buffer_cache.IsRegionGpuModified(address,size);
            const bool budget_ok = !extra_constants || audit.projection_bytes + size <= projection_limit;
            if (projection_audit && !desc.is_written && size>=16 && size<=4096) {
                buffer["constants_status"] = !readable ? "unavailable-or-gpu-modified" :
                    !budget_ok ? "capture-budget" : "captured";
            }
            if (readable && budget_ok) {
                std::vector<float> values(size / sizeof(float));
                std::memcpy(values.data(),reinterpret_cast<const void*>(address),size);
                buffer["constants"]=values;
                if (extra_constants) audit.projection_bytes += size;
            }
            st["buffers"].push_back(std::move(buffer));
            if (size==864 && !desc.is_written && memory->IsValidGpuMapping(address,size)) {
                const auto key=fmt::format("{:x}",address);
                if (!audit.constants.contains(key)) {
                    std::array<float,216> values{};
                    std::memcpy(values.data(),reinterpret_cast<const void*>(address),sizeof(values));
                    audit.constants[key]=values;
                }
            }
        }
        for (const auto& desc : stage->images) {
            const auto sharp=desc.GetSharp(*stage);
            st["images"].push_back({{"address",sharp.Address()},{"width",sharp.width+1},
                {"height",sharp.height+1},{"write",desc.is_written},{"array",desc.is_array},
                {"depth",desc.is_depth},{"atomic",desc.is_atomic}});
        }
        op["stages"].push_back(std::move(st));
    }
    audit.calls.push_back(std::move(op));
}

void Rasterizer::VrNativeAuditEnd() {
    if (!native_audit) return;
    auto& audit=*native_audit;
    if (audit.active) {
        const auto r=VR::DrawFrameRecord();
        const nlohmann::json camera={{"id",r.id},{"vfov",r.vfov},{"aspect",r.aspect},
            {"world_scale",r.world_scale},{"base_yaw",r.base_yaw},{"head_yaw",r.head_yaw},
            {"head_pitch",r.head_pitch},{"render_position",{r.render_position.x,r.render_position.y,r.render_position.z}}};
        const nlohmann::json output={{"frame",audit.frame},{"camera",camera},
            {"calls",audit.calls},{"constants864",audit.constants}};
        std::ofstream(audit.dir/"native_frame.json") << output.dump(2);
        LOG_INFO(Render_Vulkan,"VR native audit: saved frame {} with {} calls and {} scene buffers",audit.frame,audit.calls.size(),audit.constants.size());
        audit.active=false;
        audit.calls=nlohmann::json::array();
        audit.constants=nlohmann::json::object();
        audit.projection_bytes=0;
    }
    ++audit.frame;
    if (std::filesystem::exists(audit.dir/"native_capture")) {
        std::filesystem::remove(audit.dir/"native_capture");
        audit.active=true;
    }
}
