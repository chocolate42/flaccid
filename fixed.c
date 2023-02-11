#include "fixed.h"

#include <assert.h>
#include <stdlib.h>

int fixed_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	clock_t cstart;
	MD5_CTX ctx;
	queue q;
	stats stat={0};
	uint64_t curr_sample=0;
	simple_enc *a;

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
	while(curr_sample<stat.tot_samples){
		a->sample_cnt=(curr_sample+set->blocksize_min)<=stat.tot_samples?set->blocksize_min:stat.tot_samples-curr_sample;
		a->curr_sample=curr_sample;
		if(set->md5)
			MD5_UpdateSamples(&ctx, input, curr_sample, a->sample_cnt, set);
		a=simple_enc_out(&q, a, set, input, &curr_sample, &stat, fout);
	}

	mode_boilerplate_finish(set, &cstart, &ctx, &q, &stat, input, fout);

	simple_enc_dealloc(a);
	set->diff_comp_settings=set->diff_comp_settings==2?0:1;//reverse hack just in case
	return 0;
}
