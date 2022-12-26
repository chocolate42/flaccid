/*Tweak pass implementation. Tweak pass optimises where adjacent blocks are divided*/
#ifndef TWEAK
#define TWEAK

#include "common.h"

void tweak(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, size_t amount);
void tweak_pass(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input);
void tweak_pass_mt(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input);
void tweaktest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, size_t newsplit);

#endif
