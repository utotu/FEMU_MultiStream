#include <stdint.h>
#include <stdio.h>
#include "stream.h"
#include "entropy.h"
#include "kmeans.h"

extern uint64_t TotalNumberOfPages;
KmeansCtx_t ctx;

void multistream_mapper_init(u32 nclusters)
{
    KmeansNormalizer n = {8, TotalNumberOfPages};
    KmeansInit(&ctx, nclusters, 6 /* batch_size */, n);
}

/*
void multistream_mapper_destroy() {
    // deallocate heap memory
    free(ctx.means);
    free(ctx.clusterCnt);
    free(ctx.batch);
}
*/

double calc_compress_ratio(void *mbe, uint64_t lpn)
{
    char *addr = (char *)mbe + (lpn << 12); // page_size == 4KB
    return calculate_entropy4k_opt(addr, 4096);
}

uint32_t get_stream_id(double compress_ratio, uint64_t lpn)
{
    KmeansFeautre feature = {compress_ratio, (double)lpn};
    return GetClusters(&ctx, feature);
}
