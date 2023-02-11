#include "fixed.h"

#include <assert.h>
#include <stdlib.h>

int fixed_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	FLAC__StaticEncoder *partial;
	void *partial_out;
	size_t partial_outsize;
	MD5_CTX ctx;
	uint64_t curr_sample=0;
	stats stat={0};
	clock_t cstart;
	simple_enc *a;
	queue q;

	mode_boilerplate_init(set, &cstart, &ctx, &q, &stat, input_size);

	if(set->blocksize_min!=set->blocksize_max)
		goodbye("Error: Fixed blocking strategy cannot use multiple block sizes\n");
	if(set->tweak)
		goodbye("Error: Fixed blocking strategy cannot tweak\n");
	if(set->merge)
		goodbye("Error: Fixed blocking strategy cannot merge\n");
	if(set->diff_comp_settings)
		goodbye("Error: Fixed blocking strategy cannot have different comp settings\n");

	/*Instead of reimplementing a multithreaded output queue here, hack simple_enc to do it.
	By faking analysis frames we can add to output queue and rely on existing code*/
	set->diff_comp_settings=set->diff_comp_settings?1:2;
	a=calloc(1, sizeof(simple_enc));
	while((curr_sample+set->blocksize_min)<=stat.tot_samples){
		a->sample_cnt=set->blocksize_min;
		a->curr_sample=curr_sample;
		if(set->md5)
			MD5_UpdateSamples(&ctx, input, curr_sample, a->sample_cnt, set);
		a=simple_enc_out(&q, a, set, input, &curr_sample, &stat, fout);
	}
	queue_dealloc(&q, set, input, &stat, fout);

	if(curr_sample!=stat.tot_samples){/*partial*/
		/*Can't add to queue because simple_enc isn't set up for fixed partial last frame
		to be encoded properly (fixed encodes frame_number=sample/blocksize, simple_enc
		doesn't know blocksize of the stream)*/
		partial=init_static_encoder(set, set->blocksize_min, set->comp_output, set->apod_output);
		set->encode_func(partial,
			input+(curr_sample*set->channels*(set->bps==16?2:4)),
			stat.tot_samples-curr_sample,
			curr_sample,
			&partial_out,
			&partial_outsize
		);
		if(set->md5)
			MD5_UpdateSamples(&ctx, input, curr_sample, stat.tot_samples-curr_sample, set);
		stat.outsize+=fwrite(partial_out, 1, partial_outsize, fout);
		stat.effort_output[0]+=stat.tot_samples-curr_sample;
	}

	//cannot boilerplate finish because partial not in queue
	if(set->md5)
		MD5_Final(set->hash, &ctx);
	stat.cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	print_settings(set);
	print_stats(&stat);

	simple_enc_dealloc(a);
	set->diff_comp_settings=set->diff_comp_settings==2?0:1;//reverse hack just in case
	return 0;
}
