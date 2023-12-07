#pragma once

typedef unsigned int u32;
typedef unsigned long long u64;

#define U64_MAX     (0xFFFFFFFFFFFFFFFFLL)  ///< max value for uint64_t data type

typedef enum _KmeansFeature_e {
    KMEANS_CPS_RATE = 0,
    KMEANS_LMA,
    KMEANS_FEATURE_CNT,
} KmeansFeature_e;

typedef double KmeansFeautre[KMEANS_FEATURE_CNT];
typedef double KmeansNormalizer[KMEANS_FEATURE_CNT];

typedef struct _KmeansBatch_t {
    KmeansFeautre feature;
    u32 cluster;
} KmeansBatch_t;

typedef struct _KmeansCtx_t {
    u32 nclusters;
    KmeansFeautre *means;
    u32 *clusterCnt;

    u32 batch_size;
    KmeansBatch_t *batch;

    KmeansNormalizer normalizer;
    KmeansFeautre weights;
    u32 weights_sum;
    u32 batchCnt;
} KmeansCtx_t;

void KmeansInit(KmeansCtx_t *ctx, u32 nclusters, u32 batch_szie, KmeansNormalizer n);
unsigned int GetClusters(KmeansCtx_t *ctx, KmeansFeautre kmeansFeature);
