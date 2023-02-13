#include "gasc.h"

#include <assert.h>
#include <stdlib.h>

int gasc_main(input *in, output *out, flac_settings *set){
	clock_t cstart;
	queue q;
	stats stat={0};

	simple_enc *a, *ab, *b, *swap;

	mode_boilerplate_init(set, &cstart, &q, &stat);

	if(set->blocks_count!=1)
		goodbye("Error: gasc cannot use multiple block sizes\n");
	if(2*set->blocks[0]>set->blocksize_limit_upper)
		goodbye("Error: gasc needs an upper blocksize limit at least twice that of the blocksize used\n");

	a =calloc(1, sizeof(simple_enc));
	b =calloc(1, sizeof(simple_enc));
	ab=calloc(1, sizeof(simple_enc));

	in->input_read(in, set->blocksize_limit_upper);
	if(!simple_enc_eof(&q, &a, set, in, 2*set->blocks[0], &stat, out)){//if not eof, init
		simple_enc_analyse(a , set, in, set->blocks[0], 0, &stat);
		simple_enc_analyse(b , set, in, set->blocks[0], set->blocks[0], &stat);
		simple_enc_analyse(ab, set, in, 2*set->blocks[0], 0, &stat);
	}

	while(in->sample_cnt){
		if((a->outbuf_size+b->outbuf_size)<ab->outbuf_size){//dump a naturally
			a=simple_enc_out(&q, a, set, in, &stat, out);
			in->input_read(in, set->blocksize_limit_upper);
			if(!simple_enc_eof(&q, &a, set, in, 2*set->blocks[0], &stat, out)){//if next !eof, iterate
				swap=a;
				a=b;
				b=swap;
				simple_enc_analyse(b, set, in, set->blocks[0], in->loc_analysis+set->blocks[0], &stat);
				simple_enc_analyse(ab, set, in, 2*set->blocks[0], in->loc_analysis, &stat);
			}
		}
		else if(ab->sample_cnt+set->blocks[0]>set->blocksize_limit_upper){//dump ab as hit upper limit
			ab=simple_enc_out(&q, ab, set, in, &stat, out);
			in->input_read(in, set->blocksize_limit_upper);
			if(!simple_enc_eof(&q, &a, set, in, 2*set->blocks[0], &stat, out)){//if next !eof, iterate
				simple_enc_analyse(a, set, in, set->blocks[0], in->loc_analysis, &stat);
				simple_enc_analyse(b, set, in, set->blocks[0], in->loc_analysis+set->blocks[0], &stat);
				simple_enc_analyse(ab, set, in, 2*set->blocks[0], in->loc_analysis, &stat);
			}
		}
		else if((ab->sample_cnt+set->blocks[0]>in->sample_cnt) && !simple_enc_eof(&q, &a, set, in, in->sample_cnt+1, &stat, out))//hit eof in middle of frame
				goodbye("Error: Failed to finalise in-progress frame\n");
		else{//iterate
			swap=a;
			a=ab;
			ab=swap;
			simple_enc_analyse(b, set, in, set->blocks[0], in->loc_analysis+a->sample_cnt, &stat);
			simple_enc_analyse(ab, set, in, a->sample_cnt+set->blocks[0], in->loc_analysis, &stat);
		}
	}
	mode_boilerplate_finish(set, &cstart, &q, &stat, in, out);
	simple_enc_dealloc(a);
	simple_enc_dealloc(b);
	simple_enc_dealloc(ab);
	return 0;
}
