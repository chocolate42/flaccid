#include "common.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/*Implement OpenSSL MD5 API with mbedtls*/
#ifndef USE_OPENSSL
void MD5_Init(MD5_CTX *ctx){
	mbedtls_md5_init(ctx);
	mbedtls_md5_starts(ctx);
}

void MD5_Final(uint8_t *h, MD5_CTX *ctx){
	mbedtls_md5_finish(ctx, h);
}

int MD5(const unsigned char *d, size_t s, unsigned char *h){
	return mbedtls_md5(d, s, h);
}

int MD5_Update(MD5_CTX* ctx, const unsigned char *d, size_t s){
	return mbedtls_md5_update(ctx, d, s);
}
#endif

void MD5_UpdateSamples(MD5_CTX *ctx, const void *input, size_t curr_sample, size_t sample_cnt, flac_settings *set){
	size_t i, j, width;
	if(set->bps==16)
		MD5_Update(ctx, input+(curr_sample*2*set->channels), sample_cnt*2*set->channels);
	else if(set->bps==32)
		MD5_Update(ctx, input+(curr_sample*4*set->channels), sample_cnt*4*set->channels);
	else{
		width=set->bps==8?1:(set->bps==12?2:3);
		for(i=0;i<sample_cnt;++i){
			for(j=0;j<set->channels;++j)
				MD5_Update(ctx, input+((curr_sample+i)*4*set->channels)+(j*4), width);
		}
	}
}

void goodbye(char *s){
	fprintf(stderr, "%s", s);
	exit(1);
}

FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, char *comp, char *apod){
	FLAC__StaticEncoder *r;
	r=FLAC__static_encoder_new();
	r->is_variable_blocksize=set->mode==4?0:1;
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
		FLAC__stream_encoder_set_max_lpc_order(r->stream_encoder, (atoi(strchr(comp, 'l')+1)<=set->lpc_order_limit)?atoi(strchr(comp, 'l')+1):set->lpc_order_limit);
	if(strchr(comp, 'm'))
		FLAC__stream_encoder_set_do_mid_side_stereo(r->stream_encoder, true);
	if(strchr(comp, 'p'))
		FLAC__stream_encoder_set_do_qlp_coeff_prec_search(r->stream_encoder, true);
	if(strchr(comp, 'q'))
		FLAC__stream_encoder_set_qlp_coeff_precision(r->stream_encoder, atoi(strchr(comp, 'q')+1));
	if(strchr(comp, 'r')&&strchr(comp, ',')){
		FLAC__stream_encoder_set_min_residual_partition_order(r->stream_encoder, (atoi(strchr(comp, 'r')+1)<=set->rice_order_limit)?atoi(strchr(comp, 'r')+1):set->rice_order_limit);
		FLAC__stream_encoder_set_max_residual_partition_order(r->stream_encoder, (atoi(strchr(comp, ',')+1)<=set->rice_order_limit)?atoi(strchr(comp, ',')+1):set->rice_order_limit);
	}
	else if(strchr(comp, 'r'))
		FLAC__stream_encoder_set_max_residual_partition_order(r->stream_encoder, (atoi(strchr(comp, 'r')+1)<=set->rice_order_limit)?atoi(strchr(comp, 'r')+1):set->rice_order_limit);

	if(apod)
		FLAC__stream_encoder_set_apodization(r->stream_encoder, apod);

	FLAC__stream_encoder_set_blocksize(r->stream_encoder, blocksize);/* override compression level blocksize */
	FLAC__stream_encoder_set_loose_mid_side_stereo(r->stream_encoder, false);/* override adaptive mid-side, this doesn't play nice */

	if(FLAC__static_encoder_init(r)!=FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		goodbye("Init failed\n");

	return r;
}

void print_settings(flac_settings *set){
	char *modes[]={"chunk", "gset", "peakset", "gasc", "fixed"};
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
	uint64_t anal=0, out=0, tweak=0, merge=0, i;
	for(i=0;i<stat->work_count;++i){
		anal+=stat->effort_anal[i];
		out+=stat->effort_output[i];
		tweak+=stat->effort_tweak[i];
		merge+=stat->effort_merge[i];
	}
	printf("\teffort\tanalysis(%.3f);tweak(%.3f);merge(%.3f);output(%.3f)", ((double)anal)/stat->tot_samples, ((double)tweak)/stat->tot_samples, ((double)merge)/stat->tot_samples, ((double)out)/stat->tot_samples);
	printf("\tsize\t%zu\tcpu_time\t%.5f\n", stat->outsize+42, stat->cpu_time);
}

static void simple_enc_encode(simple_enc *senc, flac_settings *set, void *input, uint32_t samples, uint64_t curr_sample, int is_anal, stats *stat){
	assert(senc&&set&&input);
	assert(samples);
	if(senc->enc)
		FLAC__static_encoder_delete(senc->enc);
	senc->enc=init_static_encoder(set, samples<16?16:samples, is_anal==1?set->comp_anal:(is_anal==0?set->comp_output:set->comp_outputalt), is_anal==1?set->apod_anal:(is_anal==0?set->apod_output:set->apod_outputalt));
	senc->sample_cnt=samples;
	senc->curr_sample=curr_sample;
	set->encode_func(senc->enc, input+curr_sample*set->channels*(set->bps==16?2:4), samples, curr_sample, &(senc->outbuf), &(senc->outbuf_size));//do encode
	if(stat&&(is_anal==1))
		stat->effort_anal[omp_get_thread_num()]+=samples;
	else if(stat)
		stat->effort_output[omp_get_thread_num()]+=samples;
}

void simple_enc_analyse(simple_enc *senc, flac_settings *set, void *input, uint32_t samples, uint64_t curr_sample, stats *stat, MD5_CTX *ctx){
	simple_enc_encode(senc, set, input, samples, curr_sample, 1, stat);
	if(ctx && set->md5)
		MD5_UpdateSamples(ctx, input, curr_sample, samples, set);
}

int simple_enc_eof(queue *q, simple_enc **senc, flac_settings *set, void *input, uint64_t *curr_sample, uint64_t tot_samples, uint64_t threshold, stats *stat, MD5_CTX *ctx, FILE *fout){
	if((tot_samples-*curr_sample)<threshold){//EOF
		if(tot_samples-*curr_sample){
			if(ctx && set->md5)
				MD5_UpdateSamples(ctx, input, *curr_sample, tot_samples-*curr_sample, set);
			simple_enc_encode(*senc, set, input, tot_samples-*curr_sample, *curr_sample, 1, stat);//do analysis just to treat final frame the same as the rest
			*senc=simple_enc_out(q, *senc, set, input, curr_sample, stat, fout);//just add to queue, let analysis implementation flush when it deallocates queue
		}
		return 1;
	}
	return 0;
}

void simple_enc_dealloc(simple_enc *senc){
	FLAC__static_encoder_delete(senc->enc);
	free(senc);
}

static size_t qmerge(queue *q, flac_settings *set, void *input, stats *stat, int i, size_t *saved){
	simple_enc *a;
	if(!(q->sq[i]->sample_cnt) || !(q->sq[i+1]->sample_cnt))
		return 0;
	if((q->sq[i]->sample_cnt+q->sq[i+1]->sample_cnt)>set->blocksize_limit_upper)
		return 0;
	a=calloc(1, sizeof(simple_enc));
	stat->effort_merge[omp_get_thread_num()]+=q->sq[i]->sample_cnt+q->sq[i+1]->sample_cnt;
	simple_enc_analyse(a, set, input, q->sq[i]->sample_cnt+q->sq[i+1]->sample_cnt, q->sq[i]->curr_sample, NULL, NULL);
	if(a->outbuf_size<(q->sq[i]->outbuf_size+q->sq[i+1]->outbuf_size)){
		(*saved)+=(q->sq[i]->outbuf_size+q->sq[i+1]->outbuf_size) - a->outbuf_size;
		FLAC__static_encoder_delete(q->sq[i+1]->enc);//simple_enc only deletes previous if sample_cnt>0, and we're manually messing with that
		q->sq[i+1]->enc=NULL;
		q->sq[i+1]->sample_cnt=0;
		simple_enc_dealloc(q->sq[i]);
		q->sq[i]=a;
		return 1;
	}
	simple_enc_dealloc(a);
	return 0;
}

static int senc_comp_merge(const void *aa, const void *bb){
	simple_enc *a=*(simple_enc**)aa;
	simple_enc *b=*(simple_enc**)bb;
	if(!a->sample_cnt)
		return 1;
	if(!b->sample_cnt)
		return -1;
	return (a->curr_sample<b->curr_sample)?-1:1;
}

/*Do merge passes on queue*/
static void queue_merge(queue *q, flac_settings *set, void *input, stats *stat){
	size_t i, ind=0, merged=0, *saved, saved_tot;
	if(!set->merge)
		return;
	saved=calloc(set->work_count, sizeof(size_t));
	do{
		merged=0;
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<q->depth/2;++i){//even pairs
			merged+=qmerge(q, set, input, stat, 2*i, &saved[omp_get_thread_num()]);
		}
		#pragma omp barrier

		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<(q->depth-1)/2;++i){//odd pairs
			merged+=qmerge(q, set, input, stat, (2*i)+1, &saved[omp_get_thread_num()]);
		}
		#pragma omp barrier

		//shift empty simple_enc to end
		if(merged)
			qsort(q->sq, q->depth, sizeof(simple_enc*), senc_comp_merge);
		q->depth-=merged;

		saved_tot=0;
		for(i=0;i<set->work_count;++i){
			saved_tot+=saved[i];
			saved[i]=0;
		}
		++ind;
		if(saved_tot)
			fprintf(stderr, "merge(%zu) saved %zu bytes with %zu merges\n", ind, saved_tot, merged);
	}while(saved_tot>=set->merge);
	free(saved);
}

