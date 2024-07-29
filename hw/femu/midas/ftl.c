#include "ftl.h"
#include "model.h"
#include "hot.h"
#include "midas.h"
// #include "model.h"
//#define FEMU_DEBUG_FTL

extern STAT* midas_stat;
extern HF* hotfilter;
extern HF_Q* hot_q;

uint64_t utilization = 0;
unsigned long long gpgs_per_line=0;
unsigned long long gtt_pgs=0;
static void *ftl_thread(void *arg);

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    // lm->invalid_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
    //         victim_line_get_pri, victim_line_set_pri,
    //         victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);  //for heap

    lm->victim_line_stream = g_malloc0(sizeof(QTAILQ_HEAD(, line)) * spp->MAXGNUM);
    lm->full_line_stream = g_malloc0(sizeof(QTAILQ_HEAD(, line)) * spp->MAXGNUM);
    for(int i = 0; i < spp->MAXGNUM;i++)
    {
        QTAILQ_INIT(&lm->victim_line_stream[i]);
        QTAILQ_INIT(&lm->full_line_stream[i]);
        // lm->victim_line_stream[i]=pqueue_init(spp->tt_lines, victim_line_cmp_pri,
        //     victim_line_get_pri, victim_line_set_pri,
        //     victim_line_get_pos, victim_line_set_pos);
    }

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->in_heap = 0;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        line->sid = 0;
        line->timestamp = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static int get_cur_sid(struct ssd *ssd, struct ppa ppa) {
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = &lm->lines[ppa.g.blk];
    return curline->sid;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }
    curline->timestamp = ssd->cur_timestamp;  //record line birth time
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_init_write_pointer(struct ssd *ssd, uint32_t sid)
{
    struct write_pointer *wpp = &ssd->wp[sid];
    struct line *curline = NULL;

    midas_stat->g->gsize[sid]++;

    curline = get_next_free_line(ssd);
    //  QTAILQ_FIRST(&lm->free_line_list);
    // QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    // lm->free_line_cnt--;
    curline->sid = sid;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;
}


static void ssd_advance_write_pointer(struct ssd *ssd, uint32_t sid)
{

    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp[sid];
    struct line_mgmt *lm = &ssd->lm;
    pm_body *p = (pm_body*)ssd->pm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    if(sid == p->gnum - 1)
                    {
                        wpp->curline->in_heap=1;
                        QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    }
                    else
                        QTAILQ_INSERT_TAIL(&lm->full_line_stream[sid], wpp->curline, entry);
                    ftl_assert(wpp->curline->ipc == 0);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    if(sid == p->gnum - 1)
                    {
                        wpp->curline->in_heap=1;
                        pqueue_insert(lm->victim_line_pq, wpp->curline);
                    }
                    else
                        QTAILQ_INSERT_TAIL(&lm->victim_line_stream[sid], wpp->curline, entry);
                    // pqueue_insert(lm->victim_line_stream[sid], wpp->curline);
                    // pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    printf("abort\n");
                    abort();
                }
                wpp->curline->sid = sid;
                midas_stat->g->gsize[sid]++;
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }

    // ssd->stats.total_ssd_writes++;
}


