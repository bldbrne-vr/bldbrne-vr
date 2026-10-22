"""Build bbport's FSR 4 v07 post-pass store rewrite (FAST bit 37) and a fenced SPD
auto-exposure pass. Never edits original assets; writes fsr4_shaders/opt/*_post.spv,
opt/spd_auto_exposure.spv and opt/post-lds.json. Idempotent: an up-to-date manifest is kept.
Idea: upstream/bbport/tools/fsr4_post_lds.pl (bbport rebuilds the shader through GLSL).

Post pass. Each invocation computes a 2x2 block of output pixels and stores it into three
images (recurrent state, history, output): every store instruction of a wave writes every
other pixel. bbport measured the stores at ~4x the pass's arithmetic (RX 7800 XT, 4K: post
2.1-3.5 -> 0.81-0.87 ms; 1440p 1.2-1.5 -> 0.36-0.38 ms). The rewrite collects the 8x8
workgroup's 16x16 block in shared memory (as float, so the store converts exactly as before)
and writes contiguous rows after one barrier.

Recipe v2 patches the original SPIR-V instead of recompiling spirv-cross GLSL with glslang.
Every instruction of the original is kept verbatim (same ids, constants, order and
decorations); only the three OpImageWrite become shared-memory stores, and the barrier and
the row-wise flush go before the single OpReturn. The reassembled module is checked to
contain the original as an ordered subsequence and to add no floating-point instruction, so
the pass computes exactly what the original computes. (v1's GLSL round trip was
op-for-op identical too, but it depended on spirv-cross/glslang details: signed int8
unpacks, dozens of function-local copies of the constant weight tables.) spirv-cross is used
only to prove the structure: the rewrite is valid only when every invocation reaches the
barrier and fills its own four slots of all three arrays exactly once, so the script refuses
any shader with early exits, conditional or extra stores, or another block shape.

SPD fence. spd_auto_exposure.spv uses SPD's last-workgroup pattern: thread 0 of every
workgroup stores its luma tile to a coherent image, then increments a global counter (relaxed
atomic, no fence); the workgroup that sees the final count averages all tiles. Nothing orders
the store before the increment (Vulkan memory model: a data race), so a GPU that reorders
them - NVIDIA may - hands the last workgroup a stale tile: a one-frame exposure error. Two FSR
contexts fed identical frames then drift apart through their history (the FAST_UPSCALER_VERIFY
twin drifted after ~2300 bit-exact frames, most likely from this). The fix adds device-scope
fences around the atomic and widens the following workgroup barrier to device scope; no
arithmetic changes. A release fence orders only its own invocation's stores, so the script
proves that the tile store and the increment both run in invocation 0 alone. With FAST bit 37
the host requests opt/spd_auto_exposure.spv for every FSR context (VR_FSR4_FLAG_FENCED_SPD);
bit off, or FAST_UPSCALER_SPD_FENCE=0, keeps the original.

The host requests the post files only with FAST bit 37 (FAST_UPSCALER_VERIFY compares them
with the original post pass in game).
"""
from pathlib import Path
import argparse, hashlib, json, os, re, shutil, subprocess, tempfile

RECIPE = 'bbport-post-lds-v2-spirv'
SLOTS = (('rw_recurrent_0', 'bb_rec'), ('rw_history_color', 'bb_hist'),
         ('rw_mlsr_output_color', 'bb_out'))
SPD = 'spd_auto_exposure.spv'
LINE = re.compile(r'^\s*(?:%(\w+) = )?(Op\w+)\s*(.*)$')
# Only these may appear in what the rewrite adds: integer index math, shared memory, the
# barrier, the flush's image stores, and declarations. No floating-point instruction.
POST_ADDED = {'OpName', 'OpDecorate', 'OpConstant', 'OpConstantComposite', 'OpTypeArray',
              'OpTypePointer', 'OpVariable', 'OpEntryPoint', 'OpLoad', 'OpVectorShuffle',
              'OpIMul', 'OpISub', 'OpIAdd', 'OpCompositeExtract', 'OpCompositeConstruct',
              'OpAccessChain', 'OpStore', 'OpControlBarrier', 'OpBitwiseAnd',
              'OpShiftRightLogical', 'OpImageWrite'}
