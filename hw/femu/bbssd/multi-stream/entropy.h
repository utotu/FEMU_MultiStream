/* entropy lib*/
#include <math.h>
#include <time.h>
#include <stdlib.h>

/*
 * 2^N * N mapping table
 * N = 1, 2^N * N = 2 * 1 = 2
 * N = 2, 2^N * N = 4 * 2 = 8
 * N = 3, 2^N * N = 8 * 3 = 24
 * ------
 * N = 12, 2^N * N = 4096 * 12
 * */
static inline int makehist(unsigned char *S,int *hist,int len){
        int wherechar[256];
        int i,histlen;
        histlen=0;
        for(i=0;i<256;i++)wherechar[i]=-1;
        for(i=0;i<len;i++){
                if(wherechar[(int)S[i]]==-1){
                        wherechar[(int)S[i]]=histlen;
                        histlen++;
                }
                hist[wherechar[(int)S[i]]]++;
        }
        return histlen;
}
 
static inline double entropy(int *hist,int histlen,int len){
        int i;
        double H;
        H=0;
        for(i=0;i<histlen;i++){
                H-=(double)hist[i]/len*log2((double)hist[i]/len);
        }
        return H;
}

static inline int xlogx(unsigned val)
{   
    if (val == 0)
            return 0;

    int hb_pos = __builtin_clz (val) ^ 31;
    int base = (1 << hb_pos) * hb_pos;;
    int remain = ~(1 << hb_pos) & val;
    int delta = remain * (hb_pos + 2);
    return (int)(base + delta);
}

/*
 * optimize the entropy calculate
 * to save cost and ease hardware implementation
 */
static inline double entropy4k_opt(int *hist,int histlen,int len){
        int i;
        double H, r, sum_xlogx = 0;
        H = (double)xlogx(len) / len;
        r = 1.0/len;
        for(i=0;i<histlen;i++)
                sum_xlogx += xlogx(hist[i]);

        return H - r*sum_xlogx;
}


double calculate_entropy(char* S, int len);

double calculate_entropy4k_opt(char* S, int len);
