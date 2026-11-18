# SPDX-FileCopyrightText: 2026 BB-VR contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
# Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
# You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
# keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
# licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
from pathlib import Path
import glob
import hashlib
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'ROM/_tools/capetable'))
import gen_cape_table as cape

OUT = ROOT / 'src/video_core/renderer_vulkan/vr_pouch_table.h'
PARTS = {'bd_m_8000', 'bd_f_8000', 'bd_m_8000_l', 'bd_f_8000_l'}

def main():
    selected, other, found = {}, set(), set()
    for path in sorted(glob.glob(str(Path(cape.PARTS) / '*.partsbnd.dcx'))):
        part = Path(path).name.split('.')[0]
        for name, data in cape.bnd4(cape.dcx(path)).items():
            if not name.lower().endswith('.flver'):
                continue
            for mesh, material, bone, sets in cape.facesets(data):
                wanted = part in PARTS and material == 'body03_cloth' and bone.lower().endswith('_chain')
                for count, raw in sets:
                    key = (count, cape.fnv1a64(raw))
                    if wanted:
                        found.add(part)
                        digest = hashlib.sha256(raw).digest()
                        if key in selected:
                            assert selected[key][0] == digest
                        selected[key] = (digest, part)
                    else:
                        other.add(key)
    assert found == PARTS, found
    assert selected and not selected.keys() & other, selected.keys() & other
    lines = ['#pragma once', '', '#include <array>', '#include "common/types.h"', '',
             'namespace Vulkan::VrPouchTable {', '',
             'struct Key {', '    u32 count;', '    u64 hash;', '};', '',
             'inline constexpr std::array<Key, %d> Keys{{' % len(selected)]
    lines += ['    {%d, 0x%016xull},' % key for key in sorted(selected)]
    lines += ['}};', '', '}', '']
    text = '\n'.join(lines)
    if '--check' in sys.argv:
        assert OUT.read_text() == text, 'Regenerate vr_pouch_table.h'
    else:
        OUT.write_text(text, encoding='utf-8', newline='\n')
    print('%d chain keys; all four Foreign Garb top variants; no collisions with other parts' % len(selected))

if __name__ == '__main__':
    main()
