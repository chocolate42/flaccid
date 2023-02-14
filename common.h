/*Functions and structs common to frontend*/
#ifndef COMMON
#define COMMON

#include "FLAC/stream_encoder.h"

#include "dr_wav.h"

#include <inttypes.h>
#include <omp.h>
#include <time.h>

#ifdef USE_OPENSSL
#include <openssl/md5.h>
#else
#include "mbedtls/md5.h"
typedef mbedtls_md5_context MD5_CTX;
#endif

typedef struct{
	int *blocks, diff_comp_settings, tweak, merge, mode, wildcard, outperc, queue_size, md5, lpc_order_limit, rice_order_limit, work_count, peakset_window, seek;
	size_t blocks_count;
	char *input_format;
	char *comp_anal, *comp_output, *comp_outputalt, *apod_anal, *apod_output, *apod_outputalt;
	int lax, channels, bps, sample_rate;/*flac*/
	uint32_t minf, maxf;
	uint8_t hash[16], input_md5[16], zero[16];
	uint64_t input_tot_samples;//total samples if available, probably from input flac header
	int blocksize_min, blocksize_max, blocksize_limit_lower, blocksize_limit_upper;
	FLAC__bool (*encode_func) (FLAC__StaticEncoder*, const void*, uint32_t, uint64_t, void*, size_t*);
} flac_settings;

typedef struct{
	uint64_t *effort_anal, *effort_output, *effort_tweak, *effort_merge;
	double cpu_time;
	size_t outsize, work_count;
} stats;

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

typedef struct{
	int usecache;
	FILE *fout;
	uint8_t *cache;
	size_t cache_size, cache_alloc;
} output;

int out_open(output *out, const char *pathname, int seek);
size_t out_write(output *out, const void *ptr, size_t size);
void out_close(output *out);

typedef struct input input;

typedef struct input{
	void *buf;
	uint64_t loc_analysis;//global loc of where analysis is processing
	uint64_t loc_output;//global loc of what has been fully processed
	uint64_t loc_buffer;//global loc in stream that the start of input array points to
	uint64_t sample_cnt;//local number of samples in input array available to analysis

	FLAC__StreamDecoder *dec;//flac
	drwav wav;//wav
	flac_settings *set;//flac metadata callback fills vitals in

	MD5_CTX ctx;

	size_t (*input_read) (input*, size_t);
	void (*input_close) (input*);
} input;

void goodbye(char *s);
FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, char *comp, char *apod);
void print_settings(flac_settings *set);
void print_stats(stats *stat, input *in);

/*allocate the queue*/
void queue_alloc(queue *q, flac_settings *set);

/*flush the queue then deallocate*/
void queue_dealloc(queue *q, flac_settings *set, input *in, stats *stat, output *out);

/*encode an analysis frame with a simple encoder instance
Also MD5 input if context present, it is up to the analysis algorithm if and when to hash*/
void simple_enc_analyse(simple_enc *senc, flac_settings *set, input *in, uint32_t samples, uint64_t curr_sample, stats *stat);

void simple_enc_dealloc(simple_enc *senc);

/*Encode and output the rest of the file as a single frame with output settings if there's not enough of the file left for analysis to chew on
Advance curr_sample if necessary*/
int simple_enc_eof(queue *q, simple_enc **senc, flac_settings *set, input *in, uint64_t threshold, stats *stat, output *out);

/* Assumes the context has already done an analysis encode with the same input
If analysis settings == output settings, add precomputed frame to output queue
Otherwise, redo frame encode using output settings and add to queue
Return a fresh context as the queue has taken the old one
Advance curr_sample value*/
simple_enc *simple_enc_out(queue *q, simple_enc *senc, flac_settings *set, input *in, stats *stat, output *out);

void mode_boilerplate_init(flac_settings *set, clock_t *cstart, queue *q, stats *stat);
void mode_boilerplate_finish(flac_settings *set, clock_t *cstart, queue *q, stats *stat, input *in, output *out);

#endif
