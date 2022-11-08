#include <assert.h>
#include <getopt.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
	int *blocks, diff_comp_settings, tweak;
	size_t blocks_count;
	int work_count, comp_anal_used, do_merge;/*working variables*/
	char *comp_anal, *comp_output, *apod_anal, *apod_output;
	int lax, channels, bps, sample_rate;/*flac*/
	uint32_t minf, maxf;
	uint8_t hash[16];
	int blocksize_min, blocksize_max, blocksize_stride;/*chunk*/
} flac_settings;

typedef struct frame_list frame_list;

struct frame_list{
	uint8_t *outbuf;
	size_t outbuf_size;
	size_t blocksize;
	uint64_t curr_sample;
	frame_list *next, *prev;
};

char *help=
	"Usage: flaccid [options]\n"
	"\nOptions:\n"
	" --lax : Allow non-subset settings\n"
	" --mode mode : Which variable-blocksize algorithm to use. Valid modes: chunk, greed, peakset\n"
	" --tweak : Enable tweaking the partition of adjacent frames as a last-pass optimisation\n"
	" --workers integer : The maximum number of encode threads to run simultaenously\n"
	" --analysis-comp comp_string: Compression settings to use during analysis\n"
	" --output-comp comp_string: Compression settings to use during output\n"
	" --analysis-apod apod_string : Apodization settings to use during analysis\n"
	" --output-apod apod_string : Apodization settings to use during output\n"
	" --in infile : Source, pipe unsupported\n"
	" --out outfile : Destination\n"
	" --blocksize-list list,of,blocksizes : Blocksizes that a mode is allowed to use for its main\n"
	"                                       execution. Different modes have different constraints\n"
	"                                       on valid combinations\n"
	"\nCompression settings format:\n"
	" * Mostly follows ./flac interface, but requires settings to be concatenated into a single string\n"
	" * Compression level must be the first element\n"
	" * Supported settings: e, m, l, p, q, r (see ./flac -h)\n"
	" * Adaptive mid-side from ./flac is not supported (-M), affects compression levels 1 and 4\n"
	" * ie \"5er4\" defines compression level 5, with exhaustive model search and max rice partition order of 4\n"
	"\nApodization settings format:\n"
	" * All apodization settings in a single semi-colon-delimited string\n"
	" * ie tukey(0.5);partial_tukey(2);punchout_tukey(3)\n";

int comp_int_asc(const void *aa, const void *bb);
int comp_int_desc(const void *aa, const void *bb);
size_t fwrite_framestat(const void *ptr, size_t size, FILE *stream, uint32_t *minf, uint32_t *maxf);
void goodbye(char *s);
FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, char *comp, char *apod);
void parse_blocksize_list(char *list, int **res, size_t *res_cnt);

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
	if(set->blocksize_max>16384 || (set->sample_rate<=48000 && set->blocksize_max>4608)){
		if(!set->lax)
			goodbye("Error: Tried to use a non-subset blocksize without setting --lax\n");
		FLAC__stream_encoder_set_streamable_subset(r->stream_encoder, false);
	}
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

int chunk_main(int16_t *input, size_t input_size, FILE *fout, flac_settings *set);
int greed_main(int16_t *input, size_t input_size, FILE *fout, flac_settings *set);
int  peak_main(int16_t *input, size_t input_size, FILE *fout, flac_settings *set);

