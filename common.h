/*Functions and structs common to frontend*/
#ifndef COMMON
#define COMMON

#include "FLAC/stream_encoder.h"

#include <inttypes.h>

#ifdef USE_OPENSSL
#include <openssl/md5.h>
#else
#include "mbedtls/md5.h"
typedef mbedtls_md5_context MD5_CTX;
#endif

typedef struct{
	int *blocks, diff_comp_settings, tweak, merge, tweak_early_exit, mode, wildcard, outperc;
	size_t blocks_count;
	int work_count, comp_anal_used, do_merge;/*working variables*/
	char *comp_anal, *comp_output, *comp_outputalt, *apod_anal, *apod_output, *apod_outputalt;
	int lax, channels, bps, sample_rate;/*flac*/
	uint32_t minf, maxf;
	uint8_t hash[16];
	int blocksize_min, blocksize_max, blocksize_stride, blocksize_limit_lower, blocksize_limit_upper;
	FLAC__bool (*encode_func) (FLAC__StaticEncoder*, const void*, uint32_t, uint64_t, void*, size_t*);
} flac_settings;

typedef struct{
	double effort_anal, effort_output, effort_tweak, effort_merge;
	double cpu_time, time_anal, time_tweak, time_merge;
	size_t outsize;
} stats;

typedef struct flist flist;

struct flist{
	int is_outbuf_alloc, merge_tried;
	uint8_t *outbuf;
	size_t outbuf_size;
	size_t blocksize;
	uint64_t curr_sample;
	flist *next, *prev;
};

int comp_int_asc(const void *aa, const void *bb);
int comp_int_desc(const void *aa, const void *bb);
void flist_initial_output_encode(flist *frame, flac_settings *set, void *input);
void flist_write(flist *frame, flac_settings *set, void *input, size_t *outsize, FILE *fout);
size_t fwrite_framestat(const void *ptr, size_t size, FILE *stream, uint32_t *minf, uint32_t *maxf);
void goodbye(char *s);
FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, char *comp, char *apod);
void parse_blocksize_list(char *list, int **res, size_t *res_cnt);
void print_settings(flac_settings *set);
void print_stats(stats *stat);

/*wrap a static encoder with its output*/
typedef struct{
	FLAC__StaticEncoder *enc;
	uint8_t *outbuf;
	size_t outbuf_size, sample_cnt;
} simple_enc;

/*encode a frame with a simple_encoder instance*/
void simple_enc_encode(simple_enc *senc, flac_settings *set, void *input, uint32_t samples, uint64_t curr_sample, int is_anal, stats *stat);

/*encode a frame with a simple encoder instance
Also MD5 input if context present
Also write to file if IO stream present*/
void simple_enc_aio(simple_enc *senc, flac_settings *set, void *input, uint32_t samples, uint64_t curr_sample, int is_anal, MD5_CTX *ctx, FILE *fout, stats *stat);

/*If analysis settings == output settings, write precomputed frame to file
Otherwise, redo frame encode using output settings and write to file
Advance curr_sample value*/
void simple_enc_out(simple_enc *senc, flac_settings *set, void *input, uint64_t *curr_sample, FILE *fout, stats *stat, int *outstate);

/*Encode and output the rest of the file as a single frame with output settings if there's not much left of the file
Advance curr_sample if necessary*/
int simple_enc_eof(simple_enc *senc, flac_settings *set, void *input, uint64_t *curr_sample, uint64_t tot_samples, uint64_t threshold, MD5_CTX *ctx, FILE *fout, stats *stat);

#endif
