#include "gasc.h"

#include <assert.h>
#include <stdlib.h>

int gasc_main(void *input, size_t input_size, output *out, flac_settings *set){
	clock_t cstart;
	MD5_CTX ctx;
	queue q;
	stats stat={0};
	uint64_t curr_sample=0;

	simple_enc *a, *ab, *b, *swap;

	mode_boilerplate_init(set, &cstart, &ctx, &q, &stat, input_size);

	if(set->blocksize_min!=set->blocksize_max)
		goodbye("Error: gasc cannot use multiple block sizes\n");

	a =calloc(1, sizeof(simple_enc));
	b =calloc(1, sizeof(simple_enc));
	ab=calloc(1, sizeof(simple_enc));

	if(!simple_enc_eof(&q, &a, set, input, &curr_sample, stat.tot_samples, 2*set->blocks[0], &stat, &ctx, out)){//if not eof, init
		simple_enc_analyse(a , set, input, set->blocks[0], 0, &stat, &ctx);
		simple_enc_analyse(b , set, input, set->blocks[0], set->blocks[0], &stat, &ctx);
		simple_enc_analyse(ab, set, input, 2*set->blocks[0], 0, &stat, NULL);
	}

	while(curr_sample<stat.tot_samples){
		if((a->outbuf_size+b->outbuf_size)<ab->outbuf_size){//dump a naturally
			a=simple_enc_out(&q, a, set, input, &curr_sample, &stat, out);
			if(!simple_enc_eof(&q, &a, set, input, &curr_sample, stat.tot_samples, 2*set->blocks[0], &stat, NULL, out)){//if next !eof, iterate
				swap=a;
				a=b;
				b=swap;
				simple_enc_analyse(b, set, input, set->blocks[0], curr_sample+set->blocks[0], &stat, &ctx);
				simple_enc_analyse(ab, set, input, 2*set->blocks[0], curr_sample, &stat, NULL);
			}
			else if(set->md5)//have to manually finish md5 as eof came at an awkward time
				MD5_UpdateSamples(&ctx, input, curr_sample-(curr_sample%set->blocks[0]), (curr_sample%set->blocks[0]), set);
		}
		else if(ab->sample_cnt+set->blocks[0]>set->blocksize_limit_upper){//dump ab as hit upper limit
			ab=simple_enc_out(&q, ab, set, input, &curr_sample, &stat, out);
			if(!simple_enc_eof(&q, &a, set, input, &curr_sample, stat.tot_samples, 2*set->blocks[0], &stat, &ctx, out)){//if next !eof, iterate
				simple_enc_analyse(a, set, input, set->blocks[0], curr_sample, &stat, &ctx);
				simple_enc_analyse(b, set, input, set->blocks[0], curr_sample+set->blocks[0], &stat, &ctx);
				simple_enc_analyse(ab, set, input, 2*set->blocks[0], curr_sample, &stat, NULL);
			}
		}
		else if(curr_sample+ab->sample_cnt+set->blocks[0]>stat.tot_samples){//hit eof in middle of frame
			if(set->md5)
				MD5_UpdateSamples(&ctx, input, curr_sample+ab->sample_cnt, (stat.tot_samples-(curr_sample+ab->sample_cnt)), set);
			if(!simple_enc_eof(&q, &a, set, input, &curr_sample, stat.tot_samples, stat.tot_samples, &stat, NULL, out))
				goodbye("Error: Failed to finalise in-progress frame\n");
		}
		else{//iterate
			swap=a;
			a=ab;
			ab=swap;
			simple_enc_analyse(b, set, input, set->blocks[0], curr_sample+a->sample_cnt, &stat, &ctx);
			simple_enc_analyse(ab, set, input, a->sample_cnt+set->blocks[0], curr_sample, &stat, NULL);
		}
	}

	mode_boilerplate_finish(set, &cstart, &ctx, &q, &stat, input, out);

	simple_enc_dealloc(a);
	simple_enc_dealloc(b);
	simple_enc_dealloc(ab);
	return 0;
}