int main(int argc, char *argv[]){
	int (*encoder[3])(int16_t*, size_t, FILE*, flac_settings*)={chunk_main, greed_main, peak_main};
	char *ipath=NULL, *opath=NULL;
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
	static int lax=0;
	char *blocklist_str="1152,2304,4608";

	int c, mode=-1, option_index;
	static struct option long_options[]={
		{"lax", no_argument, &lax, 1},
		{"help", no_argument, 0, 'h'},
		{"tweak", required_argument, 0, 261},
		{"analysis-comp", required_argument, 0, 256},
		{"output-comp", required_argument, 0, 257},
		{"analysis-apod", required_argument, 0, 259},
		{"output-apod", required_argument, 0, 260},
		{"workers", required_argument, 0, 'w'},
		{"in", required_argument, 0, 'i'},
		{"out", required_argument, 0, 'o'},
		{"mode", required_argument, 0, 'm'},
		{"blocksize-list",	required_argument, 0, 258},
		{0, 0, 0, 0}
	};

	memset(&set, 0, sizeof(flac_settings));
	set.apod_anal=NULL;
	set.apod_output=NULL;
	set.blocksize_max=4096;
	set.blocksize_min=4096;
	set.bps=16;
	set.channels=2;
	set.comp_anal="5";
	set.comp_output="8p";
	set.diff_comp_settings=0;
	set.minf=UINT32_MAX;
	set.maxf=0;
	set.sample_rate=44100;
	set.work_count=1;

	while (1){
		if(-1==(c=getopt_long(argc, argv, "hi:m:o:w:", long_options, &option_index)))
			break;
		switch(c){
			case 'h':
				goodbye(help);
				break;

			case 'i':
				ipath=optarg;
				break;

			case 'm':
				if(strcmp(optarg, "chunk")==0)
					mode=0;
				else if(strcmp(optarg, "greed")==0)
					mode=1;
				else if(strcmp(optarg, "peakset")==0)
					mode=2;
				else
					goodbye("Unknown mode\n");
				break;

			case 'o':
				opath=optarg;
				break;

			case 'w':
				set.work_count=atoi(optarg);
				if(set.work_count<1)
					goodbye("Error: Worker count must be >=1, it is the number of encoder cores to use\n");
				break;

			case 256:
				set.comp_anal=optarg;
				break;

			case 257:
				set.comp_output=optarg;
				break;

			case 258:
				blocklist_str=optarg;
				break;

			case 259:
				set.apod_anal=optarg;
				break;

			case 260:
				set.apod_output=optarg;
				break;

			case 261:
				set.tweak=atoi(optarg);
				break;

			case '?':
				goodbye("");
				break;
		}
	}
	set.lax=lax;

	set.diff_comp_settings=strcmp(set.comp_anal, set.comp_output)!=0;
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && !set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(!set.apod_anal && set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && set.apod_output && strcmp(set.apod_anal, set.apod_output)!=0);

	if(!ipath)
		goodbye("Error: No input\n");

	if(!opath)/* Add test option with no no output TODO */
		goodbye("Error: No output\n");

	if(mode==-1)
		goodbye("Error: No mode specified\n");

	parse_blocksize_list(blocklist_str, &(set.blocks), &(set.blocks_count));
	qsort(set.blocks, set.blocks_count, sizeof(int), comp_int_asc);
	set.blocksize_min=set.blocks[0];
	set.blocksize_max=set.blocks[set.blocks_count-1];

	if(!(fout=fopen(opath, "wb+")))
		goodbye("Error: fopen() output failed\n");
	fwrite(header, 1, 42, fout);

	//input
	#ifdef _WIN32
	//for now have windows read the entire input immediately instead of using mmap
	//reduces accuracy of speed comparison but at least allows windows users to play
	//for large effort runs this hack shouldn't matter much to speed comparisons
	if(!(fin=fopen(ipath, "rb")))
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
	if(-1==(fd=open(ipath, O_RDONLY)))
		goodbye("Error: open() input failed\n");
	fstat(fd, &sb);
	input_size=sb.st_size;
	if(MAP_FAILED==(input=mmap(NULL, input_size, PROT_READ, MAP_SHARED, fd, 0)))
		goodbye("Error: mmap failed\n");
	close(fd);
	#endif
	tot_samples=input_size/(set.channels*(set.bps/8));

	printf("%s\t", ipath);
	encoder[mode](input, input_size, fout, &set);

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
	uint64_t curr_sample;
	int blocksize;
	uint8_t *outbuf;
	size_t outbuf_size;
	uint8_t use_this;
};

