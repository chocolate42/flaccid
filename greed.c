#include "greed.h"

#include <assert.h>
#include <omp.h>
#include <stdlib.h>
#include <time.h>

int greed_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	size_t i;
	uint64_t curr_sample, tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	greed_controller greed;
	MD5_CTX ctx;
	FLAC__StaticEncoder *oenc;
	double effort_anal, effort_output, effort_tweak=0, effort_merge=0;
	size_t effort_anal_run=0, outsize=42;
	clock_t cstart;
	double cpu_time, anal_time=-1, tweak_time=-1, merge_time=-1;
	cstart=clock();

	MD5_Init(&ctx);

	//blocks sorted descending should help occupancy of multithreading
	qsort(set->blocks, set->blocks_count, sizeof(int), comp_int_desc);

	greed.genc=malloc(set->blocks_count*sizeof(greed_encoder));
	greed.genc_count=set->blocks_count;
	for(i=0;i<set->blocks_count;++i)
		greed.genc[i].enc=init_static_encoder(set, set->blocks[i], set->comp_anal, set->apod_anal);

	for(curr_sample=0;curr_sample<tot_samples;){
		size_t skip=0;
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<greed.genc_count;++i){
			if((tot_samples-curr_sample)>=FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder)){
				set->encode_func(greed.genc[i].enc, input+(curr_sample*set->channels*(set->bps==16?2:4)), FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder), curr_sample, &(greed.genc[i].outbuf), &(greed.genc[i].outbuf_size));
				greed.genc[i].frame_efficiency=greed.genc[i].outbuf_size;
				greed.genc[i].frame_efficiency/=FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder);
			}
			else
				greed.genc[i].frame_efficiency=9999.0;
		}
		#pragma omp barrier
		for(i=0;i<greed.genc_count;++i){
			if(greed.genc[i].frame_efficiency>9998.0)
				++skip;
		}
		if(skip==greed.genc_count){//partial frame at end
			oenc=init_static_encoder(set, (tot_samples-curr_sample)<16?16:(tot_samples-curr_sample), set->comp_output, set->apod_output);
			set->encode_func(oenc, input+(curr_sample*set->channels*(set->bps==16?2:4)), tot_samples-curr_sample, curr_sample, &(greed.genc[0].outbuf), &(greed.genc[0].outbuf_size));
			MD5_Update(&ctx, ((void*)input)+curr_sample*set->channels*(set->bps/8), (tot_samples-curr_sample)*set->channels*(set->bps/8));
			outsize+=fwrite_framestat(greed.genc[0].outbuf, greed.genc[0].outbuf_size, fout, &(set->minf), &(set->maxf));
			FLAC__static_encoder_delete(oenc);
			curr_sample=tot_samples;
		}
		else{
			double smallest_val=8888.0;
			size_t smallest_index=greed.genc_count;
			for(i=0;i<greed.genc_count;++i){
				if(greed.genc[i].frame_efficiency<smallest_val){
					smallest_val=greed.genc[i].frame_efficiency;
					smallest_index=i;
				}
			}
			assert(smallest_index!=greed.genc_count);
			MD5_Update(&ctx, ((void*)input)+curr_sample*set->channels*(set->bps/8), set->blocks[smallest_index]*set->channels*(set->bps/8));
			if(set->diff_comp_settings){
				oenc=init_static_encoder(set, set->blocks[smallest_index], set->comp_output, set->apod_output);
				set->encode_func(oenc, input+(curr_sample*set->channels*(set->bps==16?2:4)), set->blocks[smallest_index], curr_sample, &(greed.genc[smallest_index].outbuf), &(greed.genc[smallest_index].outbuf_size));
				outsize+=fwrite_framestat(greed.genc[smallest_index].outbuf, greed.genc[smallest_index].outbuf_size, fout, &(set->minf), &(set->maxf));
				FLAC__static_encoder_delete(oenc);
			}
			else
				outsize+=fwrite_framestat(greed.genc[smallest_index].outbuf, greed.genc[smallest_index].outbuf_size, fout, &(set->minf), &(set->maxf));
			curr_sample+=set->blocks[smallest_index];
		}
		++effort_anal_run;
	}
	if(set->bps==16)//non-16 bit TODO
		MD5_Final(set->hash, &ctx);

	effort_anal=0;
	for(i=0;i<set->blocks_count;++i)
		effort_anal+=set->blocks[i];
	effort_anal*=effort_anal_run;
	effort_anal/=tot_samples;

	effort_output=1;
	if(!set->diff_comp_settings)
		effort_anal=0;

	qsort(set->blocks, set->blocks_count, sizeof(int), comp_int_asc);

	printf("settings\tmode(greed);lax(%u);analysis_comp(%s);analysis_apod(%s);output_comp(%s);output_apod(%s);tweak_after(%u);tweak(%u);tweak_early_exit(%u);merge_after(%u);merge(%u);"
		"blocksize_limit_lower(%u);blocksize_limit_upper(%u);analysis_blocksizes(%u", set->lax, set->comp_anal, set->apod_anal, set->comp_output, set->apod_output, set->tweak_after, set->tweak, set->tweak_early_exit, set->merge_after, set->merge, set->blocksize_limit_lower, set->blocksize_limit_upper, set->blocks[0]);
	for(i=1;i<set->blocks_count;++i)
		printf(",%u", set->blocks[i]);
	cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	printf(")\teffort\tanalysis(%.3f);tweak(%.3f);merge(%.3f);output(%.3f)", effort_anal, effort_tweak, effort_merge, effort_output);
	printf("\tsubtiming\tanalysis(%.5f);tweak(%.5f);merge(%.5f)", anal_time, tweak_time, merge_time);
	printf("\tsize\t%zu\tcpu_time\t%.5f\n", outsize, cpu_time);

	return 0;
}