SPD_ADDED = {'OpConstant', 'OpMemoryBarrier', 'OpControlBarrier'}
SCOPE_DEVICE, SCOPE_WORKGROUP = 1, 2
ACQ_REL, UNIFORM_MEMORY, WORKGROUP_MEMORY, IMAGE_MEMORY = 0x8, 0x40, 0x100, 0x800


def depth_at(source, index):
    """Brace depth of the text before index (decompiled GLSL has no braces in strings)."""
    prefix = source[:index]
    return prefix.count('{') - prefix.count('}')


def prove_block_loop(source):
    """Prove the post pass's shape on spirv-cross GLSL of the original. Returns the SPIR-V id
    of the 2x2 block coordinate (spirv-cross names an unnamed id N as _N)."""
    if 'layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;' not in source:
        raise ValueError('Unexpected local size; refusing an unknown shader')
    if re.search(r'\bbarrier\(|\bshared\b|\breturn\b|gl_LocalInvocationIndex|\bimageAtomic',
                 source):
        raise ValueError('Early exits, shared memory or barriers present; rewrite unsafe')
    table = re.search(r'const ivec2 (_\d+)\[4\] = ivec2\[\]\(ivec2\(0\), ivec2\(1, 0\), '
                      r'ivec2\(0, 1\), ivec2\(1\)\);', source)
    if not table:
        raise ValueError('No 2x2 block offset table; refusing an unknown shader')
    mains = list(re.finditer(r'^void main\(\)\n\{\n', source, flags=re.M))
    if len(mains) != 1:
        raise ValueError('Expected one main')
    main_at = mains[0].end()
    # The block loop: exactly four iterations, directly in main.
    loop = re.search(r'^    for \(int (_\d+) = 0; \1 < 4; \1\+\+\)\n    \{\n', source[main_at:],
                     flags=re.M)
    if not loop:
        raise ValueError('No four-iteration block loop in main')
    loop_at = main_at + loop.end()
    # The loop body ends where the depth returns to main's.
    depth, loop_end = 2, None
    for index in range(loop_at, len(source)):
        depth += {'{': 1, '}': -1}.get(source[index], 0)
        if depth == 1:
            loop_end = index
            break
    if loop_end is None:
        raise ValueError('Unterminated block loop')
    body = source[loop_at:loop_end]
    counter = loop.group(1)
    if re.search(r'\b' + counter + r'\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|(?:\+\+|--)\s*' + counter +
                 r'\b|\b' + counter + r'\s*(?:\+\+|--)', body):
        raise ValueError('The block loop counter changes inside the loop')
    coordinate = re.search(r'uvec2 (_\d+) = \(gl_GlobalInvocationID\.xy \* uvec2\(2u\)\) \+ '
                           r'uvec2\(' + table.group(1) + r'\[' + counter + r'\]\);', body)
    if not coordinate:
        raise ValueError('Block coordinate is not gid * 2 + offset[loop]')
    pixel = coordinate.group(1)
    for image, _ in SLOTS:
        stores = list(re.finditer(r'imageStore\(' + image + r', ', source))
        if len(stores) != 1:
            raise ValueError(f'Expected one store to {image}, found {len(stores)}')
        store = stores[0]
        if not source.startswith('imageStore(' + image + ', ivec2(' + pixel + '), ',
                                 store.start()):
            raise ValueError(f'{image} is not stored at the block coordinate')
        # Unconditional: directly in the block loop body (main=1, loop=2), not in a branch
        # or an inner loop, and no break/continue at that level can skip it.
        if not loop_at <= store.start() < loop_end or depth_at(source, store.start()) != 2:
            raise ValueError(f'{image} store is conditional or outside the block loop')
    # No break/continue of the block loop itself (inner loops keep their own).
    level = 2
    for index in range(loop_at, loop_end):
        level += {'{': 1, '}': -1}.get(source[index], 0)
        if level == 2 and (source.startswith('break;', index) or
                           source.startswith('continue;', index)):
            raise ValueError('The block loop can skip its stores')
    return pixel[1:]


