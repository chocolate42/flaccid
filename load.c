#include "common.h"
#include "load.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/*Implement OpenSSL MD5 API with mbedtls*/
#ifndef USE_OPENSSL
static void MD5_Init(MD5_CTX *ctx){
	mbedtls_md5_init(ctx);
	mbedtls_md5_starts(ctx);
}

static void MD5_Final(uint8_t *h, MD5_CTX *ctx){
	mbedtls_md5_finish(ctx, h);
}

static int MD5_Update(MD5_CTX* ctx, const unsigned char *d, size_t s){
	return mbedtls_md5_update(ctx, d, s);
}
#endif

static void MD5_UpdateSamplesRelative(MD5_CTX *ctx, const void *inp, size_t sample_cnt, flac_settings *set){
	size_t i, j, width;
	if(set->bps==16)
		MD5_Update(ctx, inp, sample_cnt*2*set->channels);//16
	else if(set->bps==32)
		MD5_Update(ctx, inp, sample_cnt*4*set->channels);//32
	else{
		width=set->bps==8?1:(set->bps==12?2:3);//8/12/20/24
		for(i=0;i<sample_cnt;++i){
			for(j=0;j<set->channels;++j)
				MD5_Update(ctx, inp+(i*4*set->channels)+(j*4), width);
		}
	}
}

/*Input buffer maintains all input being processed
	Two separate processing phases, analysis and output
	Buffer: |DDDDDOOOOOAAAAAAEE|
	         ^    ^    ^
	         ^    ^    loc_analysis
	         ^    loc_output
	         loc_buffer

	D: Samples we are done with, they have been output encoded and will be discarded on the next input read
	O: Samples waiting to be output encoded
	A: Samples being analysed, before analysing a chunk of input the ideal number of samples to work with is requested
	E: Samples that analysis hasn't requested, but have been loaded anyway (probably end of an input flac frame)

	* loc_analysis is updated as frames are sent to the output queue
	* loc_output is updated by the queue as output frames are encoded

	large queue size means large O section, wide analysis means large A section

	What makes implementation simpler is that all multithreading is restricted to a section, ie
	input-request/analysis/output-encoding may be multithreaded internally but control passing
	from section to section is single-threaded
*/

//try and read sample_cnt samples from input, if available at least sample_cnt samples unhandled by analysis will be in the buffer
//also shift buffer to remove samples handled by output buffer
static size_t input_read_flac(input *in, size_t sample_cnt){
	if(in->sample_cnt>=sample_cnt)
		return in->sample_cnt;
	//max frame is 65535, so overallocating by more means we should always have enough buffer
	in->buf=realloc(in->buf, ((in->loc_analysis-in->loc_buffer)+sample_cnt+65536)*(in->set->bps==16?2:4)*in->set->channels);
	while(in->sample_cnt<sample_cnt){
		if(!FLAC__stream_decoder_process_single(in->dec))
			goodbye("Error: Fatal error decoding flac input (FLAC__stream_decoder_process_single), check input\n");
		if(FLAC__STREAM_DECODER_END_OF_STREAM==FLAC__stream_decoder_get_state(in->dec))
			break;
	}
	return in->sample_cnt;
}

static void input_close_flac(input *in){
	if(in->set->md5)
		MD5_Final(in->set->hash, &(in->ctx));
}

//assumes alloc has been done
static FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *dec, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data){
	input *in=(input*)client_data;
	size_t i, j, index=0;
	int16_t *raw16;
	int32_t *raw32;
	(void)dec;
	if(in->set->bps==16){
		raw16=in->buf;
		raw16+=(((in->loc_analysis-in->loc_buffer)+in->sample_cnt)*in->set->channels);
		for(i=0;i<frame->header.blocksize;++i){
			for(j=0;j<in->set->channels;++j)
				raw16[index++]=(FLAC__int16)buffer[j][i];
		}
		if(in->set->md5)
			MD5_UpdateSamplesRelative(&(in->ctx), raw16, frame->header.blocksize, in->set);
	}
	else{
		raw32=in->buf;
		raw32+=(((in->loc_analysis-in->loc_buffer)+in->sample_cnt)*in->set->channels);
		for(i=0;i<frame->header.blocksize;++i){
			for(j=0;j<in->set->channels;++j)
				raw32[index++]=buffer[j][i];
		}
		if(in->set->md5)
			MD5_UpdateSamplesRelative(&(in->ctx), raw32, frame->header.blocksize, in->set);
	}
	in->sample_cnt+=frame->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_callback(const FLAC__StreamDecoder *dec, const FLAC__StreamMetadata *metadata, void *client_data){
	input *in=(input*)client_data;
	(void)dec;
	if(metadata->type==FLAC__METADATA_TYPE_STREAMINFO){
		in->set->sample_rate = metadata->data.stream_info.sample_rate;
		in->set->channels = metadata->data.stream_info.channels;
		in->set->bps = metadata->data.stream_info.bits_per_sample;
		in->set->encode_func=(in->set->bps==16)?FLAC__static_encoder_process_frame_bps16_interleaved:FLAC__static_encoder_process_frame_interleaved;
		in->set->input_tot_samples=metadata->data.stream_info.total_samples;
		memcpy(in->set->input_md5, metadata->data.stream_info.md5sum, 16);
	}
}

static void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data){
	(void)decoder, (void)client_data;
	fprintf(stderr, "Got error callback: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
	goodbye("");
}

static int input_fopen_flac(input *in, char *path){
	FLAC__StreamDecoderInitStatus status;
	in->input_read=input_read_flac;
	in->input_close=input_close_flac;
	in->dec=FLAC__stream_decoder_new();
	FLAC__stream_decoder_set_md5_checking(in->dec, true);
	if(strcmp(path, "-")==0){
		if(FLAC__STREAM_DECODER_INIT_STATUS_OK!=(status=FLAC__stream_decoder_init_FILE(in->dec, stdin, write_callback, metadata_callback, error_callback, in))){
			fprintf(stderr, "ERROR: initializing decoder: %s\n", FLAC__StreamDecoderInitStatusString[status]);
			goodbye("");
		}
	}
	else{
		if(FLAC__STREAM_DECODER_INIT_STATUS_OK!=(status=FLAC__stream_decoder_init_file(in->dec, path, write_callback, metadata_callback, error_callback, in))){
			fprintf(stderr, "ERROR: initializing decoder: %s\n", FLAC__StreamDecoderInitStatusString[status]);
			goodbye("");
		}
	}

	if(!FLAC__stream_decoder_process_until_end_of_metadata(in->dec))
		goodbye("Error: Failed to read flac input metadata\n");
	return 1;
}

int input_fopen(input *in, char *path, flac_settings *set){
	in->set=set;
	if(in->set->md5)
		MD5_Init(&(in->ctx));
	if((set->input_format && strcmp(set->input_format, "flac")==0) || (strlen(path)>4 && strcmp(".flac", path+strlen(path)-5)==0))
		return input_fopen_flac(in, path);
	return 0;
	//else if(strlen(path)>3 && strcmp(".wav", path+strlen(path)-4)==0)
	//	return input_fopen_wav(input, path, set);
	//else
	//	return input_fopen_raw(input, path, set);
}
