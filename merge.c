#include "merge.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

int mergetest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, int free_removed){
	FLAC__StaticEncoder *enc;
	flist *deleteme;
	void *buf;
	size_t buf_size;
	assert(f && f->next);
	enc=init_static_encoder(set, f->blocksize+f->next->blocksize, comp, apod);
	set->encode_func(
		enc,
		input+(f->curr_sample*set->channels*(set->bps==16?2:4)),
		f->blocksize+f->next->blocksize,
		f->curr_sample,
		&buf,
		&buf_size
	);
	(*effort)+=f->blocksize+f->next->blocksize;
	if(buf_size<(f->outbuf_size+f->next->outbuf_size)){
		(*saved)+=((f->outbuf_size+f->next->outbuf_size)-buf_size);
		/*delete last frame, relink*/
		deleteme=f->next;
		f->blocksize=f->blocksize+f->next->blocksize;
		assert(f->blocksize!=0);
		f->next=deleteme->next;
		if(f->next)
			f->next->prev=f;
		if(deleteme->is_outbuf_alloc)
			free(deleteme->outbuf);
		if(free_removed)
			free(deleteme);/*delete now*/
		else
			deleteme->blocksize=0;/*delete later*/

		if(f->is_outbuf_alloc)
			f->outbuf=realloc(f->outbuf, buf_size);
		else{
			f->outbuf=malloc(buf_size);
			f->is_outbuf_alloc=1;
		}
		memcpy(f->outbuf, buf, buf_size);
		f->outbuf_size=buf_size;
		if(f->prev)
			f->prev->merge_tried=0;//reset previous pair merge flag, second frame is now different
		FLAC__static_encoder_delete(enc);
		return 1;
	}
	else{
		f->merge_tried=1;//set flag so future passes don't redo identical test
		FLAC__static_encoder_delete(enc);
		return 0;
	}
}

int merge(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, int free_removed){
	int merged_size=f->blocksize+f->next->blocksize;
	if(merged_size>set->blocksize_max && merged_size<=set->blocksize_limit_upper)
		return mergetest(effort, saved, set, comp, apod, f, input, free_removed);
	return 0;
}

void merge_pass(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input){
	flist *frame_curr;
	int ind=0;
	size_t saved;
	do{//keep doing passes until merge threshold is not hit
		saved=*tsaved;
		for(frame_curr=head;frame_curr&&frame_curr->next;frame_curr=frame_curr->next)
			merge(teff, tsaved, set, comp, apod, frame_curr, input, 1);
		++ind;
		fprintf(stderr, "merge(%d) saved %zu bytes\n", ind, (*tsaved-saved));
	}while((*tsaved-saved)>=set->merge);
}

void merge_pass_mt(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input){
	flist *frame_curr;
	flist **array;
	int ind=0;
	size_t cnt=0, i, saved;
	//build pointer array to make omp code easy
	for(frame_curr=head;frame_curr;frame_curr=frame_curr->next)
		++cnt;
	array=malloc(sizeof(flist*)*cnt);
	cnt=0;
	for(frame_curr=head;frame_curr;frame_curr=frame_curr->next){
		array[cnt++]=frame_curr;
		assert(frame_curr->blocksize!=0);
	}
	do{
		saved=*tsaved;
		#pragma omp parallel for num_threads(set->work_count) shared(array)
		for(i=0;i<cnt/2;++i){//even
			if(merge(teff, tsaved, set, comp, apod, array[2*i], input, 0))
				assert(array[2*i]->blocksize && !array[(2*i)+1]->blocksize);
		}
		#pragma omp barrier
		/*odd indexes may have blocksize zero if merges occurred, these are dummy frames to maintain array indexing*/
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<(cnt-1)/2;++i){//odd, only do if there's no potential for overlap
			if(array[(2*i)+1]->blocksize && array[(2*i)+2]->blocksize)
				merge(teff, tsaved, set, comp, apod, array[(2*i)+1], input, 0);
		}
		#pragma omp barrier
		/*remove dummy frames*/
		for(i=0;i<cnt;++i){//mildly inefficient fix TODO
			if(array[i]->blocksize==0){
				free(array[i]);
				memcpy(array+i, array+i+1, sizeof(flist*)*((cnt-i)-1));
				--i;
				--cnt;
			}
		}

		++ind;
		fprintf(stderr, "merge(%d) saved %zu bytes\n", ind, (*tsaved-saved));
	}while((*tsaved-saved)>=set->merge);
	free(array);
}
