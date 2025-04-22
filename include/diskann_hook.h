#pragma once

#include "windows_customizations.h"
#include <cstring>

namespace diskann
{

using FVEC_L2SQR_HOOK = float (*)(const float *, const float *, size_t);

using FVEC_INNER_PRODUCT_HOOK = float (*)(const float *, const float *, size_t);

DISKANN_DLLEXPORT void set_fvec_L2sqr_hook(FVEC_L2SQR_HOOK hook);
DISKANN_DLLEXPORT FVEC_L2SQR_HOOK get_fvec_L2sqr_hook();

DISKANN_DLLEXPORT void set_fvec_inner_product_hook(FVEC_INNER_PRODUCT_HOOK hook);
DISKANN_DLLEXPORT FVEC_INNER_PRODUCT_HOOK get_fvec_inner_product_hook();

} // namespace diskann