typedef struct{
	chunk_enc chunk[SHARD_COUNT];
	uint32_t curr_chunk[SHARD_COUNT];
	int status[SHARD_COUNT];
} chunk_worker;
/*status
	1: Waiting to be encoded
	2: Waiting for output
*/

chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set);
void chunk_process(chunk_enc *c, int16_t *input, uint64_t sample_number);
void chunk_invalidate(chunk_enc *c);
size_t chunk_analyse(chunk_enc *c);
void chunk_write(flac_settings *set, chunk_enc *c, int16_t *input, FILE *fout, uint32_t *minf, uint32_t *maxf, size_t *outsize);

chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set){
	size_t i;
	assert(min!=0);
	assert(max>=min);

	c->enc=init_static_encoder(set, max, set->comp_anal, set->apod_anal);
	c->blocksize=max;
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
	c->curr_sample=sample_number;
	if(!FLAC__static_encoder_process_frame_bps16_interleaved(c->enc, input, c->blocksize, sample_number, &(c->outbuf), &(c->outbuf_size))){ //using bps16
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

/* Analyse processed chunk to find best frame combination */
size_t chunk_analyse(chunk_enc *c){
	size_t children_size=0, i;
	if(!(c->child)){
		c->use_this=1;
		return c->outbuf_size;
	}

	for(i=0;i<c->child_count;++i)
		children_size+=chunk_analyse(c->child+i);
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
void chunk_write(flac_settings *set, chunk_enc *c, int16_t *input, FILE *fout, uint32_t *minf, uint32_t *maxf, size_t *outsize){
	size_t i;
	if(c->use_this){
		if(set->diff_comp_settings){
			FLAC__StaticEncoder *enc;
			enc=init_static_encoder(set, c->blocksize<16?16:c->blocksize, set->comp_output, set->apod_output);
			if(!FLAC__static_encoder_process_frame_bps16_interleaved(enc, input+(c->curr_sample*set->channels), c->blocksize, c->curr_sample, &(c->outbuf), &(c->outbuf_size))){ //using bps16
				fprintf(stderr, "Static encode failed, state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(c->enc->stream_encoder)]);
				exit(1);
			}
			(*outsize)+=fwrite_framestat(c->outbuf, c->outbuf_size, fout, minf, maxf);
			FLAC__static_encoder_delete(enc);
		}
		else
			(*outsize)+=fwrite_framestat(c->outbuf, c->outbuf_size, fout, minf, maxf);
	}
	else{
		assert(c->child);
		for(i=0;i<c->child_count;++i)
			chunk_write(set, c->child+i, input, fout, minf, maxf, outsize);
	}
}

int chunk_main(int16_t *input, size_t input_size, FILE *fout, flac_settings *set){
	unsigned int bytes_per_sample;
	chunk_worker *work;

	//input thread variables
	int i;
	size_t input_chunk_size, block_index;
	uint32_t curr_chunk_in=0, last_chunk, last_chunk_sample_count;
	omp_lock_t curr_chunk_lock;
	//output thread variables
	int output_skipped=0;
	uint32_t curr_chunk_out=0;
	double effort_anal, effort_output, effort_tweak=0;
	size_t outsize=42;
	clock_t cstart;
	double cpu_time;
	cstart=clock();

	if(set->blocks_count==1)
		goodbye("Error: Chunk mode needs multiple blocksizes to work with\n");

	set->blocksize_stride=set->blocks[1]/set->blocks[0];
	if(set->blocksize_stride<2)
		goodbye("Error: Chunk mode requires blocksizes to be a fixed multiple away from each other, >=2\n");

	for(block_index=1;block_index<set->blocks_count;++block_index){
		if(set->blocks[block_index-1]*set->blocksize_stride!=set->blocks[block_index])
			goodbye("Error: Chunk mode requires blocksizes to be a fixed multiple away from each other, >=2\n");
	}

	bytes_per_sample=set->bps/8;

	input_chunk_size=set->blocksize_max*set->channels*bytes_per_sample;

	omp_init_lock(&curr_chunk_lock);

	last_chunk=(input_size/input_chunk_size)+(input_size%input_chunk_size?0:-1);
	last_chunk_sample_count=(input_size%input_chunk_size)/(set->channels*bytes_per_sample);

	//seektable dummy TODO

	work=malloc(sizeof(chunk_worker)*set->work_count);
	memset(work, 0, sizeof(chunk_worker)*set->work_count);
	for(i=0;i<set->work_count;++i){
		for(int h=0;h<SHARD_COUNT;++h){
			chunk_init(&(work[i].chunk[h]), set->blocksize_min, set->blocksize_max, set);
			work[i].status[h]=1;
			work[i].curr_chunk[h]=UINT32_MAX;
		}
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
						chunk_write(set, &(work[j].chunk[h]), input, fout, &(set->minf), &(set->maxf), &outsize);
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
						work[omp_get_thread_num()].chunk[h].curr_sample=work[omp_get_thread_num()].curr_chunk[h]*set->blocksize_max;
						work[omp_get_thread_num()].chunk[h].blocksize=last_chunk_sample_count;
						work[omp_get_thread_num()].chunk[h].use_this=1;
					}
					else{//do a full chunk
						chunk_process(&(work[omp_get_thread_num()].chunk[h]), input+work[omp_get_thread_num()].curr_chunk[h]*(input_chunk_size/bytes_per_sample), set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h]);
						chunk_analyse(&(work[omp_get_thread_num()].chunk[h]));
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

	effort_anal=set->blocks_count;
	effort_output=1;
	if(!set->diff_comp_settings){
		effort_output=effort_anal;
		effort_anal=0;
	}

	printf("chunk\t%s\t%s\t%u\t%u", set->comp_anal, set->comp_output, set->tweak, set->blocks[0]);
	for(block_index=1;block_index<set->blocks_count;++block_index)
		printf(",%u", set->blocks[block_index]);
	cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	printf("\t%.3f\t%.3f\t%.3f\t%zu\t%.5f\n", effort_anal, effort_tweak, effort_output, outsize, cpu_time);

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

int greed_main(int16_t *input, size_t input_size, FILE *fout, flac_settings *set){
	size_t i;
	uint64_t curr_sample, tot_samples=input_size/(set->channels*(set->bps/8));
	greed_controller greed;
	MD5_CTX ctx;
	FLAC__StaticEncoder *oenc;
	double effort_anal, effort_output, effort_tweak=0;
	size_t effort_anal_run=0, outsize=42;
	clock_t cstart;
	double cpu_time;
	cstart=clock();

	MD5_Init(&ctx);

	//blocks sorted descending should help occupancy of multithreading
	qsort(set->blocks, set->blocks_count, sizeof(int), comp_int_desc);

	greed.genc=malloc(set->blocks_count*sizeof(greed_encoder));
	greed.genc_count=set->blocks_count;
	for(i=0;i<set->blocks_count;++i)
		greed.genc[i].enc=init_static_encoder(set, set->blocks[i], set->comp_anal, set->apod_anal);

	for(curr_sample=0;curr_sample<tot_samples;){
		size_t skip=0;
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<greed.genc_count;++i){
			if((tot_samples-curr_sample)>=FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder)){
				FLAC__static_encoder_process_frame_bps16_interleaved(greed.genc[i].enc, input+(curr_sample*set->channels), FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder), curr_sample, &(greed.genc[i].outbuf), &(greed.genc[i].outbuf_size));
				greed.genc[i].frame_efficiency=greed.genc[i].outbuf_size;
				greed.genc[i].frame_efficiency/=FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder);
			}
			else
				greed.genc[i].frame_efficiency=9999.0;
		}
		#pragma omp barrier
		for(i=0;i<greed.genc_count;++i){
			if(greed.genc[i].frame_efficiency>9998.0)
				++skip;
		}
		if(skip==greed.genc_count){//partial frame at end
			oenc=init_static_encoder(set, (tot_samples-curr_sample)<16?16:(tot_samples-curr_sample), set->comp_output, set->apod_output);
			FLAC__static_encoder_process_frame_bps16_interleaved(oenc, input+(curr_sample*set->channels), tot_samples-curr_sample, curr_sample, &(greed.genc[0].outbuf), &(greed.genc[0].outbuf_size));
			MD5_Update(&ctx, ((void*)input)+curr_sample*set->channels*(set->bps/8), (tot_samples-curr_sample)*set->channels*(set->bps/8));
			outsize+=fwrite_framestat(greed.genc[0].outbuf, greed.genc[0].outbuf_size, fout, &(set->minf), &(set->maxf));
			FLAC__static_encoder_delete(oenc);
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
			MD5_Update(&ctx, ((void*)input)+curr_sample*set->channels*(set->bps/8), set->blocks[smallest_index]*set->channels*(set->bps/8));
			if(set->diff_comp_settings){
				oenc=init_static_encoder(set, set->blocks[smallest_index], set->comp_output, set->apod_output);
				FLAC__static_encoder_process_frame_bps16_interleaved(oenc, input+(curr_sample*set->channels), set->blocks[smallest_index], curr_sample, &(greed.genc[smallest_index].outbuf), &(greed.genc[smallest_index].outbuf_size));
				outsize+=fwrite_framestat(greed.genc[smallest_index].outbuf, greed.genc[smallest_index].outbuf_size, fout, &(set->minf), &(set->maxf));
				FLAC__static_encoder_delete(oenc);
			}
			else
				outsize+=fwrite_framestat(greed.genc[smallest_index].outbuf, greed.genc[smallest_index].outbuf_size, fout, &(set->minf), &(set->maxf));
			curr_sample+=set->blocks[smallest_index];
		}
		++effort_anal_run;
	}
	MD5_Final(set->hash, &ctx);

	effort_anal=0;
	for(i=0;i<set->blocks_count;++i)
		effort_anal+=set->blocks[i];
	effort_anal*=effort_anal_run;
	effort_anal/=tot_samples;

	effort_output=1;
	if(!set->diff_comp_settings){
		effort_output=effort_anal;
		effort_anal=0;
	}

	qsort(set->blocks, set->blocks_count, sizeof(int), comp_int_asc);
	printf("greed\t%s\t%s\t%u\t%u", set->comp_anal, set->comp_output, set->tweak, set->blocks[0]);
	for(i=1;i<set->blocks_count;++i)
		printf(",%u", set->blocks[i]);
	cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	printf("\t%.3f\t%.3f\t%.3f\t%zu\t%.5f\n", effort_anal, effort_tweak, effort_output, outsize, cpu_time);

	return 0;
}
/* end of greed mode code */

//Tweak partition of adjacent blocksizes to look for more efficient form
void tweak(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, frame_list *f, int16_t *input, size_t amount);
static void pairtest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, frame_list *f, int16_t *input, size_t newsplit);

