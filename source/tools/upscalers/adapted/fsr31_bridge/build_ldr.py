"""Build the pinned FSR3.1.5 Vulkan permutation for our post-composite LDR input."""
from pathlib import Path
import re,subprocess,hashlib,json,sys
root=Path(__file__).resolve().parents[4]
src=root/'build-dibr-check/fsr31-source'
out=root/'build-dibr-check/fsr31-ldr';out.mkdir(exist_ok=True)
sdk=src/'upstream/ffx-2.3.0/Kits/FidelityFX'
bin=Path('C:/VulkanSDK/1.4.357.0/Bin')
recipe=(src/'tools/generate_fsr3_3_1_5_spirv.sh').read_text()
section=recipe.split('common_args=(')[1].split('\n)')[0]
defines=re.findall(r'-D[^\s]+',section)
defines=[d.replace('HDR_COLOR_INPUT=1','HDR_COLOR_INPUT=0') for d in defines]
base=[str(bin/'dxc.exe'),'-spirv','-fspv-target-env=vulkan1.2','-T','cs_6_2','-E','CS','-fvk-t-shift','0','all','-fvk-s-shift','1000','all',*defines,'-I',str(sdk/'api/internal/gpu'),'-I',str(sdk/'upscalers/fsr3/include/gpu')]
uavs=dict(re.findall(r'\[(\w+)\]=(\d+)',recipe.split('declare -A uav_shifts=(')[1].split('\n)')[0]))
cbvs=dict(re.findall(r'\[(\w+)\]=(\d+)',recipe.split('declare -A cbv_shifts=(')[1].split('\n)')[0]))
manifest={'upstream_commit':'c64f093404125e960813f95f1996c14780f0dd69','dxc_sha256':hashlib.sha256((bin/'dxc.exe').read_bytes()).hexdigest(),'defines':defines,'modules':{}}
for name in [*uavs,'accumulate_sharpen']:
 p='accumulate' if name=='accumulate_sharpen' else name
 source=sdk/f'upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_{p}_pass.hlsl'
 dest=out/f'fsr3_3_1_5_{name}.spv'
 cmd=[*base,'-fvk-u-shift',uavs[p],'all','-fvk-b-shift',cbvs[p],'all',f'-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={int(name=="accumulate_sharpen")}',str(source),'-Fo',str(dest)]
 subprocess.run(cmd,check=True)
 subprocess.run([str(bin/'spirv-val.exe'),'--target-env','vulkan1.2',str(dest)],check=True)
 manifest['modules'][dest.name]=hashlib.sha256(dest.read_bytes()).hexdigest()
subprocess.run([sys.executable,str(src/'tools/package_fsr3_3_1_5_spirv.py'),str(out),str(out/'ffx_vk_fsr3_3_1_5_embedded_spirv.h')],check=True)
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Built and validated 11 LDR modules')
