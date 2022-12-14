#include "merge.h"
#include "peakset.h"
#include "tweak.h"

#include <assert.h>
#include <omp.h>
#include <stdlib.h>
#include <time.h>

int peak_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	int k;
	size_t effort=0, i, j, *step, step_count;
	size_t *frame_results, *running_results;
	uint8_t *running_step;
	size_t window_size, window_size_check=0;
	size_t tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	size_t print_effort=0;
	flist *frame=NULL, *frame_curr;
	peak_hunter *work;
	FLAC__StaticEncoder *encout;
	clock_t cstart, cstart_sub;
	stats stat={0};
	cstart=clock();

	if(set->blocks_count==1)
		goodbye("Error: At least two blocksizes must be available\n");
	if(set->blocks_count>255)
		goodbye("Error: Implementation limited to up to 255 blocksizes. This is also just an insane heat-death-of-the-universe setting\n");

	for(i=1;i<set->blocks_count;++i){
		if(set->blocks[i]%set->blocks[0])
			goodbye("Error: All blocksizes must be a multiple of the minimum blocksize\n");
	}
	window_size=tot_samples/set->blocks[0];
	if(!window_size)
		goodbye("Error: Input too small\n");

	step=malloc(sizeof(size_t)*set->blocks_count);
	step_count=set->blocks_count;
	for(i=0;i<set->blocks_count;++i)
		step[i]=set->blocks[i]/set->blocks[0];

	for(i=1;i<step_count;++i){
		if(step[i]==step[i-1])
			goodbye("Error: Duplicate blocksizes in argument\n");
	}

	for(i=0;i<step_count;++i)
		effort+=step[i];

	fprintf(stderr, "anal %s comp %s tweak %d merge %d\n", set->comp_anal, set->comp_output, set->tweak, set->merge);

	frame_results=malloc(sizeof(size_t)*step_count*(window_size+1));
	running_results=malloc(sizeof(size_t)*(window_size+1));
	running_step=malloc(sizeof(size_t)*(window_size+1));


	cstart_sub=clock();
	/* process frames for stats */
	omp_set_num_threads(set->work_count);
	work=malloc(sizeof(peak_hunter)*set->work_count);
	for(j=0;j<step_count;++j){
		for(k=0;k<set->work_count;++k)
			work[k].enc=init_static_encoder(set, set->blocks[j], set->comp_anal, set->apod_anal);
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<window_size-(step[j]-1);++i){
			set->encode_func(work[omp_get_thread_num()].enc,
				input+(set->blocksize_min*i*set->channels*(set->bps==16?2:4)),
				set->blocks[j],
				set->blocksize_min*i,
				&(work[omp_get_thread_num()].outbuf),
				frame_results+(i*step_count)+j
			);
		}
		#pragma omp barrier
		for(i=window_size-(step[j]-1);i<window_size;++i)
			frame_results[(i*step_count)+j]=SIZE_MAX;
		for(k=0;k<set->work_count;++k)
			FLAC__static_encoder_delete(work[k].enc);
		print_effort+=step[j];
		fprintf(stderr, "Processed %zu/%zu\n", print_effort, effort);
	}

	/* analyse stats */
	running_results[0]=0;
	for(i=1;i<=window_size;++i){
		size_t curr_run=SIZE_MAX, curr_step=step_count;
		for(j=0;j<step_count;++j){
			if(step[j]>i)//near the beginning we need to ensure we don't go beyond window start
				break;
			if(curr_run>(running_results[i-step[j]]+frame_results[((i-step[j])*step_count)+j])){
				assert(frame_results[((i-step[j])*step_count)+j]!=SIZE_MAX);
				curr_run=(running_results[i-step[j]]+frame_results[((i-step[j])*step_count)+j]);
				curr_step=j;
			}
		}
		assert(curr_run!=SIZE_MAX);
		running_results[i]=curr_run;
		running_step[i]=curr_step;
	}
	stat.time_anal=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;

	/* traverse optimal result */
	for(i=window_size;i>0;){
		size_t frame_at=i-step[running_step[i]];
		frame_curr=frame;
		frame=malloc(sizeof(flist));
		frame->is_outbuf_alloc=0;
		frame->merge_tried=0;
		frame->curr_sample=frame_at*set->blocksize_min;
		frame->blocksize=set->blocks[running_step[i]];
		frame->outbuf=NULL;
		frame->outbuf_size=frame_results[(frame_at*step_count)+running_step[i]];
		frame->next=frame_curr;
		frame->prev=NULL;
		if(frame_curr)
			frame_curr->prev=frame;
		window_size_check+=step[running_step[i]];
		i-=step[running_step[i]];
	}
	assert(i==0);
	assert(window_size_check==window_size);

	if(set->merge){
		size_t teff=0, tsaved=0;
		cstart_sub=clock();
		merge_pass_mt(frame, set, set->comp_anal, set->apod_anal, &teff, &tsaved, input);
		stat.effort_merge=teff;
		stat.effort_merge/=tot_samples;
		stat.time_merge=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
	}

	if(set->tweak){
		size_t teff=0, tsaved=0;
		cstart_sub=clock();
		tweak_pass_mt(frame, set, set->comp_anal, set->apod_anal, &teff, &tsaved, input);
		stat.effort_tweak=teff;
		stat.effort_tweak/=tot_samples;
		stat.time_tweak=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
	}

	//if used simpler settings for analysis, encode properly here to get comp_output sizes
	//store encoded frames as most/all of these are likely to be used depending on how much tweaking is done
	flist_initial_output_encode(frame, set, input);

	/* Write optimal result */
	flist_write(frame, set, input, &(stat.outsize), fout);
	if(!set->diff_comp_settings && !set->tweak && !set->merge)
		assert(stat.outsize==running_results[window_size]);

	if(tot_samples-(window_size*set->blocks[0])){/* partial end frame */
		void *partial_outbuf;
		size_t partial_outbuf_size;
		encout=init_static_encoder(set, set->blocksize_max, set->comp_output, set->apod_output);
		set->encode_func(encout,
			input+((window_size*set->blocks[0])*set->channels*(set->bps==16?2:4)),
			tot_samples-(window_size*set->blocks[0]),
			(window_size*set->blocks[0]),
			&(partial_outbuf),
			&(partial_outbuf_size)
		);
		stat.outsize+=fwrite_framestat(partial_outbuf, partial_outbuf_size, fout, &(set->minf), &(set->maxf));
		FLAC__static_encoder_delete(encout);
	}
	if(set->bps==16)//non-16 bit TODO
		MD5(((void*)input), input_size, set->hash);

	stat.effort_anal=0;
	for(i=0;i<set->blocks_count;++i)//analysis effort approaches the sum of the normalised blocksizes as window_size approaches infinity
		stat.effort_anal+=step[i];
	stat.effort_output+=1;

	stat.cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	print_settings(set);
	print_stats(&stat);

	return 0;
}
