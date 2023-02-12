#include "fixed.h"

#include <stdlib.h>

int fixed_main(void *input, size_t input_size, output *out, flac_settings *set){
	clock_t cstart;
	MD5_CTX ctx;
	queue q;
	stats stat={0};
	uint64_t curr_sample=0;

	simple_enc *a;

	mode_boilerplate_init(set, &cstart, &ctx, &q, &stat, input_size);

	if(set->blocks_count!=1)
		goodbye("Error: Fixed blocking strategy cannot use multiple block sizes\n");
	if(set->tweak)
		goodbye("Error: Fixed blocking strategy cannot tweak\n");
	if(set->merge)
		goodbye("Error: Fixed blocking strategy cannot merge\n");
	if(set->diff_comp_settings)
		goodbye("Error: Fixed blocking strategy cannot have different comp settings\n");

	a=calloc(1, sizeof(simple_enc));
	while(curr_sample<stat.tot_samples){
		simple_enc_analyse(a, set, input, ((curr_sample+set->blocksize_min)<=stat.tot_samples?set->blocksize_min:stat.tot_samples-curr_sample), curr_sample, &stat, &ctx);
		a=simple_enc_out(&q, a, set, input, &curr_sample, &stat, out);
	}

	mode_boilerplate_finish(set, &cstart, &ctx, &q, &stat, input, out);

	simple_enc_dealloc(a);
	return 0;
}
