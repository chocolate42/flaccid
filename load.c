#include "load.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

void *load_input(char *path, size_t *input_size, flac_settings *set){
	if(strlen(path)>3 && strcmp(".wav", path+strlen(path)-4)==0)
		printf("Warning: .wav input not currently supported, input will be treated as raw\n");
	if(strlen(path)<5 || strcmp(".flac", path+strlen(path)-5)!=0)//load raw
		return load_raw(path, input_size, set);
	else
		return load_flac(path, input_size, set);
}

void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data);
void metadata_callback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data);
FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data);

struct flac_loader{
	void *input;
	size_t tot_samples, loc;
	flac_settings *set;
};

void *load_flac(char *path, size_t *input_size, flac_settings *set){
	FLAC__StreamDecoder *dec;
	FLAC__StreamDecoderInitStatus status;
	struct flac_loader loader;

	loader.set=set;
	loader.loc=0;
	dec=FLAC__stream_decoder_new();
	FLAC__stream_decoder_set_md5_checking(dec, true);

	if(FLAC__STREAM_DECODER_INIT_STATUS_OK!=(status=FLAC__stream_decoder_init_file(dec, path, write_callback, metadata_callback, error_callback, &loader))){
		fprintf(stderr, "ERROR: initializing decoder: %s\n", FLAC__StreamDecoderInitStatusString[status]);
		goodbye("");
	}

	FLAC__stream_decoder_process_until_end_of_stream(dec);
	assert(loader.tot_samples==loader.loc);
	FLAC__stream_decoder_delete(dec);
	*input_size=loader.tot_samples*set->channels*(set->bps==16?2:4);
	return loader.input;
}

FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *dec, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data){
	struct flac_loader *fl=(struct flac_loader *)client_data;
	size_t i, j, index=0;
	int16_t *raw16;
	int32_t *raw32;
	(void)dec;
	if(fl->set->bps==16){
		raw16=fl->input;
		for(i=0;i<frame->header.blocksize;++i){
			for(j=0;j<fl->set->channels;++j)
				raw16[(fl->loc*fl->set->channels)+index++]=(FLAC__int16)buffer[j][i];
		}
	}
	else{
		raw32=fl->input;
		for(i=0;i<frame->header.blocksize;++i){
			for(j=0;j<fl->set->channels;++j)
				raw32[(fl->loc*fl->set->channels)+index++]=buffer[j][i];
		}
	}
	fl->loc+=frame->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void metadata_callback(const FLAC__StreamDecoder *dec, const FLAC__StreamMetadata *metadata, void *client_data){
	struct flac_loader *fl=(struct flac_loader *)client_data;
	(void)dec;
	if(metadata->type==FLAC__METADATA_TYPE_STREAMINFO){
		/* save for later */
		if(0==(fl->tot_samples=metadata->data.stream_info.total_samples))
			goodbye("ERROR: This example only works for FLAC files that have a total_samples count in STREAMINFO\n");
		fl->set->sample_rate = metadata->data.stream_info.sample_rate;
		fl->set->channels = metadata->data.stream_info.channels;
		fl->set->bps = metadata->data.stream_info.bits_per_sample;
		fl->input=malloc(fl->tot_samples*fl->set->channels*(fl->set->bps==16?2:4));
		fl->set->encode_func=(fl->set->bps==16)?FLAC__static_encoder_process_frame_bps16_interleaved:FLAC__static_encoder_process_frame_interleaved;
	}
}

void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data){
	(void)decoder, (void)client_data;
	fprintf(stderr, "Got error callback: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
	goodbye("");
}

#define NO_MMAP

void *load_raw(char *path, size_t *input_size, flac_settings *set){
	void *input;
	#ifdef NO_MMAP
	FILE *fin;
	#else
	int fd;
	struct stat sb;
	#endif

	#ifdef NO_MMAP
	//for now have windows read the entire input immediately instead of using mmap
	//reduces accuracy of speed comparison but at least allows windows users to play
	//for large effort runs this hack shouldn't matter much to speed comparisons
	if(!(fin=fopen(path, "rb")))
		goodbye("Error: fopen() input failed\n");
	if(-1==fseek(fin, 0, SEEK_END))
		goodbye("Error: fseek() input failed\n");
	if(-1==ftell(fin))
		goodbye("Error: ftell() input failed\n");
	*input_size=ftell(fin);
	rewind(fin);
	if(!input_size)
		goodbye("Error: Input empty");
	if(!(input=malloc(*input_size)))
		goodbye("Error: malloc() input failed\n");
	if(*input_size!=fread(input, 1, *input_size, fin))
		goodbye("Error: fread() input failed\n");
	fclose(fin);
	#else
	if(-1==(fd=open(path, O_RDONLY)))
		goodbye("Error: open() input failed\n");
	fstat(fd, &sb);
	*input_size=sb.st_size;
	if(MAP_FAILED==(input=mmap(NULL, *input_size, PROT_READ, MAP_SHARED, fd, 0)))
		goodbye("Error: mmap failed\n");
	close(fd);
	#endif
	set->encode_func=(set->bps==16)?FLAC__static_encoder_process_frame_bps16_interleaved:FLAC__static_encoder_process_frame_interleaved;
	return input;
}
