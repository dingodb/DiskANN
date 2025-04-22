#include "diskann_hook.h"

namespace diskann
{

FVEC_L2SQR_HOOK fvec_L2sqr_hook = nullptr;
FVEC_INNER_PRODUCT_HOOK fvec_inner_product_hook = nullptr;

void set_fvec_L2sqr_hook(FVEC_L2SQR_HOOK hook)
{
    fvec_L2sqr_hook = hook;
}

FVEC_L2SQR_HOOK get_fvec_L2sqr_hook()
{
    return fvec_L2sqr_hook;
}

void set_fvec_inner_product_hook(FVEC_INNER_PRODUCT_HOOK hook)
{
    fvec_inner_product_hook = hook;
}

FVEC_INNER_PRODUCT_HOOK get_fvec_inner_product_hook()
{
    return fvec_inner_product_hook;
}

} // namespace diskann
