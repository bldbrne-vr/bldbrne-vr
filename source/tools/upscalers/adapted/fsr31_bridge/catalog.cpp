#include "ffx_fsr3upscaler.h"
#include "ffx_interface.h"
#include <cstring>
// The pinned bridge resolves fixed SPIR-V by pipeline name, as in its public
// fsr3_3_1_5_bridge_test.cpp. No DX12 permutation blob is used.
extern "C" FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(FfxFsr3UpscalerPass, uint32_t,
                                                              FfxShaderBlob *out) {
    if (!out)
        return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    std::memset(out, 0, sizeof(*out));
    return FFX_OK;
}
