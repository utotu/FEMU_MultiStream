#include "hot.h"
#include "model.h"
// #include "map.h"
#include "midas.h"
#include "./ftl.h"


extern STAT* midas_stat;
HF_Q* hot_q; 
HF* hotfilter;

void hf_init(struct ssd *ssd) {
	hotfilter = (HF*)malloc(sizeof(HF));
	hotfilter->max_val = 3;
	hotfilter->hot_val = 3;
	hotfilter->tw_ratio = 1.0;

	hotfilter->make_flag=1;
	hotfilter->use_flag=1;

	hotfilter->tw = (long)ssd->sp.tt_pgs;
	hotfilter->left_tw = hotfilter->tw;
	hotfilter->cold_tw = (long)ssd->sp.tt_pgs;

	hotfilter->G0_vr_sum=0.0;
	hotfilter->G0_vr_num=0.0;
	hotfilter->seg_age=0.0;
	hotfilter->seg_num=0.0;
	hotfilter->avg_seg_age=0.0;

	hotfilter->G0_traffic_ratio=0.0;
	hotfilter->tot_traffic=0.0;
	hotfilter->G0_traffic=0.0;

	hotfilter->hot_lba_num=0;

	hotfilter->err_cnt=0;
	hotfilter->tmp_err_cnt=0;

	hotfilter->cur_hf = (int*)malloc(sizeof(int)*ssd->sp.tt_pgs);
	memset(hotfilter->cur_hf, 0, sizeof(int)*ssd->sp.tt_pgs);

	hf_q_init();
}

void hf_q_init(void) {
	hot_q = (HF_Q *)malloc(sizeof(HF_Q));
	
	hot_q->queue_idx=0;

	hot_q->g0_traffic=0.0;
	hot_q->g0_size=0.0;
	hot_q->g0_valid=0.0;
	hot_q->queue_max=10;

	hot_q->extra_size=0;
	hot_q->extra_traffic=0.0;

	hot_q->calc_traffic=0.0;
	hot_q->calc_size=0;
	hot_q->calc_unit=0;

	hot_q->best_extra_size=0;
	hot_q->best_extra_traffic=0.0;
	hot_q->best_extra_unit=0.0;

	hot_q->g0_traffic_queue=(double*)malloc(sizeof(double)*hot_q->queue_max);
	hot_q->g0_size_queue=(double*)malloc(sizeof(double)*hot_q->queue_max);
	hot_q->g0_valid_queue=(double*)malloc(sizeof(double)*hot_q->queue_max);

	memset(hot_q->g0_traffic_queue, 0, sizeof(double)*hot_q->queue_max);
	memset(hot_q->g0_size_queue, 0, sizeof(double)*hot_q->queue_max);
	memset(hot_q->g0_valid_queue, 0, sizeof(double)*hot_q->queue_max);
}

void hf_q_reset(bool true_reset) {
	hot_q->g0_traffic=0;
	hot_q->g0_size=0;
	hot_q->g0_valid=0.0;
	hot_q->queue_idx=0;

	hot_q->extra_size=0;
	hot_q->extra_traffic=0.0;
	
	hot_q->best_extra_size=0;
	hot_q->best_extra_traffic=0.0;
	hot_q->best_extra_unit=0.0;
	
	hot_q->calc_traffic=0.0;
	hot_q->calc_size=0;
	hot_q->calc_unit=0;

	if (true_reset) {
		memset(hot_q->g0_traffic_queue, 0, sizeof(double)*hot_q->queue_max);
		memset(hot_q->g0_size_queue, 0, sizeof(double)*hot_q->queue_max);
		memset(hot_q->g0_valid_queue, 0, sizeof(double)*hot_q->queue_max);
	}
}

void hf_q_calculate(void) {
	hf_q_reset(false);

	double hfq_cnt=0.0;
	for (int i=0;i<hot_q->queue_max; i++) {
		if (hot_q->g0_traffic_queue[i] != 0.0) hfq_cnt++;
		hot_q->g0_traffic += hot_q->g0_traffic_queue[i];
		hot_q->g0_size += hot_q->g0_size_queue[i];
		hot_q->g0_valid += hot_q->g0_valid_queue[i];
	}
	if (hfq_cnt == 0.0) {
		printf("there is no information about hotfilter!!\n");
		abort();
	}
	hot_q->g0_traffic = hot_q->g0_traffic/hfq_cnt;
	hot_q->g0_size = hot_q->g0_size/hfq_cnt;
	hot_q->g0_valid = hot_q->g0_valid/hfq_cnt;

	hot_q->g0_size = floor(hot_q->g0_size+0.5);
	
	if ((hot_q->g0_valid > 0.2) || (hot_q->g0_traffic < 0.15)) {
		printf("hotfilter accuracy is low, so FIXED\n");
		hot_q->is_fix=true;
	}else hot_q->is_fix=false;
	
	printf("==============HOT FILTER INFO=============\n");
	printf("- avg. traffic: %.3f\n", hot_q->g0_traffic);
	printf("- avg. size   : %.3f\n", hot_q->g0_size);
	printf("- avg. valid  : %.3f\n", hot_q->g0_valid);
	printf("------------------------------------------\n");

	return;
}

void hf_destroy(void) {
	free(hotfilter->cur_hf);
	free(hotfilter);
}

