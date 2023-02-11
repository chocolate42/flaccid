#include "gset.h"

#include <assert.h>
#include <stdlib.h>

int gset_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	clock_t cstart;
	MD5_CTX ctx;
	queue q;
	stats stat={0};
	uint64_t curr_sample=0;

	double besteff, *curreff;
	simple_enc **genc;
	size_t best, i;

	mode_boilerplate_init(set, &cstart, &ctx, &q, &stat, input_size);

	genc=malloc(sizeof(simple_enc*)*set->blocks_count);
	for(i=0;i<set->blocks_count;++i)
		genc[i]=calloc(1, sizeof(simple_enc));
	curreff=malloc(sizeof(double)*set->blocks_count);

	for(curr_sample=0;curr_sample<stat.tot_samples;){
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<set->blocks_count;++i){//encode all in set
			if(curr_sample+set->blocks[i]<=stat.tot_samples){//if they don't overflow the input
				simple_enc_analyse(genc[i], set, input, set->blocks[i], curr_sample, &stat, NULL);
				curreff[i]=genc[i]->outbuf_size;
				curreff[i]/=set->blocks[i];
			}
			else
				curreff[i]=9999.0;
		}
		#pragma omp barrier
		//find the most efficient next block
		besteff=9998.0;
		best=set->blocks_count;
		for(i=0;i<set->blocks_count;++i){
			if(curreff[i]<besteff){
				besteff=curreff[i];
				best=i;
			}
		}
		if(best==set->blocks_count){//partial end frame
				simple_enc_analyse(genc[0], set, input, stat.tot_samples-curr_sample, curr_sample, &stat, &ctx);
				genc[0]=simple_enc_out(&q, genc[0], set, input, &curr_sample, &stat, fout);
		}
		else{
			if(set->md5)
				MD5_UpdateSamples(&ctx, input, curr_sample, set->blocks[best], set);
			genc[best]=simple_enc_out(&q, genc[best], set, input, &curr_sample, &stat, fout);
		}
	}

	mode_boilerplate_finish(set, &cstart, &ctx, &q, &stat, input, fout);

	for(i=0;i<set->blocks_count;++i)
		simple_enc_dealloc(genc[i]);
	free(genc);
	free(curreff);
	return 0;
}
