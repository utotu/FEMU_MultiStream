#include <stdint.h>
#include "stream.h"
#include "entropy.h"


double calc_compress_ratio(void *mbe, uint64_t lpn)
{
    char *addr = (char *)mbe + (lpn << 12); // page_size == 4KB
    return calculate_entropy4k_opt(addr, 4096);
}

uint64_t get_stream_id(double compress_ratio, uint64_t lpn)
{
    return 0;
}
