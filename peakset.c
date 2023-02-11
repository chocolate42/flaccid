#include "peakset.h"

#include <assert.h>
#include <stdlib.h>

typedef struct flist flist;

struct flist{
	size_t blocksize, outbuf_size;
	flist *next;
};

int peak_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	size_t effort=0, i, j, *step;
	size_t *frame_results, *running_results;
	uint8_t *running_step;
	size_t window_size, window_size_check=0;
	size_t print_effort=0;
	size_t frame_at;
	flist *frame=NULL, *frame_curr;
	clock_t cstart;
	queue q;
	simple_enc *a, **work;
	stats stat={0};
	uint64_t curr_sample=0;
	MD5_CTX ctx;

	mode_boilerplate_init(set, &cstart, &ctx, &q, &stat, input_size);

	if(set->blocks_count==1)
		goodbye("Error: At least two blocksizes must be available\n");

	for(i=1;i<set->blocks_count;++i){
		if(set->blocks[i]%set->blocks[0])
			goodbye("Error: All blocksizes must be a multiple of the minimum blocksize\n");
	}
	window_size=stat.tot_samples/set->blocks[0];
	if(!window_size)
		goodbye("Error: Input too small\n");

	step=malloc(sizeof(size_t)*set->blocks_count);
	for(i=0;i<set->blocks_count;++i)
		step[i]=set->blocks[i]/set->blocks[0];

	for(i=0;i<set->blocks_count;++i)
		effort+=step[i];

	frame_results=malloc(sizeof(size_t)*set->blocks_count*(window_size+1));
	running_results=malloc(sizeof(size_t)*(window_size+1));
	running_step=malloc(sizeof(size_t)*(window_size+1));

	/* process frames for stats */
	work=calloc(set->work_count, sizeof(simple_enc*));
	for(i=0;i<set->work_count;++i)
		work[i]=calloc(1, sizeof(simple_enc));
	for(j=0;j<set->blocks_count;++j){
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<window_size-(step[j]-1);++i){
			simple_enc_analyse(work[omp_get_thread_num()], set, input, set->blocks[j], set->blocksize_min*i, &stat, NULL);
			frame_results[(i*set->blocks_count)+j]=work[omp_get_thread_num()]->outbuf_size;
		}
		#pragma omp barrier
		for(i=window_size-(step[j]-1);i<window_size;++i)
			frame_results[(i*set->blocks_count)+j]=SIZE_MAX;
		print_effort+=step[j];
		fprintf(stderr, "Processed %zu/%zu\n", print_effort, effort);
	}
	for(i=0;i<set->work_count;++i)
		simple_enc_dealloc(work[i]);
	free(work);

	/* analyse stats */
	running_results[0]=0;
	for(i=1;i<=window_size;++i){
		size_t curr_run=SIZE_MAX, curr_step=set->blocks_count;
		for(j=0;j<set->blocks_count;++j){
			if(step[j]>i)//near the beginning we need to ensure we don't go beyond window start
				break;
			if(curr_run>(running_results[i-step[j]]+frame_results[((i-step[j])*set->blocks_count)+j])){
				assert(frame_results[((i-step[j])*set->blocks_count)+j]!=SIZE_MAX);
				curr_run=(running_results[i-step[j]]+frame_results[((i-step[j])*set->blocks_count)+j]);
				curr_step=j;
			}
		}
		assert(curr_run!=SIZE_MAX);
		running_results[i]=curr_run;
		running_step[i]=curr_step;
	}

	/* traverse optimal result */
	for(i=window_size;i>0;){
		frame_at=i-step[running_step[i]];
		frame_curr=frame;
		frame=malloc(sizeof(flist));
		frame->blocksize=set->blocks[running_step[i]];
		frame->next=frame_curr;
		frame->outbuf_size=frame_results[(frame_at*set->blocks_count)+running_step[i]];
		window_size_check+=step[running_step[i]];
		i-=step[running_step[i]];
	}
	assert(i==0);
	assert(window_size_check==window_size);

	//use simple_enc to encode
	set->diff_comp_settings=set->diff_comp_settings?1:2;//hack as analysis not stored
	a=calloc(1, sizeof(simple_enc));
	for(frame_curr=frame;frame_curr;frame_curr=frame_curr->next){
		a->sample_cnt=frame_curr->blocksize;
		assert(a->sample_cnt);
		a->curr_sample=curr_sample;
		a->outbuf_size=frame_curr->outbuf_size;
		a=simple_enc_out(&q, a, set, input, &curr_sample, &stat, fout);
	}
	if(curr_sample!=stat.tot_samples){//partial
		a->sample_cnt=stat.tot_samples-curr_sample;
		a->curr_sample=curr_sample;
		a=simple_enc_out(&q, a, set, input, &curr_sample, &stat, fout);
	}
	if(set->md5)
		MD5_UpdateSamples(&ctx, input, 0, stat.tot_samples, set);

	mode_boilerplate_finish(set, &cstart, &ctx, &q, &stat, input, fout);

	simple_enc_dealloc(a);
	set->diff_comp_settings=set->diff_comp_settings==2?0:1;//reverse hack just in case
	return 0;
}
