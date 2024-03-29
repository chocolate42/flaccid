#include "fixed.h"

#include <stdlib.h>

int fixed_main(input *in, output *out, flac_settings *set){
	clock_t cstart;
	queue q;
	stats stat={0};

	simple_enc *a;
	mode_boilerplate_init(set, &cstart, &q, &stat);

	_if((set->blocks_count!=1), "Fixed blocking strategy cannot use multiple block sizes");
	_if((set->tweak), "Fixed blocking strategy cannot tweak");
	_if((set->merge), "Fixed blocking strategy cannot merge");
	_if((set->diff_comp_settings), "Fixed blocking strategy cannot have different comp settings");

	a=calloc(1, sizeof(simple_enc));
	set->diff_comp_settings=1;//fake analysis to multithread in queue
	while(in->input_read(in, set->blocks[0])){
		a->sample_cnt=in->sample_cnt<set->blocks[0]?in->sample_cnt:set->blocks[0];//fake analysis to multithread in queue
		a->curr_sample=in->loc_analysis;//fake analysis to multithread in queue
		a=simple_enc_out(&q, a, set, in, &stat, out);
	}
	mode_boilerplate_finish(set, &cstart, &q, &stat, in, out);
	simple_enc_dealloc(a);
	return 0;
}
