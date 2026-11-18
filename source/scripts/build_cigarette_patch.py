"""Stage a native cigarette item from the user's own Bloodborne 1.09 files.

Optional since 2026-11-04: the emulator builds the same row and texts in memory at boot from the
files the game really loads, a -mods copy included (src/core/vr/bloodborne_kos_lights.cpp), and
gives it the game's own icon 343 (a new icon file never shows in the game's inventory).

Never modifies the source game or saves. Output contains derived game data: do not distribute it.
Dependencies: soulstruct==2.3.2, pillow, ispc_texcomp. See --help.
"""
from pathlib import Path
import argparse
import copy
import hashlib
import json
import struct
import sys
import types

GOODS = ICON = 9000
DESCRIPTION = (
    "A finely rolled tobacco, once prescribed to Gehrman by Yharnam's Surgeon General, "
    "effects unknown but believed to be positive\n\n"
    '"A packet a day keeps the lungs youthful and the heart vigorous"\n\n'
    "The Surgeon General died of a mysterious respiratory illness shortly thereafter."
)


def param_rows(data):
    if len(data) < 64 or data[44] != 0 or data[45] != 4:
        raise ValueError("Expected little-endian Bloodborne PARAM with 64-bit row offsets")
    count = struct.unpack_from('<H', data, 10)[0]
    end = 64 + count * 24
    if not 0 < count < 32768 or end > len(data):
        raise ValueError('Invalid PARAM row table')
    rows = [struct.unpack_from('<i4xqq', data, 64 + i*24) for i in range(count)]
    if len({r[0] for r in rows}) != count:
        raise ValueError('Duplicate goods IDs: refusing ambiguous mod data')
    if any(not end <= off < len(data) or name < 0 or name >= len(data)
           for _, off, name in rows):
        raise ValueError('Invalid PARAM offsets')
    return rows, end


def append_good(data, row_bytes):
    rows, end = param_rows(data)
    if GOODS in {r[0] for r in rows}:
        raise ValueError(f'Goods {GOODS} already exists; refusing to replace it')
    header = bytearray(data[:64])
    struct.pack_into('<H', header, 10, len(rows)+1)
    struct.pack_into('<I', header, 0, struct.unpack_from('<I', header, 0)[0]+24)
    struct.pack_into('<Q', header, 48, end+24)
    added_offset = len(data)+24
    pointers = [(i, off+24, name+24 if name else 0) for i, off, name in rows]
    pointers.append((GOODS, added_offset, 0))
    result = bytes(header) + b''.join(struct.pack('<i4xqq', *r) for r in sorted(pointers))
    result += data[end:] + row_bytes
    # Preserve every byte of the original rows and strings, including unknown fields.
    assert result[end+24:added_offset] == data[end:]
    param_rows(result)
    return result


def make_icon(path):
    from PIL import Image, ImageDraw, ImageFont, ImageFilter
    import random
    image = Image.new('RGBA', (400,400))
    draw = ImageDraw.Draw(image)
    draw.polygon([(101,76),(268,61),(304,87),(304,323),(135,345),(101,319)], fill='#33281e')
    draw.polygon([(101,76),(268,61),(268,312),(101,333)], fill='#d5c6a3')
    draw.polygon([(268,61),(304,87),(304,323),(268,312)], fill='#9d8465')
    draw.polygon([(101,76),(268,61),(268,151),(186,117),(101,172)], fill='#8f1720')
    draw.line([(102,110),(268,95),(302,117)], fill='#4d1919', width=2)
    draw.line([(106,80),(263,66)], fill='#d06659', width=2)
    font_path = Path('C:/Windows/Fonts/timesbd.ttf')
    font = ImageFont.truetype(str(font_path), 32) if font_path.exists() else ImageFont.load_default()
    draw.text((184,177),'Kos Lights',anchor='mt',font=font,fill='#241c16',stroke_width=0)
    small = ImageFont.truetype(str(font_path), 12) if font_path.exists() else font
    draw.text((184,256),'YHARNAM BLEND',anchor='mt',font=small,fill='#746140')
    draw.ellipse((170,224,200,246),outline='#86703f',width=2)
    draw.line((184,226,184,243),fill='#86703f',width=2)
    draw.line((174,235,194,235),fill='#86703f',width=2)
    rng = random.Random(9000)
    for _ in range(900):
        x,y=rng.randrange(108,263),rng.randrange(85,312)
        if image.getpixel((x,y))[3]:
            color=image.getpixel((x,y)); delta=rng.randrange(-15,12)
            draw.point((x,y),fill=tuple(max(0,min(255,c+delta)) for c in color[:3])+(255,))
    # A few abrasions along the packet's folded edges, baked once.
    for _ in range(28):
        y=rng.randrange(100,315)
        draw.line((103,y,106+rng.randrange(4),y-2),fill='#ad9973',width=1)
    image.save(path)
    preview=Image.new('RGBA',image.size,'#151411')
    shadow=Image.new('RGBA',image.size); shadow.putalpha(image.getchannel('A').filter(ImageFilter.GaussianBlur(12)))
    preview.alpha_composite(shadow); preview.alpha_composite(image)
    preview.convert('RGB').save(path.with_name('icon-preview.png'))
    return image


