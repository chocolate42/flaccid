#ifndef SEEKTABLE
#define SEEKTABLE

#include "common.h"

void seektable_init(seektable_t *seektable, flac_settings *set, uint8_t *header);
void seektable_add(seektable_t *seektable, uint64_t sample_num, uint64_t offset, uint16_t frame_sample_cnt);
void seektable_write_dummy(seektable_t *seektable, flac_settings *set, output *out);
void seektable_write(seektable_t *seektable, output *out);

#endif