class Module:
    """SPIR-V assembly from spirv-dis --raw-id: lines plus an id -> instruction index."""

    def __init__(self, text):
        self.lines = text.splitlines()
        self.version = re.search(r'^; Version: (\d+\.\d+)$', text, flags=re.M)
        if not self.version:
            raise ValueError('No SPIR-V version header')
        self.version = self.version.group(1)
        self.defs, self.names = {}, {}
        for index, line in enumerate(self.lines):
            m = LINE.match(line)
            if m and m.group(1):
                self.defs[m.group(1)] = (m.group(2), m.group(3).split(), index)
            n = re.match(r'^\s*OpName %(\w+) "([^"]*)"$', line)
            if n:
                self.names.setdefault(n.group(2), []).append(n.group(1))

    def where(self, opcode):
        return [i for i, line in enumerate(self.lines)
                if (m := LINE.match(line)) and m.group(2) == opcode]

    def count(self, opcode):
        return len(self.where(opcode))

    def operands(self, index):
        return LINE.match(self.lines[index]).group(3).split()

    def type_id(self, opcode, *operands):
        found = [i for i, (op, args, _) in self.defs.items()
                 if op == opcode and args == list(operands)]
        if len(found) != 1:
            raise ValueError(f'Expected one {opcode} {" ".join(operands)}, found {len(found)}')
        return '%' + found[0]

    def constant(self, ref):
        """Value of an OpConstant id ('%N'), or None."""
        op, args, _ = self.defs.get(ref[1:], (None, None, None))
        return int(args[1]) if op == 'OpConstant' and re.fullmatch(r'-?\d+', args[1]) else None

    def variable_named(self, name):
        ids = [i for i in self.names.get(name, []) if self.defs.get(i, ('',))[0] == 'OpVariable']
        if len(ids) != 1:
            raise ValueError(f'Expected one variable named {name}')
        return '%' + ids[0]

    def last_of(self, *opcodes):
        hits = [i for op in opcodes for i in self.where(op)]
        if not hits:
            raise ValueError(f'No {"/".join(opcodes)} section')
        return max(hits)

    def guards(self, index):
        """Branch conditions (id, taken) that line `index` executes under, innermost first.
        Follows a block upward only while its sole predecessor is an OpBranchConditional of a
        selection that targets it as a branch, not as the merge; stops otherwise (fewer
        guards, never a wrong one)."""
        branches = [i for i in range(len(self.lines))
                    if (m := LINE.match(self.lines[i])) and
                    m.group(2) in ('OpBranch', 'OpBranchConditional', 'OpSwitch')]
        out = []
        while True:
            label = next(('%' + m.group(1) for i in range(index, -1, -1)
                          if (m := LINE.match(self.lines[i])) and m.group(2) == 'OpLabel'), None)
            if label is None:
                raise ValueError('Instruction outside a block')
            preds = [i for i in branches if label in self.operands(i)]
            if len(preds) != 1 or not self.lines[preds[0]].split()[0] == 'OpBranchConditional':
                return out
            cond, true, false = self.operands(preds[0])[:3]
            header = LINE.match(self.lines[preds[0] - 1])
            if not header or header.group(2) != 'OpSelectionMerge' or \
                    header.group(3).split()[0] == label or true == false:
                return out
            out.append((cond, label == true))
            index = preds[0]

    def only_invocation_zero(self, index):
        """Line `index` runs only in LocalInvocationIndex 0: guarded by lid == 0, or by
        lid < a and lid % b == 0 with a <= b."""
        lidx = [d.group(1) for l in self.lines
                if (d := re.match(r'^\s*OpDecorate %(\w+) BuiltIn LocalInvocationIndex$', l))]
        loads = {i for i, (op, args, _) in self.defs.items()
                 if op == 'OpLoad' and len(lidx) == 1 and args[1] == '%' + lidx[0]}
        is_lid = lambda ref: ref[1:] in loads
        zero, below, multiple = False, set(), set()
        for cond, taken in self.guards(index):
            op, args, _ = self.defs.get(cond[1:], (None, [], None))
            if not taken or len(args) < 3:
                continue
            a, b = args[1:3]
            if op == 'OpIEqual':
                if self.constant(a) == 0:
                    a, b = b, a
                if self.constant(b) != 0:
                    continue
                inner = self.defs.get(a[1:], (None, [], None))
                if is_lid(a):
                    zero = True
                elif inner[0] == 'OpUMod' and is_lid(inner[1][1]) and self.constant(inner[1][2]):
                    multiple.add(self.constant(inner[1][2]))
            elif op == 'OpULessThan' and is_lid(a) and self.constant(b):
                below.add(self.constant(b))
        return zero or any(x <= y for x in below for y in multiple)