def pack_fmg(entries):
    # FMG v2: sorted single-ID ranges. A simple, bounded writer avoids changing any text.
    strings=bytearray(); offsets=[]; ranges=[]
    count=len(entries); start=40+24*count
    for index,(key,value) in enumerate(sorted(entries.items())):
        ranges.append(struct.pack('<iii4x',index,key,key))
        offsets.append(start+len(strings) if value else 0)
        if value: strings.extend(value.encode('utf-16le')+b'\0\0')
    header=struct.pack('<4B I 4B iii q 8x',0,0,2,0,start+len(strings),1,0,0,0,count,count,255,40+16*count)
    return header+b''.join(ranges)+struct.pack('<'+'Q'*count,*offsets)+strings


def icon_tpf(template, image):
    from soulstruct.dcx import decompress, compress, DCXType
    import ispc_texcomp as bc
    data, dcx_type = decompress(template)
    data=bytearray(data)
    if data[:4]!=b'TPF\0' or struct.unpack_from('<I',data,8)[0]!=1 or data[12]!=4:
        raise ValueError('Expected single PS4 icon texture')
    off,size=struct.unpack_from('<II',data,16)
    w,h=struct.unpack_from('<HH',data,28)
    if data[24] not in (102,106,107) or (w,h)!=(400,400) or struct.unpack_from('<I',data,36)[0]!=13:
        raise ValueError('Unexpected native icon format')
    from PIL import Image
    padded=Image.new('RGBA',(416,416)); padded.paste(image,(0,0))
    linear=bc.compress_blocks_bc7(bc.RGBASurface(padded.tobytes(),416,416),bc.BC7EncSettings.from_profile('alpha_slow'))
    tiled=bytearray(len(linear))
    for ty in range(13):
        for tx in range(13):
            for e in range(64):
                x=(e&1)|((e>>1)&2)|((e>>2)&4)
                y=((e>>1)&1)|((e>>2)&2)|((e>>3)&4)
                src=((ty*8+y)*104+tx*8+x)*16
                dst=((ty*13+tx)*64+e)*16
                tiled[dst:dst+16]=linear[src:src+16]
    if off+len(tiled)>len(data) or size!=len(tiled):
        raise ValueError('Unexpected icon payload size/mips')
    # The game's texture lookup uses the internal name as well as the filename.
    # Keep the equal-sized UTF-16 name in place so no container offsets change.
    name_offset = struct.unpack_from('<I', data, 40)[0]
    old_name = 'MENU_Knowledge_00012\0'.encode('utf-16le')
    new_name = f'MENU_Knowledge_{ICON:05}\0'.encode('utf-16le')
    if name_offset + len(old_name) > off or data[name_offset:name_offset+len(old_name)] != old_name:
        raise ValueError('Unexpected template texture name')
    data[name_offset:name_offset+len(new_name)] = new_name
    data[off:off+size]=tiled
    return compress(bytes(data),dcx_type)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dvdroot',type=Path)
    parser.add_argument('output',type=Path,help='New staging directory, outside the game')
    parser.add_argument('--dependencies',type=Path,help='Optional isolated Python dependency directory')
    args=parser.parse_args()
    source=args.dvdroot.resolve(); output=args.output.resolve()
    if source==output or source in output.parents or output.exists():
        parser.error('Output must be a new directory outside the source game')
    if args.dependencies: sys.path.insert(0,str(args.dependencies.resolve()))
    import soulstruct
    # Do not import unrelated event editors (some wheels omit their metadata).
    base=Path(next(iter(soulstruct.__path__)))
    for name in ('bloodborne','darksouls1ptde','darksouls1r'):
        package=types.ModuleType('soulstruct.'+name); package.__path__=[str(base/name)]
        sys.modules['soulstruct.'+name]=package
    from soulstruct.containers import Binder
    from soulstruct.base.params.param import TypedParam
    from soulstruct.bloodborne.params.paramdef.EQUIP_PARAM_GOODS_ST import EQUIP_PARAM_GOODS_ST
    from soulstruct.base.text.fmg import FMG
    param_path=Path('param/gameparam/gameparam.parambnd.dcx')
    binder=Binder.from_path(source/param_path)
    entry=next(e for e in binder.entries if e.name=='EquipParamGoods.param')
    original=entry.get_uncompressed_data()
    param=TypedParam(EQUIP_PARAM_GOODS_ST).from_bytes(original)
    if GOODS in param.rows: raise ValueError('Goods ID conflict')
    row=copy.deepcopy(param.rows[1411]) # reusable, equippable Monocular layout
    row.Name='Kos Lights'
    for key,value in dict(Reference=-1, ReferenceType=0, AnimationVariationID=0, UseAnimation=0,
                          GoodIcon=ICON, MaxHoldQuantity=1, MaxStorageCount=0, FramptSellValue=-1,
                          ConsumedOnUse=False, CanBeEquipped=True, AutomaticallyEquipped=False,
                          CanBeDropped=False, CanBeStored=False, GoodToReplace=-1,
                          VirtualWeaponID=-1).items(): setattr(row,key,value)
    packed=bytes(row.to_writer())
    patched=append_good(original,packed)
    check=TypedParam(EQUIP_PARAM_GOODS_ST).from_bytes(patched)
    assert check.rows[GOODS].GoodIcon==ICON and not check.rows[GOODS].ConsumedOnUse
    assert set(check.rows)==set(param.rows)|{GOODS}
    preserved={e.id:hashlib.sha256(e.get_uncompressed_data()).hexdigest() for e in binder.entries if e is not entry}
    entry.set_uncompressed_data(patched)
    icon_rel=Path(f'menu/menu_knowledge_{ICON:05}.tpf.dcx')
    if (source/icon_rel).exists(): raise ValueError('Icon ID conflict')
    output.mkdir(parents=True)
    binder.write(output/param_path)
    reread=Binder.from_path(output/param_path)
    assert all(hashlib.sha256(e.get_uncompressed_data()).hexdigest()==preserved[e.id]
               for e in reread.entries if e.name!='EquipParamGoods.param')
    files=[param_path]
    # All installed languages get a valid new entry; English fallback text is deliberate.
    for msg in sorted((source/'msg').glob('*/item.msgbnd.dcx')):
        texts=Binder.from_path(msg)
        for item in texts.entries:
            # Goods: names 10, the inventory list's line 20, the long description 24 (24 and 27
            # until 2026-11-04 showed "?GoodsInfo?" in the game's inventory).
            value={10:'Kos Lights',20:'An inexhaustible packet of yellow-paper cigarettes.',
                   24:DESCRIPTION}.get(item.id)
            if value is None: continue
            fmg=FMG.from_bytes(item.get_uncompressed_data())
            if GOODS in fmg.entries: raise ValueError(f'Text ID conflict in {msg}')
            fmg.entries[GOODS]=value
            packed_text=pack_fmg(fmg.entries)
            assert FMG.from_bytes(packed_text).entries==fmg.entries
            item.set_uncompressed_data(packed_text)
        relative=msg.relative_to(source); texts.write(output/relative); files.append(relative)
    image=make_icon(output/'icon.png')
    # Use only the user's native container header, replacing all of its pixel data.
    template=source/'menu/menu_knowledge_00012.tpf.dcx'
    icon=(output/icon_rel); icon.parent.mkdir(parents=True,exist_ok=True)
    icon.write_bytes(icon_tpf(template.read_bytes(),image)); files.append(icon_rel)
    manifest={str(p):{'source_sha256':hashlib.sha256((source/p).read_bytes()).hexdigest() if (source/p).exists() else None,
                     'patched_sha256':hashlib.sha256((output/p).read_bytes()).hexdigest()} for p in files}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2))
    print(f'Staged goods {GOODS}, icon {ICON}; original game and saves untouched: {output}')


if __name__=='__main__': main()
