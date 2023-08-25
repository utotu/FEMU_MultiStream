#include <stdint.h>
#include <stdio.h>
#include "stream.h"
#include "entropy.h"
#include "kmeans.h"

extern unsigned char KmeansIsInitialized;
extern uint64_t TotalNumberOfPages;

double calc_compress_ratio(void *mbe, uint64_t lpn)
{
    char *addr = (char *)mbe + (lpn << 12); // page_size == 4KB
    return calculate_entropy4k_opt(addr, 4096);
}

uint32_t get_stream_id(double compress_ratio, uint64_t lpn)
{
    if (KmeansIsInitialized == 0) {
        KmeansNormalizer n = {8, TotalNumberOfPages};
        printf("TotalNumberOfPages = %lu\n", TotalNumberOfPages);
        KmeansInit(n);

        KmeansIsInitialized = 1;
    }

    KmeansFeautre feature = {compress_ratio, (double)lpn};
    return GetClusters(feature);
}
