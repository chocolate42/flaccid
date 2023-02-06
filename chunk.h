/*Chunk mode implementation. Cheap chunked tree-like search-space, reasonable for low-effort encodes*/
#ifndef CHUNK
#define CHUNK

#include "common.h"

int chunk_main(void *input, size_t input_size, FILE *fout, flac_settings *set);

#endif
