#include "common.h"
#include "load.h"
#include "seektable.h"

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
		_if((!FLAC__stream_decoder_process_single(in->dec)), "Fatal error decoding flac input (FLAC__stream_decoder_process_single), check input");
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

static size_t bwrite_u24be(uint64_t n, uint8_t *data){
	data[2]=(n    )&255;
	data[1]=(n>> 8)&255;
	data[0]=(n>>16)&255;
	return 3;
}
static size_t bwrite_u32be(uint64_t n, uint8_t *data){
	data[3]=(n    )&255;
	data[2]=(n>> 8)&255;
	data[1]=(n>>16)&255;
	data[0]=(n>>24)&255;
	return 4;
}
static size_t out_write_u32be(uint64_t n, output *out){
	uint8_t d[4];
	bwrite_u32be(n, d);
	return out_write(out, d, 4);
}
static size_t bwrite_u64be(uint64_t n, uint8_t *data){
	data[7]=n&255;
	data[6]=(n>>8)&255;
	data[5]=(n>>16)&255;
	data[4]=(n>>24)&255;
	data[3]=(n>>32)&255;
	data[2]=(n>>40)&255;
	data[1]=(n>>48)&255;
	data[0]=(n>>56)&255;
	return 8;
}
static size_t out_write_u64be(uint64_t n, output *out){
	uint8_t d[8];
	bwrite_u64be(n, d);
	return out_write(out, d, 8);
}
static size_t bwrite_u32le(uint64_t n, uint8_t *data){
	data[0]=(n    )&255;
	data[1]=(n>> 8)&255;
	data[2]=(n>>16)&255;
	data[3]=(n>>24)&255;
	return 4;
}
static size_t out_write_u32le(uint64_t n, output *out){
	uint8_t d[4];
	bwrite_u32le(n, d);
	return out_write(out, d, 4);
}

