# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
from pathlib import Path
import argparse
import json
import math
import struct
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'ROM/_tools/capetable'))
import gen_cape_table as cape

GAME = ROOT / 'ROM/CUSA03173/dvdroot_ps4'
OUT = ROOT / 'src/core/vr/pouch_armor_fits.h'
ORIGIN = np.array([-.120, 1.0189744234 - .115, -.067])
RIGHT = np.array([-.8191520, 0, .5735764])
BACK = np.array([.5735764, 0, .8191520])
LIMITS = ((0, -.056, 1), (0, .056, -1), (1, -.115, 1), (1, .085, -1))
GAP = .010


def armor_names():
    files = cape.bnd4(cape.dcx(str(GAME / 'msg/engus/item.msgbnd.dcx')))
    b = files['\u9632\u5177\u540d.fmg']
    groups = struct.unpack_from('<i', b, 12)[0]
    strings = struct.unpack_from('<q', b, 24)[0]
    result = {}
    for g in range(groups):
        index, first, last = struct.unpack_from('<iii', b, 40 + 16 * g)
        for rid in range(first, last + 1):
            at = struct.unpack_from('<q', b, strings + 8 * (index + rid - first))[0]
            if at:
                result[rid] = cape.string(b, at, True)
    return result


def armors():
    names = armor_names()
    b = cape.bnd4(cape.dcx(str(GAME / 'param/gameparam/gameparam.parambnd.dcx')))['EquipParamProtector.param']
    result = []
    for j in range(struct.unpack_from('<H', b, 10)[0]):
        rid, at = struct.unpack_from('<I4xQ', b, 64 + j * 24)
        if (rid % 10000 == 1000 and rid in names and
                names[rid] != 'NOT NEEDED' and not names[rid].startswith('en')):
            model = struct.unpack_from('<H', b, at + 160)[0]
            result.append((rid, model, names[rid]))
    return sorted(result)


def clip(poly, axis, bound, sign):
    out = []
    for a, b in zip(poly, poly[1:] + poly[:1]):
        da, db = (a[axis] - bound) * sign, (b[axis] - bound) * sign
        if da >= 0:
            out.append(a)
        if (da >= 0) != (db >= 0):
            out.append(a + (b - a) * (da / (da - db)))
    return out


def measure(part):
    files = cape.bnd4(cape.dcx(str(Path(cape.PARTS) / (part + '.partsbnd.dcx'))))
    f = cape.Flver(next(b for n, b in files.items() if n.lower().endswith('.flver')))
    reach, source, samples = -math.inf, '', 0
    for m in range(f.meshes):
        mat, bone, sets, vbs = f.mesh(m)
        if part.startswith(('bd_m_8000', 'bd_f_8000')) and mat == 'body03_cloth':
            continue
        positions, _ = f.vertices(vbs)
        if positions is None:
            raise ValueError((part, m, 'Missing positions'))
        positions = np.array(positions) - ORIGIN
        local = np.column_stack((positions @ RIGHT, positions[:, 1], positions @ BACK))
        seen = set()
        for count, bits, raw in sets:
            if raw in seen:
                continue
            seen.add(raw)
            assert count % 3 == 0
            indices = np.frombuffer(raw, dtype='<u2' if bits == 16 else '<u4').reshape(-1, 3)
            triangles = local[indices]
            lo, hi = triangles.min(axis=1), triangles.max(axis=1)
            candidates = ((hi[:, 0] >= -.056) & (lo[:, 0] <= .056) &
                          (hi[:, 1] >= -.115) & (lo[:, 1] <= .085))
            for t in triangles[candidates]:
                poly = list(t)
                for axis, bound, sign in LIMITS:
                    if poly:
                        poly = clip(poly, axis, bound, sign)
                if poly:
                    samples += 1
                    value = -min(v[2] for v in poly)
                    if value > reach:
                        reach, source = value, '%s mesh %d %s' % (part, m, mat)
    return dict(part=part, reach_m=reach if samples else None, source=source, triangles=samples)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    profiles, cache = [], {}
    for rid, model, name in armors():
        measurements = []
        for gender in ('m', 'f'):
            for suffix in ('', '_l'):
                part = 'bd_%s_%04d%s' % (gender, model, suffix)
                path = Path(cape.PARTS) / (part + '.partsbnd.dcx')
                if not path.exists():
                    continue
                if part not in cache:
                    cache[part] = measure(part)
                measurements.append(cache[part])
        assert measurements, (rid, model, name)
        reach = max((m['reach_m'] for m in measurements if m['reach_m'] is not None), default=-GAP)
        outward = max(0, math.ceil((reach + GAP) * 1000))
        assert outward <= 200, (name, outward)
        profiles.append(dict(armor=rid, model=model, name=name, outward_mm=outward, variants=measurements))
        print('%6d %4d %3d mm %s' % (rid, model, outward, name), flush=True)
    lines = ['#pragma once', '', '#include <array>', '#include <cstdint>', '',
             'namespace VR::BodyMounts {', '', 'struct ArmorPouchFit {',
             '    std::int32_t armor;', '    float outward_m;', '};', '',
             'inline constexpr std::array<ArmorPouchFit, %d> ArmorPouchFits{{' % len(profiles)]
    lines += ['    {%d, %.3ff},' % (p['armor'], p['outward_mm'] / 1000) for p in profiles]
    lines += ['}};', '', 'constexpr float PouchOutwardM(std::int32_t armor) {',
              '    for (const auto& fit : ArmorPouchFits) {',
              '        if (fit.armor == armor) {', '            return fit.outward_m;',
              '        }', '    }', '    return 0.0f;', '}', '', '}', '']
    text = '\n'.join(lines)
    if args.check:
        assert OUT.read_text() == text, 'Regenerate pouch_armor_fits.h'
    else:
        OUT.write_text(text, encoding='utf-8', newline='\n')
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(profiles, indent=2), encoding='utf-8')
    print('%d armor profiles; %d model variants' % (len(profiles), len(cache)))


if __name__ == '__main__':
    main()
