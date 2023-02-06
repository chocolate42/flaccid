#include "chunk.h"

#include <assert.h>
#include <omp.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct chenc chenc;

struct chenc{
	simple_enc *enc;
	struct chenc *l, *r;
	size_t offset, blocksize, use_this;
};

// Invalidate worse combination branch
static void chunk_invalidate(chenc *c){
	c->use_this=0;
	if(c->l)
		chunk_invalidate(c->l);
	if(c->r)
		chunk_invalidate(c->r);
}

// Analyse processed chunk to find best frame combination
static size_t chunk_analyse(chenc *c){
	size_t children_size=0;
	if(!(c->l)){
		c->use_this=1;
		return c->enc->outbuf_size;
	}

	children_size+=chunk_analyse(c->l);
	children_size+=chunk_analyse(c->r);
	if(children_size<c->enc->outbuf_size){
		c->use_this=0;
		return children_size;
	}
	else{
		c->use_this=1;
		chunk_invalidate(c->l);
		chunk_invalidate(c->r);
		return c->enc->outbuf_size;
	}
}

// Write best combination of frames in correct order
static void chunk_write(chenc *c, queue *q, flac_settings *set, void *input, uint64_t *curr_sample, stats *stat, FILE *fout, int *outstate){
	if(c->use_this)
		c->enc=simple_enc_out(q, c->enc, set, input, curr_sample, stat, fout, outstate);
	else{
		if(c->l)
			chunk_write(c->l, q, set, input, curr_sample, stat, fout, outstate);
		if(c->r)
			chunk_write(c->r, q, set, input, curr_sample, stat, fout, outstate);
	}
}

int chunk_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	//input thread variables
	int i, *outstate;
	MD5_CTX ctx;
	size_t curr_sample=0, tot_samples=input_size/(set->channels*(set->bps==16?2:4)), encoder_cnt=2;
	size_t child_index, parent_index, curr_blocksize, curr_offset;
	clock_t cstart;
	queue q;
	stats stat={0};

	chenc *encoder;

	for(i=1;i<set->blocks_count;++i){
		if(set->blocks[i-1]*2!=set->blocks[i])
			goodbye("Error: Chunk mode requires blocksizes to be a multiple of two from each other\n");
		encoder_cnt*=2;
	}
	--encoder_cnt;
	encoder=calloc(encoder_cnt, sizeof(chenc));

	//build working data
	parent_index=0;
	child_index=1;
	curr_blocksize=set->blocks[set->blocks_count-1];
	while(child_index!=encoder_cnt){
		curr_offset=0;
		for(i=child_index;parent_index<i;++parent_index){
			encoder[parent_index].enc=calloc(1, sizeof(simple_enc));
			encoder[parent_index].l=&(encoder[child_index++]);
			encoder[parent_index].r=&(encoder[child_index++]);
			encoder[parent_index].offset=curr_offset;
			encoder[parent_index].blocksize=curr_blocksize;
			curr_offset+=curr_blocksize;
		}
		curr_blocksize/=2;
	}
	{
		curr_offset=0;
		for(;parent_index!=encoder_cnt;++parent_index){
			encoder[parent_index].enc=calloc(1, sizeof(simple_enc));
			encoder[parent_index].l=NULL;
			encoder[parent_index].r=NULL;
			encoder[parent_index].offset=curr_offset;
			encoder[parent_index].blocksize=curr_blocksize;
			curr_offset+=curr_blocksize;
		}
	}

	if(set->md5)
		MD5_Init(&ctx);
	cstart=clock();
	queue_alloc(&q, set);
	outstate=calloc(set->work_count, sizeof(int));
	while(!simple_enc_eof(&q, &(encoder[0].enc), set, input, &curr_sample, tot_samples, set->blocks[set->blocks_count-1], &stat, &ctx, fout, outstate)){//if enough input, chunk
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<encoder_cnt;++i){//encode using array for easy multithreading
			simple_enc_analyse(encoder[i].enc, set, input, encoder[i].blocksize, curr_sample+encoder[i].offset, &stat, i?NULL:&ctx);
		}
		#pragma omp barrier
		chunk_analyse(encoder);
		chunk_write(encoder, &q, set, input, &curr_sample, &stat, fout, outstate);
	}
	queue_dealloc(&q, set, input, &stat, fout, outstate);
	free(outstate);
	for(i=0;i<encoder_cnt;++i)
		simple_enc_dealloc(encoder[i].enc);
	free(encoder);

	if(set->md5)
		MD5_Final(set->hash, &ctx);

	stat.effort_anal=set->blocks_count;
	stat.effort_output=1;
	if(!set->diff_comp_settings){
		stat.effort_output=stat.effort_anal;
		stat.effort_anal=0;
	}
	stat.cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	print_settings(set);
	print_stats(&stat);

	return 0;
}
