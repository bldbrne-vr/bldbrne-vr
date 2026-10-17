# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture and review real held-item frames from a running VR_GRIP_PREVIEW session.

python scripts/grip_gallery.py capture --run PATH [--goods 1100,1300]
python scripts/grip_gallery.py gallery --run PATH [--crop left,top,right,bottom]
The session uses a private MockXR build with MOCKXR_GRIP_PREVIEW_FILE (pose 0..3).
No input action uses or equips an item. Raw left/right eye images are retained.
"""
import argparse, html, json, re, time
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
POSES=['front','palm','back','side']
POSE_LABELS=['Neutral wrist','Wrist roll +90°','Wrist roll −90°','Wrist yaw −60°']
GRIPS=['none','Neck','Handle','Ball','Wide','Flask','Pinch','Vial','Bottle','Cup','Tray','Cradle','Book','Hook pinch','Rub pinch']

def atomic(path,text):
    pending=path.with_name(path.name+'.pending');pending.write_text(text,encoding='utf-8');pending.replace(path)

def read_log(run):
    p=run/'stdout.txt'
    return p.read_text(encoding='utf-8',errors='replace') if p.exists() else ''

def catalog():
    source=(ROOT/'src/core/vr/quick_items.h').read_text()
    block=re.search(r'QuickUsableGoods\{([^}]+)\}',source)[1]
    goods=list(map(int,re.findall(r'\d+',block)))
    # Start with the reported issue and the new meshes; still capture every catalog entry.
    first=[1100,703,701,702,706,1500,1501,1510,120,205,225,1300,1320,1310,1000,700]
    return first+[g for g in goods if g not in first]

def metadata(run,goods):
    pattern=rf'VR grip preview: goods={goods} name=(.*?) grip=(\d+) model=(-?\d+) scale=(\S+) hold=(\S+) pitch=(\S+) yaw=(\S+) roll=(\S+) lift=(\S+) out=(\S+) side=(\S+)'
    rows=list(re.finditer(pattern,read_log(run)))
    if rows:
        v=rows[-1].groups(); row={'goods':goods,'name':v[0] or str(goods),'grip':GRIPS[int(v[1])],'model':int(v[2])}
        row.update(zip(['scale','hold','pitch','yaw','roll','lift','out','side'],map(float,v[3:])))
    else:row={'goods':goods,'name':{1000:'Blood Vial',700:'Tiny Music Box'}.get(goods,f'Goods {goods}'),'grip':'Vial' if goods==1000 else 'Special' if goods==700 else 'Unknown'}
    log=read_log(run)
    matches=re.findall(rf'VR quick: held {goods} = ([^\r\n]+)',log)
    row['render_path']=matches[-1] if matches else 'Special rendering path' if goods in (700,1000) else 'Not logged'
    row['icon_card']='its card' in row['render_path']
    reuse={100:'Parchment with Hunter rune',1400:'Parchment with Hunter rune',130:'Notebook with crimson cover',131:'Notebook with linen cover',200:'Resonant bell with patina',2050:'Resonant bell with silver-grey material',704:'Coldblood mesh',705:'Coldblood mesh',1110:'Rough pellet primitive',1330:'Cloth ash container',2000:'Phantasm Shell approximation',2110:'Skull approximation',4320:'Phantasm Shell reshaded as cord',4321:'Phantasm Shell reshaded as cord',4322:'Phantasm Shell reshaded as cord',4323:'Phantasm Shell reshaded as cord'}
    if goods in reuse and not row['icon_card']:row['reuse']=reuse[goods]
    return row

def capture(run,selected,settle):
    from PIL import Image
    manifest_path=run/'gallery.json'
    manifest=json.loads(manifest_path.read_text()) if manifest_path.exists() else {'source':'Actual game frames; see run.json for renderer and build provenance','poses':POSE_LABELS,'items':[]}
    for goods in selected:
        # Preserve user tuning lines; change only the displayed goods selector.
        tune=(run/'tune.txt').read_text()
        tune=re.sub(r'^preview\.goods=.*$',f'preview.goods={goods}',tune,flags=re.M)
        atomic(run/'tune.txt',tune)
        started=time.monotonic()
        if goods not in (1000,700):
            while f'VR grip preview: goods={goods} ' not in read_log(run):
                if time.monotonic()-started>30:raise RuntimeError(f'No production draw acknowledgement for {goods}')
                time.sleep(.1)
        row=metadata(run,goods);row['captures']=[]
        for index,pose in enumerate(POSES):
            atomic(run/'pose.txt',str(index)+'\n');time.sleep(settle)
            name=f'goods_{goods}_{pose}'
            for eye in ('L','R'):
                if (run/f'{name}_{eye}.png').exists():raise FileExistsError(f'{name} already captured; use a new run or select uncaptured goods')
            atomic(run/'dump_now',name+'\n'); deadline=time.monotonic()+30
            while True:
                try:
                    for eye in ('L','R'):
                        with Image.open(run/f'{name}_{eye}.png') as im:im.verify()
                    break
                except (FileNotFoundError,OSError,SyntaxError):
                    if time.monotonic()>deadline:raise RuntimeError(f'Incomplete stereo capture: {name}')
                    time.sleep(.1)
            row['captures'].append({'pose':pose,'left':f'{name}_L.png','right':f'{name}_R.png'})
        row.update(metadata(run,goods))
        manifest['items']=[v for v in manifest['items'] if v['goods']!=goods]+[row]
        atomic(manifest_path,json.dumps(manifest,indent=2,ensure_ascii=False))
        print(f"Captured {goods}: {row['name']} ({len(manifest['items'])}/73)",flush=True)

def gallery(run,crop):
    from PIL import Image,ImageDraw,ImageFont
    data=json.loads((run/'gallery.json').read_text()); thumbs=run/'thumbs';thumbs.mkdir(exist_ok=True)
    # Let Pillow locate a portable font; its built-in font needs no system files.
    try:
        font=ImageFont.truetype('DejaVuSans.ttf',22)
        small=ImageFont.truetype('DejaVuSans.ttf',17)
    except OSError:
        font=small=ImageFont.load_default()
    ordered=sorted(data['items'],key=lambda row:catalog().index(row['goods']))
    data['crop']=crop
    cards=[]
    provenance=json.loads((run/'run.json').read_text()) if (run/'run.json').exists() else {}
    notes_path=ROOT/'scratchpad/grip-gallery/review-notes.json'
    prior_notes=json.loads(notes_path.read_text()) if notes_path.exists() and run.name!='baseline' else {}
    renderer=html.escape(provenance.get('renderer','Not recorded'))
    for row in ordered:
        cells=[]
        for cap,label in zip(row['captures'],POSE_LABELS):
            with Image.open(run/cap['left']) as im:
                w,h=im.size
                box=tuple(round(x*s) for x,s in zip(crop,[w,h,w,h])); pic=im.convert('RGB').crop(box)
                pic.thumbnail((560,560));pic.save(thumbs/f"{row['goods']}_{cap['pose']}.jpg",quality=94)
            cells.append(f'<figure><a href="{cap["left"]}" target="_blank"><img loading="lazy" src="thumbs/{row["goods"]}_{cap["pose"]}.jpg"></a><figcaption>{label} · <a href="{cap["right"]}" target="_blank">right eye</a></figcaption></figure>')
        info=f"{row['grip']} · {'Icon card' if row['icon_card'] else row['render_path']}"
        settings=' · '.join(f'{k} {row[k]:g}' for k in ('pitch','yaw','roll','lift','out','side') if k in row)
        review=('<p class="hint"><strong>Your previous note:</strong> '+html.escape(prior_notes.get(str(row['goods']),''))+'</p>') if str(row['goods']) in prior_notes else ''
        cards.append(f'<article id="goods-{row["goods"]}" data-search="{html.escape(row["name"].lower())} {row["goods"]}" data-card="{int(row["icon_card"])}"><header><h2>{html.escape(row["name"])} <span>#{row["goods"]}</span></h2><p>{html.escape(info)}</p></header><div class="poses">{"".join(cells)}</div><details><summary>Placement values</summary>{html.escape(settings)}</details>{review}<textarea data-id="{row["goods"]}" placeholder="Review notes — e.g. lies vertically; rotate flat onto palm"></textarea></article>')
    css='''*{box-sizing:border-box}body{margin:0;background:#151818;color:#e6e5de;font:16px Segoe UI,sans-serif}main{max-width:1700px;margin:auto;padding:28px}h1{font-size:34px;margin:0 0 10px}p{color:#b7bfbb;margin:8px 0}nav{position:sticky;top:0;background:#151818ed;padding:14px 0;z-index:2;display:flex;gap:16px;align-items:center}input,select,button,textarea{background:#242a28;color:#eee;border:1px solid #46514b;border-radius:5px;padding:10px;font:inherit}input{width:360px}article{margin:25px 0 38px;padding:18px;background:#202624;border:1px solid #39453f;border-radius:9px}h2{margin:0;font-size:23px}h2 span{font-weight:400;font-size:16px;color:#c4a980}.poses{display:grid;grid-template-columns:repeat(4,1fr);gap:10px;margin-top:16px}figure{margin:0}img{width:100%;background:#101412;border-radius:4px}figcaption{font-size:13px;color:#c1c8c4;padding:8px 0}a{color:#dfc08b}textarea{width:100%;height:62px;margin-top:12px;resize:vertical}details{color:#9cab9e;font-size:13px;margin:8px 0}.hint{max-width:1050px;line-height:1.6}@media(max-width:850px){.poses{grid-template-columns:repeat(2,1fr)}}'''
    js='''const key='bloodborne-grip-review-v2:'+location.pathname;let notes=JSON.parse(localStorage.getItem(key)||'{}');document.querySelectorAll('textarea').forEach(t=>{t.value=notes[t.dataset.id]||'';t.oninput=()=>{notes[t.dataset.id]=t.value;localStorage.setItem(key,JSON.stringify(notes))}});function filter(){const q=document.querySelector('#search').value.toLowerCase();const m=document.querySelector('#kind').value;document.querySelectorAll('article').forEach(a=>a.hidden=!a.dataset.search.includes(q)||(m==='models'&&a.dataset.card==='1')||(m==='cards'&&a.dataset.card==='0'))}document.querySelector('#search').oninput=filter;document.querySelector('#kind').onchange=filter;document.querySelector('#export').onclick=()=>{let a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(notes,null,2)],{type:'application/json'}));a.download='grip-review-notes.json';a.click();URL.revokeObjectURL(a.href)};'''
    body=f'''<!doctype html><html lang="en"><meta charset="utf-8"><title>Bloodborne VR — Grip Review</title><style>{css}</style><main><h1>Bloodborne VR · Grip Review</h1><p class="hint">{len(ordered)} usable-item entries · Actual game renders with scripted controllers. Renderer: {renderer}. Four wrist orientations expose rotation, floating gaps and finger clipping. Click an image for the full left eye; each right eye is also available. Items sharing a mesh are included separately. Weapons are outside this usable-item catalog.</p><p class="hint">These are cropped game screenshots, not concept art. Notes are saved separately for this review; export them to share corrections. <a href="gallery.json">Capture manifest</a></p><nav><input id="search" placeholder="Find an item or goods ID"><select id="kind"><option value="all">All items</option><option value="models">3D / special paths</option><option value="cards">Icon cards</option></select><button id="export">Export review notes</button></nav>{''.join(cards)}</main><script>{js}</script></html>'''
    (run/'index.html').write_text(body,encoding='utf-8')
    # Paginated comparison sheets, four wrist views per item. Crops are uniform across all items.
    for page,start in enumerate(range(0,len(ordered),5),1):
        rows=ordered[start:start+5]; canvas=Image.new('RGB',(1600,90+len(rows)*415),(21,24,24));draw=ImageDraw.Draw(canvas)
        draw.text((24,14),f'BLOODBORNE VR | CURRENT GRIPS | {page}',font=font,fill='#e5decf')
        for col,label in enumerate(POSE_LABELS):draw.text((col*400+15,52),label,font=small,fill='#bdbfab')
        for r,row in enumerate(rows):
            y=90+r*415;draw.text((16,y),f"{row['name']}  #{row['goods']}  |  {row['grip']}",font=font,fill='#e1c28f')
            for col,cap in enumerate(row['captures']):
                im=Image.open(thumbs/f"{row['goods']}_{cap['pose']}.jpg");im.thumbnail((395,365));canvas.paste(im,(col*400+(400-im.width)//2,y+36))
        canvas.save(run/f'contact-sheet-{page:02}.jpg',quality=94)
    atomic(run/'gallery-view.json',json.dumps({'crop':crop,'item_count':len(ordered)},indent=2))
    print('Gallery:',run/'index.html',flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('command',choices=['capture','gallery']);parser.add_argument('--run',type=Path,required=True);parser.add_argument('--goods');parser.add_argument('--settle',type=float,default=1.6);parser.add_argument('--crop',default='0.12,0.18,0.67,0.73');args=parser.parse_args()
    if args.command=='capture':
        selected=list(map(int,args.goods.split(','))) if args.goods else catalog()
        if len(set(selected))!=len(selected) or any(g not in catalog() for g in selected):parser.error('Goods must be distinct entries in QuickUsableGoods')
        if args.settle < 1.0:parser.error('Allow at least one second for tuning reload, pose and grip settling')
        capture(args.run,selected,args.settle)
    else:gallery(args.run,list(map(float,args.crop.split(','))))
