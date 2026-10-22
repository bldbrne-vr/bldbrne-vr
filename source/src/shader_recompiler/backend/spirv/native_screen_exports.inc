// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Recognize affine clip copies and screen UVs from IR, never by shader hash.
// Unknown/nonlinear expressions and conflicting writes are deliberately left alone.
struct ScreenExpression {
    // x, y, w, x/w, y/w, constant
    std::array<float, 6> coefficients{};
    bool valid{}, reciprocal_w{};
};
IR::Value ScreenRoot(IR::Value v) {
    v = v.Resolve();
    while (!v.IsEmpty() && !v.IsImmediate()) {
        auto* i = v.InstRecursive();
        if (i->GetOpcode() != IR::Opcode::BitCastF32U32 &&
            i->GetOpcode() != IR::Opcode::BitCastU32F32) break;
        v = i->Arg(0).Resolve();
    }
    return v;
}
ScreenExpression ScreenAdd(ScreenExpression a, ScreenExpression b, float sign = 1) {
    if (!a.valid || !b.valid || a.reciprocal_w || b.reciprocal_w) return {};
    for (u32 i=0; i<6; ++i) a.coefficients[i] += sign*b.coefficients[i];
    return a;
}
ScreenExpression ScreenMultiply(ScreenExpression a, ScreenExpression b) {
    if (!a.valid || !b.valid) return {};
    if (a.reciprocal_w) std::swap(a,b);
    if (b.reciprocal_w) {
        if (a.reciprocal_w || a.coefficients[3] || a.coefficients[4] || a.coefficients[5]) return {};
        ScreenExpression r{}; r.valid=true;
        r.coefficients[3]=a.coefficients[0]; r.coefficients[4]=a.coefficients[1];
        r.coefficients[5]=a.coefficients[2]; return r;
    }
    const auto scalar=[](const ScreenExpression& e) {
        return !e.reciprocal_w && std::all_of(e.coefficients.begin(),e.coefficients.begin()+5,
                                            [](float x){return x==0;});
    };
    if (scalar(a)) std::swap(a,b);
    if (!scalar(b)) return {};
    for (auto& c:a.coefficients) c*=b.coefficients[5];
    return a;
}
ScreenExpression ScreenAnalyze(IR::Value v, const std::array<IR::Value,3>& roots, u32 depth=0) {
    if (v.IsEmpty() || depth>32) return {};
    v=ScreenRoot(v);
    ScreenExpression r{}; r.valid=true;
    for (u32 i=0;i<3;++i) if (!roots[i].IsEmpty() && !roots[i].IsImmediate() && v==roots[i]) {
        r.coefficients[i]=1; return r;
    }
    if (v.IsImmediate()) {
        if (v.Type()!=IR::Type::F32) return {};
        r.coefficients[5]=v.F32(); return r;
    }
    auto* i=v.InstRecursive();
    const auto arg=[&](u32 n){return ScreenAnalyze(i->Arg(n),roots,depth+1);};
    switch (i->GetOpcode()) {
    case IR::Opcode::FPAdd32: return ScreenAdd(arg(0),arg(1));
    case IR::Opcode::FPSub32: return ScreenAdd(arg(0),arg(1),-1);
    case IR::Opcode::FPNeg32: r.coefficients[5]=-1; return ScreenMultiply(arg(0),r);
    case IR::Opcode::FPMul32: return ScreenMultiply(arg(0),arg(1));
    case IR::Opcode::FPFma32: return ScreenAdd(ScreenMultiply(arg(0),arg(1)),arg(2));
    case IR::Opcode::FPDiv32:
        if (ScreenRoot(i->Arg(1))==roots[2]) {r.reciprocal_w=true;return ScreenMultiply(arg(0),r);}
        return {};
    case IR::Opcode::FPRecip32:
        if (ScreenRoot(i->Arg(0))==roots[2]) {r.reciprocal_w=true;return r;}
        return {};
    default: return {};
    }
}
// Screen-distortion vertices may export a*clip + b*local rather than a
// literal clip copy. Match the x/y/w triplet with the SAME uniform a and b;
// unrelated projector/shadow expressions must not qualify. Only single-block
// shaders are accepted so the dynamic scale dominates every epilogue.
struct BlendedScreenTerm {
    IR::Value scale, local;
    float sign{1.f};
};
bool ScreenUniform(IR::Value v) {
    v=ScreenRoot(v);
    if (v.IsEmpty() || v.IsImmediate()) return false;
    const auto* i=v.InstRecursive();
    return i->GetOpcode()==IR::Opcode::ReadConstBuffer &&
           i->Arg(0).IsImmediate() && i->Arg(1).IsImmediate();
}
bool SameScreenUniform(IR::Value a, IR::Value b) {
    if (!ScreenUniform(a) || !ScreenUniform(b)) return false;
    const auto* x=ScreenRoot(a).InstRecursive();
    const auto* y=ScreenRoot(b).InstRecursive();
    return x->Arg(0)==y->Arg(0) && x->Arg(1)==y->Arg(1);
}
BlendedScreenTerm ScreenBlend(IR::Value v, IR::Value root) {
    v=ScreenRoot(v);
    if (v.IsEmpty() || v.IsImmediate()) return {};
    auto* i=v.InstRecursive();
    if (i->GetOpcode()!=IR::Opcode::FPAdd32 && i->GetOpcode()!=IR::Opcode::FPFma32) return {};
    for (u32 swap=0;swap<2;++swap) {
        IR::Value a,b,local;
        if (i->GetOpcode()==IR::Opcode::FPFma32) {
            a=i->Arg(swap); b=i->Arg(1-swap); local=i->Arg(2);
        } else {
            auto product=ScreenRoot(i->Arg(swap));
            if (product.IsEmpty() || product.IsImmediate() ||
                product.InstRecursive()->GetOpcode()!=IR::Opcode::FPMul32) continue;
            auto* m=product.InstRecursive();
            a=m->Arg(0); b=m->Arg(1); local=i->Arg(1-swap);
            if (!ScreenUniform(a)) std::swap(a,b);
        }
        if (!ScreenUniform(a)) continue;
        float sign=1.f; b=ScreenRoot(b);
        if (!b.IsEmpty() && !b.IsImmediate() && b.InstRecursive()->GetOpcode()==IR::Opcode::FPNeg32) {
            sign=-1.f; b=ScreenRoot(b.InstRecursive()->Arg(0));
        }
        if (b==root) return {a,local,sign};
    }
    return {};
}
bool ScreenLocalProduct(IR::Value v, IR::Value uniform) {
    v=ScreenRoot(v);
    if (v.IsEmpty() || v.IsImmediate() || v.InstRecursive()->GetOpcode()!=IR::Opcode::FPMul32) return false;
    auto* i=v.InstRecursive();
    return SameScreenUniform(i->Arg(0),uniform) || SameScreenUniform(i->Arg(1),uniform);
}
void FindBlendedScreenExports(EmitContext& ctx, const IR::Program& program,
                             const std::array<IR::Value,3>& roots) {
    u32 work_blocks=0;
    for (auto* b:program.blocks) {
        bool work=false;
        for (const auto& i:*b) work |= i.GetOpcode()!=IR::Opcode::Epilogue;
        work_blocks+=work;
    }
    if (work_blocks!=1) return;
    struct Export { u32 param,component; IR::Value value; };
    std::vector<Export> exports;
    for (auto* b:program.blocks) for (const auto& i:*b)
        if (i.GetOpcode()==IR::Opcode::SetAttribute && IR::IsParam(i.Arg(0).Attribute()) && i.Arg(2).U32()<4)
            exports.push_back({u32(i.Arg(0).Attribute())-u32(IR::Attribute::Param0),i.Arg(2).U32(),i.Arg(1)});
    for (const auto& ew:exports) {
        const auto w=ScreenBlend(ew.value,roots[2]);
        if (w.scale.IsEmpty() || w.sign!=1.f || !ScreenUniform(w.local)) continue;
        std::array<BlendedScreenTerm,2> xy{};
        std::array<const Export*,2> found{};
        for (u32 axis=0;axis<2;++axis) for (const auto& e:exports) {
            auto term=ScreenBlend(e.value,roots[axis]);
            if (term.scale.IsEmpty() || term.sign!=(axis==0?1.f:-1.f) ||
                !SameScreenUniform(term.scale,w.scale) || !ScreenLocalProduct(term.local,w.local)) continue;
            if (found[axis]) return; // ambiguous writes or multiple coordinate sets
            found[axis]=&e; xy[axis]=term;
        }
        if (!found[0] || !found[1]) continue;
        for (u32 axis=0;axis<2;++axis) {
            const auto& e=*found[axis];
            ctx.native_screen_exports.push_back({e.param,e.component,axis,xy[axis].sign,false,xy[axis].scale});
        }
        return;
    }
}
// Only standard clip/NDC/screen-UV forms of the position: {axis, slope, normalized}. Arbitrary
// affine projections may be shadow/projector coordinates even if they share the position's inputs.
std::optional<std::tuple<u32,float,bool>> StandardScreenForm(IR::Value value,
                                                             const std::array<IR::Value,3>& roots) {
    const auto e=ScreenAnalyze(value,roots);
    u32 term=6,count=0;
    for (u32 n:{0u,1u,3u,4u}) if (e.coefficients[n]!=0) {term=n;++count;}
    if (!e.valid || e.reciprocal_w || count!=1) return std::nullopt;
    const bool normalized=term>=3;
    const float slope=e.coefficients[term], bias=e.coefficients[5];
    const bool standard = normalized ?
        (e.coefficients[2]==0 && ((std::abs(slope)==1.f && bias==0.f) ||
                                  (std::abs(slope)==.5f && bias==.5f))) :
        (bias==0.f && ((slope==1.f && e.coefficients[2]==0.f) ||
                      (std::abs(slope)==.5f && e.coefficients[2]==.5f)));
    if (!standard) return std::nullopt;
    return std::tuple{normalized?term-3:term,slope,normalized};
}
// Geometry programs write a position and its screen copies before every EmitVertex. A parameter
// is corrected only when every emitted vertex writes it, in the same standard form of that
// vertex's own position; anything else is left alone.
void FindGeometryScreenExports(EmitContext& ctx, const IR::Program& program) {
    using Form = std::tuple<u32,float,bool>;
    std::array<IR::Value,3> roots{};
    std::array<std::array<IR::Value,4>,IR::NumParams> params{};
    std::array<std::array<std::optional<Form>,4>,IR::NumParams> form{};
    std::array<std::array<u32,4>,IR::NumParams> written{};
    std::array<std::array<bool,4>,IR::NumParams> rejected{};
    u32 emits=0;
    for (const auto& node:program.syntax_list) {
        if (node.type!=IR::AbstractSyntaxNode::Type::Block) continue;
        for (auto& i:*node.data.block) {
            if (i.GetOpcode()==IR::Opcode::SetAttribute) {
                const auto attr=i.Arg(0).Attribute();
                const u32 c=i.Arg(2).U32();
                if (attr==IR::Attribute::Position0) {
                    if (c!=2 && c<=3) roots[c==3?2:c]=ScreenRoot(i.Arg(1));
                } else if (IR::IsParam(attr) && c<4) {
                    params[u32(attr)-u32(IR::Attribute::Param0)][c]=i.Arg(1);
                }
                continue;
            }
            if (i.GetOpcode()!=IR::Opcode::EmitVertex) continue;
            ++emits;
            const bool positioned=!roots[0].IsEmpty() && !roots[1].IsEmpty() && !roots[2].IsEmpty();
            for (u32 p=0;p<IR::NumParams;++p) for (u32 c=0;c<4;++c) {
                if (params[p][c].IsEmpty()) continue;
                ++written[p][c];
                const auto f=positioned ? StandardScreenForm(params[p][c],roots) : std::nullopt;
                if (!f || (form[p][c] && *form[p][c]!=*f)) rejected[p][c]=true;
                else form[p][c]=f;
            }
            roots={};
            params={};
        }
    }
    for (u32 p=0;p<IR::NumParams;++p) for (u32 c=0;c<4;++c) {
        if (rejected[p][c] || !form[p][c] || written[p][c]!=emits) continue;
        const auto [axis,slope,normalized]=*form[p][c];
        ctx.native_screen_exports.push_back({p,c,axis,slope,normalized});
    }
}
void FindNativeScreenExports(EmitContext& ctx, const IR::Program& program) {
    if (!ctx.runtime_info.native_multiview) return;
    if (ctx.stage==Stage::Geometry) {
        FindGeometryScreenExports(ctx,program);
        return;
    }
    if (ctx.stage!=Stage::Vertex) return;
    std::array<IR::Value,3> roots{};
    bool conflicting=false;
    for (auto* b:program.blocks) for (auto& i:*b) {
        if (i.GetOpcode()!=IR::Opcode::SetAttribute || i.Arg(0).Attribute()!=IR::Attribute::Position0) continue;
        const u32 c=i.Arg(2).U32(); if (c==2 || c>3) continue;
        const u32 axis=c==3?2:c;
        const auto v=ScreenRoot(i.Arg(1));
        if (!roots[axis].IsEmpty() && roots[axis]!=v) conflicting=true;
        roots[axis]=v;
    }
    if (conflicting || roots[0].IsEmpty() || roots[1].IsEmpty() || roots[2].IsEmpty()) return;
    std::array<std::array<bool,4>,IR::NumParams> rejected{};
    for (auto* b:program.blocks) for (auto& i:*b) {
        if (i.GetOpcode()!=IR::Opcode::SetAttribute || !IR::IsParam(i.Arg(0).Attribute())) continue;
        const u32 param=u32(i.Arg(0).Attribute())-u32(IR::Attribute::Param0), c=i.Arg(2).U32();
        if (c>3) continue;
        const auto f=StandardScreenForm(i.Arg(1),roots);
        if (!f) {rejected[param][c]=true;continue;}
        const auto [axis,slope,normalized]=*f;
        auto it=std::find_if(ctx.native_screen_exports.begin(),ctx.native_screen_exports.end(),
            [&](const auto& x){return x.param==param && x.component==c;});
        if (it==ctx.native_screen_exports.end())
            ctx.native_screen_exports.push_back({param,c,axis,slope,normalized});
        else if (it->axis!=axis || it->slope!=slope || it->normalized!=normalized)
            rejected[param][c]=true;
    }
    std::erase_if(ctx.native_screen_exports,[&](const auto& x){return rejected[x.param][x.component];});
    FindBlendedScreenExports(ctx,program,roots);
    if (std::getenv("VR_NATIVE_CAMERA_AUDIT"))
        for (const auto& e:ctx.native_screen_exports)
            LOG_INFO(Render_Vulkan,"VR screen export: shader {:#x} param {} component {} axis {} slope {} normalized {} dynamic {}",
                ctx.info.pgm_hash,e.param,e.component,e.axis,e.slope,e.normalized,!e.dynamic_scale.IsEmpty());
}