static struct ppa get_new_page(struct ssd *ssd, uint32_t sid)
{
    struct write_pointer *wpp = &ssd->wp[sid];
    if(wpp->curline == NULL)
        ssd_init_write_pointer(ssd, sid);
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

// static struct ppa get_new_page(struct ssd *ssd)
// {
//     struct write_pointer *wpp = &ssd->wp;
//     struct ppa ppa;
//     ppa.ppa = 0;
//     ppa.g.ch = wpp->ch;
//     ppa.g.lun = wpp->lun;
//     ppa.g.pg = wpp->pg;
//     ppa.g.blk = wpp->blk;
//     ppa.g.pl = wpp->pl;
//     ftl_assert(ppa.g.pl == 0);

//     return ppa;
// }

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp, FemuCtrl *n)
{
    spp->MAXGNUM = 20;
    spp->GROUPNUM = GC_GROUPNUM;
    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk;  // 256
    spp->blks_per_pl = n->bb_params.blks_per_pl;  /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun;  // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch;  // 8
    spp->nchs = n->bb_params.nchs;                // 8;

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;      // NAND_READ_LATENCY
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;      // NAND_PROG_LATENCY
    spp->blk_er_lat = n->bb_params.blk_er_lat;    // NAND_ERASE_LATENCY
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;  // 0

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;
    
    // printf("pgs_per_blk:%d, pgs_per_pl:%d, pgs_per_lun%d, pgs_per_ch:%d, tt_pgs=%d\n",spp->pgs_per_blk, spp->pgs_per_pl,spp->pgs_per_lun,spp->pgs_per_ch, spp->tt_pgs);
    // sleep(3);
    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent/100.0; // 0.75
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high/100.0; // 0.95
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;

    gpgs_per_line = spp->pgs_per_line;
	gtt_pgs = spp->tt_pgs;
    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

static void ssd_init_multistream(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    /* random seed for generating sid */
    srand(time(NULL));

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd->wp = g_malloc0(sizeof(struct write_pointer) * spp->MAXGNUM);
    for (int i = 0; i < spp->MAXGNUM; i++) {
        // ssd_init_write_pointer(ssd, i);
        ssd->wp[i].curline = NULL;
    }

    // ssd->pg_ssid_tbl = g_malloc0(sizeof(uint8_t) * spp->tt_pgs);
    // memset(ssd->pg_ssid_tbl, 0, sizeof(uint8_t) * spp->tt_pgs);
}

void midas_ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;
    ssd->cur_timestamp = 1;

    ftl_assert(ssd);

    ssd_init_params(spp, n);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    // ssd_init_write_pointer(ssd);
    ssd_init_multistream(ssd);

	stat_init(ssd);
	page_map_create(ssd);
	hf_init(ssd);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos && line->in_heap==1) {
        /* Note that line->vpc will be updated by this call */
        // pqueue_change_priority(lm->victim_line_stream[line->sid], line->vpc - 1, line);
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if(line->vpc == 0 && line->in_heap==0)
    {
        /* move line: "victim_stream" -> "victim_pq" */
        if(QTAILQ_IN_USE(line,entry))
        {
            line->in_heap=1;
            QTAILQ_REMOVE(&lm->victim_line_stream[line->sid], line, entry);
            pqueue_insert(lm->victim_line_pq, line);
        }
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        if(line->in_heap==1)
        {
            QTAILQ_REMOVE(&lm->full_line_list, line, entry);
            lm->full_line_cnt--;
            pqueue_insert(lm->victim_line_pq, line);
            lm->victim_line_cnt++;
        }
        else
        {
            QTAILQ_REMOVE(&lm->full_line_stream[line->sid], line, entry);
            lm->full_line_cnt--;
            QTAILQ_INSERT_TAIL(&lm->victim_line_stream[line->sid], line, entry);
            lm->victim_line_cnt++;
        }

    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa, uint32_t sid)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd, sid);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd, sid);
    midas_stat->copy++;
    midas_stat->tmp_copy++;
    /* GC-related statistics */
    // struct ssd_stats *stats = &ssd->stats;
    // stats->total_gc_writes++;
    // stats->streams[sid].gc_writes++;

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}
static void set_pos(void *d, size_t val)
{
    /* do nothing */
}

static void set_pri(void *d, pqueue_pri_t pri)
{
    /* do nothing */
}