def patch(lines, inserts, replacements):
    """inserts: {index: [lines before it]}; replacements: {index: [lines replacing it]}."""
    out = []
    for index, line in enumerate(lines):
        out += inserts.get(index, [])
        out += replacements.get(index, [line])
    return '\n'.join(out) + '\n'


def rewrite_post(module, pixel):
    """The shared-memory store rewrite on the original SPIR-V. Returns (text, removed line
    indices of the original)."""
    m = module
    for opcode in ['OpControlBarrier', 'OpMemoryBarrier', 'OpReturnValue', 'OpKill',
                   'OpTerminateInvocation', 'OpDemoteToHelperInvocation', 'OpUnreachable',
                   'OpFunctionCall', 'OpImageTexelPointer']:
        if m.count(opcode):
            raise ValueError(f'{opcode} present; rewrite unsafe')
    if any((op := LINE.match(l)) and op.group(2).startswith('OpAtomic') for l in m.lines):
        raise ValueError('Atomics present; rewrite unsafe')
    if m.count('OpFunction') != 1 or m.count('OpReturn') != 1 or m.count('OpEntryPoint') != 1:
        raise ValueError('Expected one function with one return and one entry point')
    if any(re.search(r'\bBuiltIn (WorkgroupId|LocalInvocationIndex)\b', l) for l in m.lines) or \
            any(' Workgroup' in l and 'OpVariable' in l for l in m.lines):
        raise ValueError('Workgroup builtins or shared memory already present')
    entry = m.where('OpEntryPoint')[0]
    entry_ops = m.operands(entry)
    if entry_ops[0] != 'GLCompute':
        raise ValueError('Not a compute entry point')
    main = entry_ops[1]
    if not any(re.match(r'^\s*OpExecutionMode ' + re.escape(main) + r' LocalSize 8 8 1$', l)
               for l in m.lines):
        raise ValueError('Unexpected local size')
    uint = m.type_id('OpTypeInt', '32', '0')
    flt = m.type_id('OpTypeFloat', '32')
    v2uint = m.type_id('OpTypeVector', uint, '2')
    v3uint = m.type_id('OpTypeVector', uint, '3')
    v4float = m.type_id('OpTypeVector', flt, '4')
    writes = m.where('OpImageWrite')
    if len(writes) != 3:
        raise ValueError(f'Expected three image stores, found {len(writes)}')
    sites, images = {}, {}
    for index in writes:
        args = m.operands(index)
        if len(args) not in (3, 4) or (len(args) == 4 and args[3] != 'None'):
            raise ValueError('Image store with image operands')
        image, coord, texel = args[:3]
        if coord != '%' + pixel:
            raise ValueError('Image store not at the proven block coordinate')
        load = m.defs.get(image[1:])
        if not load or load[0] != 'OpLoad':
            raise ValueError('Image operand is not a loaded variable')
        variable = load[1][1]
        name = [n for n, ids in m.names.items() if variable[1:] in ids]
        slot = [s for image_name, s in SLOTS if image_name in name]
        if len(slot) != 1 or slot[0] in sites.values():
            raise ValueError('Unexpected or repeated store target')
        if m.defs[coord[1:]][1][0] != v2uint or m.defs[texel[1:]][1][0] != v4float:
            raise ValueError('Store coordinate is not uvec2 or value is not vec4')
        sites[index] = slot[0]
        images[slot[0]] = (load[1][0], variable)
    if set(images) != {s for _, s in SLOTS}:
        raise ValueError('Missing a store target')

    ret = m.where('OpReturn')[0]
    names = [f'OpName %{s} "{s}"' for _, s in SLOTS]
    decorations = ['OpDecorate %bb_wgid BuiltIn WorkgroupId',
                   'OpDecorate %bb_lidx BuiltIn LocalInvocationIndex']
    globals_ = [
        f'%bb_u2 = OpConstant {uint} {SCOPE_WORKGROUP}',
        f'%bb_u4 = OpConstant {uint} 4',
        f'%bb_u15 = OpConstant {uint} 15',
        f'%bb_u16 = OpConstant {uint} 16',
        f'%bb_u64 = OpConstant {uint} 64',
        f'%bb_u128 = OpConstant {uint} 128',
        f'%bb_u192 = OpConstant {uint} 192',
        f'%bb_u256 = OpConstant {uint} 256',
        f'%bb_sem = OpConstant {uint} {ACQ_REL | WORKGROUP_MEMORY}',
        f'%bb_v16 = OpConstantComposite {v2uint} %bb_u16 %bb_u16',
        f'%bb_arr = OpTypeArray {v4float} %bb_u256',
        '%bb_parr = OpTypePointer Workgroup %bb_arr',
        f'%bb_pv4 = OpTypePointer Workgroup {v4float}',
        f'%bb_pin3 = OpTypePointer Input {v3uint}',
        f'%bb_pin1 = OpTypePointer Input {uint}',
        '%bb_wgid = OpVariable %bb_pin3 Input',
        '%bb_lidx = OpVariable %bb_pin1 Input',
    ] + [f'%{s} = OpVariable %bb_parr Workgroup' for _, s in SLOTS]
    replacements = {entry: [m.lines[entry].rstrip() +
                            ' %bb_wgid %bb_lidx ' + ' '.join('%' + s for _, s in SLOTS)]}
    for n, (index, slot) in enumerate(sorted(sites.items())):
        _, coord, texel = m.operands(index)[:3]
        # Local slot of the stored pixel: coord - WorkgroupId * 16, row-major in 16x16.
        replacements[index] = [
            f'%bb_wg{n} = OpLoad {v3uint} %bb_wgid',
            f'%bb_wgxy{n} = OpVectorShuffle {v2uint} %bb_wg{n} %bb_wg{n} 0 1',
            f'%bb_base{n} = OpIMul {v2uint} %bb_wgxy{n} %bb_v16',
            f'%bb_loc{n} = OpISub {v2uint} {coord} %bb_base{n}',
            f'%bb_lx{n} = OpCompositeExtract {uint} %bb_loc{n} 0',
            f'%bb_ly{n} = OpCompositeExtract {uint} %bb_loc{n} 1',
            f'%bb_row{n} = OpIMul {uint} %bb_ly{n} %bb_u16',
            f'%bb_slot{n} = OpIAdd {uint} %bb_row{n} %bb_lx{n}',
            f'%bb_ptr{n} = OpAccessChain %bb_pv4 %{slot} %bb_slot{n}',
            f'OpStore %bb_ptr{n} {texel}',
        ]
    # Every invocation reaches the single return (proven above): barrier, then each thread
    # writes four slots of each array, 64 apart, as contiguous rows of the 16x16 block.
    flush = [f'OpControlBarrier %bb_u2 %bb_u2 %bb_sem',
             f'%bb_li = OpLoad {uint} %bb_lidx',
             f'%bb_fwg = OpLoad {v3uint} %bb_wgid',
             f'%bb_fwgxy = OpVectorShuffle {v2uint} %bb_fwg %bb_fwg 0 1',
             f'%bb_fbase = OpIMul {v2uint} %bb_fwgxy %bb_v16']
    for k, step in enumerate([None, '%bb_u64', '%bb_u128', '%bb_u192']):
        slot = '%bb_li' if step is None else f'%bb_i{k}'
        if step:
            flush.append(f'%bb_i{k} = OpIAdd {uint} %bb_li {step}')
        flush += [f'%bb_x{k} = OpBitwiseAnd {uint} {slot} %bb_u15',
                  f'%bb_y{k} = OpShiftRightLogical {uint} {slot} %bb_u4',
                  f'%bb_p{k} = OpCompositeConstruct {v2uint} %bb_x{k} %bb_y{k}',
                  f'%bb_q{k} = OpIAdd {v2uint} %bb_fbase %bb_p{k}']
        for _, s in SLOTS:
            image_type, variable = images[s]
            flush += [f'%bb_a{k}_{s} = OpAccessChain %bb_pv4 %{s} {slot}',
                      f'%bb_v{k}_{s} = OpLoad {v4float} %bb_a{k}_{s}',
                      f'%bb_im{k}_{s} = OpLoad {image_type} {variable}',
                      f'OpImageWrite %bb_im{k}_{s} %bb_q{k} %bb_v{k}_{s}']
    inserts = {}

    def add(index, new):
        inserts.setdefault(index, []).extend(new)

    add(m.last_of('OpName', 'OpMemberName') + 1, names)
    add(m.last_of('OpDecorate', 'OpMemberDecorate') + 1, decorations)
    add(m.where('OpFunction')[0], globals_)
    add(ret, flush)
    return patch(m.lines, inserts, replacements), set(writes) | {entry}


