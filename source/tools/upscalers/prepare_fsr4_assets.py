"""Build bbport's pass-11 bounds fix on Windows without Perl. Never edits original assets.
Sources: upstream/bbport/tools/fsr4_pass11_guard.pl and Fsr4SpirvCrossFixes.pm.
Only the correctness fix is ported here, not the optional post-pass performance rewrite.
"""
from pathlib import Path
import argparse, hashlib, json, os, re, shutil, subprocess, tempfile


def rewrite(source):
    bounds=re.search(r"lessThan\(uvec3\(_\d+\), uvec3\((\d+)u, (\d+)u, 32u\)\)",source)
    if not bounds: raise ValueError("No known input bounds check; refusing an unknown shader")
    if re.search(r"\bbarrier\(|\bshared\b",source): raise ValueError("Early return unsafe around barriers/shared memory")
    source,count=re.subn(r"\bunpack8\(","bbUnpackS8(",source)
    if not count: raise ValueError("Expected signed int8 unpacks")
    helper="i8vec4 bbUnpackS8(uint v)\n{\n    return unpack8(int(v));\n}\n\n"
    w,h=map(int,bounds.groups())
    if not (0<w<=16384 and 0<h<=16384): raise ValueError("Invalid model bounds")
    body=f"void main()\n{{\n    if (gl_GlobalInvocationID.x >= {w}u || gl_GlobalInvocationID.y >= {h}u) return;\n"
    source,n=re.subn(r"^void main\(\)\n\{\n",helper+body,source,flags=re.M)
    if n!=1: raise ValueError("Expected one main")
    return source,(w,h),count


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets',type=Path,default=Path(__file__).parent/'local/dependencies/fsr4_shaders')
    parser.add_argument('--sdk',type=Path,default=Path(os.environ.get('VULKAN_SDK','C:/VulkanSDK/1.4.357.0')))
    args=parser.parse_args();assets=args.assets.resolve();output=assets/'opt'
    suffix='.exe' if os.name=='nt' else ''
    tools={name:args.sdk/'Bin'/(name+suffix) for name in ['spirv-cross','glslangValidator','spirv-val']}
    for name,path in list(tools.items()):
        if not path.is_file():
            found=shutil.which(name)
            if not found: raise FileNotFoundError(f'Missing shader tool: {name}')
            tools[name]=Path(found)
    originals=sorted(assets.glob('fsr4_model_v07_i8_*_pass11.spv'))
    if len(originals)!=12: raise ValueError(f'Expected 4 profiles x 3 tiers; found {len(originals)}')
    output.mkdir(exist_ok=True);manifest=[]
    with tempfile.TemporaryDirectory(prefix='vr-fsr4-guard-') as tmp:
        tmp=Path(tmp)
        for original in originals:
            subprocess.run([str(tools['spirv-cross']),str(original),'--vulkan-semantics','--entry','fsr4_model_v07_i8_pass11','--output',str(tmp/'in.comp')],check=True,capture_output=True)
            source,bounds,unpacks=rewrite((tmp/'in.comp').read_text())
            (tmp/'out.comp').write_text(source)
            subprocess.run([str(tools['glslangValidator']),'-V','--target-env','vulkan1.3','-S','comp','-e','fsr4_model_v07_i8_pass11','--source-entrypoint','main',str(tmp/'out.comp'),'-o',str(tmp/'fixed.spv')],check=True,capture_output=True)
            subprocess.run([str(tools['spirv-val']),'--target-env','vulkan1.3',str(tmp/'fixed.spv')],check=True,capture_output=True)
            destination=output/original.name;shutil.copyfile(tmp/'fixed.spv',destination)
            manifest.append({'name':original.name,'source_sha256':digest(original),'fixed_sha256':digest(destination),'bounds':bounds,'signed_unpacks':unpacks})
            print('Validated '+original.name,flush=True)
    (output/'pass11-guard.json').write_text(json.dumps({'recipe':'bbport-pass11-guard-v1','rewrite_sha256':digest(Path(__file__)),'files':manifest},indent=2)+'\n')
    print(f'FSR4_GUARD_PASS: {len(manifest)} fixed shaders; originals preserved')

if __name__=='__main__': main()
