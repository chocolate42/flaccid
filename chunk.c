#include "chunk.h"
#include "tweak.h"

#include <assert.h>
#include <omp.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Chunk encoding is a simple brute-force variable blocksize implementation
 * * Define min/max/stride, where max=min*(stride^(effort-1))
 * * effort is the number of blocksizes used, the number of times input is fully encoded
 * * A chunk is the size of the max blocksize
 * * Imagine a chunk partitioned evenly into a n-ary tree, where n=stride
 * * The root is a tier of max blocksize, every tier down represents the frames with blocksize=prev_tier_blocksize/stride
 * * The leaf nodes are the final tier of min blocksize
 * * Compute everything
 * * For every node recursively pick smallest(sum_of_children, self)
*/

chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set){
	size_t i;
	assert(min!=0);
	assert(max>=min);

	c->enc=init_static_encoder(set, max, set->comp_anal, set->apod_anal);
	c->blocksize=max;
	c->child_count=set->blocksize_stride;
	if(max/c->child_count<min)
		c->child=NULL;
	else{
		c->child=malloc(sizeof(chunk_enc)*c->child_count);
		for(i=0;i<c->child_count;++i)
			chunk_init(c->child+i, min, max/c->child_count, set);
	}
	return c;
}

/* Do all blocksize encodes for chunk */
void chunk_process(chunk_enc *c, void *input, uint64_t sample_number, flac_settings *set){
	size_t i;
	c->curr_sample=sample_number;
	if(!set->encode_func(c->enc, input, c->blocksize, sample_number, &(c->outbuf), &(c->outbuf_size))){
		fprintf(stderr, "Static encode failed, state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(c->enc->stream_encoder)]);
		exit(1);
	}
	if(c->child){
		for(i=0;i<c->child_count;++i)
			chunk_process(c->child+i, input + i*FLAC__stream_encoder_get_channels(c->child[0].enc->stream_encoder)*FLAC__stream_encoder_get_blocksize(c->child[0].enc->stream_encoder)*(set->bps==16?2:4), sample_number+i*FLAC__stream_encoder_get_blocksize(c->child[0].enc->stream_encoder), set);
	}
}

/* Invalidate worse combination branch */
void chunk_invalidate(chunk_enc *c){
	size_t i;
	c->use_this=0;
	if(c->child){
		for(i=0;i<c->child_count;++i)
			chunk_invalidate(c->child+i);
	}
}

/* Analyse processed chunk to find best frame combination */
size_t chunk_analyse(chunk_enc *c){
	size_t children_size=0, i;
	if(!(c->child)){
		c->use_this=1;
		return c->outbuf_size;
	}

	for(i=0;i<c->child_count;++i)
		children_size+=chunk_analyse(c->child+i);
	if(children_size<c->outbuf_size){
		c->use_this=0;
		return children_size;
	}
	else{
		c->use_this=1;
		for(i=0;i<c->child_count;++i)
			chunk_invalidate(c->child+i);
		return c->outbuf_size;
	}
}

flist *chunk_list(chunk_enc *c, flist *f, flist **head){
	flist *tail;
	size_t i;
	if(c->use_this){
		tail=malloc(sizeof(flist));
		tail->is_outbuf_alloc=0;
		tail->merge_tried=0;
		tail->outbuf=c->outbuf;
		tail->outbuf_size=c->outbuf_size;
		tail->blocksize=c->blocksize;
		tail->curr_sample=c->curr_sample;
		tail->next=NULL;
		tail->prev=NULL;
		if(f){
			f->next=tail;
			tail->prev=f;
		}
		else
			*head=tail;
	}
	else{
		assert(c->child);
		tail=f;
		for(i=0;i<c->child_count;++i)
			tail=chunk_list(c->child+i, tail, head);
	}
	return tail;
}

/* Write best combination of frames in correct order */
void chunk_write(flac_settings *set, chunk_enc *c, void *input, FILE *fout, uint32_t *minf, uint32_t *maxf, size_t *outsize){
	size_t i;
	if(c->use_this){
		if(set->diff_comp_settings){
			FLAC__StaticEncoder *enc;
			enc=init_static_encoder(set, c->blocksize<16?16:c->blocksize, set->comp_output, set->apod_output);
			if(!set->encode_func(enc, input+(c->curr_sample*set->channels*(set->bps==16?2:4)), c->blocksize, c->curr_sample, &(c->outbuf), &(c->outbuf_size))){
				fprintf(stderr, "Static encode failed, state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(c->enc->stream_encoder)]);
				exit(1);
			}
			(*outsize)+=fwrite_framestat(c->outbuf, c->outbuf_size, fout, minf, maxf);
			FLAC__static_encoder_delete(enc);
		}
		else
			(*outsize)+=fwrite_framestat(c->outbuf, c->outbuf_size, fout, minf, maxf);
	}
	else{
		assert(c->child);
		for(i=0;i<c->child_count;++i)
			chunk_write(set, c->child+i, input, fout, minf, maxf, outsize);
	}
}

int chunk_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	unsigned int bytes_per_sample;
	chunk_worker *work;

	//input thread variables
	int i;
	size_t input_chunk_size, block_index;
	uint32_t curr_chunk_in=0, last_chunk, last_chunk_sample_count;
	omp_lock_t curr_chunk_lock;
	//output thread variables
	int output_skipped=0;
	uint32_t curr_chunk_out=0;
	double effort_anal, effort_output, effort_tweak=0, effort_merge=0;
	size_t outsize=42, tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	clock_t cstart, cstart_sub;
	double cpu_time, anal_time=0, tweak_time=0, merge_time=0;
	cstart=clock();

	if(set->blocks_count>1){
		set->blocksize_stride=set->blocks[1]/set->blocks[0];
		if(set->blocksize_stride<2)
			goodbye("Error: Chunk mode requires blocksizes to be a fixed multiple away from each other, >=2\n");

		for(block_index=1;block_index<set->blocks_count;++block_index){
			if(set->blocks[block_index-1]*set->blocksize_stride!=set->blocks[block_index])
				goodbye("Error: Chunk mode requires blocksizes to be a fixed multiple away from each other, >=2\n");
		}
	}
	else{/*hack to use chunk code for a single fixed blocksize*/
		set->blocksize_stride=2;//dummy, anything >1
		set->apod_anal=set->apod_output;
		set->comp_anal=set->comp_output;
		set->diff_comp_settings=0;
		//tweak does nothing as it only works within chunks, which only contain 1 frame
	}

	bytes_per_sample=set->bps==16?2:4;

	input_chunk_size=set->blocksize_max*set->channels*bytes_per_sample;

	omp_init_lock(&curr_chunk_lock);

	last_chunk=(input_size/input_chunk_size)+(input_size%input_chunk_size?0:-1);
	last_chunk_sample_count=(input_size%input_chunk_size)/(set->channels*bytes_per_sample);

	//seektable dummy TODO

	work=malloc(sizeof(chunk_worker)*set->work_count);
	memset(work, 0, sizeof(chunk_worker)*set->work_count);
	for(i=0;i<set->work_count;++i){
		for(int h=0;h<SHARD_COUNT;++h){
			chunk_init(&(work[i].chunk[h]), set->blocksize_min, set->blocksize_max, set);
			work[i].status[h]=1;
			work[i].curr_chunk[h]=UINT32_MAX;
		}
	}

	omp_set_num_threads(set->work_count+2);
	#pragma omp parallel num_threads(set->work_count+2) shared(work)
	{
		if(omp_get_num_threads()!=set->work_count+2){
			fprintf(stderr, "Error: Failed to set %d threads (%dx encoders, 1x I/O, 1x MD5). Actually set %d threads\n", set->work_count+2, set->work_count, omp_get_num_threads());
			exit(1);
		}
		else if(omp_get_thread_num()==set->work_count){//md5 thread
			MD5_CTX ctx;
			uint32_t wavefront, done=0;
			size_t iloc, ilen;
			MD5_Init(&ctx);
			while(1){
				omp_set_lock(&curr_chunk_lock);
				wavefront=curr_chunk_in;
				omp_unset_lock(&curr_chunk_lock);
				if(done<wavefront){
					iloc=done*input_chunk_size;
					ilen=(wavefront>=last_chunk)?(input_size-iloc):((wavefront-done)*input_chunk_size);
					if(set->bps==16)
						MD5_Update(&ctx, ((void*)input)+iloc, ilen);
					else{/*funky input needs funky md5, uses format_input_ from libFLAC as a template*/
						/*don't get it twisted, bytes_per_sample is the size of an element in input, set->bps is the bps of the signal*/
						//TODO
						/*size_t z, q;
						for(z=0;z<ilen/(bytes_per_sample);++z){
							for(q=0;q<((set->bps+7)/8);++q)
								MD5_Update(&ctx, (((uint8_t*)input)+iloc+(z*bytes_per_sample)+(3-q)), 1);//horribly inefficient
						}*/
					}
					done=wavefront;
					if(done>=last_chunk)
						break;
				}
				else
					usleep(100);
			}
			if(set->bps==16)//non-16 bit TODO
				MD5_Final(set->hash, &ctx);
		}
		else if(omp_get_thread_num()==set->work_count+1){//output thread
			int h, j, k, status;
			uint32_t curr_chunk;
			while(curr_chunk_out<=last_chunk){
				for(j=0;j<set->work_count;++j){
					if(output_skipped==set->work_count){//sleep if there's nothing to write
						usleep(100);
						output_skipped=0;
					}
					h=-1;
					for(k=0;k<SHARD_COUNT;++k){
						#pragma omp atomic read
						status=work[j].status[k];
						#pragma omp atomic read
						curr_chunk=work[j].curr_chunk[k];
						if(status==2 && curr_chunk==curr_chunk_out){
							h=k;
							break;
						}
					}
					if(h>-1){//do write
						if(curr_chunk_out==last_chunk)//clean TODO, chunk_write only used as legacy code
							chunk_write(set, &(work[j].chunk[h]), input, fout, &(set->minf), &(set->maxf), &outsize);
						else{
							flist_write(work[j].chunk[h].list, set, input, &outsize, fout);
							//flist_delete(
							work[j].chunk[h].list=NULL;
						}
						++curr_chunk_out;
						#pragma omp atomic
						work[j].status[h]--;
						#pragma omp flush
					}
					else
						++output_skipped;
				}
			}
		}
		else{//worker
			int h;
			unsigned int k, status;
			while(1){
				h=-1;
				for(k=0;k<SHARD_COUNT;++k){
					#pragma omp atomic read
					status=work[omp_get_thread_num()].status[k];
					if(status==1){
						h=k;
						break;
					}
				}
				if(h>-1){//do work
					omp_set_lock(&curr_chunk_lock);
					work[omp_get_thread_num()].curr_chunk[h]=curr_chunk_in++;
					omp_unset_lock(&curr_chunk_lock);
					if(work[omp_get_thread_num()].curr_chunk[h]>last_chunk)
						break;
					if(work[omp_get_thread_num()].curr_chunk[h]==last_chunk && last_chunk_sample_count){//do a partial last chunk as single frame
						if(!set->encode_func(
							work[omp_get_thread_num()].chunk[h].enc,
							input+work[omp_get_thread_num()].curr_chunk[h]*set->channels*set->blocksize_max*(set->bps==16?2:4),
							last_chunk_sample_count, set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h],
							&(work[omp_get_thread_num()].chunk[h].outbuf),
							&(work[omp_get_thread_num()].chunk[h].outbuf_size))){ //using bps16
							fprintf(stderr, "processing failed worker %d sample_number %d\n", omp_get_thread_num(), set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h]);
							fprintf(stderr, "Encoder state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(work[omp_get_thread_num()].chunk[h].enc->stream_encoder)]);
							exit(1);
						}
						work[omp_get_thread_num()].chunk[h].curr_sample=work[omp_get_thread_num()].curr_chunk[h]*set->blocksize_max;
						work[omp_get_thread_num()].chunk[h].blocksize=last_chunk_sample_count;
						work[omp_get_thread_num()].chunk[h].use_this=1;
					}
					else{//do a full chunk
						cstart_sub=clock();
						chunk_process(&(work[omp_get_thread_num()].chunk[h]), input+work[omp_get_thread_num()].curr_chunk[h]*input_chunk_size, set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h], set);
						chunk_analyse(&(work[omp_get_thread_num()].chunk[h]));
						chunk_list(work[omp_get_thread_num()].chunk+h, NULL, &(work[omp_get_thread_num()].chunk[h].list));
						anal_time+=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;

						if(!set->tweak_after && set->tweak){//tweak with analysis settings
							size_t teff=0, tsaved=0;
							cstart_sub=clock();
							tweak_pass(work[omp_get_thread_num()].chunk[h].list, set, set->comp_anal, set->apod_anal, &teff, &tsaved, input);
							effort_tweak=teff;
							effort_tweak/=tot_samples;
							tweak_time+=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
						}
						flist_initial_output_encode(work[omp_get_thread_num()].chunk[h].list, set, input);
						if(set->tweak_after && set->tweak){//tweak with output settings
							size_t teff=0, tsaved=0;
							cstart_sub=clock();
							tweak_pass(work[omp_get_thread_num()].chunk[h].list, set, set->comp_output, set->apod_output, &teff, &tsaved, input);
							effort_tweak=teff;
							effort_tweak/=tot_samples;
							tweak_time+=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
						}
						/*merge does nothing as we're trying to merge within a chunk, which implicitly merges already */
					}
					#pragma omp atomic
					work[omp_get_thread_num()].status[h]++;
					#pragma omp flush
				}
				else
					usleep(100);
			}
		}
	}
	#pragma omp barrier

	effort_anal=set->blocks_count;
	effort_output=1;
	if(!set->diff_comp_settings){
		effort_output=effort_anal;
		effort_anal=0;
	}

	printf("settings\tmode(chunk);lax(%u);analysis_comp(%s);analysis_apod(%s);output_comp(%s);output_apod(%s);tweak_after(%u);tweak(%u);tweak_early_exit(%u);merge_after(%u);merge(%u);"
		"blocksize_limit_lower(%u);blocksize_limit_upper(%u);analysis_blocksizes(%u", set->lax, set->comp_anal, set->apod_anal, set->comp_output, set->apod_output, set->tweak_after, set->tweak, set->tweak_early_exit, set->merge_after, set->merge, set->blocksize_limit_lower, set->blocksize_limit_upper, set->blocks[0]);
	for(i=1;i<set->blocks_count;++i)
		printf(",%u", set->blocks[i]);
	cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	printf(")\teffort\tanalysis(%.3f);tweak(%.3f);merge(%.3f);output(%.3f)", effort_anal, effort_tweak, effort_merge, effort_output);
	printf("\tsubtiming\tanalysis(%.5f);tweak(%.5f);merge(%.5f)", anal_time, tweak_time, merge_time);
	printf("\tsize\t%zu\tcpu_time\t%.5f\n", outsize, cpu_time);

	return 0;
}
