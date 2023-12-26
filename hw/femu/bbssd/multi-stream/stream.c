#include <stdint.h>
#include <stdio.h>
#include "stream.h"
#include "entropy.h"
#include "kmeans.h"

extern uint64_t TotalNumberOfPages;
KmeansCtx_t ctx1, ctx2;

void multistream_mapper_init(u32 n_soft_clusters, u32 n_hard_clusters)
{
    KmeansNormalizer n1 = {8, TotalNumberOfPages}, n2 = {1.8 * 1000 * 1000 * 1000 * 1000, TotalNumberOfPages};
    KmeansInit(&ctx1, n_soft_clusters, 6 /* batch_size */, n1);
    KmeansInit(&ctx2, n_hard_clusters, 6 /* batch_size */, n2);
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

uint32_t get_soft_sid(double compress_ratio)
{
    KmeansFeautre feature = {compress_ratio, 0.};
    uint32_t soft_sid = GetClusters(&ctx1, feature);
    return soft_sid;
}

uint32_t get_hard_sid(uint64_t lifetime)
{
    KmeansFeautre feature = {(double)lifetime, 0.};
    uint32_t hard_sid = GetClusters(&ctx2, feature);
    return hard_sid;
}
