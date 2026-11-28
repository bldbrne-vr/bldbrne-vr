"""Build bbport's pass-11 bounds fix on Windows without Perl. Never edits original assets.
Sources: upstream/bbport/tools/fsr4_pass11_guard.pl and Fsr4SpirvCrossFixes.pm.
Keep the decoder's indexed weights in one table across the guarded shader rebuild.
"""
from pathlib import Path
from contextlib import ExitStack
import argparse, hashlib, json, os, re, shutil, subprocess, uuid

RECIPE = 'bbport-pass11-guard-v2-single-weight-table'

def rewrite(source):
    bounds=re.search(r"lessThan\(uvec3\(_\d+\), uvec3\((\d+)u, (\d+)u, 32u\)\)",source)
    if not bounds: raise ValueError("No known input bounds check; refusing an unknown shader")
    if re.search(r"\bbarrier\(|\bshared\b",source): raise ValueError("Early return unsafe around barriers/shared memory")
    # glslang materializes a const array in Function storage at every dynamic index.
    # A global with the same initializer has one Private table. The original GLSL
    # declaration was const, so its existing uses can only read from it.
    source, tables = re.subn(r"^const (?=uint _\d+\[512\] = uint\[\]\()", "", source, flags=re.M)
    if tables != 1: raise ValueError("Expected one decoder weight table")
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


def verify_weight_table(assembly):
    # Check the emitted binary too. A compiler change that reintroduces a local
    # copy at each index would pass ordinary SPIR-V validation.
    def result_id(operation):
        ids = re.findall(r"^\s*(%\d+) = " + operation + r"$", assembly, re.M)
        if len(ids) != 1:
            raise ValueError(f"Unexpected pass-11 layout for {operation}")
        return ids[0]

    uint = result_id("OpTypeInt 32 0")
    length = result_id(f"OpConstant {uint} 512")
    table = result_id(f"OpTypeArray {uint} {length}")
    pointer = result_id(f"OpTypePointer Private {table}")
    result_id(f"OpVariable {pointer} Private(?: %\\d+)?")
    if re.search(rf"OpTypePointer Function {table}$", assembly, re.M):
        raise ValueError("Decoder weights were materialized in function storage")


def current_outputs(originals, output, recipe_hash):
    try:
        record = json.loads((output / 'pass11-guard.json').read_text())
        entries = record['files']
        if record['recipe'] != RECIPE or record['rewrite_sha256'] != recipe_hash or len(entries) != len(originals):
            return False
        by_name = {entry['name']: entry for entry in entries}
        return all(by_name[path.name]['source_sha256'] == digest(path) and
                   by_name[path.name]['fixed_sha256'] == digest(output / path.name)
                   for path in originals)
    except (OSError, KeyError, TypeError, ValueError):
        return False


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets',type=Path,default=Path(__file__).parent/'local/dependencies/fsr4_shaders')
    parser.add_argument('--sdk',type=Path,default=Path(os.environ.get('VULKAN_SDK','C:/VulkanSDK/1.4.357.0')))
    args=parser.parse_args();assets=args.assets.resolve();output=assets/'opt'
    originals=sorted(assets.glob('fsr4_model_v07_i8_*_pass11.spv'))
    if not originals:
        raise SystemExit(f'FSR 4 assets missing in {assets}. Run '
                         'tools/upscalers/fetch_temporal_dependencies.py --provider fsr4 first.')
    if len(originals)!=12: raise ValueError(f'Expected 4 profiles x 3 tiers; found {len(originals)}')
    recipe_hash = digest(Path(__file__))
    if current_outputs(originals, output, recipe_hash):
        print('FSR4_GUARD_PASS: all 12 guarded shaders are current')
        return
    suffix='.exe' if os.name=='nt' else ''
    tools={name:args.sdk/'Bin'/(name+suffix) for name in ['spirv-cross','glslangValidator','spirv-val','spirv-dis']}
    for name,path in list(tools.items()):
        if not path.is_file():
            found=shutil.which(name)
            if not found: raise FileNotFoundError(f'Missing shader tool: {name}')
            tools[name]=Path(found)
    output.mkdir(exist_ok=True);manifest=[]
    # Finish validating the whole set before touching a previously usable build.
    # Staging beside the outputs also makes each file replacement atomic.
    with ExitStack() as cleanup:
        # A Windows TemporaryDirectory has an owner-only ACL. Renamed shaders
        # keep that ACL, so a player account could no longer read them.
        tmp = output / ('.pass11-' + uuid.uuid4().hex)
        tmp.mkdir()
        cleanup.callback(shutil.rmtree, tmp)
        for original in originals:
            subprocess.run([str(tools['spirv-cross']),str(original),'--vulkan-semantics','--entry','fsr4_model_v07_i8_pass11','--output',str(tmp/'in.comp')],check=True,capture_output=True)
            source,bounds,unpacks=rewrite((tmp/'in.comp').read_text())
            (tmp/'out.comp').write_text(source)
            subprocess.run([str(tools['glslangValidator']),'-V','--target-env','vulkan1.3','-S','comp','-e','fsr4_model_v07_i8_pass11','--source-entrypoint','main',str(tmp/'out.comp'),'-o',str(tmp/'fixed.spv')],check=True,capture_output=True)
            subprocess.run([str(tools['spirv-val']),'--target-env','vulkan1.3',str(tmp/'fixed.spv')],check=True,capture_output=True)
            disassembled = subprocess.run([str(tools['spirv-dis']), '--raw-id', str(tmp/'fixed.spv')],
                                          check=True, capture_output=True, text=True)
            verify_weight_table(disassembled.stdout)
            destination=tmp/original.name
            (tmp/'fixed.spv').replace(destination)
            manifest.append({'name':original.name,'source_sha256':digest(original),'fixed_sha256':digest(destination),'bounds':bounds,'signed_unpacks':unpacks})
            print('Validated '+original.name,flush=True)
        record = tmp/'pass11-guard.json'
        record.write_text(json.dumps({'recipe':RECIPE,'rewrite_sha256':recipe_hash,'files':manifest},indent=2)+'\n')
        for original in originals:
            (tmp/original.name).replace(output/original.name)
        record.replace(output/record.name)
    print(f'FSR4_GUARD_PASS: {len(manifest)} fixed shaders; originals preserved')

if __name__=='__main__': main()
