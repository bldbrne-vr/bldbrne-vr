# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import subprocess, array, hashlib, json
root=Path(__file__).resolve().parents[1]
sources=root/'Build/reload-sources'; dest=root/'src/resources/vr'
def clip(name,start,end):
 return array.array('h',subprocess.check_output(['ffmpeg','-v','error','-i',str(sources/name),'-af',f'atrim=start={start}:end={end},asetpts=PTS-STARTPTS,highpass=f=100,lowpass=f=9000,afade=t=in:d=0.002,afade=t=out:st={end-start-.015}:d=0.015','-ac','1','-ar','24000','-f','s16le','-']))
a=clip('clipload2.wav',0,.205); b=clip('clipload1.wav',0,.25); c=clip('equipmentclicks.wav',1.03,1.25)
# Three closely spaced mechanical gestures: slide, latch, closing click.
reload=array.array('h',[0])*int(.56*24000)
for samples,offset,gain in [(a,0,.98),(b,.18,.98),(c,.38,1.05)]: # x1.4 (2026-10-13)
 for i,v in enumerate(samples):
  k=int(offset*24000)+i
  if k<len(reload): reload[k]=max(-32700,min(32700,reload[k]+int(v*gain)))
empty=array.array('h',(int(v*1.26) for v in c)) # x1.4 (2026-10-13)
for name,pcm in [('gun_reload',reload),('gun_empty',empty)]:
 (dest/(name+'.pcm')).write_bytes(pcm.tobytes())
 subprocess.run(['ffmpeg','-v','error','-y','-f','s16le','-ar','24000','-ac','1','-i',str(dest/(name+'.pcm')),str(sources/(name+'.wav'))],check=True)
 print(name,len(pcm)/24000,'seconds',hashlib.sha256(pcm.tobytes()).hexdigest())
