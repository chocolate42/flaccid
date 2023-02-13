#include "peakset.h"

#include <assert.h>
#include <stdlib.h>

typedef struct flist flist;
struct flist{
	size_t blocksize, outbuf_size;
	flist *next;
};

static void peak_window(queue *q, input *in, size_t window_size, output *out, flac_settings *set, stats *stat, simple_enc **work, size_t *step, size_t *frame_results, size_t *running_results, size_t *running_step, size_t effort){
	simple_enc *a;
	size_t frame_at, i, j, print_effort=0, window_size_check=0;
	flist *frame=NULL, *frame_curr, *frame_next;

	/* process frames for stats */
	for(j=0;j<set->blocks_count;++j){
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<window_size-(step[j]-1);++i){
			simple_enc_analyse(work[omp_get_thread_num()], set, in, set->blocks[j], in->loc_analysis+(set->blocks[0]*i), stat);
			frame_results[(i*set->blocks_count)+j]=work[omp_get_thread_num()]->outbuf_size;
		}
		#pragma omp barrier
		for(i=window_size-(step[j]-1);i<window_size;++i)
			frame_results[(i*set->blocks_count)+j]=SIZE_MAX;
		print_effort+=step[j];
		fprintf(stderr, "Processed %zu/%zu\n", print_effort, effort);
	}

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
	a=calloc(1, sizeof(simple_enc));
	for(frame_curr=frame;frame_curr;frame_curr=frame_curr->next){
		a->sample_cnt=frame_curr->blocksize;
		assert(a->sample_cnt);
		a->curr_sample=in->loc_analysis;
		a->outbuf_size=frame_curr->outbuf_size;
		a=simple_enc_out(q, a, set, in, stat, out);
	}
	simple_enc_dealloc(a);

	for(frame_curr=frame;frame_curr;frame_curr=frame_next){
		frame_next=frame_curr->next;
		free(frame_curr);
	}
}

int peak_main(input *in, output *out, flac_settings *set){
	clock_t cstart;
	queue q;
	stats stat={0};

	simple_enc *a, **work;
	size_t effort=0, *frame_results, i, max_window_size, *running_results, *running_step, *step, this_window_size;

	mode_boilerplate_init(set, &cstart, &q, &stat);

	if(set->blocks_count==1)
		goodbye("Error: At least two blocksizes must be available\n");

	for(i=1;i<set->blocks_count;++i){
		if(set->blocks[i]%set->blocks[0])
			goodbye("Error: All blocksizes must be a multiple of the minimum blocksize\n");
	}

	max_window_size=(set->peakset_window*1000000)/set->blocks[0];

	step=malloc(sizeof(size_t)*set->blocks_count);
	for(i=0;i<set->blocks_count;++i)
		step[i]=set->blocks[i]/set->blocks[0];

	for(i=0;i<set->blocks_count;++i)
		effort+=step[i];

	frame_results=malloc(sizeof(size_t)*set->blocks_count*(max_window_size+1));
	running_results=malloc(sizeof(size_t)*(max_window_size+1));
	running_step=malloc(sizeof(size_t)*(max_window_size+1));

	work=calloc(set->work_count, sizeof(simple_enc*));
	for(i=0;i<set->work_count;++i)
		work[i]=calloc(1, sizeof(simple_enc));

	set->diff_comp_settings=set->diff_comp_settings?1:2;//hack as analysis not stored

	while(in->input_read(in, max_window_size*set->blocks[0])>=set->blocks[0]){//for all peak windows
		this_window_size=(in->sample_cnt/set->blocks[0])>max_window_size?max_window_size:(in->sample_cnt/set->blocks[0]);
		peak_window(&q, in, this_window_size, out, set, &stat, work, step, frame_results, running_results, running_step, effort);
	}

	for(i=0;i<set->work_count;++i)
		simple_enc_dealloc(work[i]);
	free(work);

	if(in->sample_cnt){//partial frame after last window
		a=calloc(1, sizeof(simple_enc));//reuse work instead of using a TODO
		a->sample_cnt=in->sample_cnt;
		a->curr_sample=in->loc_analysis;
		a=simple_enc_out(&q, a, set, in, &stat, out);
		simple_enc_dealloc(a);
	}
	mode_boilerplate_finish(set, &cstart, &q, &stat, in, out);
	set->diff_comp_settings=set->diff_comp_settings==2?0:1;//reverse hack just in case
	return 0;
}
