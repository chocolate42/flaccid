/*Gasc mode implementation. Tries increasingly large blocksizes for the next frame until efficiency decreases*/
#ifndef GASC
#define GASC

#include "common.h"

int gasc_main(void *input, size_t input_size, output *out, flac_settings *set);

#endif
