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
	simple_enc *swap, store[3]={0}, *a=store, *b=store+1, *ab=store+2;

	assert(set->blocksize_limit_upper>=(2*set->blocksize_limit_lower));

	cstart=clock();
	MD5_Init(&ctx);

	if(!simple_enc_eof(a, set, input, &curr_sample, tot_samples, 2*set->blocksize_limit_lower, &ctx, fout, &stat)){//if not eof, init
		simple_enc_aio(a , set, input, set->blocksize_limit_lower, 0, 1, &ctx, NULL, NULL);
		simple_enc_aio(b , set, input, set->blocksize_limit_lower, set->blocksize_limit_lower, 1, &ctx, NULL, NULL);
		simple_enc_encode(ab, set, input, 2*set->blocksize_limit_lower, 0, 1, &stat);
	}

	while(curr_sample<tot_samples){
		if((a->outbuf_size+b->outbuf_size)<ab->outbuf_size){//dump a naturally
			simple_enc_out(a, set, input, &curr_sample, fout, &stat, &outstate);
			if(!simple_enc_eof(a, set, input, &curr_sample, tot_samples, 2*set->blocksize_limit_lower, NULL, fout, &stat)){//if next !eof, iterate
				swap=a;
				a=b;
				b=swap;
				simple_enc_aio(b, set, input, set->blocksize_limit_lower, curr_sample+set->blocksize_limit_lower, 1, &ctx, NULL, NULL);
				simple_enc_encode(ab, set, input, 2*set->blocksize_limit_lower, curr_sample, 1, &stat);
			}
			else if(set->bps==16)//finish MD5
					MD5_Update(&ctx, input+input_size-((tot_samples%set->blocksize_limit_lower)*2*set->channels), (tot_samples%set->blocksize_limit_lower)*2*set->channels);
		}
		else if(ab->sample_cnt+set->blocksize_limit_lower>set->blocksize_limit_upper){//dump ab as hit upper limit
			simple_enc_out(ab, set, input, &curr_sample, fout, &stat, &outstate);
			if(!simple_enc_eof(a, set, input, &curr_sample, tot_samples, 2*set->blocksize_limit_lower, &ctx, fout, &stat)){//if next !eof, iterate
				simple_enc_aio(a, set, input, set->blocksize_limit_lower, curr_sample, 1, &ctx, NULL, NULL);
				simple_enc_aio(b, set, input, set->blocksize_limit_lower, curr_sample+set->blocksize_limit_lower, 1, &ctx, NULL, NULL);
				simple_enc_encode(ab, set, input, 2*set->blocksize_limit_lower, curr_sample, 1, &stat);
			}
		}
		else if(curr_sample+ab->sample_cnt+set->blocksize_limit_lower>tot_samples){//hit eof in middle of frame
			if(set->bps==16)
				MD5_Update(&ctx, input+(curr_sample+ab->sample_cnt)*2*set->channels, (tot_samples-(curr_sample+ab->sample_cnt))*2*set->channels);
			if(!simple_enc_eof(a, set, input, &curr_sample, tot_samples, tot_samples-curr_sample, NULL, fout, &stat))
				goodbye("Error: Failed to finalise in-progress frame\n");
		}
		else{//iterate
			swap=a;
			a=ab;
			ab=swap;
			simple_enc_aio(b, set, input, set->blocksize_limit_lower, curr_sample+a->sample_cnt, 1, &ctx, NULL, NULL);
			simple_enc_encode(ab, set, input, a->sample_cnt+set->blocksize_limit_lower, curr_sample, 1, &stat);
		}
	}

	stat.effort_anal/=tot_samples;
	stat.effort_output/=tot_samples;
	if(set->bps==16)
		MD5_Final(set->hash, &ctx);
	stat.cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	print_settings(set);
	print_stats(&stat);
	return 0;
}
