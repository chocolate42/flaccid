#ifndef LOAD
#define LOAD

#include "common.h"

void *load_input(char *path, size_t *input_size, flac_settings *set);
void *load_flac(char *path, size_t *input_size, flac_settings *set);
void *load_raw(char *path, size_t *input_size, flac_settings *set);

#endif
