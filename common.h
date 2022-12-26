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
	int *blocks, diff_comp_settings, tweak, merge, tweak_after, merge_after, tweak_early_exit, mode;
	size_t blocks_count;
	int work_count, comp_anal_used, do_merge;/*working variables*/
	char *comp_anal, *comp_output, *apod_anal, *apod_output;
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

#endif
