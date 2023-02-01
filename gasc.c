#include "gasc.h"

#include <assert.h>
#include <stdlib.h>
#include <time.h>

int gasc_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	int outstate=0;
	MD5_CTX ctx;
	uint64_t curr_sample=0, tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	stats stat={0};
	clock_t cstart;
	queue q;
	simple_enc *swap, *a, *b, *ab;

	assert(set->blocksize_limit_upper>=(2*set->blocksize_limit_lower));

	a =calloc(1, sizeof(simple_enc));
	b =calloc(1, sizeof(simple_enc));
	ab=calloc(1, sizeof(simple_enc));
	queue_alloc(&q, set);

	MD5_Init(&ctx);

	cstart=clock();
	if(!simple_enc_eof(&q, &a, set, input, &curr_sample, tot_samples, 2*set->blocksize_limit_lower, &stat, &ctx, fout, &outstate)){//if not eof, init
		simple_enc_analyse(a , set, input, set->blocksize_limit_lower, 0, &stat, &ctx);
		simple_enc_analyse(b , set, input, set->blocksize_limit_lower, set->blocksize_limit_lower, &stat, &ctx);
		simple_enc_analyse(ab, set, input, 2*set->blocksize_limit_lower, 0, &stat, NULL);
	}

	while(curr_sample<tot_samples){
		if((a->outbuf_size+b->outbuf_size)<ab->outbuf_size){//dump a naturally
			a=simple_enc_out(&q, a, set, input, &curr_sample, &stat, fout, &outstate);
			if(!simple_enc_eof(&q, &a, set, input, &curr_sample, tot_samples, 2*set->blocksize_limit_lower, &stat, NULL, fout, &outstate)){//if next !eof, iterate
				swap=a;
				a=b;
				b=swap;
				simple_enc_analyse(b, set, input, set->blocksize_limit_lower, curr_sample+set->blocksize_limit_lower, &stat, &ctx);
				simple_enc_analyse(ab, set, input, 2*set->blocksize_limit_lower, curr_sample, &stat, NULL);
			}
			else if(set->bps==16)//have to manually finish md5 as eof came at an awkward time
				MD5_Update(&ctx, input+(curr_sample+set->blocksize_limit_lower)*2*set->channels, (tot_samples-(curr_sample+set->blocksize_limit_lower))*2*set->channels);
		}
		else if(ab->sample_cnt+set->blocksize_limit_lower>set->blocksize_limit_upper){//dump ab as hit upper limit
			ab=simple_enc_out(&q, ab, set, input, &curr_sample, &stat, fout, &outstate);
			if(!simple_enc_eof(&q, &a, set, input, &curr_sample, tot_samples, 2*set->blocksize_limit_lower, &stat, &ctx, fout, &outstate)){//if next !eof, iterate
				simple_enc_analyse(a, set, input, set->blocksize_limit_lower, curr_sample, &stat, &ctx);
				simple_enc_analyse(b, set, input, set->blocksize_limit_lower, curr_sample+set->blocksize_limit_lower, &stat, &ctx);
				simple_enc_analyse(ab, set, input, 2*set->blocksize_limit_lower, curr_sample, &stat, NULL);
			}
		}
		else if(curr_sample+ab->sample_cnt+set->blocksize_limit_lower>tot_samples){//hit eof in middle of frame
			if(set->bps==16)
				MD5_Update(&ctx, input+(curr_sample+ab->sample_cnt)*2*set->channels, (tot_samples-(curr_sample+ab->sample_cnt))*2*set->channels);
			if(!simple_enc_eof(&q, &a, set, input, &curr_sample, tot_samples, tot_samples-curr_sample, &stat, NULL, fout, &outstate))
				goodbye("Error: Failed to finalise in-progress frame\n");
		}
		else{//iterate
			swap=a;
			a=ab;
			ab=swap;
			simple_enc_analyse(b, set, input, set->blocksize_limit_lower, curr_sample+a->sample_cnt, &stat, &ctx);
			simple_enc_analyse(ab, set, input, a->sample_cnt+set->blocksize_limit_lower, curr_sample, &stat, NULL);
		}
	}
	queue_dealloc(&q, set, input, &stat, fout, &outstate);
	free(a);
	free(b);
	free(ab);

	stat.effort_anal/=tot_samples;
	stat.effort_output/=tot_samples;
	if(set->bps==16)
		MD5_Final(set->hash, &ctx);
	stat.cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	print_settings(set);
	print_stats(&stat);
	return 0;
}
