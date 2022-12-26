/*Peakset mode implementation. Peakset outputs an optimal block permutation for a narrow set of constraints*/
#ifndef PEAKSET
#define PEAKSET

#include "common.h"
#include "FLAC/stream_encoder.h"

typedef struct{
	FLAC__StaticEncoder *enc;
	void *outbuf;
} peak_hunter;

int peak_main(void *input, size_t input_size, FILE *fout, flac_settings *set);

#endif
