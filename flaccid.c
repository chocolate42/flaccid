#include <assert.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_OPENSSL
#include <openssl/md5.h>
#else
#include "mbedtls/md5.h"
typedef mbedtls_md5_context MD5_CTX;

int (*MD5)(const unsigned char*, size_t, unsigned char*) = &mbedtls_md5;

void MD5_Init(MD5_CTX *ctx){
	mbedtls_md5_init(ctx);
	mbedtls_md5_starts(ctx);
}

int (*MD5_Update)(MD5_CTX*, const unsigned char*, size_t) = &mbedtls_md5_update;

void MD5_Final(uint8_t *sha1, MD5_CTX *ctx){
	mbedtls_md5_finish(ctx, sha1);
}
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

typedef struct{
	int work_count;/*working variables*/
	int channels, bps, sample_rate, compression_level, do_p, do_e;/*flac*/
	uint32_t minf, maxf;
	uint8_t hash[16];
	int blocksize_min, blocksize_max, blocksize_stride;/*chunk*/
	int *greedblocks;/*greed*/
	size_t greedblocks_count;
} flac_settings;

char *help="Usage: flaccid chunk cdda_raw output_file worker_count compression_level blocksize_min blocksize_max blocksize_stride\nOR\n"
	"       flaccid greed cdda_raw output_file worker_count compression_level list,of,blocksizes,to,try\n"
	"Compare to 'flac --force-raw-format --endian=little -channels=2 --bps=16 --sample-rate=44100 --sign=signed --no-padding --no-seektable cdda_raw'\nFor an apples to apples comparison ensure blocksize matches flac defaults for a given compression level\n";

void goodbye(char *s);

void goodbye(char *s){
	fprintf(stderr, "%s", s);
	exit(1);
}

/*chunk mode code*/

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

#define SHARD_COUNT 4

typedef struct chunk_encoder chunk_enc;

struct chunk_encoder{
	FLAC__StaticEncoder *enc;
	struct chunk_encoder *child;
	size_t child_count;
	uint64_t current_sample;
	uint8_t *outbuf;
	size_t outbuf_size;
	uint8_t use_this;
};

typedef struct{
	chunk_enc chunk[SHARD_COUNT];
	uint32_t curr_chunk[SHARD_COUNT];
	int status[SHARD_COUNT];
	uint32_t minframe, maxframe;
} chunk_worker;
/*status
	1: Waiting to be encoded
	2: Waiting for output
*/

int chunk_main(int argc, char *argv[], int16_t *input, size_t input_size, FILE *fout, flac_settings *set);
chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set);
void chunk_process(chunk_enc *c, int16_t *input, uint64_t sample_number);
void chunk_invalidate(chunk_enc *c);
size_t chunk_analyse(chunk_enc *c, uint32_t *minframe, uint32_t *maxframe);
void chunk_write(chunk_enc *c, FILE *fout);

FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize);
FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize){
	FLAC__StaticEncoder *r;
	r=FLAC__static_encoder_new();
	r->is_variable_blocksize=set->blocksize_min!=set->blocksize_max;
	if(set->blocksize_max>16384 || (set->sample_rate<=48000 && set->blocksize_max>4608))
		FLAC__stream_encoder_set_streamable_subset(r->stream_encoder, false);
	FLAC__stream_encoder_set_channels(r->stream_encoder, set->channels);
	FLAC__stream_encoder_set_bits_per_sample(r->stream_encoder, set->bps);
	FLAC__stream_encoder_set_sample_rate(r->stream_encoder, set->sample_rate);
	FLAC__stream_encoder_set_compression_level(r->stream_encoder, set->compression_level);
	FLAC__stream_encoder_set_blocksize(r->stream_encoder, blocksize);/* override compression level */
	if(set->do_p)
		FLAC__stream_encoder_set_do_qlp_coeff_prec_search(r->stream_encoder, true);
	if(set->do_e)
		FLAC__stream_encoder_set_do_exhaustive_model_search(r->stream_encoder, true);
	if(FLAC__static_encoder_init(r)!=FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		goodbye("Init failed\n");
	return r;
}

chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set){
	size_t i;
	assert(min!=0);
	assert(max>=min);

	c->enc=init_static_encoder(set, max);
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
void chunk_process(chunk_enc *c, int16_t *input, uint64_t sample_number){
	size_t i;
	c->current_sample=sample_number;
	if(!FLAC__static_encoder_process_frame_bps16_interleaved(c->enc, input, FLAC__stream_encoder_get_blocksize(c->enc->stream_encoder), sample_number, &(c->outbuf), &(c->outbuf_size))){ //using bps16
		fprintf(stderr, "Static encode failed at sample_number = %"PRIu64"\n", sample_number);
		fprintf(stderr, "Encoder state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(c->enc->stream_encoder)]);
		exit(1);
	}
	if(c->child){
		for(i=0;i<c->child_count;++i)
			chunk_process(c->child+i, input + i*FLAC__stream_encoder_get_channels(c->child[0].enc->stream_encoder)*FLAC__stream_encoder_get_blocksize(c->child[0].enc->stream_encoder), sample_number+i*FLAC__stream_encoder_get_blocksize(c->child[0].enc->stream_encoder));
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

/* Analyse processed chunk to find best frame combinations */
size_t chunk_analyse(chunk_enc *c, uint32_t *minframe, uint32_t *maxframe){
	size_t children_size=0, i;
	if(!(c->child)){
		c->use_this=1;
		return c->outbuf_size;
	}

	for(i=0;i<c->child_count;++i)
		children_size+=chunk_analyse(c->child+i, minframe, maxframe);
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

/* Write best combination of frames in correct order */
void chunk_write(chunk_enc *c, FILE *fout){
	size_t i;
	if(c->use_this)
		fwrite(c->outbuf, 1, c->outbuf_size, fout);
	else{
		assert(c->child);
		for(i=0;i<c->child_count;++i)
			chunk_write(c->child+i, fout);
	}
}

int chunk_main(int argc, char *argv[], int16_t *input, size_t input_size, FILE *fout, flac_settings *set){
	unsigned int bytes_per_sample;
	chunk_worker *work;

	//input thread variables
	int i;
	size_t input_chunk_size;
	uint32_t curr_chunk_in=0, last_chunk, last_chunk_sample_count;
	omp_lock_t curr_chunk_lock;
	//output thread variables
	int output_skipped=0;
	uint32_t curr_chunk_out=0;
	int blocksize_min_input;

	if(argc!=4)
		goodbye(help);

	blocksize_min_input=atoi(argv[1]);
	if(blocksize_min_input<16)
		goodbye("Error: blocksize_min must be >=16\n");
	set->blocksize_max=atoi(argv[2]);
	set->blocksize_stride=atoi(argv[3]);

	if(blocksize_min_input>set->blocksize_max)
		goodbye("Error: Min blocksize cannot be greater than max blocksize\n");
	if(set->blocksize_max>=65536)
		goodbye("Error: Max blocksize must be <65536\n");
	if(blocksize_min_input==set->blocksize_max){
		set->blocksize_min=blocksize_min_input;
		set->blocksize_stride=2;/*needs to be 2, hack*/
	}
	else{
		if(set->blocksize_stride<2)
			goodbye("blocksize_stride is the multiplier step between the blocksizes in use, which must be an integer >=2\n"
				"For example min=256, max=4096, stride=4 defines blocksizes 256, 1024, 4096\n");

		set->blocksize_min=set->blocksize_max;
		while((set->blocksize_min/set->blocksize_stride)>0 && (set->blocksize_min/set->blocksize_stride)>=blocksize_min_input){
			if(set->blocksize_min%set->blocksize_stride)
				goodbye("Error: Min/max blocksizes must divide neatly by stride\n");
			set->blocksize_min/=set->blocksize_stride;
		}
	}

	bytes_per_sample=set->bps/8;

	input_chunk_size=set->blocksize_max*set->channels*bytes_per_sample;

	omp_init_lock(&curr_chunk_lock);

	last_chunk=(input_size/input_chunk_size)+(input_size%input_chunk_size?0:-1);
	last_chunk_sample_count=(input_size%input_chunk_size)/(set->channels*bytes_per_sample);

	printf("flaccid -%d%s%s\n", set->compression_level, set->do_e?"e":"", set->do_p?"p":"");


	//seektable dummy TODO

	work=malloc(sizeof(chunk_worker)*set->work_count);
	memset(work, 0, sizeof(chunk_worker)*set->work_count);
	for(i=0;i<set->work_count;++i){
		for(int h=0;h<SHARD_COUNT;++h){
			chunk_init(&(work[i].chunk[h]), set->blocksize_min, set->blocksize_max, set);
			work[i].status[h]=1;
			work[i].curr_chunk[h]=UINT32_MAX;
		}
		work[i].maxframe=0;
		work[i].minframe=UINT32_MAX;
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
					MD5_Update(&ctx, ((void*)input)+iloc, ilen);
					done=wavefront;
					if(done>=last_chunk)
						break;
				}
				else
					usleep(100);
			}
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
						chunk_write(&(work[j].chunk[h]), fout);
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
						if(!FLAC__static_encoder_process_frame_bps16_interleaved(
							work[omp_get_thread_num()].chunk[h].enc,
							input+work[omp_get_thread_num()].curr_chunk[h]*set->channels*set->blocksize_max,
							last_chunk_sample_count, set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h],
							&(work[omp_get_thread_num()].chunk[h].outbuf),
							&(work[omp_get_thread_num()].chunk[h].outbuf_size))){ //using bps16
							fprintf(stderr, "processing failed worker %d sample_number %d\n", omp_get_thread_num(), set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h]);
							fprintf(stderr, "Encoder state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(work[omp_get_thread_num()].chunk[h].enc->stream_encoder)]);
							exit(1);
						}
						work[omp_get_thread_num()].chunk[h].use_this=1;

						if(work[omp_get_thread_num()].chunk[h].outbuf_size < work[omp_get_thread_num()].minframe)
							work[omp_get_thread_num()].minframe=work[omp_get_thread_num()].chunk[h].outbuf_size;
						if(work[omp_get_thread_num()].chunk[h].outbuf_size > work[omp_get_thread_num()].maxframe)
							work[omp_get_thread_num()].maxframe=work[omp_get_thread_num()].chunk[h].outbuf_size;
					}
					else{//do a full chunk
						chunk_process(&(work[omp_get_thread_num()].chunk[h]), input+work[omp_get_thread_num()].curr_chunk[h]*(input_chunk_size/bytes_per_sample), set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h]);
						chunk_analyse(&(work[omp_get_thread_num()].chunk[h]), &(work[omp_get_thread_num()].minframe), &(work[omp_get_thread_num()].maxframe));
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

	//update header
	for(i=0;i<set->work_count;++i){
		if(work[i].curr_chunk[0]!=UINT32_MAX && work[i].minframe < set->minf)
			set->minf=work[i].minframe;
		if(work[i].curr_chunk[0]!=UINT32_MAX && work[i].maxframe > set->maxf)
			set->maxf=work[i].maxframe;
	}
	return 0;
}
/* end of chunk mode code */

/* greed mode code */

typedef struct{
	FLAC__StaticEncoder *enc;
	double frame_efficiency;
	uint8_t *outbuf;
	size_t outbuf_size;
} greed_encoder;

typedef struct{
	greed_encoder *genc;
	size_t genc_count;
} greed_controller;

int comp_int(const void *aa, const void *bb);
int comp_int(const void *aa, const void *bb){
	int *a=(int*)aa;
	int *b=(int*)bb;
	if(a<b)
		return 1;
	else
		return a==b?0:-1;
}

int greed_main(int argc, char *argv[], int16_t *input, size_t input_size, FILE *fout, flac_settings *set);
int greed_main(int argc, char *argv[], int16_t *input, size_t input_size, FILE *fout, flac_settings *set){
	char *cptr;
	size_t i;
	uint64_t curr_sample, tot_samples=input_size/(set->channels*(set->bps/8));
	greed_controller greed;
	MD5_CTX ctx;
	MD5_Init(&ctx);

	if(argc!=2)
		goodbye(help);

	printf("greed mode currently single-threaded\n");

	cptr=argv[1]-1;
	set->greedblocks_count=0;
	set->greedblocks=NULL;
	do{
		set->greedblocks=realloc(set->greedblocks, sizeof(int)*(set->greedblocks_count+1));
		set->greedblocks[set->greedblocks_count]=atoi(cptr+1);
		if(set->greedblocks[set->greedblocks_count]<16)
			goodbye("Error: Blocksize must be at least 16\n");
		if(set->greedblocks[set->greedblocks_count]>65535)
			goodbye("Error: Blocksize must be at most 65535\n");
		printf("blocksize %d\n", set->greedblocks[set->greedblocks_count]);
		++set->greedblocks_count;
	}while((cptr=strchr(cptr+1, ',')));


	//blocks sorted descending should help occupancy of multithreading, TODO
	qsort(set->greedblocks, set->greedblocks_count, sizeof(int), comp_int);
	set->blocksize_max=set->greedblocks[0];
	set->blocksize_min=set->greedblocks[set->greedblocks_count-1];

	greed.genc=malloc(set->greedblocks_count*sizeof(greed_encoder));
	greed.genc_count=set->greedblocks_count;
	for(i=0;i<set->greedblocks_count;++i)
		greed.genc[i].enc=init_static_encoder(set, set->greedblocks[i]);

	for(curr_sample=0;curr_sample<tot_samples;){
		size_t skip=0;
		for(i=0;i<greed.genc_count;++i){
			if((tot_samples-curr_sample)>=FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder)){
				FLAC__static_encoder_process_frame_bps16_interleaved(greed.genc[i].enc, input+(curr_sample*set->channels), FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder), curr_sample, &(greed.genc[i].outbuf), &(greed.genc[i].outbuf_size));
				greed.genc[i].frame_efficiency=greed.genc[i].outbuf_size;
				greed.genc[i].frame_efficiency/=FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder);
				printf("frame efficiency: %f\n", greed.genc[i].frame_efficiency);
			}
			else{
				greed.genc[i].frame_efficiency=9999.0;
				++skip;
			}
		}
		if(skip==greed.genc_count){//partial frame at end
			FLAC__static_encoder_process_frame_bps16_interleaved(greed.genc[0].enc, input+(curr_sample*set->channels), tot_samples-curr_sample, curr_sample, &(greed.genc[0].outbuf), &(greed.genc[0].outbuf_size));
			MD5_Update(&ctx, ((void*)input)+curr_sample*set->channels*(set->bps/8), (tot_samples-curr_sample)*set->channels*(set->bps/8));
			fwrite(greed.genc[0].outbuf, 1, greed.genc[0].outbuf_size, fout);
			if(greed.genc[0].outbuf_size<set->minf)
				set->minf=greed.genc[0].outbuf_size;
			if(greed.genc[0].outbuf_size>set->maxf)
				set->maxf=greed.genc[0].outbuf_size;
			curr_sample=tot_samples;
		}
		else{
			double smallest_val=8888.0;
			size_t smallest_index=greed.genc_count;
			for(i=0;i<greed.genc_count;++i){
				if(greed.genc[i].frame_efficiency<smallest_val){
					smallest_val=greed.genc[i].frame_efficiency;
					smallest_index=i;
				}
			}
			assert(smallest_index!=greed.genc_count);
			MD5_Update(&ctx, ((void*)input)+curr_sample*set->channels*(set->bps/8), FLAC__stream_encoder_get_blocksize(greed.genc[smallest_index].enc->stream_encoder)*set->channels*(set->bps/8));
			fwrite(greed.genc[smallest_index].outbuf, 1, greed.genc[smallest_index].outbuf_size, fout);
			if(greed.genc[smallest_index].outbuf_size<set->minf)
				set->minf=greed.genc[smallest_index].outbuf_size;
			if(greed.genc[smallest_index].outbuf_size>set->maxf)
				set->maxf=greed.genc[smallest_index].outbuf_size;
			curr_sample+=FLAC__stream_encoder_get_blocksize(greed.genc[smallest_index].enc->stream_encoder);
		}
	}
	MD5_Final(set->hash, &ctx);

	return 0;
}
/* end of greed mode code */

