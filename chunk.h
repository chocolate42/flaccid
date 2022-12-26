/*Chunk mode implementation. Cheap chunked tree-like search-space, reasonable for low-effort encodes*/
#ifndef CHUNK
#define CHUNK

#include "common.h"

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
	flist *list;
};

typedef struct{
	chunk_enc chunk[SHARD_COUNT];
	uint32_t curr_chunk[SHARD_COUNT];
	int status[SHARD_COUNT];
} chunk_worker;

size_t chunk_analyse(chunk_enc *c);
chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set);
void chunk_invalidate(chunk_enc *c);
flist *chunk_list(chunk_enc *c, flist *f, flist **head);
void chunk_process(chunk_enc *c, void *input, uint64_t sample_number, flac_settings *set);
void chunk_write(flac_settings *set, chunk_enc *c, void *input, FILE *fout, uint32_t *minf, uint32_t *maxf, size_t *outsize);
int chunk_main(void *input, size_t input_size, FILE *fout, flac_settings *set);

#endif