static void metadata_callback(const FLAC__StreamDecoder *dec, const FLAC__StreamMetadata *metadata, void *client_data){
	input *in=(input*)client_data;
	size_t i, j, written=0;
	uint8_t head[4], lastpad[4]={0x81, 0, 0, 0}, scratch[259];
	(void)dec;
	if(metadata->type==FLAC__METADATA_TYPE_STREAMINFO){
		in->set->sample_rate = metadata->data.stream_info.sample_rate;
		in->set->channels = metadata->data.stream_info.channels;
		in->set->bps = metadata->data.stream_info.bits_per_sample;
		in->set->encode_func=(in->set->bps==16)?FLAC__static_encoder_process_frame_bps16_interleaved:FLAC__static_encoder_process_frame_interleaved;
		in->set->input_tot_samples=metadata->data.stream_info.total_samples;
		memcpy(in->set->input_md5, metadata->data.stream_info.md5sum, 16);
		if(metadata->is_last)//let prepare_io know there's nothing to potentially preserve
			in->out->blocktype_containing_islast_flag=LAST_HEADER;
		return;
	}
	else if(!(in->set->preserve_flac_metadata))
		return;

	head[0]=(metadata->is_last?0x80:0)|metadata->type;//header is_last and type
	bwrite_u24be(metadata->length, head+1);//metadata header length
	switch(metadata->type){//preserve
	case FLAC__METADATA_TYPE_PADDING:
		if(metadata->is_last)
			out_write(in->out, lastpad, 4);
		break;

	case FLAC__METADATA_TYPE_APPLICATION://preserve
		_if((metadata->length<4), "APPLICATION metadata too short to be valid");
		written+=out_write(in->out, head, 4);
		written+=out_write(in->out, metadata->data.application.id, 4);
		if(metadata->length>4)
			written+=out_write(in->out, metadata->data.application.data, metadata->length-4);
		assert(written==(metadata->length+4));
		break;

	case FLAC__METADATA_TYPE_SEEKTABLE:
		if(metadata->is_last)
			out_write(in->out, lastpad, 4);
		break;

	case FLAC__METADATA_TYPE_VORBIS_COMMENT://preserve
		written+=out_write(in->out, head, 4);
		written+=out_write_u32le(metadata->data.vorbis_comment.vendor_string.length, in->out);
		written+=out_write(in->out, metadata->data.vorbis_comment.vendor_string.entry, metadata->data.vorbis_comment.vendor_string.length);
		written+=out_write_u32le(metadata->data.vorbis_comment.num_comments, in->out);
		for(i=0;i<metadata->data.vorbis_comment.num_comments;++i){
			written+=out_write_u32le(metadata->data.vorbis_comment.comments[i].length, in->out);
			written+=out_write(in->out, metadata->data.vorbis_comment.comments[i].entry, metadata->data.vorbis_comment.comments[i].length);
		}
		assert(written==(metadata->length+4));
		break;

	case FLAC__METADATA_TYPE_CUESHEET://preserve
		written+=out_write(in->out, head, 4);
		written+=out_write(in->out, metadata->data.cue_sheet.media_catalog_number, 128);
		written+=out_write_u64be(metadata->data.cue_sheet.lead_in, in->out);
		memset(scratch, 0, 259);
		scratch[0]|=(metadata->data.cue_sheet.is_cd)?0x80:0;
		written+=out_write(in->out, scratch, 259);
		scratch[0]=(metadata->data.cue_sheet.num_tracks)&255;
		written+=out_write(in->out, scratch, 1);
		for(i=0;i<metadata->data.cue_sheet.num_tracks;++i){
			written+=out_write_u64be(metadata->data.cue_sheet.tracks[i].offset, in->out);
			written+=out_write(in->out, &(metadata->data.cue_sheet.tracks[i].number), 1);
			written+=out_write(in->out, metadata->data.cue_sheet.tracks[i].isrc, 12);
			scratch[0]=(metadata->data.cue_sheet.tracks[i].type<<7)|(metadata->data.cue_sheet.tracks[i].pre_emphasis<<6);
			memset(scratch+1, 0, 13);
			written+=out_write(in->out, scratch, 14);
			written+=out_write(in->out, &(metadata->data.cue_sheet.tracks[i].num_indices), 1);
			memset(scratch, 0, 3);
			for(j=0;j<metadata->data.cue_sheet.tracks[i].num_indices;++j){
				written+=out_write_u64be(metadata->data.cue_sheet.tracks[i].indices[j].offset, in->out);
				written+=out_write(in->out, &(metadata->data.cue_sheet.tracks[i].indices[j].number), 1);
				written+=out_write(in->out, scratch, 3);
			}
		}
		assert(written==(metadata->length+4));
		break;

	case FLAC__METADATA_TYPE_PICTURE://preserve
		written+=out_write(in->out, head, 4);
		written+=out_write_u32be(metadata->data.picture.type, in->out);
		written+=out_write_u32be(strlen(metadata->data.picture.mime_type), in->out);
		written+=out_write(in->out, metadata->data.picture.mime_type, strlen(metadata->data.picture.mime_type));
		written+=out_write_u32be(strlen((char*)metadata->data.picture.description), in->out);
		written+=out_write(in->out, metadata->data.picture.description, strlen((char*)metadata->data.picture.description));
		written+=out_write_u32be(metadata->data.picture.width, in->out);
		written+=out_write_u32be(metadata->data.picture.height, in->out);
		written+=out_write_u32be(metadata->data.picture.depth, in->out);
		written+=out_write_u32be(metadata->data.picture.colors, in->out);
		written+=out_write_u32be(metadata->data.picture.data_length, in->out);
		written+=out_write(in->out, metadata->data.picture.data, metadata->data.picture.data_length);
		assert(written==(metadata->length+4));
		break;

	case FLAC__METADATA_TYPE_UNDEFINED://preserve
		written+=out_write(in->out, head, 4);
		if(metadata->length)
			written+=out_write(in->out, metadata->data.unknown.data, metadata->length);
		assert(written==(metadata->length+4));
		break;

	default:
		_("Unknown metadata type when preservation enabled");
	}
}

static void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data){
	(void)decoder, (void)client_data;
	fprintf(stderr, "Error callback status: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
	_("Flac input error callback triggered");
}

static int input_fopen_flac_init(input *in, char *path){
	FLAC__StreamDecoderInitStatus status;
	in->input_read=input_read_flac;
	in->dec=FLAC__stream_decoder_new();
	FLAC__stream_decoder_set_md5_checking(in->dec, true);
	if(in->set->preserve_flac_metadata)
		FLAC__stream_decoder_set_metadata_respond_all(in->dec);
	if((strcmp(path, "-")==0) && FLAC__STREAM_DECODER_INIT_STATUS_OK==(status=FLAC__stream_decoder_init_FILE(in->dec, stdin, write_callback, metadata_callback, error_callback, in)));
	else if((strcmp(path, "-")!=0) && (FLAC__STREAM_DECODER_INIT_STATUS_OK==(status=FLAC__stream_decoder_init_file(in->dec, path, write_callback, metadata_callback, error_callback, in))));
	else
	{
			fprintf(stderr, "Decoder init status: %s\n", FLAC__StreamDecoderInitStatusString[status]);
			_("Flac input decoder init failed");
	}

	_if((!FLAC__stream_decoder_process_single(in->dec)), "Failed to read flac input STREAMINFO");
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
		_("Currently piping not supported for wav input");//TODO
	}
	else
		_if((!drwav_init_file(&(in->wav), path, NULL)), "initializing wav decoder");

	in->set->sample_rate = in->wav.sampleRate;
	in->set->channels = in->wav.channels;
	in->set->bps = in->wav.bitsPerSample;
	_if((in->set->bps!=16), "Currently the only wav input support is 16 bit");
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
	_if(((in->cdda=(strcmp(path, "-")==0)?stdin:fopen(path, "rb"))==NULL), "Failed to fopen CDDA input");

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
		return input_fopen_flac_init(in, path);
	else if((set->input_format && strcmp(set->input_format, "wav")==0) || (strlen(path)>3 && strcmp(".wav", path+strlen(path)-4)==0))
		return input_fopen_wav(in, path);
	else if((set->input_format && strcmp(set->input_format, "cdda")==0) || (strlen(path)>3 && strcmp(".bin", path+strlen(path)-4)==0))
		return input_fopen_cdda(in, path);
	_("Unknown input format, use --input-format if format cannot be determined from extension");
	return 0;
}

