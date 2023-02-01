#include "tweak.h"

#include <stdlib.h>
#include <string.h>

void tweaktest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, size_t newsplit){
	FLAC__StaticEncoder *af, *bf;
	void *abuf, *bbuf;
	size_t abuf_size, bbuf_size;
	size_t bsize=((f->blocksize+f->next->blocksize)-newsplit);
	if(newsplit<16 || bsize<16)
		return;
	if(newsplit>set->blocksize_limit_upper || newsplit<set->blocksize_limit_lower)
		return;
	if(bsize>set->blocksize_limit_upper || bsize<set->blocksize_limit_lower)
		return;
	if(newsplit>=(f->blocksize+f->next->blocksize))
		return;
	af=init_static_encoder(set, newsplit, comp, apod);
	bf=init_static_encoder(set, bsize, comp, apod);
	set->encode_func(
		af,
		input+(f->curr_sample*set->channels*(set->bps==16?2:4)),
		newsplit,
		f->curr_sample,
		&abuf,
		&abuf_size
	);
	set->encode_func(
		bf,
		input+((f->curr_sample+newsplit)*set->channels*(set->bps==16?2:4)),
		bsize,
		f->curr_sample+newsplit,
		&bbuf,
		&bbuf_size
	);
	(*effort)+=newsplit+bsize;

	if((abuf_size+bbuf_size)<(f->outbuf_size+f->next->outbuf_size)){
		(*saved)+=((f->outbuf_size+f->next->outbuf_size)-(abuf_size+bbuf_size));
		if(f->is_outbuf_alloc)
			f->outbuf=realloc(f->outbuf, abuf_size);
		else{
			f->outbuf=malloc(abuf_size);
			f->is_outbuf_alloc=1;
		}
		memcpy(f->outbuf, abuf, abuf_size);
		f->outbuf_size=abuf_size;
		f->blocksize=newsplit;

		if(f->next->is_outbuf_alloc)
			f->next->outbuf=realloc(f->next->outbuf, bbuf_size);
		else{
			f->next->outbuf=malloc(bbuf_size);
			f->next->is_outbuf_alloc=1;
		}
		memcpy(f->next->outbuf, bbuf, bbuf_size);
		f->next->outbuf_size=bbuf_size;
		f->next->blocksize=bsize;
		f->next->curr_sample=f->curr_sample+newsplit;
	}
	FLAC__static_encoder_delete(af);
	FLAC__static_encoder_delete(bf);
}

void tweak(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, size_t amount){
	tweaktest(effort, saved, set, comp, apod, f, input, f->blocksize-amount);
	tweaktest(effort, saved, set, comp, apod, f, input, f->blocksize+amount);
}

void tweak_pass(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input){
	flist *frame_curr;
	int ind=0;
	size_t saved;
	do{
		saved=*tsaved;
		for(frame_curr=head;frame_curr&&frame_curr->next;frame_curr=frame_curr->next)
			tweak(teff, tsaved, set, comp, apod, frame_curr, input, set->blocksize_min/(ind+2));
		++ind;
		fprintf(stderr, "tweak(%d) saved %zu bytes\n", ind, (*tsaved-saved));
	}while((*tsaved-saved)>=set->merge);
}

void tweak_pass_mt(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input){
	flist *frame_curr;
	flist **array;
	int ind=0;
	size_t cnt=0, i;
	size_t saved;

	//build pointer array to make omp code easy
	for(frame_curr=head;frame_curr;frame_curr=frame_curr->next)
		++cnt;
	array=malloc(sizeof(flist*)*cnt);
	cnt=0;
	for(frame_curr=head;frame_curr;frame_curr=frame_curr->next)
		array[cnt++]=frame_curr;

	do{
		saved=*tsaved;

		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<cnt/2;++i){//even pairs
			tweak(teff, tsaved, set, comp, apod, array[2*i], input, set->blocksize_min/(ind+2));
		}
		#pragma omp barrier

		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<(cnt-1)/2;++i){//odd pairs
			tweak(teff, tsaved, set, comp, apod, array[(2*i)+1], input, set->blocksize_min/(ind+2));
		}
		#pragma omp barrier

		++ind;
		fprintf(stderr, "tweak(%d) saved %zu bytes\n", ind, (*tsaved-saved));
	}while((*tsaved-saved)>=set->tweak);
	free(array);
}