int main(int argc, char *argv[]){
	FILE *fout;
	int fd;
	int16_t *input;
	uint8_t header[42]={
		0x66, 0x4C, 0x61, 0x43,//magic
		0x80, 0, 0, 0x22,//streaminfo header
		0, 0, 0, 0,//min/max blocksize TBD
		0, 0, 0, 0, 0, 0,//min/max frame size TBD
		0x0a, 0xc4, 0x42,//44.1khz, 2 channel
		0xf0,//16 bps, upper 4 bits of total samples
		0, 0, 0, 0, //lower 32 bits of total samples
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,//md5
	};
	uint64_t tot_samples;

	struct stat sb;
	flac_settings set;

	if(argc<6)
		goodbye(help);

	set.bps=16;
	set.sample_rate=44100;
	set.channels=2;
	set.compression_level=5;
	set.blocksize_min=4096;
	set.blocksize_max=4096;
	set.minf=UINT32_MAX;
	set.maxf=0;

	if(!(fout=fopen(argv[3], "wb+")))
		goodbye("Error: fopen() output failed\n");
	fwrite(header, 1, 42, fout);

	//input TODO
	if(-1==(fd=open(argv[2], O_RDONLY)))
		goodbye("Error: open() input failed\n");
	fstat(fd, &sb);
	if(MAP_FAILED==(input=mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0)))
		goodbye("Error: mmap failed\n");
	close(fd);
	tot_samples=sb.st_size/(set.channels*(set.bps/8));

	set.work_count=atoi(argv[4]);
	if(set.work_count<1)
		goodbye("Error: Worker count must be >=1, it is the number of encoder cores to use\n");

	set.compression_level=atoi(argv[5]);
	if(set.compression_level<0)
		set.compression_level=0;
	set.do_e=strchr(argv[5], 'e')!=NULL;
	set.do_p=strchr(argv[5], 'p')!=NULL;

	if(strcmp(argv[1], "chunk")==0)
		chunk_main(argc-5, argv+5, input, sb.st_size, fout, &set);
	else if(strcmp(argv[1], "greed")==0)
		greed_main(argc-5, argv+5, input, sb.st_size, fout, &set);
	else
		goodbye(help);

	/* write finished header */
	header[ 8]=(set.blocksize_min>>8)&255;
	header[ 9]=(set.blocksize_min>>0)&255;
	header[10]=(set.blocksize_max>>8)&255;
	header[11]=(set.blocksize_max>>0)&255;
	header[12]=(set.minf>>16)&255;
	header[13]=(set.minf>> 8)&255;
	header[14]=(set.minf>> 0)&255;
	header[15]=(set.maxf>>16)&255;
	header[16]=(set.maxf>> 8)&255;
	header[17]=(set.maxf>> 0)&255;
	header[21]|=((tot_samples>>32)&255);
	header[22]=(tot_samples>>24)&255;
	header[23]=(tot_samples>>16)&255;
	header[24]=(tot_samples>> 8)&255;
	header[25]=(tot_samples>> 0)&255;
	memcpy(header+26, set.hash, 16);
	fflush(fout);
	fseek(fout, 0, SEEK_SET);
	fwrite(header, 1, 42, fout);

	//update seektable TODO

	fclose(fout);
}
