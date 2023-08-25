#pragma once

typedef unsigned int u32;
typedef unsigned long long u64;

#define U64_MAX     (0xFFFFFFFFFFFFFFFFLL)  ///< max value for uint64_t data type

typedef enum _KmeansFeature_e {
    KMEANS_CPS_RATE = 0,
    KMEANS_LMA,
    KMEANS_FEATURE_CNT,
}KmeansFeature_e;

typedef double KmeansFeautre[KMEANS_FEATURE_CNT];
typedef double KmeansNormalizer[KMEANS_FEATURE_CNT];

void KmeansInit(KmeansNormalizer n);
unsigned int GetClusters(KmeansFeautre kmeansFeature);
