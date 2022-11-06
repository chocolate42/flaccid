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

#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
void usleep(unsigned int microseconds);
void usleep(unsigned int microseconds){
	Sleep(1+(microseconds/1000));
}
#else
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "FLAC/stream_encoder.h"

typedef struct{
	int work_count, use_simpler_comp_for_testing, do_tweak, do_merge;/*working variables*/
	int lax, channels, bps, sample_rate, compression_level, do_p, do_e;/*flac*/
	uint32_t minf, maxf;
	uint8_t hash[16];
	int blocksize_min, blocksize_max, blocksize_stride;/*chunk*/
	int *blocks;/*greed*/
	size_t blocks_count;
} flac_settings;

char *help="Usage:\n flaccid chunk cdda_raw output_file worker_count compression_level blocksize_min blocksize_max blocksize_stride\nOR\n"
	" flaccid greed cdda_raw output_file worker_count compression_level list,of,blocksizes,to,try\nOR\n"
	" flaccid peak cdda_raw output_file worker_count compression_level list,of,blocksizes,to,try [0/1 to use simpler compression level for testing]\nOR\n"
	"Compare to 'flac --force-raw-format --endian=little -channels=2 --bps=16 --sample-rate=44100 --sign=signed --no-padding --no-seektable cdda_raw'\n"
	"For an apples to apples comparison ensure blocksize matches flac defaults for a given compression level\n";

void goodbye(char *s);
int comp_int_asc(const void *aa, const void *bb);
int comp_int_desc(const void *aa, const void *bb);

void goodbye(char *s){
	fprintf(stderr, "%s", s);
	exit(1);
}

int comp_int_asc(const void *aa, const void *bb){
	int *a=(int*)aa;
	int *b=(int*)bb;
	if(a<b)
		return -1;
	else
		return a==b?0:1;
}

int comp_int_desc(const void *aa, const void *bb){
	int *a=(int*)aa;
	int *b=(int*)bb;
	if(a<b)
		return 1;
	else
		return a==b?0:-1;
}

int chunk_main(int argc, char *argv[], int16_t *input, size_t input_size, FILE *fout, flac_settings *set);
int greed_main(int argc, char *argv[], int16_t *input, size_t input_size, FILE *fout, flac_settings *set);
int  peak_main(int argc, char *argv[], int16_t *input, size_t input_size, FILE *fout, flac_settings *set);

int main(int argc, char *argv[]){
	FILE *fout;
	int16_t *input;
	size_t input_size;
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
	#ifdef _WIN32
	FILE *fin;
	#else
	int fd;
	struct stat sb;
	#endif
	flac_settings set;

	if(argc<6)
		goodbye(help);

	memset(&set, 0, sizeof(flac_settings));
	set.bps=16;
	set.sample_rate=44100;
	set.channels=2;
	set.compression_level=5;
	set.blocksize_min=4096;
	set.blocksize_max=4096;
	set.minf=UINT32_MAX;
	set.maxf=0;
	set.lax=0;
	set.use_simpler_comp_for_testing=0;

	if(!(fout=fopen(argv[3], "wb+")))
		goodbye("Error: fopen() output failed\n");
	fwrite(header, 1, 42, fout);

	//input
	#ifdef _WIN32
	//for now have windows read the entire input immediately instead of using mmap
	//reduces accuracy of speed comparison but at least allows windows users to play
	//for large effort runs this hack shouldn't matter much to speed comparisons
	if(!(fin=fopen(argv[2], "rb")))
		goodbye("Error: fopen() input failed\n");
	if(-1==fseek(fin, 0, SEEK_END))
		goodbye("Error: fseek() input failed\n");
	if(-1==ftell(fin))
		goodbye("Error: ftell() input failed\n");
	input_size=ftell(fin);
	rewind(fin);
	if(!input_size)
		goodbye("Error: Input empty");
	if(!(input=malloc(input_size)))
		goodbye("Error: malloc() input failed\n");
	if(input_size!=fread(input, 1, input_size, fin))
		goodbye("Error: fread() input failed\n");
	fclose(fin);
	#else
	if(-1==(fd=open(argv[2], O_RDONLY)))
		goodbye("Error: open() input failed\n");
	fstat(fd, &sb);
	input_size=sb.st_size;
	if(MAP_FAILED==(input=mmap(NULL, input_size, PROT_READ, MAP_SHARED, fd, 0)))
		goodbye("Error: mmap failed\n");
	close(fd);
	#endif
	tot_samples=input_size/(set.channels*(set.bps/8));

	set.work_count=atoi(argv[4]);
	if(set.work_count<1)
		goodbye("Error: Worker count must be >=1, it is the number of encoder cores to use\n");

	set.compression_level=atoi(argv[5]);
	if(set.compression_level<0)
		set.compression_level=0;
	set.do_e=strchr(argv[5], 'e')!=NULL;
	set.do_p=strchr(argv[5], 'p')!=NULL;

	if(strcmp(argv[1], "chunk")==0)
		chunk_main(argc-5, argv+5, input, input_size, fout, &set);
	else if(strcmp(argv[1], "greed")==0)
		greed_main(argc-5, argv+5, input, input_size, fout, &set);
	else if(strcmp(argv[1], "peak")==0)
		peak_main(argc-5, argv+5, input, input_size, fout, &set);
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

chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set);
void chunk_process(chunk_enc *c, int16_t *input, uint64_t sample_number);
void chunk_invalidate(chunk_enc *c);
size_t chunk_analyse(chunk_enc *c, uint32_t *minframe, uint32_t *maxframe);
void chunk_write(chunk_enc *c, FILE *fout);

FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, int is_simple);
FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, int is_simple){
	FLAC__StaticEncoder *r;
	r=FLAC__static_encoder_new();
	r->is_variable_blocksize=set->blocksize_min!=set->blocksize_max;
	if(set->blocksize_max>16384 || (set->sample_rate<=48000 && set->blocksize_max>4608)){
		//if(!set->lax)
		//	goodbye("Error: Tried to use a non-subset blocksize without setting --lax\n");
		FLAC__stream_encoder_set_streamable_subset(r->stream_encoder, false);
	}
	FLAC__stream_encoder_set_channels(r->stream_encoder, set->channels);
	FLAC__stream_encoder_set_bits_per_sample(r->stream_encoder, set->bps);
	FLAC__stream_encoder_set_sample_rate(r->stream_encoder, set->sample_rate);
	if(is_simple){
		//FLAC__stream_encoder_set_compression_level(r->stream_encoder, set->compression_level);
		//FLAC__stream_encoder_set_max_residual_partition_order(r->stream_encoder, 0);
		//FLAC__stream_encoder_set_max_residual_partition_order(r->stream_encoder, 1);
		FLAC__stream_encoder_set_compression_level(r->stream_encoder, 0);
	}
	else{
		FLAC__stream_encoder_set_compression_level(r->stream_encoder, set->compression_level);
		if(set->do_p)
			FLAC__stream_encoder_set_do_qlp_coeff_prec_search(r->stream_encoder, true);
		if(set->do_e)
			FLAC__stream_encoder_set_do_exhaustive_model_search(r->stream_encoder, true);
	}
	FLAC__stream_encoder_set_blocksize(r->stream_encoder, blocksize);/* override compression level blocksize */
	FLAC__stream_encoder_set_loose_mid_side_stereo(r->stream_encoder, false);/* override adaptive mid-side */
	if(FLAC__static_encoder_init(r)!=FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		goodbye("Init failed\n");
	return r;
}

chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set){
	size_t i;
	assert(min!=0);
	assert(max>=min);

	c->enc=init_static_encoder(set, max, 0);
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
		fprintf(stderr, "Static encode failed, state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(c->enc->stream_encoder)]);
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
	set->blocks_count=0;
	set->blocks=NULL;
	do{
		set->blocks=realloc(set->blocks, sizeof(int)*(set->blocks_count+1));
		set->blocks[set->blocks_count]=atoi(cptr+1);
		if(set->blocks[set->blocks_count]<16)
			goodbye("Error: Blocksize must be at least 16\n");
		if(set->blocks[set->blocks_count]>65535)
			goodbye("Error: Blocksize must be at most 65535\n");
		printf("blocksize %d\n", set->blocks[set->blocks_count]);
		++set->blocks_count;
	}while((cptr=strchr(cptr+1, ',')));

	//blocks sorted descending should help occupancy of multithreading
	qsort(set->blocks, set->blocks_count, sizeof(int), comp_int_desc);
	set->blocksize_max=set->blocks[0];
	set->blocksize_min=set->blocks[set->blocks_count-1];

	greed.genc=malloc(set->blocks_count*sizeof(greed_encoder));
	greed.genc_count=set->blocks_count;
	for(i=0;i<set->blocks_count;++i)
		greed.genc[i].enc=init_static_encoder(set, set->blocks[i], 0);

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

typedef struct frame_list frame_list;

struct frame_list{
	uint8_t *outbuf;
	size_t outbuf_size;
	size_t blocksize;
	frame_list *next, *prev;
};

/* peak mode code */
typedef struct{
	FLAC__StaticEncoder *enc;
	void *outbuf;
} peak_hunter;

int peak_main(int argc, char *argv[], int16_t *input, size_t input_size, FILE *fout, flac_settings *set){
	char *cptr;
	int k;
	size_t effort=0, i, j, *step, step_count;
	size_t *frame_results, *running_results;
	uint8_t *running_step;
	size_t window_size, window_size_check=0;
	size_t curr_sample=0;
	size_t outsize=0;
	size_t tot_samples;
	size_t effort_print=0;
	frame_list *frame=NULL, *frame_next;
	peak_hunter *work;
	FLAC__StaticEncoder *encout;

	if(argc!=3)
		goodbye(help);

	tot_samples=input_size/(set->channels*(set->bps/8));

	set->use_simpler_comp_for_testing=atoi(argv[2]);
	assert(set->use_simpler_comp_for_testing==0 || set->use_simpler_comp_for_testing==1);

	cptr=argv[1]-1;
	set->blocks_count=0;
	set->blocks=NULL;
	do{
		set->blocks=realloc(set->blocks, sizeof(int)*(set->blocks_count+1));
		set->blocks[set->blocks_count]=atoi(cptr+1);
		if(set->blocks[set->blocks_count]<16)
			goodbye("Error: Blocksize must be at least 16\n");
		if(set->blocks[set->blocks_count]>65535)
			goodbye("Error: Blocksize must be at most 65535\n");
		++set->blocks_count;
	}while((cptr=strchr(cptr+1, ',')));

	if(set->blocks_count==1)
		goodbye("Error: At least two blocksizes must be available\n");
	if(set->blocks_count>255)
		goodbye("Error: Implementation limited to up to 255 blocksizes. This is also just an insane heat-death-of-the-universe setting\n");

	qsort(set->blocks, set->blocks_count, sizeof(int), comp_int_asc);
	set->blocksize_min=set->blocks[0];
	set->blocksize_max=set->blocks[set->blocks_count-1];

	for(i=1;i<set->blocks_count;++i){
		if(set->blocks[i]%set->blocks[0])
			goodbye("Error: All blocksizes must be a multiple of the minimum blocksize\n");
	}
	window_size=tot_samples/set->blocks[0];
	if(!window_size)
		goodbye("Error: Input too small\n");

	step=malloc(sizeof(size_t)*set->blocks_count);
	step_count=set->blocks_count;
	for(i=0;i<set->blocks_count;++i)
		step[i]=set->blocks[i]/set->blocks[0];

	for(i=1;i<step_count;++i){
		if(step[i]==step[i-1])
			goodbye("Error: Duplicate blocksizes in argument\n");
	}

	for(i=0;i<step_count;++i)
		effort+=step[i];

	printf("Peakset threads %d blocksize_min %u effort %zu window_size %zu\n", set->work_count, set->blocks[0], effort, window_size);

	frame_results=malloc(sizeof(size_t)*step_count*(window_size+1));
	running_results=malloc(sizeof(size_t)*(window_size+1));
	running_step=malloc(sizeof(size_t)*(window_size+1));

	/* process frames for stats */
	omp_set_num_threads(set->work_count);
	work=malloc(sizeof(peak_hunter)*set->work_count);
	for(j=0;j<step_count;++j){
		for(k=0;k<set->work_count;++k)
			work[k].enc=init_static_encoder(set, set->blocks[j], set->use_simpler_comp_for_testing);
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<window_size-(step[j]-1);++i){
			FLAC__static_encoder_process_frame_bps16_interleaved(work[omp_get_thread_num()].enc,
				input+(set->blocksize_min*i*set->channels),
				set->blocks[j],
				set->blocksize_min*i,
				&(work[omp_get_thread_num()].outbuf),
				frame_results+(i*step_count)+j
			);
		}
		#pragma omp barrier
		for(i=window_size-(step[j]-1);i<window_size;++i)
			frame_results[(i*step_count)+j]=SIZE_MAX;
		for(k=0;k<set->work_count;++k)
			FLAC__static_encoder_delete(work[k].enc);
		effort_print+=step[j];
		printf("Processed %zu/%zu\n", effort_print, effort);
	}

	/* analyse stats */
	running_results[0]=0;
	for(i=1;i<=window_size;++i){
		size_t curr_run=SIZE_MAX, curr_step=step_count;
		for(j=0;j<step_count;++j){
			if(step[j]>i)//near the beginning we need to ensure we don't go beyond window start
				break;
			if(curr_run>(running_results[i-step[j]]+frame_results[((i-step[j])*step_count)+j])){
				assert(frame_results[((i-step[j])*step_count)+j]!=SIZE_MAX);
				curr_run=(running_results[i-step[j]]+frame_results[((i-step[j])*step_count)+j]);
				curr_step=j;
			}
		}
		assert(curr_run!=SIZE_MAX);
		running_results[i]=curr_run;
		running_step[i]=curr_step;
	}

	/* traverse optimal result */
	for(i=window_size;i>0;){
		size_t frame_at=i-step[running_step[i]];
		frame_next=frame;
		frame=malloc(sizeof(frame_list));
		frame->blocksize=set->blocks[running_step[i]];
		frame->outbuf=NULL;
		frame->outbuf_size=frame_results[(frame_at*step_count)+running_step[i]];
		frame->next=frame_next;
		if(frame_next)
			frame_next->prev=frame;
		window_size_check+=step[running_step[i]];
		i-=step[running_step[i]];
	}
	assert(i==0);
	assert(window_size_check==window_size);

	//if used simpler settings for analysis, encode properly here
	//store encoded frames as most/all of these are likely to be used
	if(set->use_simpler_comp_for_testing){
		size_t fi=0;
		for(frame_next=frame;frame_next;){
			printf("Encode frame %zu\n", fi);
			if(!(frame_next->outbuf)){//encode if never encoded before
				void *tmpbuf;
				encout=init_static_encoder(set, frame_next->blocksize, 0);
				FLAC__static_encoder_process_frame_bps16_interleaved(encout,
					input+(curr_sample*set->channels),
					frame_next->blocksize,
					curr_sample,
					&tmpbuf,
					&(frame_next->outbuf_size)
				);
				frame_next->outbuf=malloc(frame_next->outbuf_size);
				FLAC__static_encoder_delete(encout);
			}
			outsize+=fwrite(frame_next->outbuf, 1, frame_next->outbuf_size, fout);
			curr_sample+=frame_next->blocksize;
			frame_next=frame_next->next;
		}
	}

	/* merge/tweak whatever here TODO */
	if(set->do_tweak){
	}

	/* Write optimal result */
	curr_sample=0;
	for(frame_next=frame;frame_next;frame_next=frame_next->next){
		if(!(frame_next->outbuf)){//encode if not stored
			size_t comp;
			encout=init_static_encoder(set, frame_next->blocksize, 0);
			FLAC__static_encoder_process_frame_bps16_interleaved(encout,
				input+(curr_sample*set->channels),
				frame_next->blocksize,
				curr_sample,
				&(frame_next->outbuf),
				&(comp)
			);
			if(set->use_simpler_comp_for_testing==0)
				assert(frame_next->outbuf_size==comp);
			outsize+=fwrite(frame_next->outbuf, 1, frame_next->outbuf_size, fout);
			FLAC__static_encoder_delete(encout);
		}
		else
			outsize+=fwrite(frame_next->outbuf, 1, frame_next->outbuf_size, fout);

		curr_sample+=frame_next->blocksize;
	}
	if(set->use_simpler_comp_for_testing==0)
		assert(outsize==running_results[window_size]);

	if(tot_samples-curr_sample){/* partial end frame */
		void *partial_outbuf;
		size_t partial_outbuf_size;
		encout=init_static_encoder(set, set->blocksize_max, 0);
		FLAC__static_encoder_process_frame_bps16_interleaved(encout,
			input+(curr_sample*set->channels),
			tot_samples-curr_sample,
			curr_sample,
			&(partial_outbuf),
			&(partial_outbuf_size)
		);
		outsize+=fwrite(partial_outbuf, 1, partial_outbuf_size, fout);
		FLAC__static_encoder_delete(encout);
	}
	MD5(((void*)input), input_size, set->hash);
	return 0;
}
/* end of peak mode code */
