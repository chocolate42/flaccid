/*Input loading*/
#ifndef LOAD
#define LOAD

#include "common.h"

//relative locations of analysis pointer, output pointer and pointer to end of last sample
#define INLOC_ANAL ((in->set->bps==16?2:4)*in->set->channels*(in->loc_analysis-in->loc_buffer))

int input_fopen(input *input, char *path, flac_settings *set);

#endif