static void qtweak(queue *q, flac_settings *set, void *input, stats *stat, int i, size_t newsplit, size_t *saved){
	simple_enc *a, *b;
	size_t bsize, tot=q->sq[i]->sample_cnt+q->sq[i+1]->sample_cnt;

	if(newsplit<16 || newsplit>=(tot-16))
		return;
	if(newsplit>set->blocksize_limit_upper || newsplit<set->blocksize_limit_lower)
		return;
	bsize=tot-newsplit;
	if(bsize>set->blocksize_limit_upper || bsize<set->blocksize_limit_lower)
		return;

	a=calloc(1, sizeof(simple_enc));
	b=calloc(1, sizeof(simple_enc));
	stat->effort_tweak[omp_get_thread_num()]+=q->sq[i]->sample_cnt+q->sq[i+1]->sample_cnt;
	simple_enc_analyse(a, set, input, newsplit, q->sq[i]->curr_sample, NULL, NULL);
	simple_enc_analyse(b, set, input, bsize, q->sq[i]->curr_sample+newsplit, NULL, NULL);
	if((a->outbuf_size+b->outbuf_size)<(q->sq[i]->outbuf_size+q->sq[i+1]->outbuf_size)){
		(*saved)+=((q->sq[i]->outbuf_size+q->sq[i+1]->outbuf_size) - (a->outbuf_size+b->outbuf_size));
		simple_enc_dealloc(q->sq[i]);
		simple_enc_dealloc(q->sq[i+1]);
		q->sq[i]=a;
		q->sq[i+1]=b;
	}
	else{
		simple_enc_dealloc(a);
		simple_enc_dealloc(b);
	}
}

/*Do tweak passes on queue*/
static void queue_tweak(queue *q, flac_settings *set, void *input, stats *stat){
	size_t i, ind=0, saved_tot;
	if(!set->tweak)
		return;
	do{
		for(i=0;i<set->work_count;++i)
			q->saved[i]=0;

		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<q->depth/2;++i){//even pairs
			size_t pivot=q->sq[2*i]->sample_cnt;
			qtweak(q, set, input, stat, 2*i, pivot-(set->blocks[0]/(ind+2)), &(q->saved[omp_get_thread_num()]));
			qtweak(q, set, input, stat, 2*i, pivot+(set->blocks[0]/(ind+2)), &(q->saved[omp_get_thread_num()]));
		}
		#pragma omp barrier

		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<(q->depth-1)/2;++i){//odd pairs
			size_t pivot=q->sq[(2*i)+1]->sample_cnt;
			qtweak(q, set, input, stat, (2*i)+1, pivot-(set->blocks[0]/(ind+2)), &(q->saved[omp_get_thread_num()]));
			qtweak(q, set, input, stat, (2*i)+1, pivot+(set->blocks[0]/(ind+2)), &(q->saved[omp_get_thread_num()]));
		}
		#pragma omp barrier

		saved_tot=0;
		for(i=0;i<set->work_count;++i)
			saved_tot+=q->saved[i];
		++ind;
		if(saved_tot)
			fprintf(stderr, "tweak(%zu) saved %zu bytes\n", ind, saved_tot);
	}while(saved_tot>=set->tweak);
}

