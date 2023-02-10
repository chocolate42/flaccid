/*Functions and structs common to frontend*/
#ifndef COMMON
#define COMMON

#include "FLAC/stream_encoder.h"

#include <inttypes.h>
#include <omp.h>
#include <time.h>

#ifdef USE_OPENSSL
#include <openssl/md5.h>
#else
#include "mbedtls/md5.h"
typedef mbedtls_md5_context MD5_CTX;
void MD5_Init(MD5_CTX *ctx);
void MD5_Final(uint8_t *h, MD5_CTX *ctx);
int MD5(const unsigned char *d, size_t s, unsigned char *h);
int MD5_Update(MD5_CTX *ctx, const unsigned char *d, size_t s);
#endif

typedef struct{
	int *blocks, diff_comp_settings, tweak, merge, mode, wildcard, outperc, queue_size, md5, lpc_order_limit, rice_order_limit;
	size_t blocks_count;
	int work_count, comp_anal_used, do_merge;/*working variables*/
	char *comp_anal, *comp_output, *comp_outputalt, *apod_anal, *apod_output, *apod_outputalt;
	int lax, channels, bps, sample_rate;/*flac*/
	uint32_t minf, maxf;
	uint8_t hash[16];
	int blocksize_min, blocksize_max, blocksize_stride, blocksize_limit_lower, blocksize_limit_upper;
	FLAC__bool (*encode_func) (FLAC__StaticEncoder*, const void*, uint32_t, uint64_t, void*, size_t*);
} flac_settings;

void MD5_UpdateSamples(MD5_CTX *ctx, const void *input, size_t curr_sample, size_t sample_cnt, flac_settings *set);

typedef struct{
	double effort_anal, effort_output, effort_tweak, effort_merge;
	double cpu_time, time_anal, time_tweak, time_merge;
	size_t outsize;
} stats;

void goodbye(char *s);
FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, char *comp, char *apod);
void print_settings(flac_settings *set);
void print_stats(stats *stat);

/*wrap a static encoder with its output*/
typedef struct{
	FLAC__StaticEncoder *enc;
	uint8_t *outbuf;
	size_t outbuf_size, sample_cnt;
	uint64_t curr_sample;
} simple_enc;

/*output queue*/
typedef struct{
	simple_enc **sq;
	size_t depth;
	int *outstate;
	size_t *saved;
} queue;

/*allocate the queue*/
void queue_alloc(queue *q, flac_settings *set);

/*flush the queue then deallocate*/
void queue_dealloc(queue *q, flac_settings *set, void *input, stats *stat, FILE *fout);

/*encode an analysis frame with a simple encoder instance
Also MD5 input if context present, it is up to the analysis algorithm if and when to hash*/
void simple_enc_analyse(simple_enc *senc, flac_settings *set, void *input, uint32_t samples, uint64_t curr_sample, stats *stat, MD5_CTX *ctx);

void simple_enc_dealloc(simple_enc *senc);

/*Encode and output the rest of the file as a single frame with output settings if there's not enough of the file left for analysis to chew on
Advance curr_sample if necessary*/
int simple_enc_eof(queue *q, simple_enc **senc, flac_settings *set, void *input, uint64_t *curr_sample, uint64_t tot_samples, uint64_t threshold, stats *stat, MD5_CTX *ctx, FILE *fout);

/* Assumes the context has already done an analysis encode with the same input
If analysis settings == output settings, add precomputed frame to output queue
Otherwise, redo frame encode using output settings and add to queue
Return a fresh context as the queue has taken the old one
Advance curr_sample value*/
simple_enc *simple_enc_out(queue *q, simple_enc *senc, flac_settings *set, void *input, uint64_t *curr_sample, stats *stat, FILE *fout);

void mode_boilerplate_init(flac_settings *set, clock_t *cstart, MD5_CTX *ctx, queue *q);

#endif
