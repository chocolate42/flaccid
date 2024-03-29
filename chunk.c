#include "chunk.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

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
static void chunk_write(chenc *c, queue *q, flac_settings *set, input *in, stats *stat, output *out){
	if(c->use_this)
		c->enc=simple_enc_out(q, c->enc, set, in, stat, out);
	else{
		if(c->l)
			chunk_write(c->l, q, set, in, stat, out);
		if(c->r)
			chunk_write(c->r, q, set, in, stat, out);
	}
}

int chunk_main(input *in, output *out, flac_settings *set){
	clock_t cstart;
	queue q;
	stats stat={0};

	chenc *encoder;
	size_t i, child_index, curr_blocksize, curr_offset, encoder_cnt=2, parent_index;

	mode_boilerplate_init(set, &cstart, &q, &stat);

	for(i=1;i<set->blocks_count;++i){
		_if((set->blocks[i-1]*2!=set->blocks[i]), "Chunk mode requires blocksizes to be a multiple of two from each other");
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
		for(;parent_index<encoder_cnt;++parent_index){
			encoder[parent_index].enc=calloc(1, sizeof(simple_enc));
			encoder[parent_index].l=NULL;
			encoder[parent_index].r=NULL;
			encoder[parent_index].offset=curr_offset;
			encoder[parent_index].blocksize=curr_blocksize;
			curr_offset+=curr_blocksize;
		}
	}

	in->input_read(in, set->blocks[set->blocks_count-1]);
	while(!simple_enc_eof(&q, &(encoder[0].enc), set, in, set->blocks[set->blocks_count-1], &stat, out)){//if enough input, chunk
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<encoder_cnt;++i){//encode using array for easy multithreading
			simple_enc_analyse(encoder[i].enc, set, in, encoder[i].blocksize, in->loc_analysis+encoder[i].offset, &stat);
		}
		#pragma omp barrier
		chunk_analyse(encoder);
		chunk_write(encoder, &q, set, in, &stat, out);
		in->input_read(in, set->blocks[set->blocks_count-1]);
	}

	mode_boilerplate_finish(set, &cstart, &q, &stat, in, out);

	for(i=0;i<encoder_cnt;++i)
		simple_enc_dealloc(encoder[i].enc);
	free(encoder);
	return 0;
}