/*Flush queue to file*/
static void simple_enc_flush(queue *q, flac_settings *set, void *input, stats *stat, FILE *fout){
	size_t i;
	if(!q->depth)
		return;
	if(set->merge)
		queue_merge(q, set, input, stat);
	if(set->tweak)
		queue_tweak(q, set, input, stat);
	if(set->diff_comp_settings){//encode with output settings if necessary
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<q->depth;++i){
			q->outstate[omp_get_thread_num()]+=set->outperc;
			simple_enc_encode(q->sq[i], set, input, q->sq[i]->sample_cnt, q->sq[i]->curr_sample, (q->outstate[omp_get_thread_num()]>=100)?0:2, stat);
			q->outstate[omp_get_thread_num()]%=100;
		}
		#pragma omp barrier
	}
	for(i=0;i<q->depth;++i){//dump to file
		if(q->sq[i]->outbuf_size<set->minf)
			set->minf=q->sq[i]->outbuf_size;
		if(q->sq[i]->outbuf_size>set->maxf)
			set->maxf=q->sq[i]->outbuf_size;
		if(q->sq[i]->sample_cnt<set->blocksize_min)
			set->blocksize_min=q->sq[i]->sample_cnt;
		if(q->sq[i]->sample_cnt>set->blocksize_max)
			set->blocksize_max=q->sq[i]->sample_cnt;
		stat->outsize+=fwrite(q->sq[i]->outbuf, 1, q->sq[i]->outbuf_size, fout);
	}
	q->depth=0;//reset
}

/*Add analysed+chosen frame to output queue. Swap out simple_enc instance to an unused one, queue takes control of senc*/
simple_enc* simple_enc_out(queue *q, simple_enc *senc, flac_settings *set, void *input, uint64_t *curr_sample, stats *stat, FILE *fout){
	simple_enc *ret;
	if(q->depth==set->queue_size)
		simple_enc_flush(q, set, input, stat, fout);
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
	q->outstate=calloc(set->work_count, sizeof(int));
	q->saved=calloc(set->work_count, sizeof(size_t));
}

void queue_dealloc(queue *q, flac_settings *set, void *input, stats *stat, FILE *fout){
	size_t i;
	simple_enc_flush(q, set, input, stat, fout);
	for(i=0;i<set->queue_size;++i)
		simple_enc_dealloc(q->sq[i]);
	free(q->sq);
	q->sq=NULL;
	free(q->outstate);
	q->outstate=NULL;
	free(q->saved);
	q->saved=NULL;
}

void mode_boilerplate_init(flac_settings *set, clock_t *cstart, MD5_CTX *ctx, queue *q, stats *stat, size_t input_size){
	if(set->md5)
		MD5_Init(ctx);
	*cstart=clock();
	stat->tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	stat->work_count=set->work_count;
	stat->effort_anal=calloc(set->work_count, sizeof(uint64_t));
	stat->effort_output=calloc(set->work_count, sizeof(uint64_t));
	stat->effort_tweak=calloc(set->work_count, sizeof(uint64_t));
	stat->effort_merge=calloc(set->work_count, sizeof(uint64_t));
	queue_alloc(q, set);
}

void mode_boilerplate_finish(flac_settings *set, clock_t *cstart, MD5_CTX *ctx, queue *q, stats *stat, void *input, FILE *fout){
	queue_dealloc(q, set, input, stat, fout);
	if(set->md5)
		MD5_Final(set->hash, ctx);
	stat->cpu_time=((double)(clock()-*cstart))/CLOCKS_PER_SEC;
	print_settings(set);
	print_stats(stat);
}
