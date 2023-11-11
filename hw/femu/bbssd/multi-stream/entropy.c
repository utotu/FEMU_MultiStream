/* entropy lib*/
#include "entropy.h"

double calculate_entropy(char* S, int len){
	int *hist,histlen;
	double H;
	hist=(int*)calloc(len,sizeof(int));
	histlen=makehist((unsigned char *)S,hist,len);

	H=entropy(hist,histlen,len);
    free(hist);
	return H;
}

double calculate_entropy4k_opt(char* S, int len){
	int *hist,histlen;
	double H;
	hist=(int*)calloc(len,sizeof(int));
	histlen=makehist((unsigned char *)S,hist,len);

	H=entropy4k_opt(hist,histlen,len);
    free(hist);

	return H;
}