def rewrite_spd(module):
    """Fence SPD's last-workgroup handoff. Returns (text, removed line indices)."""
    m = module
    atomics = m.where('OpAtomicIAdd')
    every_atomic = [l for l in m.lines
                    if (op := LINE.match(l)) and op.group(2).startswith('OpAtomic')]
    if len(atomics) != 1 or len(every_atomic) != 1 or m.count('OpMemoryBarrier') or \
            m.count('OpFunction') != 1:
        raise ValueError('Unexpected SPD shape (atomics, fences or functions)')
    atomic = atomics[0]
    args = m.operands(atomic)  # result type, pointer, scope, semantics, value
    if m.constant(args[2]) != SCOPE_DEVICE or m.constant(args[3]) != 0:
        raise ValueError('SPD counter increment is not a relaxed device-scope atomic')
    pointer = m.defs.get(args[1][1:])
    if not pointer or pointer[0] != 'OpImageTexelPointer' or \
            pointer[1][1] != m.variable_named('rw_spd_global_atomic'):
        raise ValueError('SPD atomic is not the global workgroup counter')
    tile_image = m.variable_named('rw_autoexp_mip_5')
    tile_stores = [i for i in m.where('OpImageWrite')
                   if m.defs[m.operands(i)[0][1:]][1][1] == tile_image]
    if len(tile_stores) != 1 or tile_stores[0] > atomic:
        raise ValueError('Expected one tile store before the counter increment')
    # The release fence goes beside the increment, so it orders the tile store only if the
    # same invocation made it (with another storing invocation the fix would be a no-op).
    if not m.only_invocation_zero(tile_stores[0]) or not m.only_invocation_zero(atomic):
        raise ValueError('Tile store and counter increment are not both invocation 0 only')
    barriers = [i for i in m.where('OpControlBarrier') if i > atomic]
    if not barriers:
        raise ValueError('No workgroup barrier after the counter increment')
    barrier = barriers[0]
    execution, scope, semantics = m.operands(barrier)
    if m.constant(execution) != SCOPE_WORKGROUP or m.constant(scope) != SCOPE_WORKGROUP or \
            m.constant(semantics) != ACQ_REL | WORKGROUP_MEMORY:
        raise ValueError('Unexpected barrier after the counter increment')
    uint = m.type_id('OpTypeInt', '32', '0')
    # The tile and the counter are images; storage buffers join the fences only if present.
    buffers = any(re.search(r'OpVariable %\w+ StorageBuffer$|OpDecorate %\w+ BufferBlock$', l)
                  for l in m.lines)
    fence = ACQ_REL | IMAGE_MEMORY | (UNIFORM_MEMORY if buffers else 0)
    globals_ = [f'%bb_dev = OpConstant {uint} {SCOPE_DEVICE}',
                f'%bb_fence = OpConstant {uint} {fence}',
                f'%bb_cbsem = OpConstant {uint} {fence | WORKGROUP_MEMORY}']
    inserts = {m.where('OpFunction')[0]: globals_,
               # Release: this thread's tile store is visible before its increment counts.
               atomic: ['OpMemoryBarrier %bb_dev %bb_fence'],
               # Acquire: what the final increment saw is visible to this thread ...
               atomic + 1: ['OpMemoryBarrier %bb_dev %bb_fence']}
    # ... and, through a device-scope barrier, to the whole last workgroup.
    replacements = {barrier: [f'OpControlBarrier {execution} %bb_dev %bb_cbsem']}
    return patch(m.lines, inserts, replacements), {barrier}


