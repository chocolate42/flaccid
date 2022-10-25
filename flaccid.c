#include <assert.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_OPENSSL
#include <openssl/md5.h>
#else
#include "mbedtls/md5.h"
int (*MD5)(const unsigned char*, size_t, unsigned char*) = &mbedtls_md5;
#endif


#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "mman.h"
#else
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "FLAC/stream_encoder.h"

#define SHARD_COUNT 4

typedef struct{
	FLAC__StaticEncoder *enc[SHARD_COUNT];
	uint32_t curr_frame[SHARD_COUNT];
	int status[SHARD_COUNT];
	void *outbuf[SHARD_COUNT];
	size_t outbuf_size[SHARD_COUNT];
	uint32_t minframe, maxframe;
} worker;
/*status
	1: Waiting to be encoded
	2: Waiting for output
*/

int main(int argc, char *argv[]){
	unsigned int blocksize=4096, bps=16, rate=44100, channels=2, compression_level, bytes_per_sample=bps/8;
	worker *work;
	int work_count;

	//input thread variables
	struct stat sb;
	int fin_fd, i;
	int16_t *input;
	size_t input_size, input_frame_size;
	uint32_t curr_frame_in=0, last_frame, last_frame_sample_count;
	omp_lock_t curr_frame_lock;
	//output thread variables
	FILE *fout;
	int output_skipped=0;
	uint32_t curr_frame_out=0;
	size_t outsize=0;
	//header variables
	uint32_t minf=UINT32_MAX, maxf=0;
	uint64_t tot_samples;
	uint8_t header[42]={
		0x66, 0x4C, 0x61, 0x43,//magic
		0x80, 0, 0, 0x22,//streaminfo header
		0x10, 0, 0x10, 0,//blocksize 4096
		0, 0, 0, 0, 0, 0,//unknown min/max frame size
		0x0a, 0xc4, 0x42,//44.1khz, 2 channel
		0xf0,//16 bps, upper 4 bits of total samples
		0, 0, 0, 0, //lower 32 bits of total samples
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,//md5
	};

	if(argc!=4)
		return printf("Usage: flaccid worker_count compression_level cdda_raw\nCompare to 'flac --force-raw-format --endian=little -channels=2 --bps=16 --sample-rate=44100 --sign=signed --no-padding --no-seektable cdda_raw'\n");

	work_count=atoi(argv[1]);
	compression_level=atoi(argv[2]);
	input_frame_size=blocksize*channels*bytes_per_sample;

	omp_init_lock(&curr_frame_lock);

	if(-1==(fin_fd=open(argv[3], O_RDONLY)))
		return printf("Error: open() input failed\n");
	fstat(fin_fd, &sb);
	input_size=sb.st_size;
	if(MAP_FAILED==(input=mmap(NULL, input_size, PROT_READ, MAP_SHARED, fin_fd, 0)))
		return printf("Error: mmap failed\n");
	close(fin_fd);

	last_frame=(input_size/input_frame_size)+(input_size%input_frame_size?0:-1);
	last_frame_sample_count=(input_size%input_frame_size)/(channels*bytes_per_sample);
	if(last_frame_sample_count==0)
		last_frame_sample_count=blocksize;
	tot_samples=input_size/(channels*bytes_per_sample);
	printf("flaccid -%d%s%s\n", compression_level, strchr(argv[2], 'e')?"e":"", strchr(argv[2], 'p')?"p":"");

	if(!(fout=fopen("flaccid.flac", "wb+")))
		return printf("Error: fopen() output failed\n");
	outsize+=fwrite(header, 1, 42, fout);
	//seektable dummy TODO

	assert(work_count>0);
	work=malloc(sizeof(worker)*work_count);
	memset(work, 0, sizeof(worker)*work_count);
	for(i=0;i<work_count;++i){
		for(int h=0;h<SHARD_COUNT;++h){
			work[i].enc[h]=FLAC__static_encoder_new();
			FLAC__stream_encoder_set_channels(work[i].enc[h]->stream_encoder, channels);
			FLAC__stream_encoder_set_bits_per_sample(work[i].enc[h]->stream_encoder, bps);
			FLAC__stream_encoder_set_sample_rate(work[i].enc[h]->stream_encoder, rate);
			FLAC__stream_encoder_set_compression_level(work[i].enc[h]->stream_encoder, compression_level);
			FLAC__stream_encoder_set_blocksize(work[i].enc[h]->stream_encoder, blocksize);
			if(strchr(argv[2], 'p'))
				FLAC__stream_encoder_set_do_qlp_coeff_prec_search(work[i].enc[h]->stream_encoder, true);
			if(strchr(argv[2], 'e'))
				FLAC__stream_encoder_set_do_exhaustive_model_search(work[i].enc[h]->stream_encoder, true);
			if(FLAC__static_encoder_init(work[i].enc[h])!=FLAC__STREAM_ENCODER_INIT_STATUS_OK)
				return printf("Init failed\n");
		work[i].status[h]=1;
		work[i].curr_frame[h]=UINT32_MAX;
		}
		work[i].maxframe=0;
		work[i].minframe=UINT32_MAX;
	}

	omp_set_num_threads(work_count+2);
	#pragma omp parallel num_threads(work_count+2) shared(work)
	{
		if(omp_get_num_threads()!=work_count+2){
			printf("Error: Failed to set %d threads (%dx encoders, 1x I/O, 1x MD5). Actually set %d threads\n", work_count+2, work_count, omp_get_num_threads());
			exit(1);
		}
		else if(omp_get_thread_num()==work_count){//md5 thread
			MD5((void*)input, input_size, header+26);//hack because we're flailing about in memory, rule of thumb
			//update close to worker wavefront to access input when it's likely in cache TODO
		}
		else if(omp_get_thread_num()==work_count+1){//output thread
			int h, j, k, status;
			void *obuf;
			uint32_t curr_frame;
			size_t obuf_size;
			while(curr_frame_out<=last_frame){
				for(j=0;j<work_count;++j){
					if(output_skipped==work_count){//sleep if there's nothing to write
						sleep(0.001);
						output_skipped=0;
					}
					h=-1;
					for(k=0;k<SHARD_COUNT;++k){
						#pragma omp atomic read
						status=work[j].status[k];
						#pragma omp atomic read
						curr_frame=work[j].curr_frame[k];
						if(status==2 && curr_frame==curr_frame_out){
							h=k;
							break;
						}
					}
					if(h>-1){//do write
						#pragma omp atomic read
						obuf=work[j].outbuf[h];
						#pragma omp atomic read
						obuf_size=work[j].outbuf_size[h];
						outsize+=fwrite(obuf, 1, obuf_size, fout);
						++curr_frame_out;
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
					omp_set_lock(&curr_frame_lock);
					work[omp_get_thread_num()].curr_frame[h]=curr_frame_in++;
					omp_unset_lock(&curr_frame_lock);
					if(work[omp_get_thread_num()].curr_frame[h]>last_frame)
						break;
					/*for(k=0;k<work[omp_get_thread_num()].samples*channels;++k) //not using bps16
						work[omp_get_thread_num()].input32[k]=work[omp_get_thread_num()].input[k];
					if(!FLAC__static_encoder_process_frame_interleaved(work[omp_get_thread_num()].enc, work[omp_get_thread_num()].input32, work[omp_get_thread_num()].samples, work[omp_get_thread_num()].curr_frame, &(work[omp_get_thread_num()].outbuf), &(work[omp_get_thread_num()].outbuf_size))){*/
					if(!FLAC__static_encoder_process_frame_bps16_interleaved(work[omp_get_thread_num()].enc[h], input+work[omp_get_thread_num()].curr_frame[h]*channels*blocksize, work[omp_get_thread_num()].curr_frame[h]==last_frame?last_frame_sample_count:blocksize, work[omp_get_thread_num()].curr_frame[h], &(work[omp_get_thread_num()].outbuf[h]), &(work[omp_get_thread_num()].outbuf_size[h]))){ //using bps16
						printf("processing failed worker %d frame %d\n", omp_get_thread_num(), work[omp_get_thread_num()].curr_frame[h]);fflush(stdout);
						printf("Encoder state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(work[omp_get_thread_num()].enc[h]->stream_encoder)]);fflush(stdout);
						exit(1);
					}
					if(work[omp_get_thread_num()].outbuf_size[h] < work[omp_get_thread_num()].minframe)
						work[omp_get_thread_num()].minframe=work[omp_get_thread_num()].outbuf_size[h];
					if(work[omp_get_thread_num()].outbuf_size[h] > work[omp_get_thread_num()].maxframe)
						work[omp_get_thread_num()].maxframe=work[omp_get_thread_num()].outbuf_size[h];
					#pragma omp atomic
					work[omp_get_thread_num()].status[h]++;
					#pragma omp flush
				}
				else
					sleep(0.001);
			}
		}
	}
	#pragma omp barrier

	//update header
	for(i=0;i<work_count;++i){
		if(work[i].curr_frame[0]!=UINT32_MAX && work[i].minframe < minf)
			minf=work[i].minframe;
		if(work[i].curr_frame[0]!=UINT32_MAX && work[i].maxframe > maxf)
			maxf=work[i].maxframe;
	}
	header[12]=(minf>>16)&255;
	header[13]=(minf>> 8)&255;
	header[14]=(minf>> 0)&255;
	header[15]=(maxf>>16)&255;
	header[16]=(maxf>> 8)&255;
	header[17]=(maxf>> 0)&255;
	header[21]|=((tot_samples>>32)&255);
	header[22]=(tot_samples>>24)&255;
	header[23]=(tot_samples>>16)&255;
	header[24]=(tot_samples>> 8)&255;
	header[25]=(tot_samples>> 0)&255;
	fflush(fout);
	fseek(fout, 0, SEEK_SET);
	fwrite(header, 1, 42, fout);

	//update seektable TODO

	fclose(fout);
	return 0;
}