void hf_metadata_reset(void) {
	hotfilter->make_flag=0;
	hotfilter->use_flag=0;
	hotfilter->G0_vr_num=0;
	hotfilter->G0_vr_sum=0;
	hotfilter->seg_age=0.0;
	hotfilter->seg_num=0.0;
	hotfilter->avg_seg_age=0.0;
	hotfilter->G0_traffic_ratio=0.0;
	hotfilter->tot_traffic=0.0;
	hotfilter->G0_traffic=0.0;
	hotfilter->hot_lba_num=0;
	hotfilter->left_tw = hotfilter->tw;
	hotfilter->err_cnt=0;
	hotfilter->tmp_err_cnt=0;
}

void hot_merge(struct ssd *ssd) {
	pm_body *p = (pm_body*)ssd->pm;
	printf("NAIVE_START is 1!!! HOT MERGE on\n");
	//0: move the whole queue to the heap
	int size = mvoe_line_q2h(ssd, 0, 0, 0);  
	if ((size+1) != midas_stat->g->gsize[0]) {
		printf("size miss: stat->gsize and real queue size is different\n");
		printf("in hot merge function\n");
		abort();
	}
	midas_stat->g->gsize[p->n->naive_start] += midas_stat->g->gsize[0];
	midas_stat->g->gsize[0]=0;
	// if (p->active[0] != NULL) {
	// 	midas_stat->g->gsize[p->n->naive_start]--;
	// 	midas_stat->g->gsize[0]=1;
	// }
	//TODO p->m->config[i]?
}



void hf_update_model(double traffic) {
	if (traffic==0.0) return;
	hot_q->g0_traffic_queue[hot_q->queue_idx] = traffic;
	hot_q->g0_size_queue[hot_q->queue_idx] = midas_stat->g->gsize[0];
	hot_q->g0_valid_queue[hot_q->queue_idx] = hotfilter->G0_vr_sum/hotfilter->G0_vr_num;
	hot_q->queue_idx++;
	if (hot_q->queue_idx == hot_q->queue_max) hot_q->queue_idx = 0;
	
	return;
}

void hf_update(struct ssd *ssd) {
	double prev_seg_age=0.0;
	double tr=0.0;
	tr = hotfilter->G0_traffic/hotfilter->tot_traffic;
	hotfilter->G0_traffic_ratio=tr;

	hf_update_model(tr);
	prev_seg_age = hotfilter->tw/hotfilter->tw_ratio;

	// double avg_g0 = 0.0;
	hotfilter->avg_seg_age=(double)hotfilter->seg_age/(double)hotfilter->seg_num;
	// avg_g0 = hotfilter->avg_seg_age/(double)ssd->sp.pgs_per_line;

	hotfilter->avg_seg_age=(hotfilter->avg_seg_age+prev_seg_age)/2.0;
	// avg_g0 = hotfilter->avg_seg_age/(double)ssd->sp.pgs_per_line;
	long tw = (long)(hotfilter->avg_seg_age*hotfilter->tw_ratio);

	hotfilter->tw=tw;
	hotfilter->left_tw=tw-1;
	hotfilter->seg_age=0.0;
	hotfilter->seg_num=0.0;
	hotfilter->G0_traffic=0.0;
	hotfilter->tot_traffic=0.0;
	hotfilter->G0_vr_sum=0.0;
	hotfilter->G0_vr_num=0.0;

	return;
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static uint64_t get_timestamp(struct ssd *ssd, uint64_t lba)
{
	struct ppa ppa = get_maptbl_ent(ssd, lba);
	struct line *p=get_line(ssd, &ppa);
	long age= ssd->cur_timestamp - p->timestamp;
	if (p->timestamp == 0) abort();
	if (age < 0) {
		printf("age of the segment is below 0: %ld\n", age);
		abort();
	}
	return age;
}

static uint32_t get_group(struct ssd *ssd, uint64_t lba)
{
	struct ppa ppa = get_maptbl_ent(ssd, lba);
	struct line *p=get_line(ssd, &ppa);
	return p->sid;
}

void hf_generate(struct ssd *ssd, uint64_t lba, int gnum, int hflag) {
	if ((hotfilter->left_tw<=0) && (hotfilter->seg_num)) hf_update(ssd);
	
	if (hotfilter->cold_tw>0) {
		//printf("[HF-NOTICE] COLD start end\n");
		hotfilter->cold_tw--;
		return;
	}
	if (hflag) {
		//update the LBA to the hotfilter
		hotfilter->left_tw--;
		if (gnum==1) {
			uint64_t seg_age=get_timestamp(ssd, lba);
			double tmp_age=hotfilter->tw/hotfilter->tw_ratio;
			if (seg_age <= tmp_age) {
				//hot lba	
				if (hotfilter->cur_hf[lba] <= hotfilter->max_val-1) {
					hotfilter->cur_hf[lba]++;
				}
			} else {
				//not hot lba
				if (hotfilter->cur_hf[lba]>0) {
					hotfilter->cur_hf[lba]--;
				}
			}
		}
	} else {
		if (hotfilter->cur_hf[lba]>0) {
			hotfilter->cur_hf[lba]--;
			uint32_t gnum = get_group(ssd, lba);
			if (gnum == 0) hotfilter->cur_hf[lba]--;
		}
	}	
	return;
}


int hf_check(uint64_t lba) {
	hotfilter->tot_traffic++;
	if (hotfilter->cur_hf[lba] >= hotfilter->hot_val) {
		hotfilter->G0_traffic++;
		return 0;
	} else return 1;
}

