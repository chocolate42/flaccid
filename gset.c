#include "gset.h"

#include <assert.h>
#include <stdlib.h>

int gset_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	uint64_t curr_sample, tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	MD5_CTX ctx;
	stats stat={0};
	clock_t cstart;
	simple_enc **genc;
	double besteff, *curreff;
	size_t best, iterations=0, i;
	queue q;

	mode_boilerplate_init(set, &cstart, &ctx, &q);

	genc=malloc(sizeof(simple_enc*)*set->blocks_count);
	for(i=0;i<set->blocks_count;++i)
		genc[i]=calloc(1, sizeof(simple_enc));
	curreff=malloc(sizeof(double)*set->blocks_count);

	for(curr_sample=0;curr_sample<tot_samples;){
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<set->blocks_count;++i){//encode all in set
			if(curr_sample+set->blocks[i]<=tot_samples){//if they don't overflow the input
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
				simple_enc_analyse(genc[0], set, input, tot_samples-curr_sample, curr_sample, &stat, &ctx);
				genc[0]=simple_enc_out(&q, genc[0], set, input, &curr_sample, &stat, fout);
		}
		else{
			if(set->md5)
				MD5_UpdateSamples(&ctx, input, curr_sample, set->blocks[best], set);
			genc[best]=simple_enc_out(&q, genc[best], set, input, &curr_sample, &stat, fout);
		}
		++iterations;
	}
	if(set->md5)
		MD5_Final(set->hash, &ctx);
	queue_dealloc(&q, set, input, &stat, fout);
	for(i=0;i<set->blocks_count;++i)
		simple_enc_dealloc(genc[i]);
	free(genc);
	free(curreff);

	stat.effort_anal=0;
	for(i=0;i<set->blocks_count;++i)
		stat.effort_anal+=set->blocks[i];
	stat.effort_anal*=iterations;
	stat.effort_anal/=tot_samples;

	stat.effort_output=1;
	if(!set->diff_comp_settings)
		stat.effort_anal=0;

	stat.cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	stat.time_anal=stat.cpu_time;
	print_settings(set);
	print_stats(&stat);

	return 0;
}
