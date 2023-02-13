#include "fixed.h"

#include <stdlib.h>

int fixed_main(input *in, output *out, flac_settings *set){
	clock_t cstart;
	MD5_CTX ctx;
	queue q;
	stats stat={0};

	simple_enc *a;
	printf("go go gadget fixed encode\n");fflush(stdout);
	mode_boilerplate_init(set, &cstart, &ctx, &q, &stat);

	if(set->blocks_count!=1)
		goodbye("Error: Fixed blocking strategy cannot use multiple block sizes\n");
	if(set->tweak)
		goodbye("Error: Fixed blocking strategy cannot tweak\n");
	if(set->merge)
		goodbye("Error: Fixed blocking strategy cannot merge\n");
	if(set->diff_comp_settings)
		goodbye("Error: Fixed blocking strategy cannot have different comp settings\n");

	a=calloc(1, sizeof(simple_enc));
	while(in->input_read(in, set->blocks[0])){
		simple_enc_analyse(a, set, in, in->sample_cnt<set->blocks[0]?in->sample_cnt:set->blocks[0], in->loc_analysis, &stat, &ctx);		
		a=simple_enc_out(&q, a, set, in, &stat, out);
	}
	mode_boilerplate_finish(set, &cstart, &ctx, &q, &stat, in, out);
	simple_enc_dealloc(a);
	return 0;
}
