/*Gset mode implementation. Tries a fixed set of candidates to be the next block, greedily picking the most efficient*/
#ifndef GSET
#define GSET

#include "common.h"

int gset_main(void *input, size_t input_size, output *out, flac_settings *set);

#endif