static void pairtest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, frame_list *f, int16_t *input, size_t newsplit){
	FLAC__StaticEncoder *af, *bf;
	void *abuf, *bbuf;
	size_t abuf_size, bbuf_size;
	size_t bsize=((f->blocksize+f->next->blocksize)-newsplit);
	if(newsplit<16 || bsize<16)
		return;
	if(newsplit>65535 || (!set->lax && newsplit>4608))
		return;
	if(bsize>65535 || (!set->lax && bsize>4608))
		return;
	af=init_static_encoder(set, newsplit, comp, apod);
	bf=init_static_encoder(set, bsize, comp, apod);
	FLAC__static_encoder_process_frame_bps16_interleaved(
		af,
		input+(f->curr_sample*set->channels),
		newsplit,
		f->curr_sample,
		&abuf,
		&abuf_size
	);
	FLAC__static_encoder_process_frame_bps16_interleaved(
		bf,
		input+((f->curr_sample+newsplit)*set->channels),
		bsize,
		f->curr_sample+newsplit,
		&bbuf,
		&bbuf_size
	);
	(*effort)+=newsplit+bsize;
	/*struct frame_list{
	uint8_t *outbuf;
	size_t outbuf_size;
	size_t blocksize;
	uint64_t curr_sample;
	frame_list *next, *prev;
};*/
	if((abuf_size+bbuf_size)<(f->outbuf_size+f->next->outbuf_size)){
		(*saved)+=((f->outbuf_size+f->next->outbuf_size)-(abuf_size+bbuf_size));

		f->outbuf=realloc(f->outbuf, abuf_size);
		memcpy(f->outbuf, abuf, abuf_size);
		f->outbuf_size=abuf_size;
		f->blocksize=newsplit;

		f->next->outbuf=realloc(f->next->outbuf, bbuf_size);
		memcpy(f->next->outbuf, bbuf, bbuf_size);
		f->next->outbuf_size=bbuf_size;
		f->next->blocksize=bsize;
		f->next->curr_sample=f->curr_sample+newsplit;
	}
	FLAC__static_encoder_delete(af);
	FLAC__static_encoder_delete(bf);
}

