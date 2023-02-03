#include "fixed.h"

#include <assert.h>
#include <stdlib.h>
#include <time.h>

int fixed_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	FLAC__StaticEncoder *partial;
	void *partial_out;
	size_t partial_outsize;
	int *outstate;
	MD5_CTX ctx;
	uint64_t curr_sample=0, tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	stats stat={0};
	clock_t cstart;
	simple_enc *a;
	queue q;

	if(set->blocksize_min!=set->blocksize_max)
		goodbye("Error: Fixed blocking strategy cannot use multiple block sizes\n");
	if(set->tweak)
		goodbye("Error: Fixed blocking strategy cannot tweak\n");
	if(set->merge)
		goodbye("Error: Fixed blocking strategy cannot merge\n");
	if(set->diff_comp_settings)
		goodbye("Error: Fixed blocking strategy cannot have different comp settings\n");

	MD5_Init(&ctx);
	cstart=clock();

	/*Instead of reimplementing a multithreaded output queue here, hack simple_enc to do it.
	By faking analysis frames we can add to output queue and rely on existing code*/
	set->diff_comp_settings=set->diff_comp_settings?1:2;
	a=calloc(1, sizeof(simple_enc));
	queue_alloc(&q, set);
	outstate=calloc(set->work_count, sizeof(int));
	while((curr_sample+set->blocksize_min)<=tot_samples){
		a->sample_cnt=set->blocksize_min;
		a->curr_sample=curr_sample;
		if(set->bps==16)
			MD5_Update(&ctx, input+(curr_sample*2*set->channels), a->sample_cnt*2*set->channels);
		a=simple_enc_out(&q, a, set, input, &curr_sample, &stat, fout, outstate);
	}
	queue_dealloc(&q, set, input, &stat, fout, outstate);
	simple_enc_dealloc(a);
	free(outstate);
	set->diff_comp_settings=set->diff_comp_settings==2?0:1;//reverse hack just in case

	if(curr_sample!=tot_samples){/*partial*/
		/*Can't add to queue because simple_enc isn't set up for fixed partial last frame
		to be encoded properly (fixed encodes frame_number=sample/blocksize, simple_enc
		doesn't know blocksize of the stream)*/
		partial=init_static_encoder(set, set->blocksize_min, set->comp_output, set->apod_output);
		set->encode_func(partial,
			input+(curr_sample*set->channels*(set->bps==16?2:4)),
			tot_samples-curr_sample,
			curr_sample,
			&partial_out,
			&partial_outsize
		);
		if(set->bps==16)
			MD5_Update(&ctx, input+(curr_sample*2*set->channels), (tot_samples-curr_sample)*2*set->channels);
		fwrite(partial_out, 1, partial_outsize, fout);
	}

	stat.effort_output/=tot_samples;
	if(set->bps==16)
		MD5_Final(set->hash, &ctx);
	stat.cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	print_settings(set);
	print_stats(&stat);
	return 0;
}
