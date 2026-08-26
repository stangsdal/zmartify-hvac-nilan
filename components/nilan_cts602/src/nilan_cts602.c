#include "nilan_cts602.h"

/* Kept in its own translation unit as the future transaction scheduler seam. */
static const uint8_t supported_functions[] = { 3, 4, 16 };
const uint8_t *nilan_supported_functions(size_t *count)
{
    if (count) *count = sizeof(supported_functions);
    return supported_functions;
}
