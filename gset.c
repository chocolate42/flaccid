#include "gset.h"

#include <assert.h>
#include <stdlib.h>

int gset_main(input *in, output *out, flac_settings *set){
	clock_t cstart;
	queue q;
	stats stat={0};

	double besteff, *curreff;
	simple_enc **genc;
	size_t best=0, i;

	mode_boilerplate_init(set, &cstart, &q, &stat);

	genc=malloc(sizeof(simple_enc*)*set->blocks_count);
	for(i=0;i<set->blocks_count;++i)
		genc[i]=calloc(1, sizeof(simple_enc));
	curreff=malloc(sizeof(double)*set->blocks_count);

	while(in->input_read(in, set->blocks[set->blocks_count-1])>set->blocks[0]){
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<set->blocks_count;++i){//encode all in set
			if(set->blocks[i]<=in->sample_cnt){//if they don't overflow the input
				simple_enc_analyse(genc[i], set, in, set->blocks[i], in->loc_analysis, &stat);
				curreff[i]=genc[i]->outbuf_size;
				curreff[i]/=set->blocks[i];
			}
			else
				curreff[i]=9999.0;
		}
		#pragma omp barrier
		//find the most efficient next block
		besteff=9998.0;
		for(i=0;i<set->blocks_count;++i){
			if(curreff[i]<besteff){
				besteff=curreff[i];
				best=i;
			}
		}
		genc[best]=simple_enc_out(&q, genc[best], set, in, &stat, out);
	}
	simple_enc_eof(&q, genc, set, in, in->sample_cnt+1, &stat, out);//partial last frame

	mode_boilerplate_finish(set, &cstart, &q, &stat, in, out);

	for(i=0;i<set->blocks_count;++i)
		simple_enc_dealloc(genc[i]);
	free(genc);
	free(curreff);
	return 0;
}
