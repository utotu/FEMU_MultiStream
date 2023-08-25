#include "kmeans.h"

/* number of clusters*/
#define MEANS (4)

/* number of points to be clustered for each round*/
#define BATCH_SIZE (6)

#define ABS_SUB(a,b)    (((a)>(b)) ? ((a)-(b)) : ((b)-(a)))

#define LIST_EACH_FEATURE(feature)  \
     for (KmeansFeature_e feature = KMEANS_CPS_RATE;feature < KMEANS_FEATURE_CNT; feature++)

static double gs_MeansInit[MEANS][KMEANS_FEATURE_CNT] = {
    {0.2, 0},
    {0.4, 0},
    {0.6, 0},
    {0.8, 0},
};

// store layout : weight 0, weight 1
static u32 gs_WeightsInit[KMEANS_FEATURE_CNT] = {
    10, 1
};

typedef struct _KmeansBatch_t {
    KmeansFeautre feature;
    u32 cluster;
}KmeansBatch_t;

typedef struct _KmeansCtx_t {
    KmeansFeautre means[MEANS];

    // for update
    KmeansNormalizer normalizer;
    KmeansFeautre weights;
    KmeansBatch_t batch[BATCH_SIZE];
    u32 weights_sum;
    u32 batchCnt;
    u32 clusterCnt[MEANS];
}KmeansCtx_t;

static volatile KmeansCtx_t gs_KmeansCtx;
unsigned char KmeansIsInitialized = 0;

void KmeansInit(KmeansNormalizer n)
{
    for (u32 i = 0; i < MEANS; i++) {
        LIST_EACH_FEATURE(feature) {
            gs_KmeansCtx.means[i][feature] = gs_MeansInit[i][feature];
        }
    }

    for (u32 i = 0; i < KMEANS_FEATURE_CNT; i++) {
        gs_KmeansCtx.weights[i] = gs_WeightsInit[i];
    }

    LIST_EACH_FEATURE(feature) {
        gs_KmeansCtx.weights_sum += gs_KmeansCtx.weights[feature];
        gs_KmeansCtx.normalizer[feature] = n[feature];
    }

}

static void UpdateMeans(void)
{
    // update means
    if (gs_KmeansCtx.batchCnt == BATCH_SIZE) {
        for (u32 i = 0; i < BATCH_SIZE; i++) {
            u32 mean_idx = gs_KmeansCtx.batch[i].cluster;
            gs_KmeansCtx.clusterCnt[mean_idx] += 1;
            double eta = 1. / gs_KmeansCtx.clusterCnt[mean_idx];

            LIST_EACH_FEATURE(feature) {
                gs_KmeansCtx.means[mean_idx][feature] = \
                    gs_KmeansCtx.means[mean_idx][feature] \
                    + eta * gs_KmeansCtx.batch[i].feature[feature] \
                    - eta * gs_KmeansCtx.means[mean_idx][feature];
            }
        }
        gs_KmeansCtx.batchCnt = 0;
    }
}

unsigned int GetClusters(KmeansFeautre kmeansFeature)
{
    double dist_min = U64_MAX;
    double diff = 0;
    u32 cluster = 0;
    kmeansFeature[KMEANS_CPS_RATE] = kmeansFeature[KMEANS_CPS_RATE] / gs_KmeansCtx.normalizer[KMEANS_CPS_RATE];
    kmeansFeature[KMEANS_LMA] = kmeansFeature[KMEANS_LMA] / gs_KmeansCtx.normalizer[KMEANS_LMA];

    // compute cluster
    for (u32 i = 0; i < MEANS; i++) {
        double dist = 0;

        LIST_EACH_FEATURE(feature) {
            if (feature == KMEANS_LMA) {
                diff = ABS_SUB(kmeansFeature[feature],gs_KmeansCtx.means[i][feature]);
            }
            else {
                diff = ABS_SUB(kmeansFeature[feature],gs_KmeansCtx.means[i][feature]);
            }
            dist += gs_KmeansCtx.weights[feature] * diff * diff / gs_KmeansCtx.weights_sum;
            // PERFLOG_APPEND(i, feature, diff, dist, dist < dist_min, "calc dist");
        }

        if ((dist < dist_min) ) {
            dist_min = dist;
            cluster = i;
        }
    }

    // record point to batch
    LIST_EACH_FEATURE(feature) {
        gs_KmeansCtx.batch[gs_KmeansCtx.batchCnt].feature[feature] = kmeansFeature[feature];
    }
    gs_KmeansCtx.batch[gs_KmeansCtx.batchCnt].cluster = cluster;
    gs_KmeansCtx.batchCnt++;

    UpdateMeans();

    return cluster;
}
