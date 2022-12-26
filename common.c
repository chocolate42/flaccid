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
