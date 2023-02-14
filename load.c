#include "common.h"
#include "load.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

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

static void input_close(input *in){
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

static size_t input_read_wav(input *in, size_t sample_cnt){
	int16_t *raw16;
	int32_t *raw32;
	size_t amount;
	if(in->sample_cnt>=sample_cnt)
		return in->sample_cnt;
	//max frame is 65535, so overallocating by more means we should always have enough buffer
	in->buf=realloc(in->buf, ((in->loc_analysis-in->loc_buffer)+sample_cnt+65536)*(in->set->bps==16?2:4)*in->set->channels);
	if(in->set->bps==16){
		raw16=in->buf;
		raw16+=(((in->loc_analysis-in->loc_buffer)+in->sample_cnt)*in->set->channels);
		amount=drwav_read_pcm_frames_s16(&(in->wav), sample_cnt-in->sample_cnt, raw16);
		if(in->set->md5)
			MD5_UpdateSamplesRelative(&(in->ctx), raw16, amount, in->set);
	}
	else{//currently broken fix TODO
		raw32=in->buf;
		raw32+=(((in->loc_analysis-in->loc_buffer)+in->sample_cnt)*in->set->channels);
		amount=drwav_read_pcm_frames_s32(&(in->wav), sample_cnt-in->sample_cnt, raw32);
		if(in->set->md5)
			MD5_UpdateSamplesRelative(&(in->ctx), raw32, amount, in->set);
	}
	in->sample_cnt+=amount;
	return in->sample_cnt;
}

static int input_fopen_wav(input *in, char *path){
	in->input_read=input_read_wav;

	if(strcmp(path, "-")==0){
		//drwav doesn't seem to have convenient FILE* functions
		//so something might have to be done with raw memory
		goodbye("Error: Currently piping not supported for wav input\n");//TODO
	}
	else{
		if(!drwav_init_file(&(in->wav), path, NULL))
			goodbye("Error: initializing wav decoder\n");
	}

	in->set->sample_rate = in->wav.sampleRate;
	in->set->channels = in->wav.channels;
	in->set->bps = in->wav.bitsPerSample;
	if(in->set->bps!=16)
		goodbye("Error: Currently the only wav input support is 16 bit\n");
	in->set->encode_func=(in->set->bps==16)?FLAC__static_encoder_process_frame_bps16_interleaved:FLAC__static_encoder_process_frame_interleaved;
	in->set->input_tot_samples=in->wav.totalPCMFrameCount;

	return 1;
}

static size_t input_read_cdda(input *in, size_t sample_cnt){
	size_t amount;
	if(in->sample_cnt>=sample_cnt)
		return in->sample_cnt;
	//max frame is 65535, so overallocating by more means we should always have enough buffer
	in->buf=realloc(in->buf, ((in->loc_analysis-in->loc_buffer)+sample_cnt+65536)*4);
	amount=fread(((uint8_t*)in->buf)+(in->loc_analysis-in->loc_buffer)*4, 1, (sample_cnt-in->sample_cnt)*4, in->cdda)/4;
	if(in->set->md5)
		MD5_UpdateSamplesRelative(&(in->ctx), ((uint8_t*)in->buf)+(in->loc_analysis-in->loc_buffer)*4, amount, in->set);
	in->sample_cnt+=amount;
	return in->sample_cnt;
}

static int input_fopen_cdda(input *in, char *path){
	in->input_read=input_read_cdda;
	if((in->cdda=(strcmp(path, "-")==0)?stdin:fopen(path, "rb"))==NULL)
		goodbye("Error: Failed to fopen CDDA input\n");

	in->set->sample_rate = 44100;
	in->set->channels = 2;
	in->set->bps = 16;
	in->set->encode_func=FLAC__static_encoder_process_frame_bps16_interleaved;
	in->set->input_tot_samples=0;
	return 1;
}

int input_fopen(input *in, char *path, flac_settings *set){
	in->set=set;
	in->input_close=input_close;
	if(in->set->md5)
		MD5_Init(&(in->ctx));
	if((set->input_format && strcmp(set->input_format, "flac")==0) || (strlen(path)>4 && strcmp(".flac", path+strlen(path)-5)==0))
		return input_fopen_flac(in, path);
	else if((set->input_format && strcmp(set->input_format, "wav")==0) || (strlen(path)>3 && strcmp(".wav", path+strlen(path)-4)==0))
		return input_fopen_wav(in, path);
	else if((set->input_format && strcmp(set->input_format, "cdda")==0) || (strlen(path)>3 && strcmp(".bin", path+strlen(path)-4)==0))
		return input_fopen_cdda(in, path);
	goodbye("Error: Unknown input format, use --input-format if format cannot be determined from extension\n");
	return 0;
}
