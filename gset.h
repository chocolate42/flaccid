/*Gset mode implementation. Tries a fixed set of candidates to be the next block, greedily picking the most efficient*/
#ifndef GSET
#define GSET

#include "common.h"

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

int gset_main(void *input, size_t input_size, FILE *fout, flac_settings *set);

#endif
