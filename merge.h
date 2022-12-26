/*Merge pass implementation. Merge pass merges adjacent blocks if it's more optimal*/
#ifndef MERGE
#define MERGE

#include "common.h"

void merge_pass(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input);
void merge_pass_mt(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input);
int merge(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, int free_removed);
int mergetest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, int free_removed);

#endif