void tweak(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, frame_list *f, int16_t *input, size_t amount){
	pairtest(effort, saved, set, comp, apod, f, input, f->blocksize-amount);
	//if(eff==*effort)//If one direction improved efficiency, it's unlikely the other way will?
	pairtest(effort, saved, set, comp, apod, f, input, f->blocksize+amount);
}

/* peak mode code */
typedef struct{
	FLAC__StaticEncoder *enc;
	void *outbuf;
} peak_hunter;

int peak_main(int16_t *input, size_t input_size, FILE *fout, flac_settings *set){
	int k;
	size_t effort=0, i, j, *step, step_count;
	size_t *frame_results, *running_results;
	uint8_t *running_step;
	size_t window_size, window_size_check=0;
	size_t outsize=42;
	size_t tot_samples;
	size_t effort_print=0;
	frame_list *frame=NULL, *frame_next;
	peak_hunter *work;
	FLAC__StaticEncoder *encout;
	double effort_anal, effort_output=0, effort_tweak=0;
	clock_t cstart;
	double cpu_time;
	cstart=clock();

	tot_samples=input_size/(set->channels*(set->bps/8));

	if(set->blocks_count==1)
		goodbye("Error: At least two blocksizes must be available\n");
	if(set->blocks_count>255)
		goodbye("Error: Implementation limited to up to 255 blocksizes. This is also just an insane heat-death-of-the-universe setting\n");

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

	frame_results=malloc(sizeof(size_t)*step_count*(window_size+1));
	running_results=malloc(sizeof(size_t)*(window_size+1));
	running_step=malloc(sizeof(size_t)*(window_size+1));

	/* process frames for stats */
	omp_set_num_threads(set->work_count);
	work=malloc(sizeof(peak_hunter)*set->work_count);
	for(j=0;j<step_count;++j){
		for(k=0;k<set->work_count;++k)
			work[k].enc=init_static_encoder(set, set->blocks[j], set->comp_anal, set->apod_anal);
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
		fprintf(stderr, "Processed %zu/%zu\n", effort_print, effort);
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
		frame->curr_sample=frame_at*set->blocksize_min;
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

	//if used simpler settings for analysis, encode properly here to get comp_output sizes
	//store encoded frames as most/all of these are likely to be used depending on how much tweaking is done
	if(set->diff_comp_settings){
		for(frame_next=frame;frame_next;frame_next=frame_next->next){
			void *tmpbuf;
			encout=init_static_encoder(set, frame_next->blocksize, set->comp_output, set->apod_output);
			FLAC__static_encoder_process_frame_bps16_interleaved(encout,
				input+(frame_next->curr_sample*set->channels),
				frame_next->blocksize,
				frame_next->curr_sample,
				&tmpbuf,
				&(frame_next->outbuf_size)
			);
			frame_next->outbuf=malloc(frame_next->outbuf_size);
			memcpy(frame_next->outbuf, tmpbuf, frame_next->outbuf_size);
			FLAC__static_encoder_delete(encout);
		}
	}

	if(set->tweak){
		int ind;
		size_t teff=0, tsaved=0;
		for(ind=0;ind<set->tweak;++ind){
			for(frame_next=frame;frame_next && frame_next->next;frame_next=frame_next->next)
				tweak(&teff, &tsaved, set, set->comp_output, set->apod_output, frame_next, input, set->blocksize_min/(ind+2));
		}
		effort_tweak=teff;
		effort_tweak/=tot_samples;
	}

	//merge TODO

	/* Write optimal result */
	for(frame_next=frame;frame_next;frame_next=frame_next->next){
		if(!(frame_next->outbuf)){//encode if not stored
			size_t comp;
			encout=init_static_encoder(set, frame_next->blocksize, set->comp_output, set->apod_output);
			FLAC__static_encoder_process_frame_bps16_interleaved(encout,
				input+(frame_next->curr_sample*set->channels),
				frame_next->blocksize,
				frame_next->curr_sample,
				&(frame_next->outbuf),
				&(comp)
			);
			if(!set->diff_comp_settings)
				assert(frame_next->outbuf_size==comp);
			outsize+=fwrite_framestat(frame_next->outbuf, frame_next->outbuf_size, fout, &(set->minf), &(set->maxf));
			FLAC__static_encoder_delete(encout);
		}
		else
			outsize+=fwrite_framestat(frame_next->outbuf, frame_next->outbuf_size, fout, &(set->minf), &(set->maxf));
	}
	if(!set->diff_comp_settings && !set->tweak)
		assert(outsize==(running_results[window_size]+42));

	if(tot_samples-(window_size*set->blocks[0])){/* partial end frame */
		void *partial_outbuf;
		size_t partial_outbuf_size;
		encout=init_static_encoder(set, set->blocksize_max, set->comp_output, set->apod_output);
		FLAC__static_encoder_process_frame_bps16_interleaved(encout,
			input+((window_size*set->blocks[0])*set->channels),
			tot_samples-(window_size*set->blocks[0]),
			(window_size*set->blocks[0]),
			&(partial_outbuf),
			&(partial_outbuf_size)
		);
		outsize+=fwrite_framestat(partial_outbuf, partial_outbuf_size, fout, &(set->minf), &(set->maxf));
		FLAC__static_encoder_delete(encout);
	}
	MD5(((void*)input), input_size, set->hash);

	effort_anal=0;
	for(i=0;i<set->blocks_count;++i)//analysis effort approaches the sum of the normalised blocksizes as window_size approaches infinity
		effort_anal+=step[i];
	effort_output+=1;
	if(!set->diff_comp_settings){
		effort_output+=effort_anal;
		effort_anal=0;
	}

	printf("peakset\t%s\t%s\t%u\t%u", set->comp_anal, set->comp_output, set->tweak, set->blocks[0]);
	for(i=1;i<set->blocks_count;++i)
		printf(",%u", set->blocks[i]);
	cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	printf("\t%.3f\t%.3f\t%.3f\t%zu\t%.5f\n", effort_anal, effort_tweak, effort_output, outsize, cpu_time);

	return 0;
}
/* end of peak mode code */