int mvoe_line_q2h(struct ssd *ssd, uint32_t sid, int size, int new_group)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;
    int num = 0;
    if(size == 0) //0 for all
        size = -1;
    
    pqueue_t *q=lm->victim_line_pq;
    pqueue_t *dup;
	struct line *e;
    dup = pqueue_init(q->size, q->cmppri, q->getpri, set_pri, q->getpos, set_pos);
    dup->size = q->size;
    dup->avail = q->avail;
    dup->step = q->step;
    memcpy(dup->d, q->d, (q->size * sizeof(void *)));
    while (size && (NULL != (e = (struct line *)pqueue_pop(dup))))
    {
        if(e->sid == sid)
        {
            size--;
            num++;
            e->sid = new_group;
        }
    }
    pqueue_free(dup);

    // q=lm->invalid_line_pq;
    // dup = pqueue_init(q->size, q->cmppri, q->getpri, set_pri, q->getpos, set_pos);
    // dup->size = q->size;
    // dup->avail = q->avail;
    // dup->step = q->step;
    // memcpy(dup->d, q->d, (q->size * sizeof(void *)));
    // while (size && (NULL != (e = (struct line *)pqueue_pop(dup))))
    // {
    //     if(e->sid == sid)
    //     {
    //         size--;
    //         num++;
    //         e->sid = new_group;
    //     }
    // }
    // pqueue_free(dup);

    QTAILQ_FOREACH(victim_line, &lm->full_line_list, entry) {
        if(victim_line->sid == sid)
        {
            size--;
            num++;
            victim_line->sid = new_group;
        }
        if(size == 0)
            break;
    }

    while( size && !QTAILQ_EMPTY(&lm->victim_line_stream[sid]))
    {
        size--;
        num++;
        victim_line=QTAILQ_FIRST(&lm->victim_line_stream[sid]);
        pqueue_insert(lm->victim_line_pq, victim_line);
        victim_line->in_heap = 1;
        victim_line->sid = new_group;
        QTAILQ_REMOVE(&lm->victim_line_stream[sid], victim_line, entry);
    }
    while( size && !QTAILQ_EMPTY(&lm->full_line_stream[sid]))
    {
        size--;
        num++;
        victim_line=QTAILQ_FIRST(&lm->full_line_stream[sid]);
        QTAILQ_REMOVE(&lm->full_line_stream[sid], victim_line, entry);
        QTAILQ_INSERT_TAIL(&lm->full_line_list, victim_line, entry);
        victim_line->in_heap = 1;
        victim_line->sid = new_group;
    }
    printf("mvoe_line_q2h: %d -> heap, num:%d, size:%d, new_group:%d\n",sid, num, size, new_group);
    return num;
}

static struct line *select_victim_line(struct ssd *ssd, bool force, pqueue_t *p)
{
    struct line *victim_line = NULL;
    struct line_mgmt *lm = &ssd->lm;
    victim_line = pqueue_peek(p);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(p);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

static struct line *select_victim_line_stream(struct ssd *ssd, bool force, uint32_t sid)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    // victim_line = pqueue_peek(lm->victim_line_stream[sid]);
    victim_line = QTAILQ_FIRST(&lm->victim_line_stream[sid]);
    if (!victim_line) {
        return NULL;
    }

    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }
    QTAILQ_REMOVE(&lm->victim_line_stream[sid], victim_line, entry);
    // pqueue_pop(lm->victim_line_stream[sid]);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
static int clean_one_block(struct ssd *ssd, struct ppa *ppa, uint32_t sid, uint32_t old_sid)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);

            uint64_t lpn = get_rmap_ent(ssd, ppa); //get lpn before gc write
            if (old_sid == 0 || old_sid == 1) 
                hf_generate(ssd, lpn, old_sid, 0);
            // assert(ssd->pg_ssid_tbl[lpn]==old_sid);
            // ssd->pg_ssid_tbl[lpn] = sid;
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa, sid);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
    return cnt;
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->in_heap = 0;
    line->vpc = 0;
    line->sid = 0;
    line->timestamp = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

