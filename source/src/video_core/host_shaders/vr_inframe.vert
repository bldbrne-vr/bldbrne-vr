// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#version 450

// In-frame items (video_core/renderer_vulkan/vr_inframe.cpp): drawn into the game's scene colour
// with the game's own camera. Positions are in the camera record's view space (game units; x
// right, y up, z forward along the view); the projection is the game's D3D-style one (not
// reversed, depth 0..1), with NDC y flipped for Vulkan. The layout mirrors VrInFrame's
// DrawConstants (128 bytes).
//
// The depth mapping (A z + B, from the near and far planes) is specialized: it is the same for
// every frame of a headset session. The eye's projection is pushed with each draw instead (its
// clip scale folded into P00 / P11, its clip offset in proj.zw): the Monocular's stabilized
// window slides the zoomed eye's projection a little every frame, and a specialized projection
// built a new pipeline every frame (2026-11-03: ~10 fps while looking around through it).

layout(constant_id = 0) const float depth_a = 1.0;
layout(constant_id = 1) const float depth_b = 0.0;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_aux;

layout(push_constant, std430) uniform DrawData {
    vec4 mv[3];   // view <- mesh rows [R | t]
    vec4 color;   // linear rgb, a
    vec4 emissive; // Shade::Model: w = the grip's centre (mesh y)
    vec4 uv;      // u0 v0 u1 v1
    vec4 proj;    // P00 sx, P11 sy, ox, oy: clip = (P00 sx x + ox z, -P11 sy y + oy z, A z + B, z)
    vec4 extra;   // x shade, y exposure
} dd;

layout(set=0,binding=2,std430) readonly buffer AnimationData { vec4 values[]; } anim;
layout(location=5) out vec4 out_vertex_color;
layout(location=6) flat out int out_gray;
layout(location = 0) out vec3 out_view;
layout(location = 1) out vec3 out_normal;
layout(location = 2) out vec2 out_uv;
layout(location = 3) out vec3 out_local; // mesh space (the cards' analytic edges)
// A game model: where the hand that holds it is on screen (frame uv; -1 = none): the mesh point
// (0, emissive.w, 0), the grip's centre, projected. The fragment reads the scene's light there.
layout(location = 4) flat out vec2 out_anchor;

// Prefractured Messenger's Gift: deterministic cosmetic motion, no particle state.
// Centres correspond to the skull's twelve regions.
void MessengerCrumble(vec3 source, vec3 sourceNormal, float code, float seconds,
                      vec3 gravity, out vec3 position, out vec3 normal) {
    const vec3 centres[12]=vec3[12](
        vec3(-.038421,.020037,-.044084),vec3(.036299,.020738,-.046961),
        vec3(-.045786,.010443,.060019),vec3(.044885,.009613,.060210),
        vec3(-.033296,.064163,-.050279),vec3(.032682,.063167,-.053914),
        vec3(-.043085,.068724,.056453),vec3(.042626,.067921,.059469),
        vec3(-.023853,-.030334,-.073705),vec3(.024667,-.028365,-.071440),
        vec3(-.018493,.094099,.002861),vec3(.020116,.093838,.006155));
    int part=clamp(int(floor(fract(code)*100.0+.5))%25,0,11);
    float t=clamp(seconds,0.0,.85);
    float pressure=smoothstep(0.0,.17,t);
    vec3 stretch=mix(vec3(1),vec3(.94,.97,.94),pressure);
    vec3 pivot=vec3(0,.02,0);
    position=pivot+(source-pivot)*stretch;
    normal=normalize(sourceNormal/stretch);
    if(t<=.17) return;
    // A short cascading failure, not twelve pieces moving in lockstep.
    float delay=float((part*7)%5)*.008;
    float age=max(0.0,t-.17-delay);
    vec3 centre=pivot+(centres[part]-pivot)*stretch;
    vec3 axis=normalize(vec3(float(part%3)-.8,.55,float((part+1)%4)-1.4));
    float angle=age*(part%2==0?2.6:-2.2)*(1.0+.09*float(part%3));
    float cs=cos(angle),sn=sin(angle);
    vec3 offset=(position-centre)*(1.0-smoothstep(.35+delay*.5,.68-delay,age));
    offset=offset*cs+cross(axis,offset)*sn+axis*dot(axis,offset)*(1.0-cs);
    normal=normal*cs+cross(axis,normal)*sn+axis*dot(axis,normal)*(1.0-cs);
    vec3 velocity=normalize(centres[part]-pivot)*(.075+.014*float(part%3));
    position=centre+offset+velocity*age+gravity*(.9*age*age);
}

