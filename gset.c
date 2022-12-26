#include "gset.h"

#include <assert.h>
#include <omp.h>
#include <stdlib.h>
#include <time.h>

int gset_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	size_t i;
	uint64_t curr_sample, tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	greed_controller greed;
	MD5_CTX ctx;
	FLAC__StaticEncoder *oenc;
	size_t anal_runs=0;
	clock_t cstart;
	stats stat={0};
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
			stat.outsize+=fwrite_framestat(greed.genc[0].outbuf, greed.genc[0].outbuf_size, fout, &(set->minf), &(set->maxf));
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
				stat.outsize+=fwrite_framestat(greed.genc[smallest_index].outbuf, greed.genc[smallest_index].outbuf_size, fout, &(set->minf), &(set->maxf));
				FLAC__static_encoder_delete(oenc);
			}
			else
				stat.outsize+=fwrite_framestat(greed.genc[smallest_index].outbuf, greed.genc[smallest_index].outbuf_size, fout, &(set->minf), &(set->maxf));
			curr_sample+=set->blocks[smallest_index];
		}
		++anal_runs;
	}
	if(set->bps==16)//non-16 bit TODO
		MD5_Final(set->hash, &ctx);

	stat.effort_anal=0;
	for(i=0;i<set->blocks_count;++i)
		stat.effort_anal+=set->blocks[i];
	stat.effort_anal*=anal_runs;
	stat.effort_anal/=tot_samples;

	stat.effort_output=1;
	if(!set->diff_comp_settings)
		stat.effort_anal=0;

	

	qsort(set->blocks, set->blocks_count, sizeof(int), comp_int_asc);

	stat.cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	stat.time_anal=stat.cpu_time;
	print_settings(set);
	print_stats(&stat);

	return 0;
}
