#include "common.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/*Implement OpenSSL MD5 API with mbedtls*/
#ifndef USE_OPENSSL
void MD5_Init(MD5_CTX *ctx);
void MD5_Final(uint8_t *sha1, MD5_CTX *ctx);
int (*MD5)(const unsigned char*, size_t, unsigned char*) = &mbedtls_md5;
int (*MD5_Update)(MD5_CTX*, const unsigned char*, size_t) = &mbedtls_md5_update;

void MD5_Init(MD5_CTX *ctx){
	mbedtls_md5_init(ctx);
	mbedtls_md5_starts(ctx);
}

void MD5_Final(uint8_t *sha1, MD5_CTX *ctx){
	mbedtls_md5_finish(ctx, sha1);
}
#endif

int comp_int_asc(const void *aa, const void *bb){
	int *a=(int*)aa;
	int *b=(int*)bb;
	if(*a<*b)
		return -1;
	else
		return *a==*b?0:1;
}

int comp_int_desc(const void *aa, const void *bb){
	int *a=(int*)aa;
	int *b=(int*)bb;
	if(*a<*b)
		return 1;
	else
		return *a==*b?0:-1;
}

void flist_initial_output_encode(flist *frame, flac_settings *set, void *input){
	FLAC__StaticEncoder *enc;
	flist *frame_curr;
	void *tmpbuf;
	for(frame_curr=frame;frame_curr;frame_curr=frame_curr->next){
		if(set->diff_comp_settings){/*encode if settings different*/
			enc=init_static_encoder(set, frame_curr->blocksize, set->comp_output, set->apod_output);
			set->encode_func(enc,
				input+(frame_curr->curr_sample*set->channels*(set->bps==16?2:4)),
				frame_curr->blocksize,
				frame_curr->curr_sample,
				&tmpbuf,
				&(frame_curr->outbuf_size)
			);
			frame_curr->outbuf=malloc(frame_curr->outbuf_size);
			frame_curr->is_outbuf_alloc=1;
			memcpy(frame_curr->outbuf, tmpbuf, frame_curr->outbuf_size);
			FLAC__static_encoder_delete(enc);
		}
	}
}

void flist_write(flist *frame, flac_settings *set, void *input, size_t *outsize, FILE *fout){
	FLAC__StaticEncoder *enc;
	flist *frame_curr;
	size_t comp;
	for(frame_curr=frame;frame_curr;frame_curr=frame_curr->next){
		if(!(frame_curr->outbuf)){//encode if not stored
			enc=init_static_encoder(set, frame_curr->blocksize, set->comp_output, set->apod_output);
			set->encode_func(enc,
				input+(frame_curr->curr_sample*set->channels*(set->bps==16?2:4)),
				frame_curr->blocksize,
				frame_curr->curr_sample,
				&(frame_curr->outbuf),
				&(comp)
			);
			if(!set->diff_comp_settings)
				assert(frame_curr->outbuf_size==comp);
			(*outsize)+=fwrite_framestat(frame_curr->outbuf, frame_curr->outbuf_size, fout, &(set->minf), &(set->maxf));
			FLAC__static_encoder_delete(enc);
		}
		else
			(*outsize)+=fwrite_framestat(frame_curr->outbuf, frame_curr->outbuf_size, fout, &(set->minf), &(set->maxf));
		if(set->blocksize_min>frame_curr->blocksize)
			set->blocksize_min=frame_curr->blocksize;
		if(set->blocksize_max<frame_curr->blocksize)
			set->blocksize_max=frame_curr->blocksize;
	}
}

size_t fwrite_framestat(const void *ptr, size_t size, FILE *stream, uint32_t *minf, uint32_t *maxf){
	if(size<*minf)
		*minf=size;
	if(size>*maxf)
		*maxf=size;
	return fwrite(ptr, 1, size, stream);
}

void goodbye(char *s){
	fprintf(stderr, "%s", s);
	exit(1);
}

FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, char *comp, char *apod){
	FLAC__StaticEncoder *r;
	r=FLAC__static_encoder_new();
	r->is_variable_blocksize=set->blocksize_min!=set->blocksize_max;
	if(set->lax)
		FLAC__stream_encoder_set_streamable_subset(r->stream_encoder, false);
	else if(blocksize>16384 || (set->sample_rate<=48000 && blocksize>4608))
		goodbye("Error: Tried to use a non-subset blocksize without setting --lax\n");
	FLAC__stream_encoder_set_channels(r->stream_encoder, set->channels);
	FLAC__stream_encoder_set_bits_per_sample(r->stream_encoder, set->bps);
	FLAC__stream_encoder_set_sample_rate(r->stream_encoder, set->sample_rate);
	if(comp[0]>='0'&&comp[0]<='8')
		FLAC__stream_encoder_set_compression_level(r->stream_encoder, comp[0]-'0');
	if(strchr(comp, 'e'))
		FLAC__stream_encoder_set_do_exhaustive_model_search(r->stream_encoder, true);
	if(strchr(comp, 'l'))
		FLAC__stream_encoder_set_max_lpc_order(r->stream_encoder, atoi(strchr(comp, 'l')+1));
	if(strchr(comp, 'm'))
		FLAC__stream_encoder_set_do_mid_side_stereo(r->stream_encoder, true);
	if(strchr(comp, 'p'))
		FLAC__stream_encoder_set_do_qlp_coeff_prec_search(r->stream_encoder, true);
	if(strchr(comp, 'q'))
		FLAC__stream_encoder_set_qlp_coeff_precision(r->stream_encoder, atoi(strchr(comp, 'q')+1));
	if(strchr(comp, 'r')&&strchr(comp, ',')){
		FLAC__stream_encoder_set_min_residual_partition_order(r->stream_encoder, atoi(strchr(comp, 'r')+1));
		FLAC__stream_encoder_set_max_residual_partition_order(r->stream_encoder, atoi(strchr(comp, ',')+1));
	}
	else if(strchr(comp, 'r'))
		FLAC__stream_encoder_set_max_residual_partition_order(r->stream_encoder, atoi(strchr(comp, 'r')+1));

	if(apod)
		FLAC__stream_encoder_set_apodization(r->stream_encoder, apod);

	FLAC__stream_encoder_set_blocksize(r->stream_encoder, blocksize);/* override compression level blocksize */
	FLAC__stream_encoder_set_loose_mid_side_stereo(r->stream_encoder, false);/* override adaptive mid-side, this doesn't play nice */

	if(FLAC__static_encoder_init(r)!=FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		goodbye("Init failed\n");

	return r;
}

void parse_blocksize_list(char *list, int **res, size_t *res_cnt){
	char *cptr=list-1;
	*res_cnt=0;
	*res=NULL;
	do{
		*res=realloc(*res, sizeof(int)*(*res_cnt+1));
		(*res)[*res_cnt]=atoi(cptr+1);
		if((*res)[*res_cnt]<16)
			goodbye("Error: Blocksize must be at least 16\n");
		if((*res)[*res_cnt]>65535)
			goodbye("Error: Blocksize must be at most 65535\n");
		*res_cnt=*res_cnt+1;
	}while((cptr=strchr(cptr+1, ',')));
}

void print_settings(flac_settings *set){
	char *modes[]={"chunk", "gset", "peakset", "gasc"};
	int i;
	printf("settings\tmode(%s);lax(%u);analysis_comp(%s);analysis_apod(%s);output_comp(%s);output_apod(%s);tweak(%u);merge(%u);", modes[set->mode], set->lax, set->comp_anal, set->apod_anal, set->comp_output, set->apod_output, set->tweak, set->merge);

	if(set->merge||set->tweak||set->mode==3)
		printf("blocksize_limit_lower(%u);blocksize_limit_upper(%u)", set->blocksize_limit_lower, set->blocksize_limit_upper);

	if(set->outperc!=100)
		printf("outperc(%u);outputalt_comp(%s);outputalt_apod(%s);", set->outperc, set->comp_outputalt, set->apod_outputalt);
	if(set->blocks_count && set->mode!=3){//gasc doesn't use the list
		printf(";analysis_blocksizes(%u", set->blocks[0]);
		for(i=1;i<set->blocks_count;++i)
			printf(",%u", set->blocks[i]);
		printf(")");
	}
}

void print_stats(stats *stat){
	printf("\teffort\tanalysis(%.3f);tweak(%.3f);merge(%.3f);output(%.3f)", stat->effort_anal, stat->effort_tweak, stat->effort_merge, stat->effort_output);
	printf("\tsubtiming\tanalysis(%.5f);tweak(%.5f);merge(%.5f)", stat->time_anal, stat->time_tweak, stat->time_merge);
	printf("\tsize\t%zu\tcpu_time\t%.5f\n", stat->outsize+42, stat->cpu_time);
}

/*internal*/
void queue_merge(queue *q, flac_settings *set);
void queue_tweak(queue *q, flac_settings *set);
void simple_enc_encode(simple_enc *senc, flac_settings *set, void *input, uint32_t samples, uint64_t curr_sample, int is_anal, stats *stat);
void simple_enc_flush(queue *q, flac_settings *set, void *input, stats *stat, FILE *fout, int *outstate);

void simple_enc_encode(simple_enc *senc, flac_settings *set, void *input, uint32_t samples, uint64_t curr_sample, int is_anal, stats *stat){
	assert(senc&&set&&input);
	assert(samples);
	if(senc->sample_cnt)
		FLAC__static_encoder_delete(senc->enc);
	senc->enc=init_static_encoder(set, samples<16?16:samples, is_anal==1?set->comp_anal:(is_anal==0?set->comp_output:set->comp_outputalt), is_anal==1?set->apod_anal:(is_anal==0?set->apod_output:set->apod_outputalt));
	senc->sample_cnt=samples;
	senc->curr_sample=curr_sample;
	set->encode_func(senc->enc, input+curr_sample*set->channels*(set->bps==16?2:4), samples, curr_sample, &(senc->outbuf), &(senc->outbuf_size));//do encode
	if(stat&&(is_anal==1))
			stat->effort_anal+=samples;
	else if(stat)
		stat->effort_output+=samples;
}

void simple_enc_analyse(simple_enc *senc, flac_settings *set, void *input, uint32_t samples, uint64_t curr_sample, stats *stat, MD5_CTX *ctx){
	simple_enc_encode(senc, set, input, samples, curr_sample, 1, stat);
	if(ctx && set->bps==16)//bps!=16 TODO
		MD5_Update(ctx, input+set->channels*curr_sample*2, samples*set->channels*2);
}

int simple_enc_eof(queue *q, simple_enc **senc, flac_settings *set, void *input, uint64_t *curr_sample, uint64_t tot_samples, uint64_t threshold, stats *stat, MD5_CTX *ctx, FILE *fout, int *outstate){
	if((tot_samples-*curr_sample)<=threshold){//EOF
		if(tot_samples-*curr_sample){
			if(ctx && set->bps==16)//bps!=16 TODO
				MD5_Update(ctx, input+set->channels**curr_sample*2, (tot_samples-*curr_sample)*set->channels*2);
			simple_enc_encode(*senc, set, input, tot_samples-*curr_sample, *curr_sample, 1, stat);//do analysis just to treat final frame the same as the rest
			*senc=simple_enc_out(q, *senc, set, input, curr_sample, stat, fout, outstate);//just add to queue, let analysis implementation flush when it deallocates queue
		}
		return 1;
	}
	else
		return 0;
}

/*Flush queue to file*/
void simple_enc_flush(queue *q, flac_settings *set, void *input, stats *stat, FILE *fout, int *outstate){
	size_t i;
	if(!q->depth)
		return;
	if(set->merge)
		queue_merge(q, set);
	if(set->tweak)
		queue_tweak(q, set);
	if(set->diff_comp_settings){//encode with output settings if necessary
		for(i=0;i<q->depth;++i){//OMP TODO
			*outstate+=set->outperc;			
			simple_enc_encode(q->sq[i], set, input, q->sq[i]->sample_cnt, q->sq[i]->curr_sample, (*outstate>=100)?0:2, stat);
			*outstate=*outstate%100;
		}
	}
	for(i=0;i<q->depth;++i)//dump to file
		stat->outsize+=fwrite_framestat(q->sq[i]->outbuf, q->sq[i]->outbuf_size, fout, &(set->minf), &(set->maxf));
	q->depth=0;//reset
}

/*Add analysed+chosen frame to output queue. Swap out simple_enc instance to an unused one, queue takes control of senc*/
simple_enc* simple_enc_out(queue *q, simple_enc *senc, flac_settings *set, void *input, uint64_t *curr_sample, stats *stat, FILE *fout, int *outstate){
	simple_enc *ret;
	if(set->queue_size && q->depth==set->queue_size)
		simple_enc_flush(q, set, input, stat, fout, outstate);
	(*curr_sample)+=senc->sample_cnt;
	ret=q->sq[q->depth];
	q->sq[q->depth++]=senc;
	return ret;
}

void queue_alloc(queue *q, flac_settings *set){
	size_t i;
	assert(set->queue_size>0);
	q->depth=0;
	q->sq=calloc(set->queue_size, sizeof(simple_enc*));
	for(i=0;i<set->queue_size;++i)
		q->sq[i]=calloc(1, sizeof(simple_enc));
}

void queue_dealloc(queue *q, flac_settings *set, void *input, stats *stat, FILE *fout, int *outstate){
	size_t i;
	simple_enc_flush(q, set, input, stat, fout, outstate);
	for(i=0;i<set->queue_size;++i)
		free(q->sq[i]);
	free(q->sq);
	q->sq=NULL;
}

/*Do merge passes on queue*/
void queue_merge(queue *q, flac_settings *set){//TODO
}

/*Do tweak passes on queue*/
void queue_tweak(queue *q, flac_settings *set){//TODO
}