// BEGIN TONITRUS BRIDGE
float BoltHash(float n) { n=fract(n*.1031);n*=n+33.33;n*=n+n;return fract(n)*2.0-1.0; }
vec2 BoltNoise(float t,float tick,float frequency) {
    float s=t*frequency,i=floor(s),f=fract(s),seed=tick*51.0;
    return mix(vec2(BoltHash(i+seed),BoltHash(i+seed+23.0)),
               vec2(BoltHash(i+1.0+seed),BoltHash(i+1.0+seed+23.0)),f);
}
vec3 BoltCentre(float u,float branch,float tick) {
    // The trunk (0) and four forks: two long ones off its middle, a short early one and one
    // spreading where it lands.
    int b=int(branch+.5);
    const float begin[5]=float[5](0.0,.3125,.5625,.18,.70);
    const float end[5]=float[5](1.0,.80,.94,.50,.985);
    const vec2 lean[5]=vec2[5](vec2(0),vec2(1.35,.45),vec2(-1.15,.45),vec2(-.95,-.55),
                               vec2(.85,-.60));
    float t=mix(begin[b],end[b],u);
    // Broad path with fine crackle, rather than a uniformly noisy zigzag. Forks
    // share the exact trunk root then find their own tapering path.
    vec2 offset=(BoltNoise(t,tick,8.0)*.65+BoltNoise(t,tick+7.0,32.0)*.13)*(4.0*t*(1.0-t));
    if(b>0)offset+=(lean[b]+BoltNoise(u,tick+branch*9.0,8.0)*.22)*u;
    return vec3(offset.x,t,offset.y);
}
// `width`: the trunk's half-width; < 0 a small arc (the charged tool's: small flashes, no light
// pool). The path jitters once, 45 ms in, then holds, as a real stroke's channel.
void BoltVertex(vec3 parameter,vec2 aux,mat3 axes,vec3 origin,float age,float width,
                out vec3 eye,out vec3 local) {
    float code=aux.y,w=abs(width),scale=w/.06;
    bool small=width<0.0;
    local=vec3(aux.x,parameter.y,code);
    if(code<4.5) {
        float tick=min(floor(age/.045),1.0),u=parameter.y;
        eye=origin+axes*BoltCentre(u,code,tick);
        vec3 before=BoltCentre(max(0.0,u-1.0/32.0),code,tick);
        vec3 after=BoltCentre(min(1.0,u+1.0/32.0),code,tick);
        vec3 tangent=normalize(axes*(after-before));
        vec3 side=cross(tangent,normalize(-eye+vec3(0,0,1e-8)));
        if(dot(side,side)<1e-8)side=cross(tangent,abs(tangent.y)<.9?vec3(0,1,0):vec3(1,0,0));
        float taper=code<.5?(.85+.15*(1.0-u)):(code<2.5?.45:.30)*(1.0-.8*u);
        eye+=normalize(side)*aux.x*w*taper;
    } else if(code<7.5) {
        eye=origin+(code<5.5?vec3(0):axes[1]);
        float radius=code<5.5 ? .06*(1.0+age*2.0)
                   : code<6.5 ? .28*(1.0+age*3.0)
                              : (small ? 0.0 : .90*(1.0+age*1.5));
        eye.xy+=parameter.xy*radius*scale*(small?.35:1.0);
        local=vec3(parameter.xy,code);
    } else {
        float seed=code*9.13;
        vec3 direction=normalize(normalize(axes[0])*BoltHash(seed)
            -normalize(axes[1])*(.45+.35*abs(BoltHash(seed+1.0)))
            +normalize(axes[2])*BoltHash(seed+2.0));
        float speed=(.6+.8*abs(BoltHash(seed+3.0)))*scale;
        eye=origin+axes[1]+direction*speed*age;
        vec2 along=direction.xy;
        along=dot(along,along)>1e-8?normalize(along):vec2(0,1);
        vec2 across=vec2(along.y,-along.x);
        eye.xy+=(across*parameter.x*.003+along*parameter.y*.014)*scale;
        local=vec3(parameter.xy,code);
    }
}
// END TONITRUS BRIDGE
// BEGIN SOFT GLOVE
void GloveSway(inout vec3 p,inout vec3 n,vec3 drag) {
    // Pinned gathered cuff, progressively softer palm and fingertips.
    float t=clamp(-p.z/.16,0.0,1.0),w=t*t;
    float dw=t>0.0&&t<1.0?-2.0*t/.16:0.0;
    float det=1.0+drag.z*dw;
    n.z-=dw*dot(drag,n)/max(det,.25);
    p+=drag*w;
}
void GloveDeform(inout vec3 p,inout vec3 n,float charge,float time,vec3 drag) {
    GloveSway(p,n,drag);
    float reach=clamp((-p.z-.1197)/.0704,0.0,1.0);
    float amount=charge*.24+min(length(drag)*5.0,.24)*sin(time*23.0);
    float a=amount*reach,c=cos(a),s=sin(a),z=p.z+.1197;
    float da=reach>0.0&&reach<1.0?-amount/.0704:0.0;
    float yy=c*p.y-s*z,zz=s*p.y+c*z;
    float j01=-s-da*zz,j11=c+da*yy,d=c*j11-s*j01;
    if(abs(d)>.01)n=vec3(n.x,(j11*n.y-s*n.z)/d,(-j01*n.y+c*n.z)/d);
    p.y=yy;p.z=zz-.1197;
}
// END SOFT GLOVE
// BEGIN AUGUR SPRAY
// Each kind's life in seconds (core/vr/augur.h DropKind: mist, blood, strand, glob, puff); style
// 1 (Beast Roar's burst): ash, embers, sparks, burning ash flakes, (no smoke).
float AugurSprayLife(float kind,float style) {
    if(style>.5)return kind<.5?1.1:kind<1.5?.8:kind<2.5?.35:kind<3.5?.9:.6;
    return kind<.5?.55:kind<1.5?.95:kind<2.5?.5:kind<3.5?1.15:.45;
}
void AugurSprayVertex(vec3 corner,vec3 velocity,vec2 shape,float age,mat3 axes,
                      vec3 origin,vec3 down,float style,out vec3 eye,out vec3 drop_data) {
    const float kind=corner.z;
    vec3 v=velocity;
    float k=kind<.5?4.:kind<1.5?2.:kind<2.5?2.6:kind<3.5?1.1:6.;
    float g=kind>3.5?.6:3.9;
    if(style>.5) {
        // Beast Roar (the tester's 2026-11-04 "particles fly out around the hand when it gets
        // cast"): thrown all round the claw (half of them turned down), embers drifting up as
        // they cool, ash sinking, sparks quick.
        if(fract(velocity.x*37.1+velocity.z*11.3)>.5)v.y=-v.y;
        // Sparks tear out ahead of the slower, tumbling ash.
        v*=kind<.5?.65:kind<1.5?1.5:kind<2.5?2.6:kind<3.5?1.15:1.4;
        k=kind<.5?3.:kind<1.5?2.4:kind<2.5?3.5:kind<3.5?1.8:5.;
        g=kind<.5?.5:kind<1.5?-.9:kind<2.5?1.5:kind<3.5?-.6:-.2;
    }
    v=axes*v;
    // A burst out of the crushed fist, analytic, no simulation (the tester's 2026-11-04 "almost
    // absurd! Cinematic too"): heavy globs keep their speed and arc far, the mist and the puffs
    // stop at once and hang, all of it falling slow as in a slowed shot, then heavily.
    float drag=1.+k*age;
    vec3 centre=origin+v*(1.9*age/drag+.012)+down*(g*age*age);
    vec2 projected=v.xy*(1.9/(drag*drag))+down.xy*(2.*g*age);
    vec2 along=length(projected)>.0001?normalize(projected):vec2(0,1);
    vec2 across=vec2(along.y,-along.x);
    float life=AugurSprayLife(kind,style);
    // A puff blooms as it fades; the rest swell out of the burst, then shrink as they go.
    float grow=kind>3.5?1.+2.5*age/life:mix(.6,1.,smoothstep(0.,.05,age));
    // (The roar's embers and flakes smaller than the Augur's drops: flecks, not blobs; its smoke
    // none: a puff at the hand hid the claw, the tester's 2026-11-04 "it makes the mesh disappear".)
    float scale=style>.5&&kind>3.5?0.:style>.5&&kind>.5&&kind<3.5?.6:1.25;
    float size=scale*shape.x*length(axes[0])*grow*mix(1.,.65,smoothstep(.5*life,life,age));
    float stretch=mix(shape.y,1.,smoothstep(.015,.18,age));
    eye=centre+vec3((across*corner.x+along*corner.y*stretch)*size,0);
    drop_data=vec3(corner.xy,shape.y);
}
// END AUGUR SPRAY
void main() {
    out_gray = gl_InstanceIndex;
    if (out_gray == 256) {
        vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
        gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
        return;
    }
    vec3 position=in_position;
    vec3 mesh_normal=in_normal;
    out_vertex_color=vec4(1.0);
    const int shade = int(dd.extra.x + 0.5) & 15;
    if (shade == 1 && dd.uv.x == -2010.0) {
        // Sky-only quads need projection, not model skinning, a normal-matrix inverse,
        // depth pull or a scene-light anchor. Test at the lighting system's distant-sky
        // cutoff, not depth 1: the game's sky dome itself writes a finite depth. Keep XY
        // directional and retain foreground occlusion; translucent/no depth writes.
        const vec4 p = vec4(in_position, 1.0);
        const vec3 eye = vec3(dot(dd.mv[0], p), dot(dd.mv[1], p), dot(dd.mv[2], p));
        out_view = eye;
        out_local = in_position;
        out_normal = vec3(0, 0, 1);
        out_uv = in_aux;
        out_anchor = vec2(-1.0);
        const float sky_depth = clamp(depth_a + depth_b * dd.uv.w, 0.0, 1.0);
        gl_Position = vec4(dd.proj.x * eye.x, -dd.proj.y * eye.y,
                           sky_depth * eye.z, eye.z);
        gl_Position.xy += gl_Position.w * dd.proj.zw;
        return;
    }
    if(shade==1 && dd.uv.x==-2000.0) {
        mat3 axes=transpose(mat3(dd.mv[0].xyz,dd.mv[1].xyz,dd.mv[2].xyz));
        vec3 eye,drop_data;
        AugurSprayVertex(in_position,in_normal,in_aux,dd.uv.y,axes,
            vec3(dd.mv[0].w,dd.mv[1].w,dd.mv[2].w),dd.emissive.xyz,dd.uv.z,eye,drop_data);
        // uv: the particle's kind and the burst's style for its shading (AugurSprayShade).
        out_view=eye;out_local=drop_data;out_uv=vec2(in_position.z,dd.uv.z);out_normal=vec3(0,0,1);out_anchor=vec2(-1);
        gl_Position=vec4(dd.proj.x*eye.x,-dd.proj.y*eye.y,depth_a*eye.z+depth_b,eye.z);
        gl_Position.xy+=gl_Position.w*dd.proj.zw;
        return;
    }
    const bool bolt=shade==1 && dd.uv.x==-2070.0;
    if(bolt) {
        mat3 axes=transpose(mat3(dd.mv[0].xyz,dd.mv[1].xyz,dd.mv[2].xyz));
        vec3 eye,local;
        BoltVertex(in_position,in_aux,axes,vec3(dd.mv[0].w,dd.mv[1].w,dd.mv[2].w),
                   dd.uv.y,dd.uv.z,eye,local);
        out_view=eye;out_local=local;out_uv=in_aux;out_normal=vec3(0,0,1);out_anchor=vec2(-1);
        gl_Position=vec4(dd.proj.x*eye.x,-dd.proj.y*eye.y,depth_a*eye.z+depth_b,eye.z);
        gl_Position.xy+=gl_Position.w*dd.proj.zw;
        return;
    }
    if(shade==13 && dd.uv.x>1.5 && dd.uv.x<2.5 && dd.uv.y>=0.0)
        MessengerCrumble(in_position,in_normal,in_aux.x,dd.uv.y,dd.emissive.xyz,position,mesh_normal);
    if(shade==13 && dd.uv.x>3.5 && dd.uv.x<4.5 &&
       (dd.uv.z>0.0 || dot(dd.emissive.xyz,dd.emissive.xyz)>1e-10))
        GloveDeform(position,mesh_normal,clamp(dd.uv.z,0.0,1.0),max(dd.uv.y,0.0),dd.emissive.xyz);
    if(shade==4 && dd.uv.z<0.0 && dd.uv.w>0.0) {
        const int vertex=gl_VertexIndex;
        const vec4 weights=anim.values[vertex];
        out_vertex_color=anim.values[int(dd.uv.y)+vertex];
        const float frame=clamp(dd.uv.x,0.0,254.0);
        const int f0=int(floor(frame)),f1=min(f0+1,254);
        const float blend=fract(frame);
        const bool scaled_bones=dd.uv.z<=-4.0;
        const float second_frame=clamp(-dd.uv.z-4.0,0.0,254.0);
        const int s0=int(floor(second_frame)),s1=min(s0+1,254);
        const float second_mix=fract(second_frame),pose_mix=2.0*fract(dd.uv.w);
        vec4 rows[3];
        for(int r=0;r<3;r++) {
            const int a=2*int(dd.uv.y)+f0*int(dd.uv.w)+r;
            const int b=2*int(dd.uv.y)+f1*int(dd.uv.w)+r;
            // Stored items sample one rest frame. Most vertices have one joint; do not fetch
            // and blend a second matrix whose weight is zero.
            vec4 first=anim.values[a+int(weights.x)*3];
            if(blend>0.0) first=mix(first,anim.values[b+int(weights.x)*3],blend);
            rows[r]=first*weights.z;
            if(weights.w>0.0) {
                vec4 second=anim.values[a+int(weights.y)*3];
                if(blend>0.0) second=mix(second,anim.values[b+int(weights.y)*3],blend);
                rows[r]+=second*weights.w;
            }
            if(scaled_bones && pose_mix>0.0) {
                const int c=2*int(dd.uv.y)+s0*int(dd.uv.w)+r;
                const int d=2*int(dd.uv.y)+s1*int(dd.uv.w)+r;
                vec4 second=mix(anim.values[c+int(weights.x)*3],anim.values[d+int(weights.x)*3],second_mix)*weights.z;
                if(weights.w>0.0)second+=mix(anim.values[c+int(weights.y)*3],anim.values[d+int(weights.y)*3],second_mix)*weights.w;
                rows[r]=mix(rows[r],second,pose_mix);
            }
        }
        const vec4 bind=vec4(in_position,1.0);
        position=vec3(dot(rows[0],bind),dot(rows[1],bind),dot(rows[2],bind));
        mesh_normal=vec3(dot(rows[0].xyz,in_normal),dot(rows[1].xyz,in_normal),dot(rows[2].xyz,in_normal));
        if(scaled_bones) {
            // Cofactors: inverse-transpose direction without a matrix inverse. Positive
            // determinant is validated offline across the authored poses and crossfades.
            mesh_normal=vec3(dot(cross(rows[1].xyz,rows[2].xyz),in_normal),
                             dot(cross(rows[2].xyz,rows[0].xyz),in_normal),
                             dot(cross(rows[0].xyz,rows[1].xyz),in_normal));
        }
        // Charge is skeletal; movement reuses only the bounded spring shear, with no
        // extra trigonometry or a second fingertip curl on top of the rig.
        if(dd.uv.z < -2.5 && dd.uv.z > -3.5)
            GloveSway(position,mesh_normal,dd.emissive.xyz);
    }
    const vec4 p = vec4(position, 1.0);
    const vec3 view0 = vec3(dot(dd.mv[0], p), dot(dd.mv[1], p), dot(dd.mv[2], p));
    // R's columns are the rows given; the normal goes by inverse(transpose(R^T)) = inverse(R).
    const mat3 r = mat3(dd.mv[0].xyz, dd.mv[1].xyz, dd.mv[2].xyz);
    // VialDrain (8), Syringe (10), NativeLiquid (14): chamber data replaces texture bounds.
    // Goods carries material/cavity in aux, not texture coordinates. Its draw uv holds
    // per-item selectors; remapping aux with those selectors corrupts material IDs.
    out_uv = (shade == 8 || shade == 10 || shade == 13 || shade == 14 || (shade==4 && dd.uv.z<0.0)) ? in_aux : mix(dd.uv.xy, dd.uv.zw, in_aux);
    out_local = shade==13 && dd.uv.x>1.5 ? in_position : position;
    vec3 view = view0;
    vec3 normal = inverse(r) * mesh_normal;
    // Shade::Steel (9) as a wire (uv.x > 0: the vial's needle, core/vr/vial_needle.h): the mesh's
    // vertices are on its axis (in_aux = (side -1 / +1, the radius there as a share of the
    // shaft's 0.5)); each goes out sideways, square to the axis and the eye, to the needle's edge
    // plus one pixel. out_local = (the distance from the axis in pixels, the half width drawn in
    // pixels: at least uv.x, the share of it that is needle); out_normal = the sideways direction
    // (the fragment rebuilds the round normal from both). uv.y = 1 / the frame's height.
    if (shade == 9 && dd.uv.x > 0.0 && dd.uv.y > 0.0 && view0.z > 1e-4) {
        // (v * r = R v: the mesh direction v in view.)
        const vec3 axis = normalize(vec3(0.0, 1.0, 0.0) * r);
        const vec3 to_eye = normalize(-view0);
        vec3 side = cross(axis, to_eye);
        if (dot(side, side) < 1e-10) {
            side = cross(axis, vec3(0.0, 1.0, 0.0)); // pointing at the eye: any side
        }
        side = normalize(side);
        const float pixel = 2.0 * view0.z * dd.uv.y / max(dd.proj.y, 1e-6);
        const float radius_px = length(vec3(0.5, 0.0, 0.0) * r) * in_aux.y / pixel;
        const float drawn_px = max(radius_px, dd.uv.x);
        const float half_px = drawn_px + 1.0;
        view = view0 + side * (in_aux.x * half_px * pixel);
        normal = side;
        out_local = vec3(in_aux.x * half_px, drawn_px, radius_px / max(drawn_px, 1e-6));
    }
    out_normal = normal;
    out_view = view;
    // Item::depth_pull (extra.x's bits 6 and up, in 1e-4 game units): depth-tested as if that
    // much nearer, along the ray to the eye (the pixel stays where it is), never nearer than just
    // past the near plane (-B / A).
    vec3 pv = view;
    const float pull = float(int(dd.extra.x + 0.5) >> 6) * 1e-4;
    if (pull > 0.0 && view.z > 1e-4) {
        const float near_z = -depth_b / max(depth_a, 1e-6);
        const float z = max(view.z - pull, min(view.z, near_z * 1.02));
        pv = view * (z / view.z);
    }
    gl_Position = vec4(dd.proj.x * pv.x, -dd.proj.y * pv.y, depth_a * pv.z + depth_b, pv.z);
    gl_Position.xy += gl_Position.w * dd.proj.zw;
    out_anchor = vec2(-1.0);
    // Held models, including liquid bottles: light sampled at the holding hand.
    if (shade == 4 || shade == 8 || shade == 9 || shade == 10 || shade == 11 || shade == 14) {
        // (Steel: the fist is emissive.xyz in its own mesh.)
        const vec4 a = shade == 9 ? vec4(dd.emissive.xyz, 1.0)
                     : shade == 4 && dd.uv.z < -2.5 ? vec4(0.0,0.0,.006,1.0)
                     : vec4(0.0, dd.emissive.w, 0.0, 1.0);
        const vec3 av = vec3(dot(dd.mv[0], a), dot(dd.mv[1], a), dot(dd.mv[2], a));
        if (av.z > 1e-3) {
            out_anchor = (vec2(dd.proj.x * av.x, -dd.proj.y * av.y) / av.z + dd.proj.zw) * 0.5 +
                         0.5;
        }
    }
}