/* all-in-wonder function that handles all I/O to the point of being ready to encode frames

	* Opens output for writing
	* Opens input for reading
	* Sizes output seektable based on input metadata, if seektable used
	* Optionally Preserves flac input metadata
	* Writes all that making sure the is_last flag is set only for the last written metadata block,
	  which could be the header, the seektable or the last preserved block
	  Because we write preserved blocks as encountered, the last block in
	  input may not be something to preserve, so an ampty padding block is added
*/
void prepare_io(input *in, char *ipath, output *out, char *opath, uint8_t *header, flac_settings *set){
	//open output for writing
	_if((!(out_open(out, opath, set->seek))), "Failed to open output");
	in->out=out;
	//open input for reading
	_if((!input_fopen(in, ipath, set)), "Failed to open input");
	//change subset based on input samplerate
	if(!set->lax){
		if(set->blocksize_limit_lower>((set->sample_rate<=48000)?4608:16384))
			set->blocksize_limit_lower=(set->sample_rate<=48000)?4608:16384;
		if(!set->blocksize_limit_upper || set->blocksize_limit_upper>((set->sample_rate<=48000)?4608:16384))
			set->blocksize_limit_upper=(set->sample_rate<=48000)?4608:16384;
		if(set->sample_rate<=48000)
			set->lpc_order_limit=12;
		set->rice_order_limit=8;
	}
	else if(!set->blocksize_limit_upper)
		set->blocksize_limit_upper=65535;
	_if((set->mode!=MODE_FIXED && set->blocksize_limit_lower==set->blocksize_limit_upper), "Variable encode modes need a range to work with");
	//populate header with best known information, in case seeking to update isn't possible
	if(set->mode==MODE_FIXED){
		header[ 8]=(set->blocksize_min>>8)&255;
		header[ 9]=(set->blocksize_min>>0)&255;
		header[10]=(set->blocksize_min>>8)&255;
		header[11]=(set->blocksize_min>>0)&255;
	}
	else{
		header[ 8]=(set->blocksize_limit_lower>>8)&255;
		header[ 9]=(set->blocksize_limit_lower>>0)&255;
		header[10]=(set->blocksize_limit_upper>>8)&255;
		header[11]=(set->blocksize_limit_upper>>0)&255;
	}
	header[18]=(set->sample_rate>>12)&255;
	header[19]=(set->sample_rate>> 4)&255;
	header[20]=((set->sample_rate&15)<<4)|((set->channels-1)<<1)|(((set->bps-1)>>4)&1);
	header[21]=(((set->bps-1)&15)<<4);
	//determine seektable size
	seektable_init(&(out->seektable), set);//between ihead read ohead write
	//determine which blocktype is last
	if((out->blocktype_containing_islast_flag==LAST_UNDEFINED)//more than just STREAMINFO
		&& in->dec && set->preserve_flac_metadata)//preserve
		out->blocktype_containing_islast_flag=LAST_PRESERVED;
	else
		out->blocktype_containing_islast_flag=(set->seektable!=0)?LAST_SEEKTABLE:LAST_HEADER;
	//write header
	if(out->blocktype_containing_islast_flag==LAST_HEADER)
		header[4]|=0x80;
	out_write(out, header, 42);
	//write seektable if applicable
	seektable_write_dummy(&(out->seektable), set, out);
	//finish reading flac input metadata, preserving is done in metadata callback
	_if((in->dec && !FLAC__stream_decoder_process_until_end_of_metadata(in->dec)), "Failed to read flac input metadata");
	out->seektable.firstframe_loc=out->outloc;
}