// update the results of GC
// e.g., valid ratio, erase count, segment age in HotFilter
static void update_gc_results(struct ssd *ssd, int group, int gc_page_num, struct line *p) {
	//update segment age when group number is 0
	if ((group == 0) && (hotfilter->make_flag==1)) {
		long seg_age = ssd->cur_timestamp - p->timestamp;
		if (p->timestamp == 0 || seg_age < 0) abort();
		//printf("\nseg age: %ld (%.3f%%)\n", seg_age, (double)seg_age/(double)_PPS/(double)L2PGAP);
		hotfilter->seg_age += seg_age;
		hotfilter->seg_num++;
		hotfilter->G0_vr_sum += (double)gc_page_num/(double)ssd->sp.pgs_per_line;
		hotfilter->G0_vr_num++;
	}
	
	midas_stat->g->gsize[group]--;
	midas_stat->erase++;
	midas_stat->g->tmp_vr[group] += (double)gc_page_num/(double)ssd->sp.pgs_per_line;
	midas_stat->g->tmp_erase[group]++;
	
	if (midas_stat->e->collect) {
		midas_stat->e->vr[group] += (double)gc_page_num/(double)ssd->sp.pgs_per_line;
		midas_stat->e->erase[group]++;
	}

	return;
}

static int do_gc(struct ssd *ssd, bool force)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
    pm_body *p=(pm_body*)ssd->pm;
    int target_group;
    int page_num = 0;
    struct line_mgmt *lm = &ssd->lm;

    int victim_group = -1;
    victim_line = select_victim_line(ssd, force, lm->victim_line_pq);
    if(victim_line!=NULL)
        victim_group = victim_line->sid;
    else
        for (int i=0;i<p->gnum;i++) {
            if (p->m->config[i] < midas_stat->g->gsize[i]) {
                victim_group = i;
                victim_line = select_victim_line_stream(ssd, force, victim_group);
                if (victim_line)
                    break;
            }
        }

    // if(victim_line==NULL)
    // {
    //     victim_line = select_victim_line(ssd, force, lm->victim_line_pq);
    //     if(!victim_line)
    //         victim_group = victim_line->sid;
    // }

    // if(force && !victim_line)
    // {
    //     for (int i=0;i<p->gnum;i++) {
    //         victim_group = i;
    //         victim_line = select_victim_line_stream(ssd, force, victim_group);
    //         if (victim_line)
    //             break;
    //     }
    // }

    // if (p->n->naive_start != 1) {
	// }
    // if (victim_group==-1) victim_group = p->n->naive_start;

    // victim_line = select_victim_line_stream(ssd, force, victim_group);
    if (!victim_line) {
        // printf("groups  no victim_line\n");
        return -1;
    }
    // 
    // if (!victim_line) {
    //     printf("heap no victim_line\n");
    //     return -1;
    // }
    // victim_group = victim_line->sid;

    if (victim_group < p->gnum-1) target_group = victim_group + 1; //goto next group if victim group is not last group
	else target_group = victim_group; // go back to GC group because the victim group is last group

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,sid=%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,victim_line->sid,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            page_num+=clean_one_block(ssd, &ppa, target_group, victim_group);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
        }
    }
    update_gc_results(ssd, victim_group, page_num, victim_line);

    /* update line status */
    mark_line_free(ssd, &ppa);

    return victim_group;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
            //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
            //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int user_group = 0;

    ssd->cur_timestamp ++;
    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }
    
    int gc_count=0;
	int gc_num = -1;
	int tmp_gnum = -1;
    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        gc_num = do_gc(ssd, true);
        if (gc_num == -1)
        {
            // struct line_mgmt *lm=&ssd->lm;
            // pm_body *p=(pm_body*)ssd->pm;
            // printf("no victime line, free line: %d, victim_line_pq size: %ld,naive_start:%d\n",ssd->lm.free_line_cnt, pqueue_size(lm->victim_line_pq),p->n->naive_start);
            // for(int i=0; i<p->gnum;i++)
            // {
            //     int full_num=0,victim_num=0;
            //     struct line *line;
            //     QTAILQ_FOREACH(line, &lm->full_line_stream[i], entry) {
            //         full_num++;
            //     }  
            //     QTAILQ_FOREACH(line, &lm->victim_line_stream[i], entry) {
            //         victim_num++;
            //     }  
            //     printf("group[%d] victim=%d, full=%d\n",i,victim_num,full_num);
            // }
            break;
        }
            
		if (tmp_gnum != gc_num) {
			gc_count=0;
			tmp_gnum = gc_num;
		}
		gc_count++;
		// if (gc_count > 100) {
		// 	printf("!!!!Infinite GC occur!!!!\n");
        //     pm_body *p=(pm_body*)ssd->pm;
		// 	if (gc_num == p->n->naive_start) {
		// 		printf("=> infinite GC in last group...\n");
		// 		if (p->n->naive_on == false) naive_mida_on(ssd);
		// 		else {
		// 			printf("already naive mida on!!! break\n");
		// 			abort();
		// 		}
		// 	} else {
		// 		if (p->n->naive_on) naive_mida_off(ssd);
		// 		printf("=> MERGE GROUP : G%d ~ G%d\n", gc_num, p->gnum-1);
		// 		for (int j=gc_num+1;j<p->gnum;j++) p->m->config[user_group] += p->m->config[j];
		// 		merge_group(gc_num, ssd);
		// 		naive_mida_on(ssd);
		// 		gc_count=0;
		// 	}
		// 	stat_clear(ssd);
		// 	errstat_clear(ssd);
		// }
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        check_time_window(ssd, lpn, M_WRITE);
        midas_stat->cur_req++;
        midas_stat->write++;
		midas_stat->tmp_write++;
        do_modeling(ssd);

        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            int sid = get_cur_sid(ssd, ppa);
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
            // int sid = ssd->pg_ssid_tbl[lpn];
            hf_generate(ssd, lpn, sid, 1);
	    } 
        else
            utilization++;
        user_group = hf_check(lpn);

        // ssd->pg_ssid_tbl[lpn] = user_group;

        /* new write */
        ppa = get_new_page(ssd, user_group);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd, user_group);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_discard(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint16_t nr = req->range_nr;
    NvmeDsmRange *range = req->range;
    int i;
    uint64_t lba;
    uint32_t len;
    uint64_t start_lpn;
    uint64_t end_lpn;
    uint64_t lpn;
    struct ppa ppa;

    for (i = 0; i < nr; i++) {
        lba = le64_to_cpu(range[i].slba);
        len = le32_to_cpu(range[i].nlb);

        start_lpn = lba / spp->secs_per_pg;
        end_lpn = (lba + len - 1) / spp->secs_per_pg;
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (mapped_ppa(&ppa)) {
                mark_page_invalid(ssd, &ppa);
                set_rmap_ent(ssd, INVALID_LPN, &ppa);
                ppa.ppa = UNMAPPED_PPA;
                set_maptbl_ent(ssd, lpn, &ppa);
                utilization--;
            }

            // uint64_t wtime = ssd->pg_wtime_tbl[lpn];
            // if (wtime != 0) {
            //     int sid = ssd->pg_ssid_tbl[lpn];

            //     if (!ssd->stats.soft_streams[sid].updates)
            //         ssd->stats.valid_lifetime_cnt++;
            //     ssd->stats.soft_streams[sid].updates++;
            //     double duration = (double)(req->stime - wtime); // 更新前的均值
            //     double old_mean = ssd->stats.soft_streams[sid].lifetime;     
            //     ssd->stats.soft_streams[sid].lifetime += (duration - ssd->stats.soft_streams[sid].lifetime) / ssd->stats.soft_streams[sid].updates;
            //     double new_mean = ssd->stats.soft_streams[sid].lifetime;  // 更新后的均值
            //     double old_variance = ssd->stats.soft_streams[sid].variance;  // 旧方差（假设已定义）
            //     ssd->stats.soft_streams[sid].variance = (((ssd->stats.soft_streams[sid].updates-1) * old_variance) + (duration - new_mean) * (duration - old_mean)) / ssd->stats.soft_streams[sid].updates;
            // }
            // ssd->pg_wtime_tbl[lpn] = 0;
        }
    }

    g_free(req->range);
    return 0;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                uint32_t dw11 =le32_to_cpu(req->cmd.cdw11);
                if (dw11 & NVME_DSMGMT_AD)
                    ssd_discard(ssd, req);
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}