def normalized(text):
    return [' '.join(l.split()) for l in text.splitlines() if l.strip() and not
            l.lstrip().startswith(';')]


def check_preserved(original, removed, rebuilt, allowed):
    """The rebuilt module holds every original instruction (minus `removed`) in order, and
    adds only `allowed` opcodes."""
    keep = [l for i, l in enumerate(original.lines)
            if l.strip() and not l.lstrip().startswith(';') and i not in removed]
    keep = [' '.join(l.split()) for l in keep]
    out = normalized(rebuilt)
    j, added = 0, []
    for line in keep:
        while j < len(out) and out[j] != line:
            added.append(out[j])
            j += 1
        if j == len(out):
            raise ValueError(f'Original instruction lost: {line}')
        j += 1
    added += out[j:]
    bad = sorted({LINE.match(l).group(2) for l in added} - allowed)
    if bad:
        raise ValueError(f'Rewrite added unexpected instructions: {bad}')
    return len(keep), len(added)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def up_to_date(assets, output, posts):
    manifest = output / 'post-lds.json'
    try:
        data = json.loads(manifest.read_text())
        entries = data['files'] + [data['spd']]
        return (data['recipe'] == RECIPE and data['rewrite_sha256'] == digest(Path(__file__))
                and {e['name'] for e in data['files']} == {p.name for p in posts} and
                all(e['source_sha256'] == digest(assets / e['name']) and
                    e['fixed_sha256'] == digest(output / e['name']) for e in entries))
    except (OSError, KeyError, TypeError, ValueError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--assets', type=Path,
                        default=Path(__file__).parent / 'local/dependencies/fsr4_shaders')
    parser.add_argument('--output', type=Path, default=None,
                        help='default: <assets>/opt (beside the guarded pass-11 shaders)')
    parser.add_argument('--sdk', type=Path,
                        default=Path(os.environ.get('VULKAN_SDK', 'C:/VulkanSDK/1.4.357.0')))
    parser.add_argument('--force', action='store_true', help='rebuild an up-to-date set')
    args = parser.parse_args()
    assets = args.assets.resolve()
    output = (args.output or assets / 'opt').resolve()
    originals = sorted(assets.glob('fsr4_model_v07_i8_*_post.spv'))
    if len(originals) != 12:
        raise ValueError(f'Expected 4 profiles x 3 tiers; found {len(originals)}')
    if not (assets / SPD).is_file():
        raise FileNotFoundError(f'Missing {SPD}')
    if not args.force and up_to_date(assets, output, originals):
        print(f'FSR4_POST_LDS_PASS: up to date ({RECIPE})')
        return
    suffix = '.exe' if os.name == 'nt' else ''
    tools = {name: args.sdk / 'Bin' / (name + suffix)
             for name in ['spirv-cross', 'spirv-dis', 'spirv-as', 'spirv-val']}
    for name, path in list(tools.items()):
        if not path.is_file():
            found = shutil.which(name)
            if not found:
                raise FileNotFoundError(f'Missing shader tool: {name}')
            tools[name] = Path(found)

    def run(*command):
        done = subprocess.run([str(c) for c in command], capture_output=True, text=True)
        if done.returncode:  # say why (spirv-val's message), not just which command failed
            raise ValueError(f'{Path(str(command[0])).stem} failed on '
                             f'{Path(str(command[-1])).name}: {(done.stderr or done.stdout).strip()}')
        return done.stdout

    output.mkdir(parents=True, exist_ok=True)
    # A manifest that no longer matches its files must never be packaged.
    (output / 'post-lds.json').unlink(missing_ok=True)
    manifest = []
    with tempfile.TemporaryDirectory(prefix='vr-fsr4-post-') as tmp:
        tmp = Path(tmp)

        def build(original, rewrite, allowed):
            module = Module(run(tools['spirv-dis'], '--raw-id', original))
            text, removed = rewrite(module)
            (tmp / 'fixed.spvasm').write_text(text)
            run(tools['spirv-as'], '--preserve-numeric-ids', '--target-env',
                'spv' + module.version, tmp / 'fixed.spvasm', '-o', tmp / 'fixed.spv')
            # Keep the original generator word (DXC's): drivers may key compiler workarounds
            # on it, and the rewrite must compile like the original, not like spirv-as output.
            fixed = bytearray((tmp / 'fixed.spv').read_bytes())
            fixed[8:12] = original.read_bytes()[8:12]
            (tmp / 'fixed.spv').write_bytes(bytes(fixed))
            run(tools['spirv-val'], '--target-env', 'vulkan1.3', tmp / 'fixed.spv')
            kept, added = check_preserved(module, removed,
                                          run(tools['spirv-dis'], '--raw-id', tmp / 'fixed.spv'),
                                          allowed)
            destination = output / original.name
            shutil.copyfile(tmp / 'fixed.spv', destination)
            print(f'Validated {original.name}: {kept} original instructions kept, '
                  f'{added} added', flush=True)
            return {'name': original.name, 'source_sha256': digest(original),
                    'fixed_sha256': digest(destination), 'kept': kept, 'added': added}

        for original in originals:
            run(tools['spirv-cross'], original, '--vulkan-semantics', '--entry', 'main',
                '--output', tmp / 'in.comp')
            pixel = prove_block_loop((tmp / 'in.comp').read_text())
            manifest.append(build(original, lambda m: rewrite_post(m, pixel), POST_ADDED))
        spd = build(assets / SPD, rewrite_spd, SPD_ADDED)
    (output / 'post-lds.json').write_text(json.dumps(
        {'recipe': RECIPE, 'rewrite_sha256': digest(Path(__file__)), 'files': manifest,
         'spd': spd}, indent=2) + '\n')
    print(f'FSR4_POST_LDS_PASS: {len(manifest)} rewritten post passes and a fenced SPD; '
          'originals preserved')


if __name__ == '__main__':
    main()
