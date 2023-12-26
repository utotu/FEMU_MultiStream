#include "kmeans.h"
#include <stdlib.h>

/* number of points to be clustered for each round*/
#define BATCH_SIZE (6)

#define ABS_SUB(a,b)    (((a)>(b)) ? ((a)-(b)) : ((b)-(a)))

#define LIST_EACH_FEATURE(feature)  \
     for (KmeansFeature_e feature = KMEANS_CPS_RATE;feature < KMEANS_FEATURE_CNT; feature++)

// store layout : weight 0, weight 1
static u32 gs_WeightsInit[KMEANS_FEATURE_CNT] = {
    10, 0
};

void KmeansInit(KmeansCtx_t *ctx, u32 nclusters, u32 batch_size, KmeansNormalizer n)
{
    ctx->nclusters = nclusters;
    ctx->means = (KmeansFeautre *)calloc(nclusters, sizeof(KmeansFeautre));
    ctx->clusterCnt = (u32 *)calloc(nclusters, sizeof(u32));

    ctx->batch_size = batch_size;
    ctx->batch = (KmeansBatch_t *)calloc(batch_size, sizeof(KmeansBatch_t));

    for (u32 i = 0; i < ctx->nclusters; i++) {
        //ctx->means[i][KMEANS_CPS_RATE] = 1.0 / (ctx->nclusters + 1) * (i + 1);
        ctx->means[i][KMEANS_CPS_RATE] = 1.0 / (ctx->nclusters) * i;
        ctx->means[i][KMEANS_LMA] = 0;
    }

    for (u32 i = 0; i < KMEANS_FEATURE_CNT; i++) {
        ctx->weights[i] = gs_WeightsInit[i];
    }

    LIST_EACH_FEATURE(feature) {
        ctx->weights_sum += ctx->weights[feature];
        ctx->normalizer[feature] = n[feature];
    }
}

static void UpdateMeans(KmeansCtx_t *ctx)
{
    // update means
    u32 batch_size = ctx->batch_size;
    if (ctx->batchCnt == batch_size) {
        for (u32 i = 0; i < batch_size; i++) {
            u32 mean_idx = ctx->batch[i].cluster;
            ctx->clusterCnt[mean_idx] += 1;
            double eta = 1. / ctx->clusterCnt[mean_idx];

            LIST_EACH_FEATURE(feature) {
                ctx->means[mean_idx][feature] = \
                    ctx->means[mean_idx][feature] \
                    + eta * ctx->batch[i].feature[feature] \
                    - eta * ctx->means[mean_idx][feature];
            }
        }
        ctx->batchCnt = 0;
    }
}

unsigned int GetClusters(KmeansCtx_t *ctx, KmeansFeautre kmeansFeature)
{
    double dist_min = U64_MAX;
    double diff = 0;
    u32 cluster = 0;
    kmeansFeature[KMEANS_CPS_RATE] = kmeansFeature[KMEANS_CPS_RATE] / ctx->normalizer[KMEANS_CPS_RATE];
    kmeansFeature[KMEANS_LMA] = kmeansFeature[KMEANS_LMA] / ctx->normalizer[KMEANS_LMA];

    // compute cluster
    for (u32 i = 0; i < ctx->nclusters; i++) {
        double dist = 0;

        LIST_EACH_FEATURE(feature) {
            if (feature == KMEANS_LMA) {
                diff = ABS_SUB(kmeansFeature[feature], ctx->means[i][feature]);
            }
            else {
                diff = ABS_SUB(kmeansFeature[feature], ctx->means[i][feature]);
            }
            dist += ctx->weights[feature] * diff * diff / ctx->weights_sum;
            // PERFLOG_APPEND(i, feature, diff, dist, dist < dist_min, "calc dist");
        }

        if ((dist < dist_min) ) {
            dist_min = dist;
            cluster = i;
        }
    }

    // record point to batch
    LIST_EACH_FEATURE(feature) {
        ctx->batch[ctx->batchCnt].feature[feature] = kmeansFeature[feature];
    }
    ctx->batch[ctx->batchCnt].cluster = cluster;
    ctx->batchCnt++;

    UpdateMeans(ctx);

    return cluster;
}
