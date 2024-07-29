#include <stdint.h>
#include "./ftl.h"
#include "./queue.h"
#define GIGAUNIT 32L
#define TIME_WINDOW GIGAUNIT*4
#define GB_REQ 262144

typedef struct group_info {
	uint32_t *gsize;
	uint32_t *heapsize;

        double *tmp_vr;
        double *tmp_erase;
        double *cur_vr;
}G_VAL;

typedef struct err_info {
	bool errcheck;
	bool collect;
	
	uint32_t errcheck_time;

	uint32_t err_start;
	uint32_t err_window;
	double *vr;
	double *erase;
} ERR;

typedef struct stats {
	uint32_t cur_req;
	uint32_t write_gb;
	uint32_t write;
	uint32_t copy;
	uint32_t erase;

	uint32_t tmp_write;
	uint32_t tmp_copy;

	double tmp_waf;
	
	ERR *e;
	G_VAL *g;
}STAT;

typedef struct naive_mida {
        bool naive_on;
        uint32_t naive_start;
	queue* naive_q;
}naive;

typedef struct MiDAS_system {
	uint32_t *config; //size of each group. if (gsize[i] > config[i]), need to gc group i
	double *vr;
	double WAF;
	bool status;
	uint32_t errcheck_time;
	uint32_t time_window;
}midas;

typedef struct page_map_body{
	// uint32_t *mapping;
	bool isfull;
	uint32_t assign_page;
	uint32_t gcur;

	// uint32_t *ginfo; //segment's group number info
	// queue** group; 
	uint32_t gnum; //# of groups
	naive *n;
	midas *m;

	/*segment is a kind of Physical Block*/
	// line **reserve; //for gc
	// line **active; //for gc

	// queue* active_q; //unused active seg by merge group
}pm_body;

void page_map_create(struct ssd *ssd);
void naive_mida_on(struct ssd *ssd);
void naive_mida_off(struct ssd *ssd);
void stat_init(struct ssd *ssd);
void stat_clear(struct ssd *ssd);
void errstat_clear(struct ssd *ssd);
// void print_stat(struct ssd *ssd);

int change_group_number(struct ssd *ssd, int prevnum, int newnum);
int merge_group(int group_num, struct ssd *ssd);
int decrease_group_size(struct ssd *ssd, int gnum, int block_num);

int check_applying_config(double calc_waf);
int check_modeling(struct ssd *ssd);
// int err_check(struct ssd *ssd);
int do_modeling(struct ssd *ssd);
