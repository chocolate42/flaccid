#include <assert.h>
#include <getopt.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef USE_OPENSSL
#include <openssl/md5.h>
#else
#include "mbedtls/md5.h"
typedef mbedtls_md5_context MD5_CTX;
void MD5_Init(MD5_CTX *ctx);
void MD5_Final(uint8_t *sha1, MD5_CTX *ctx);
int (*MD5)(const unsigned char*, size_t, unsigned char*) = &mbedtls_md5;
int (*MD5_Update)(MD5_CTX*, const unsigned char*, size_t) = &mbedtls_md5_update;

void MD5_Init(MD5_CTX *ctx){
	mbedtls_md5_init(ctx);
	mbedtls_md5_starts(ctx);
}

void MD5_Final(uint8_t *sha1, MD5_CTX *ctx){
	mbedtls_md5_finish(ctx, sha1);
}
#endif

#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
void usleep(unsigned int microseconds);
void usleep(unsigned int microseconds){
	Sleep(1+(microseconds/1000));
}
#else
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "FLAC/stream_encoder.h"

typedef struct{
	int *blocks, diff_comp_settings, tweak, merge, tweak_after, merge_after, tweak_early_exit;
	size_t blocks_count;
	int work_count, comp_anal_used, do_merge;/*working variables*/
	char *comp_anal, *comp_output, *apod_anal, *apod_output;
	int lax, channels, bps, sample_rate;/*flac*/
	uint32_t minf, maxf;
	uint8_t hash[16];
	int blocksize_min, blocksize_max, blocksize_stride, blocksize_limit_lower, blocksize_limit_upper;
	FLAC__bool (*encode_func) (FLAC__StaticEncoder*, const void*, uint32_t, uint64_t, void*, size_t*);
} flac_settings;

typedef struct flist flist;

struct flist{
	int is_outbuf_alloc, merge_tried;
	uint8_t *outbuf;
	size_t outbuf_size;
	size_t blocksize;
	uint64_t curr_sample;
	flist *next, *prev;
};

void flist_initial_output_encode(flist *frame, flac_settings *set, void *input);
void flist_write(flist *frame, flac_settings *set, void *input, size_t *outsize, FILE *fout);

char *help=
	"Usage: flaccid [options]\n"
	"\nOptions:\n"
	" --analysis-apod apod_string : Apodization settings to use during analysis\n"
	" --analysis-comp comp_string: Compression settings to use during analysis\n"
	" --blocksize-list list,of,sizes : Blocksizes that a mode is allowed to use for analysis.\n"
	"                                  Different modes have different constraints on valid combinations\n"
	" --blocksize-limit-lower limit: Minimum blocksize tweak/merge passes can test\n"
	" --blocksize-limit-upper limit: Maximum blocksize tweak/merge passes can test\n"
	" --in infile : Source, pipe unsupported\n"
	" --lax : Allow non-subset settings\n"
	" --merge threshold : If set enables merge mode, doing passes until a pass saves less than threshold bytes\n"
	" --merge-after : Merge using output settings instead of analysis settings\n"
	" --mode mode : Which variable-blocksize algorithm to use. Valid modes: chunk, greed, peakset\n"
	" --out outfile : Destination\n"
	" --output-apod apod_string : Apodization settings to use during output\n"
	" --output-comp comp_string: Compression settings to use during output\n"
	" --sample-rate num : Set sample rate\n"
	" --tweak threshold : If set enables tweak mode, doing passes until a pass saves less than threshold bytes\n"
	" --tweak-early-exit : Tweak tries increasing and decreasing partition in a single pass. Early exit doesn't\n"
	"                      try the second direction if the first saved space\n"
	" --tweak-after : Tweak using output settings instead of nalysis settings\n"
	" --workers integer : The maximum number of encode threads to run simultaneously\n"
	"\nCompression settings format:\n"
	" * Mostly follows ./flac interface, but requires settings to be concatenated into a single string\n"
	" * Compression level must be the first element\n"
	" * Supported settings: e, m, l, p, q, r (see ./flac -h)\n"
	" * Adaptive mid-side from ./flac is not supported (-M), affects compression levels 1 and 4\n"
	" * ie \"5er4\" defines compression level 5, exhaustive model search, max rice partition order up to 4\n"
	"\nApodization settings format:\n"
	" * All apodization settings in a single semi-colon-delimited string\n"
	" * ie tukey(0.5);partial_tukey(2);punchout_tukey(3)\n";

int comp_int_asc(const void *aa, const void *bb);
int comp_int_desc(const void *aa, const void *bb);
size_t fwrite_framestat(const void *ptr, size_t size, FILE *stream, uint32_t *minf, uint32_t *maxf);
void goodbye(char *s);
FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, char *comp, char *apod);
void parse_blocksize_list(char *list, int **res, size_t *res_cnt);

int comp_int_asc(const void *aa, const void *bb){
	int *a=(int*)aa;
	int *b=(int*)bb;
	if(*a<*b)
		return -1;
	else
		return *a==*b?0:1;
}

int comp_int_desc(const void *aa, const void *bb){
	int *a=(int*)aa;
	int *b=(int*)bb;
	if(*a<*b)
		return 1;
	else
		return *a==*b?0:-1;
}

size_t fwrite_framestat(const void *ptr, size_t size, FILE *stream, uint32_t *minf, uint32_t *maxf){
	if(size<*minf)
		*minf=size;
	if(size>*maxf)
		*maxf=size;
	return fwrite(ptr, 1, size, stream);
}

void goodbye(char *s){
	fprintf(stderr, "%s", s);
	exit(1);
}

FLAC__StaticEncoder *init_static_encoder(flac_settings *set, int blocksize, char *comp, char *apod){
	FLAC__StaticEncoder *r;
	r=FLAC__static_encoder_new();
	r->is_variable_blocksize=set->blocksize_min!=set->blocksize_max;
	if(set->lax)
		FLAC__stream_encoder_set_streamable_subset(r->stream_encoder, false);
	else if(blocksize>16384 || (set->sample_rate<=48000 && blocksize>4608))
		goodbye("Error: Tried to use a non-subset blocksize without setting --lax\n");
	FLAC__stream_encoder_set_channels(r->stream_encoder, set->channels);
	FLAC__stream_encoder_set_bits_per_sample(r->stream_encoder, set->bps);
	FLAC__stream_encoder_set_sample_rate(r->stream_encoder, set->sample_rate);
	if(comp[0]>='0'&&comp[0]<='8')
		FLAC__stream_encoder_set_compression_level(r->stream_encoder, comp[0]-'0');
	if(strchr(comp, 'e'))
		FLAC__stream_encoder_set_do_exhaustive_model_search(r->stream_encoder, true);
	if(strchr(comp, 'l'))
		FLAC__stream_encoder_set_max_lpc_order(r->stream_encoder, atoi(strchr(comp, 'l')+1));
	if(strchr(comp, 'm'))
		FLAC__stream_encoder_set_do_mid_side_stereo(r->stream_encoder, true);
	if(strchr(comp, 'p'))
		FLAC__stream_encoder_set_do_qlp_coeff_prec_search(r->stream_encoder, true);
	if(strchr(comp, 'q'))
		FLAC__stream_encoder_set_qlp_coeff_precision(r->stream_encoder, atoi(strchr(comp, 'q')+1));
	if(strchr(comp, 'r')&&strchr(comp, ',')){
		FLAC__stream_encoder_set_min_residual_partition_order(r->stream_encoder, atoi(strchr(comp, 'r')+1));
		FLAC__stream_encoder_set_max_residual_partition_order(r->stream_encoder, atoi(strchr(comp, ',')+1));
	}
	else if(strchr(comp, 'r'))
		FLAC__stream_encoder_set_max_residual_partition_order(r->stream_encoder, atoi(strchr(comp, 'r')+1));

	if(apod)
		FLAC__stream_encoder_set_apodization(r->stream_encoder, apod);

	FLAC__stream_encoder_set_blocksize(r->stream_encoder, blocksize);/* override compression level blocksize */
	FLAC__stream_encoder_set_loose_mid_side_stereo(r->stream_encoder, false);/* override adaptive mid-side, this doesn't play nice */

	if(FLAC__static_encoder_init(r)!=FLAC__STREAM_ENCODER_INIT_STATUS_OK)
		goodbye("Init failed\n");

	return r;
}

void parse_blocksize_list(char *list, int **res, size_t *res_cnt){
	char *cptr=list-1;
	*res_cnt=0;
	*res=NULL;
	do{
		*res=realloc(*res, sizeof(int)*(*res_cnt+1));
		(*res)[*res_cnt]=atoi(cptr+1);
		if((*res)[*res_cnt]<16)
			goodbye("Error: Blocksize must be at least 16\n");
		if((*res)[*res_cnt]>65535)
			goodbye("Error: Blocksize must be at most 65535\n");
		*res_cnt=*res_cnt+1;
	}while((cptr=strchr(cptr+1, ',')));
}

//Tweak partition of adjacent blocksizes to look for more efficient form
void tweak(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, size_t amount);
void tweak_pass(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input);
void tweak_pass_mt(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input);
static void tweaktest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, size_t newsplit);

static void tweaktest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, size_t newsplit){
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

void flist_initial_output_encode(flist *frame, flac_settings *set, void *input){
	FLAC__StaticEncoder *enc;
	flist *frame_curr;
	void *tmpbuf;
	for(frame_curr=frame;frame_curr;frame_curr=frame_curr->next){
		if(set->diff_comp_settings){/*encode if settings different*/
			enc=init_static_encoder(set, frame_curr->blocksize, set->comp_output, set->apod_output);
			set->encode_func(enc,
				input+(frame_curr->curr_sample*set->channels*(set->bps==16?2:4)),
				frame_curr->blocksize,
				frame_curr->curr_sample,
				&tmpbuf,
				&(frame_curr->outbuf_size)
			);
			frame_curr->outbuf=malloc(frame_curr->outbuf_size);
			frame_curr->is_outbuf_alloc=1;
			memcpy(frame_curr->outbuf, tmpbuf, frame_curr->outbuf_size);
			FLAC__static_encoder_delete(enc);
		}
	}
}

void flist_write(flist *frame, flac_settings *set, void *input, size_t *outsize, FILE *fout){
	FLAC__StaticEncoder *enc;
	flist *frame_curr;
	size_t comp;
	for(frame_curr=frame;frame_curr;frame_curr=frame_curr->next){
		if(!(frame_curr->outbuf)){//encode if not stored
			enc=init_static_encoder(set, frame_curr->blocksize, set->comp_output, set->apod_output);
			set->encode_func(enc,
				input+(frame_curr->curr_sample*set->channels*(set->bps==16?2:4)),
				frame_curr->blocksize,
				frame_curr->curr_sample,
				&(frame_curr->outbuf),
				&(comp)
			);
			if(!set->diff_comp_settings)
				assert(frame_curr->outbuf_size==comp);
			(*outsize)+=fwrite_framestat(frame_curr->outbuf, frame_curr->outbuf_size, fout, &(set->minf), &(set->maxf));
			FLAC__static_encoder_delete(enc);
		}
		else
			(*outsize)+=fwrite_framestat(frame_curr->outbuf, frame_curr->outbuf_size, fout, &(set->minf), &(set->maxf));
		if(set->blocksize_min>frame_curr->blocksize)
			set->blocksize_min=frame_curr->blocksize;
		if(set->blocksize_max<frame_curr->blocksize)
			set->blocksize_max=frame_curr->blocksize;
	}
}

void tweak(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, size_t amount){
	size_t eff=*effort;
	tweaktest(effort, saved, set, comp, apod, f, input, f->blocksize-amount);
	if(set->tweak_early_exit && eff!=*effort)//If one direction improved efficiency, it's unlikely the other way will?
		return;
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

void merge_pass(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input);
void merge_pass_mt(flist *head, flac_settings *set, char *comp, char *apod, size_t *teff, size_t *tsaved, void *input);
int merge(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, int free_removed);
static int mergetest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, int free_removed);

static int mergetest(size_t *effort, size_t *saved, flac_settings *set, char *comp, char *apod, flist *f, void *input, int free_removed){
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

int chunk_main(void *input, size_t input_size, FILE *fout, flac_settings *set);
int greed_main(void *input, size_t input_size, FILE *fout, flac_settings *set);
int  peak_main(void *input, size_t input_size, FILE *fout, flac_settings *set);

void *load_input(char *path, size_t *input_size, flac_settings *set);
void *load_flac(char *path, size_t *input_size, flac_settings *set);
void *load_raw(char *path, size_t *input_size, flac_settings *set);

void *load_input(char *path, size_t *input_size, flac_settings *set){
	if(strlen(path)<5 || strcmp(".flac", path+strlen(path)-5)!=0)//load raw
		return load_raw(path, input_size, set);
	else//load flac TODO
		return load_flac(path, input_size, set);
}

void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data);
void metadata_callback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data);
FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data);

struct flac_loader{
	void *input;
	size_t tot_samples, loc;
	flac_settings *set;
};

void *load_flac(char *path, size_t *input_size, flac_settings *set){
	FLAC__StreamDecoder *dec;
	FLAC__StreamDecoderInitStatus status;
	struct flac_loader loader;

	loader.set=set;
	loader.loc=0;
	dec=FLAC__stream_decoder_new();
	FLAC__stream_decoder_set_md5_checking(dec, true);

	if(FLAC__STREAM_DECODER_INIT_STATUS_OK!=(status=FLAC__stream_decoder_init_file(dec, path, write_callback, metadata_callback, error_callback, &loader))){
		fprintf(stderr, "ERROR: initializing decoder: %s\n", FLAC__StreamDecoderInitStatusString[status]);
		goodbye("");
	}

	FLAC__stream_decoder_process_until_end_of_stream(dec);
	assert(loader.tot_samples==loader.loc);
	FLAC__stream_decoder_delete(dec);
	*input_size=loader.tot_samples*set->channels*(set->bps==16?2:4);
	return loader.input;
}

FLAC__StreamDecoderWriteStatus write_callback(const FLAC__StreamDecoder *dec, const FLAC__Frame *frame, const FLAC__int32 * const buffer[], void *client_data){
	struct flac_loader *fl=(struct flac_loader *)client_data;
	size_t i, j, index=0;
	int16_t *raw16;
	int32_t *raw32;
	(void)dec;
	if(fl->set->bps==16){
		raw16=fl->input;
		for(i=0;i<frame->header.blocksize;++i){
			for(j=0;j<fl->set->channels;++j)
				raw16[(fl->loc*fl->set->channels)+index++]=(FLAC__int16)buffer[j][i];
		}
	}
	else{
		raw32=fl->input;
		for(i=0;i<frame->header.blocksize;++i){
			for(j=0;j<fl->set->channels;++j)
				raw32[(fl->loc*fl->set->channels)+index++]=buffer[j][i];
		}
	}
	fl->loc+=frame->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void metadata_callback(const FLAC__StreamDecoder *dec, const FLAC__StreamMetadata *metadata, void *client_data){
	struct flac_loader *fl=(struct flac_loader *)client_data;
	(void)dec;
	if(metadata->type==FLAC__METADATA_TYPE_STREAMINFO){
		/* save for later */
		if(0==(fl->tot_samples=metadata->data.stream_info.total_samples))
			goodbye("ERROR: This example only works for FLAC files that have a total_samples count in STREAMINFO\n");
		fl->set->sample_rate = metadata->data.stream_info.sample_rate;
		fl->set->channels = metadata->data.stream_info.channels;
		fl->set->bps = metadata->data.stream_info.bits_per_sample;
		fl->input=malloc(fl->tot_samples*fl->set->channels*(fl->set->bps==16?2:4));
		fl->set->encode_func=(fl->set->bps==16)?FLAC__static_encoder_process_frame_bps16_interleaved:FLAC__static_encoder_process_frame_interleaved;
	}
}

void error_callback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data){
	(void)decoder, (void)client_data;
	fprintf(stderr, "Got error callback: %s\n", FLAC__StreamDecoderErrorStatusString[status]);
	goodbye("");
}

#define NO_MMAP

void *load_raw(char *path, size_t *input_size, flac_settings *set){
	void *input;
	#ifdef NO_MMAP
	FILE *fin;
	#else
	int fd;
	struct stat sb;
	#endif

	#ifdef NO_MMAP
	//for now have windows read the entire input immediately instead of using mmap
	//reduces accuracy of speed comparison but at least allows windows users to play
	//for large effort runs this hack shouldn't matter much to speed comparisons
	if(!(fin=fopen(path, "rb")))
		goodbye("Error: fopen() input failed\n");
	if(-1==fseek(fin, 0, SEEK_END))
		goodbye("Error: fseek() input failed\n");
	if(-1==ftell(fin))
		goodbye("Error: ftell() input failed\n");
	*input_size=ftell(fin);
	rewind(fin);
	if(!input_size)
		goodbye("Error: Input empty");
	if(!(input=malloc(*input_size)))
		goodbye("Error: malloc() input failed\n");
	if(*input_size!=fread(input, 1, *input_size, fin))
		goodbye("Error: fread() input failed\n");
	fclose(fin);
	#else
	if(-1==(fd=open(path, O_RDONLY)))
		goodbye("Error: open() input failed\n");
	fstat(fd, &sb);
	*input_size=sb.st_size;
	if(MAP_FAILED==(input=mmap(NULL, *input_size, PROT_READ, MAP_SHARED, fd, 0)))
		goodbye("Error: mmap failed\n");
	close(fd);
	#endif
	set->encode_func=(set->bps==16)?FLAC__static_encoder_process_frame_bps16_interleaved:FLAC__static_encoder_process_frame_interleaved;
	return input;
}

int main(int argc, char *argv[]){
	int (*encoder[3])(void*, size_t, FILE*, flac_settings*)={chunk_main, greed_main, peak_main};
	char *ipath=NULL, *opath=NULL;
	FILE *fout;
	void *input;
	size_t input_size;
	uint8_t header[42]={
		0x66, 0x4C, 0x61, 0x43,//magic
		0x80, 0, 0, 0x22,//streaminfo header
		0, 0, 0, 0,//min/max blocksize TBD
		0, 0, 0, 0, 0, 0,//min/max frame size TBD
		0x0a, 0xc4, 0x42,//44.1khz, 2 channel
		0xf0,//16 bps, upper 4 bits of total samples
		0, 0, 0, 0, //lower 32 bits of total samples
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,//md5
	};

	uint64_t tot_samples;
	flac_settings set;
	static int lax=0;
	static int merge_after=0;
	static int tweak_after=0;
	static int tweak_early_exit=0;
	char *blocklist_str="1152,2304,4608";

	int c, mode=-1, option_index;
	static struct option long_options[]={
		{"analysis-apod", required_argument, 0, 259},
		{"analysis-comp", required_argument, 0, 256},
		{"blocksize-list",	required_argument, 0, 258},
		{"blocksize-limit-lower",	required_argument, 0, 263},
		{"blocksize-limit-upper",	required_argument, 0, 264},
		{"help", no_argument, 0, 'h'},
		{"in", required_argument, 0, 'i'},
		{"lax", no_argument, &lax, 1},
		{"merge",	required_argument, 0, 265},
		{"merge-after", no_argument, &merge_after, 1},
		{"mode", required_argument, 0, 'm'},
		{"out", required_argument, 0, 'o'},
		{"output-apod", required_argument, 0, 260},
		{"output-comp", required_argument, 0, 257},
		{"sample-rate",	required_argument, 0, 262},
		{"tweak", required_argument, 0, 261},
		{"tweak-early-exit", no_argument, &tweak_early_exit, 1},
		{"tweak-after",	required_argument, &tweak_after, 1},
		{"workers", required_argument, 0, 'w'},
		{0, 0, 0, 0}
	};

	memset(&set, 0, sizeof(flac_settings));
	set.apod_anal=NULL;
	set.apod_output=NULL;
	set.blocksize_limit_lower=256;
	set.blocksize_limit_upper=-1;
	set.blocksize_max=4096;
	set.blocksize_min=4096;
	set.bps=16;
	set.channels=2;
	set.comp_anal="5";
	set.comp_output="8p";
	set.diff_comp_settings=0;
	set.merge=4096;
	set.minf=UINT32_MAX;
	set.maxf=0;
	set.sample_rate=44100;
	set.work_count=1;

	while (1){
		if(-1==(c=getopt_long(argc, argv, "hi:m:o:w:", long_options, &option_index)))
			break;
		switch(c){
			case 'h':
				goodbye(help);
				break;

			case 'i':
				ipath=optarg;
				break;

			case 'm':
				if(strcmp(optarg, "chunk")==0)
					mode=0;
				else if(strcmp(optarg, "greed")==0)
					mode=1;
				else if(strcmp(optarg, "peakset")==0)
					mode=2;
				else
					goodbye("Unknown mode\n");
				break;

			case 'o':
				opath=optarg;
				break;

			case 'w':
				set.work_count=atoi(optarg);
				if(set.work_count<1)
					goodbye("Error: Worker count must be >=1, it is the number of encoder cores to use\n");
				break;

			case 256:
				set.comp_anal=optarg;
				break;

			case 257:
				set.comp_output=optarg;
				break;

			case 258:
				blocklist_str=optarg;
				break;

			case 259:
				set.apod_anal=optarg;
				break;

			case 260:
				set.apod_output=optarg;
				break;

			case 261:
				set.tweak=atoi(optarg);
				if(atoi(optarg)<0)
					goodbye("Error: Invalid tweak setting\n");
				break;

			case 262:
				set.sample_rate=atoi(optarg);
				break;

			case 263:
				set.blocksize_limit_lower=atoi(optarg);
				if(atoi(optarg)>65535 || atoi(optarg)<16)
					goodbye("Error: Invalid lower limit blocksize\n");
				break;

			case 264:
				set.blocksize_limit_upper=atoi(optarg);
				if(atoi(optarg)>65535 || atoi(optarg)<16)
					goodbye("Error: Invalid upper limit blocksize\n");
				break;

			case 265:
				set.merge=atoi(optarg);
				if(atoi(optarg)<0)
					goodbye("Error: Invalid merge setting\n");
				break;

			case '?':
				goodbye("");
				break;
		}
	}
	set.merge_after=merge_after;
	set.tweak_after=tweak_after;
	set.tweak_early_exit=tweak_early_exit;
	set.lax=lax;
	if(set.lax && set.blocksize_limit_upper==-1)
		set.blocksize_limit_upper=65535;
	else if(!set.lax && set.blocksize_limit_upper==-1)
		set.blocksize_limit_upper=4608;//<=48KHz assumed fix TODO

	set.diff_comp_settings=strcmp(set.comp_anal, set.comp_output)!=0;
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && !set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(!set.apod_anal && set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && set.apod_output && strcmp(set.apod_anal, set.apod_output)!=0);

	if(!ipath)
		goodbye("Error: No input\n");

	if(!opath)/* Add test option with no no output TODO */
		goodbye("Error: No output\n");

	if(mode==-1)
		goodbye("Error: No mode\n");

	parse_blocksize_list(blocklist_str, &(set.blocks), &(set.blocks_count));
	qsort(set.blocks, set.blocks_count, sizeof(int), comp_int_asc);
	set.blocksize_min=set.blocks[0];
	set.blocksize_max=set.blocks[set.blocks_count-1];

	if(!(fout=fopen(opath, "wb+")))
		goodbye("Error: fopen() output failed\n");
	fwrite(header, 1, 42, fout);

	input=load_input(ipath, &input_size, &set);

	tot_samples=input_size/(set.channels*(set.bps==16?2:4));

	printf("%s\t", ipath);
	encoder[mode](input, input_size, fout, &set);

	/* write finished header */
	header[ 8]=(set.blocksize_min>>8)&255;
	header[ 9]=(set.blocksize_min>>0)&255;
	header[10]=(set.blocksize_max>>8)&255;
	header[11]=(set.blocksize_max>>0)&255;
	header[12]=(set.minf>>16)&255;
	header[13]=(set.minf>> 8)&255;
	header[14]=(set.minf>> 0)&255;
	header[15]=(set.maxf>>16)&255;
	header[16]=(set.maxf>> 8)&255;
	header[17]=(set.maxf>> 0)&255;
	header[18]=(set.sample_rate>>12)&255;
	header[19]=(set.sample_rate>> 4)&255;
	header[20]=((set.sample_rate&15)<<4)|((set.channels-1)<<1)|(((set.bps-1)>>4)&1);
	header[21]=(((set.bps-1)&15)<<4)|((tot_samples>>32)&15);
	header[22]=(tot_samples>>24)&255;
	header[23]=(tot_samples>>16)&255;
	header[24]=(tot_samples>> 8)&255;
	header[25]=(tot_samples>> 0)&255;
	memcpy(header+26, set.hash, 16);
	fflush(fout);
	fseek(fout, 0, SEEK_SET);
	fwrite(header, 1, 42, fout);

	//update seektable TODO

	fclose(fout);
}

/*chunk mode code*/
/* Chunk encoding is a simple brute-force variable blocksize implementation
 * * Define min/max/stride, where max=min*(stride^(effort-1))
 * * effort is the number of blocksizes used, the number of times input is fully encoded
 * * A chunk is the size of the max blocksize
 * * Imagine a chunk partitioned evenly into a n-ary tree, where n=stride
 * * The root is a tier of max blocksize, every tier down represents the frames with blocksize=prev_tier_blocksize/stride
 * * The leaf nodes are the final tier of min blocksize
 * * Compute everything
 * * For every node recursively pick smallest(sum_of_children, self)
*/

#define SHARD_COUNT 4

typedef struct chunk_encoder chunk_enc;

struct chunk_encoder{
	FLAC__StaticEncoder *enc;
	struct chunk_encoder *child;
	size_t child_count;
	uint64_t curr_sample;
	int blocksize;
	uint8_t *outbuf;
	size_t outbuf_size;
	uint8_t use_this;
	flist *list;
};

typedef struct{
	chunk_enc chunk[SHARD_COUNT];
	uint32_t curr_chunk[SHARD_COUNT];
	int status[SHARD_COUNT];
} chunk_worker;
/*status
	1: Waiting to be encoded
	2: Waiting for output
*/

size_t chunk_analyse(chunk_enc *c);
chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set);
void chunk_invalidate(chunk_enc *c);
flist *chunk_list(chunk_enc *c, flist *f, flist **head);
void chunk_process(chunk_enc *c, void *input, uint64_t sample_number, flac_settings *set);
void chunk_write(flac_settings *set, chunk_enc *c, void *input, FILE *fout, uint32_t *minf, uint32_t *maxf, size_t *outsize);

chunk_enc *chunk_init(chunk_enc *c, unsigned int min, unsigned int max, flac_settings *set){
	size_t i;
	assert(min!=0);
	assert(max>=min);

	c->enc=init_static_encoder(set, max, set->comp_anal, set->apod_anal);
	c->blocksize=max;
	c->child_count=set->blocksize_stride;
	if(max/c->child_count<min)
		c->child=NULL;
	else{
		c->child=malloc(sizeof(chunk_enc)*c->child_count);
		for(i=0;i<c->child_count;++i)
			chunk_init(c->child+i, min, max/c->child_count, set);
	}
	return c;
}

/* Do all blocksize encodes for chunk */
void chunk_process(chunk_enc *c, void *input, uint64_t sample_number, flac_settings *set){
	size_t i;
	c->curr_sample=sample_number;
	if(!set->encode_func(c->enc, input, c->blocksize, sample_number, &(c->outbuf), &(c->outbuf_size))){
		fprintf(stderr, "Static encode failed, state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(c->enc->stream_encoder)]);
		exit(1);
	}
	if(c->child){
		for(i=0;i<c->child_count;++i)
			chunk_process(c->child+i, input + i*FLAC__stream_encoder_get_channels(c->child[0].enc->stream_encoder)*FLAC__stream_encoder_get_blocksize(c->child[0].enc->stream_encoder)*(set->bps==16?2:4), sample_number+i*FLAC__stream_encoder_get_blocksize(c->child[0].enc->stream_encoder), set);
	}
}

/* Invalidate worse combination branch */
void chunk_invalidate(chunk_enc *c){
	size_t i;
	c->use_this=0;
	if(c->child){
		for(i=0;i<c->child_count;++i)
			chunk_invalidate(c->child+i);
	}
}

/* Analyse processed chunk to find best frame combination */
size_t chunk_analyse(chunk_enc *c){
	size_t children_size=0, i;
	if(!(c->child)){
		c->use_this=1;
		return c->outbuf_size;
	}

	for(i=0;i<c->child_count;++i)
		children_size+=chunk_analyse(c->child+i);
	if(children_size<c->outbuf_size){
		c->use_this=0;
		return children_size;
	}
	else{
		c->use_this=1;
		for(i=0;i<c->child_count;++i)
			chunk_invalidate(c->child+i);
		return c->outbuf_size;
	}
}

flist *chunk_list(chunk_enc *c, flist *f, flist **head){
	flist *tail;
	size_t i;
	if(c->use_this){
		tail=malloc(sizeof(flist));
		tail->is_outbuf_alloc=0;
		tail->merge_tried=0;
		tail->outbuf=c->outbuf;
		tail->outbuf_size=c->outbuf_size;
		tail->blocksize=c->blocksize;
		tail->curr_sample=c->curr_sample;
		tail->next=NULL;
		tail->prev=NULL;
		if(f){
			f->next=tail;
			tail->prev=f;
		}
		else
			*head=tail;
	}
	else{
		assert(c->child);
		tail=f;
		for(i=0;i<c->child_count;++i)
			tail=chunk_list(c->child+i, tail, head);
	}
	return tail;
}

/* Write best combination of frames in correct order */
void chunk_write(flac_settings *set, chunk_enc *c, void *input, FILE *fout, uint32_t *minf, uint32_t *maxf, size_t *outsize){
	size_t i;
	if(c->use_this){
		if(set->diff_comp_settings){
			FLAC__StaticEncoder *enc;
			enc=init_static_encoder(set, c->blocksize<16?16:c->blocksize, set->comp_output, set->apod_output);
			if(!set->encode_func(enc, input+(c->curr_sample*set->channels*(set->bps==16?2:4)), c->blocksize, c->curr_sample, &(c->outbuf), &(c->outbuf_size))){
				fprintf(stderr, "Static encode failed, state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(c->enc->stream_encoder)]);
				exit(1);
			}
			(*outsize)+=fwrite_framestat(c->outbuf, c->outbuf_size, fout, minf, maxf);
			FLAC__static_encoder_delete(enc);
		}
		else
			(*outsize)+=fwrite_framestat(c->outbuf, c->outbuf_size, fout, minf, maxf);
	}
	else{
		assert(c->child);
		for(i=0;i<c->child_count;++i)
			chunk_write(set, c->child+i, input, fout, minf, maxf, outsize);
	}
}

int chunk_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	unsigned int bytes_per_sample;
	chunk_worker *work;

	//input thread variables
	int i;
	size_t input_chunk_size, block_index;
	uint32_t curr_chunk_in=0, last_chunk, last_chunk_sample_count;
	omp_lock_t curr_chunk_lock;
	//output thread variables
	int output_skipped=0;
	uint32_t curr_chunk_out=0;
	double effort_anal, effort_output, effort_tweak=0, effort_merge=0;
	size_t outsize=42, tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	clock_t cstart, cstart_sub;
	double cpu_time, anal_time=0, tweak_time=0, merge_time=0;
	cstart=clock();

	if(set->blocks_count>1){
		set->blocksize_stride=set->blocks[1]/set->blocks[0];
		if(set->blocksize_stride<2)
			goodbye("Error: Chunk mode requires blocksizes to be a fixed multiple away from each other, >=2\n");

		for(block_index=1;block_index<set->blocks_count;++block_index){
			if(set->blocks[block_index-1]*set->blocksize_stride!=set->blocks[block_index])
				goodbye("Error: Chunk mode requires blocksizes to be a fixed multiple away from each other, >=2\n");
		}
	}
	else{/*hack to use chunk code for a single fixed blocksize*/
		set->blocksize_stride=2;//dummy, anything >1
		set->apod_anal=set->apod_output;
		set->comp_anal=set->comp_output;
		set->diff_comp_settings=0;
		//tweak does nothing as it only works within chunks, which only contain 1 frame
	}

	bytes_per_sample=set->bps==16?2:4;

	input_chunk_size=set->blocksize_max*set->channels*bytes_per_sample;

	omp_init_lock(&curr_chunk_lock);

	last_chunk=(input_size/input_chunk_size)+(input_size%input_chunk_size?0:-1);
	last_chunk_sample_count=(input_size%input_chunk_size)/(set->channels*bytes_per_sample);

	//seektable dummy TODO

	work=malloc(sizeof(chunk_worker)*set->work_count);
	memset(work, 0, sizeof(chunk_worker)*set->work_count);
	for(i=0;i<set->work_count;++i){
		for(int h=0;h<SHARD_COUNT;++h){
			chunk_init(&(work[i].chunk[h]), set->blocksize_min, set->blocksize_max, set);
			work[i].status[h]=1;
			work[i].curr_chunk[h]=UINT32_MAX;
		}
	}

	omp_set_num_threads(set->work_count+2);
	#pragma omp parallel num_threads(set->work_count+2) shared(work)
	{
		if(omp_get_num_threads()!=set->work_count+2){
			fprintf(stderr, "Error: Failed to set %d threads (%dx encoders, 1x I/O, 1x MD5). Actually set %d threads\n", set->work_count+2, set->work_count, omp_get_num_threads());
			exit(1);
		}
		else if(omp_get_thread_num()==set->work_count){//md5 thread
			MD5_CTX ctx;
			uint32_t wavefront, done=0;
			size_t iloc, ilen;
			MD5_Init(&ctx);
			while(1){
				omp_set_lock(&curr_chunk_lock);
				wavefront=curr_chunk_in;
				omp_unset_lock(&curr_chunk_lock);
				if(done<wavefront){
					iloc=done*input_chunk_size;
					ilen=(wavefront>=last_chunk)?(input_size-iloc):((wavefront-done)*input_chunk_size);
					if(set->bps==16)
						MD5_Update(&ctx, ((void*)input)+iloc, ilen);
					else{/*funky input needs funky md5, uses format_input_ from libFLAC as a template*/
						/*don't get it twisted, bytes_per_sample is the size of an element in input, set->bps is the bps of the signal*/
						//TODO
						/*size_t z, q;
						for(z=0;z<ilen/(bytes_per_sample);++z){
							for(q=0;q<((set->bps+7)/8);++q)
								MD5_Update(&ctx, (((uint8_t*)input)+iloc+(z*bytes_per_sample)+(3-q)), 1);//horribly inefficient
						}*/
					}
					done=wavefront;
					if(done>=last_chunk)
						break;
				}
				else
					usleep(100);
			}
			if(set->bps==16)//non-16 bit TODO
				MD5_Final(set->hash, &ctx);
		}
		else if(omp_get_thread_num()==set->work_count+1){//output thread
			int h, j, k, status;
			uint32_t curr_chunk;
			while(curr_chunk_out<=last_chunk){
				for(j=0;j<set->work_count;++j){
					if(output_skipped==set->work_count){//sleep if there's nothing to write
						usleep(100);
						output_skipped=0;
					}
					h=-1;
					for(k=0;k<SHARD_COUNT;++k){
						#pragma omp atomic read
						status=work[j].status[k];
						#pragma omp atomic read
						curr_chunk=work[j].curr_chunk[k];
						if(status==2 && curr_chunk==curr_chunk_out){
							h=k;
							break;
						}
					}
					if(h>-1){//do write
						if(curr_chunk_out==last_chunk)//clean TODO, chunk_write only used as legacy code
							chunk_write(set, &(work[j].chunk[h]), input, fout, &(set->minf), &(set->maxf), &outsize);
						else{
							flist_write(work[j].chunk[h].list, set, input, &outsize, fout);
							//flist_delete(
							work[j].chunk[h].list=NULL;
						}
						++curr_chunk_out;
						#pragma omp atomic
						work[j].status[h]--;
						#pragma omp flush
					}
					else
						++output_skipped;
				}
			}
		}
		else{//worker
			int h;
			unsigned int k, status;
			while(1){
				h=-1;
				for(k=0;k<SHARD_COUNT;++k){
					#pragma omp atomic read
					status=work[omp_get_thread_num()].status[k];
					if(status==1){
						h=k;
						break;
					}
				}
				if(h>-1){//do work
					omp_set_lock(&curr_chunk_lock);
					work[omp_get_thread_num()].curr_chunk[h]=curr_chunk_in++;
					omp_unset_lock(&curr_chunk_lock);
					if(work[omp_get_thread_num()].curr_chunk[h]>last_chunk)
						break;
					if(work[omp_get_thread_num()].curr_chunk[h]==last_chunk && last_chunk_sample_count){//do a partial last chunk as single frame
						if(!set->encode_func(
							work[omp_get_thread_num()].chunk[h].enc,
							input+work[omp_get_thread_num()].curr_chunk[h]*set->channels*set->blocksize_max*(set->bps==16?2:4),
							last_chunk_sample_count, set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h],
							&(work[omp_get_thread_num()].chunk[h].outbuf),
							&(work[omp_get_thread_num()].chunk[h].outbuf_size))){ //using bps16
							fprintf(stderr, "processing failed worker %d sample_number %d\n", omp_get_thread_num(), set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h]);
							fprintf(stderr, "Encoder state: %s\n", FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(work[omp_get_thread_num()].chunk[h].enc->stream_encoder)]);
							exit(1);
						}
						work[omp_get_thread_num()].chunk[h].curr_sample=work[omp_get_thread_num()].curr_chunk[h]*set->blocksize_max;
						work[omp_get_thread_num()].chunk[h].blocksize=last_chunk_sample_count;
						work[omp_get_thread_num()].chunk[h].use_this=1;
					}
					else{//do a full chunk
						cstart_sub=clock();
						chunk_process(&(work[omp_get_thread_num()].chunk[h]), input+work[omp_get_thread_num()].curr_chunk[h]*input_chunk_size, set->blocksize_max*work[omp_get_thread_num()].curr_chunk[h], set);
						chunk_analyse(&(work[omp_get_thread_num()].chunk[h]));
						chunk_list(work[omp_get_thread_num()].chunk+h, NULL, &(work[omp_get_thread_num()].chunk[h].list));
						anal_time+=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;

						if(!set->tweak_after && set->tweak){//tweak with analysis settings
							size_t teff=0, tsaved=0;
							cstart_sub=clock();
							tweak_pass(work[omp_get_thread_num()].chunk[h].list, set, set->comp_anal, set->apod_anal, &teff, &tsaved, input);
							effort_tweak=teff;
							effort_tweak/=tot_samples;
							tweak_time+=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
						}
						flist_initial_output_encode(work[omp_get_thread_num()].chunk[h].list, set, input);
						if(set->tweak_after && set->tweak){//tweak with output settings
							size_t teff=0, tsaved=0;
							cstart_sub=clock();
							tweak_pass(work[omp_get_thread_num()].chunk[h].list, set, set->comp_output, set->apod_output, &teff, &tsaved, input);
							effort_tweak=teff;
							effort_tweak/=tot_samples;
							tweak_time+=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
						}
						/*merge does nothing as we're trying to merge within a chunk, which implicitly merges already */
					}
					#pragma omp atomic
					work[omp_get_thread_num()].status[h]++;
					#pragma omp flush
				}
				else
					usleep(100);
			}
		}
	}
	#pragma omp barrier

	effort_anal=set->blocks_count;
	effort_output=1;
	if(!set->diff_comp_settings){
		effort_output=effort_anal;
		effort_anal=0;
	}

	printf("settings\tmode(chunk);lax(%u);analysis_comp(%s);analysis_apod(%s);output_comp(%s);output_apod(%s);tweak_after(%u);tweak(%u);tweak_early_exit(%u);merge_after(%u);merge(%u);"
		"blocksize_limit_lower(%u);blocksize_limit_upper(%u);analysis_blocksizes(%u", set->lax, set->comp_anal, set->apod_anal, set->comp_output, set->apod_output, set->tweak_after, set->tweak, set->tweak_early_exit, set->merge_after, set->merge, set->blocksize_limit_lower, set->blocksize_limit_upper, set->blocks[0]);
	for(i=1;i<set->blocks_count;++i)
		printf(",%u", set->blocks[i]);
	cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	printf(")\teffort\tanalysis(%.3f);tweak(%.3f);merge(%.3f);output(%.3f)", effort_anal, effort_tweak, effort_merge, effort_output);
	printf("\tsubtiming\tanalysis(%.5f);tweak(%.5f);merge(%.5f)", anal_time, tweak_time, merge_time);
	printf("\tsize\t%zu\tcpu_time\t%.5f\n", outsize, cpu_time);

	return 0;
}
/* end of chunk mode code */

/* greed mode code */
typedef struct{
	FLAC__StaticEncoder *enc;
	double frame_efficiency;
	uint8_t *outbuf;
	size_t outbuf_size;
} greed_encoder;

typedef struct{
	greed_encoder *genc;
	size_t genc_count;
} greed_controller;

int greed_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	size_t i;
	uint64_t curr_sample, tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	greed_controller greed;
	MD5_CTX ctx;
	FLAC__StaticEncoder *oenc;
	double effort_anal, effort_output, effort_tweak=0, effort_merge=0;
	size_t effort_anal_run=0, outsize=42;
	clock_t cstart;
	double cpu_time, anal_time=-1, tweak_time=-1, merge_time=-1;
	cstart=clock();

	MD5_Init(&ctx);

	//blocks sorted descending should help occupancy of multithreading
	qsort(set->blocks, set->blocks_count, sizeof(int), comp_int_desc);

	greed.genc=malloc(set->blocks_count*sizeof(greed_encoder));
	greed.genc_count=set->blocks_count;
	for(i=0;i<set->blocks_count;++i)
		greed.genc[i].enc=init_static_encoder(set, set->blocks[i], set->comp_anal, set->apod_anal);

	for(curr_sample=0;curr_sample<tot_samples;){
		size_t skip=0;
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<greed.genc_count;++i){
			if((tot_samples-curr_sample)>=FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder)){
				set->encode_func(greed.genc[i].enc, input+(curr_sample*set->channels*(set->bps==16?2:4)), FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder), curr_sample, &(greed.genc[i].outbuf), &(greed.genc[i].outbuf_size));
				greed.genc[i].frame_efficiency=greed.genc[i].outbuf_size;
				greed.genc[i].frame_efficiency/=FLAC__stream_encoder_get_blocksize(greed.genc[i].enc->stream_encoder);
			}
			else
				greed.genc[i].frame_efficiency=9999.0;
		}
		#pragma omp barrier
		for(i=0;i<greed.genc_count;++i){
			if(greed.genc[i].frame_efficiency>9998.0)
				++skip;
		}
		if(skip==greed.genc_count){//partial frame at end
			oenc=init_static_encoder(set, (tot_samples-curr_sample)<16?16:(tot_samples-curr_sample), set->comp_output, set->apod_output);
			set->encode_func(oenc, input+(curr_sample*set->channels*(set->bps==16?2:4)), tot_samples-curr_sample, curr_sample, &(greed.genc[0].outbuf), &(greed.genc[0].outbuf_size));
			MD5_Update(&ctx, ((void*)input)+curr_sample*set->channels*(set->bps/8), (tot_samples-curr_sample)*set->channels*(set->bps/8));
			outsize+=fwrite_framestat(greed.genc[0].outbuf, greed.genc[0].outbuf_size, fout, &(set->minf), &(set->maxf));
			FLAC__static_encoder_delete(oenc);
			curr_sample=tot_samples;
		}
		else{
			double smallest_val=8888.0;
			size_t smallest_index=greed.genc_count;
			for(i=0;i<greed.genc_count;++i){
				if(greed.genc[i].frame_efficiency<smallest_val){
					smallest_val=greed.genc[i].frame_efficiency;
					smallest_index=i;
				}
			}
			assert(smallest_index!=greed.genc_count);
			MD5_Update(&ctx, ((void*)input)+curr_sample*set->channels*(set->bps/8), set->blocks[smallest_index]*set->channels*(set->bps/8));
			if(set->diff_comp_settings){
				oenc=init_static_encoder(set, set->blocks[smallest_index], set->comp_output, set->apod_output);
				set->encode_func(oenc, input+(curr_sample*set->channels*(set->bps==16?2:4)), set->blocks[smallest_index], curr_sample, &(greed.genc[smallest_index].outbuf), &(greed.genc[smallest_index].outbuf_size));
				outsize+=fwrite_framestat(greed.genc[smallest_index].outbuf, greed.genc[smallest_index].outbuf_size, fout, &(set->minf), &(set->maxf));
				FLAC__static_encoder_delete(oenc);
			}
			else
				outsize+=fwrite_framestat(greed.genc[smallest_index].outbuf, greed.genc[smallest_index].outbuf_size, fout, &(set->minf), &(set->maxf));
			curr_sample+=set->blocks[smallest_index];
		}
		++effort_anal_run;
	}
	if(set->bps==16)//non-16 bit TODO
		MD5_Final(set->hash, &ctx);

	effort_anal=0;
	for(i=0;i<set->blocks_count;++i)
		effort_anal+=set->blocks[i];
	effort_anal*=effort_anal_run;
	effort_anal/=tot_samples;

	effort_output=1;
	if(!set->diff_comp_settings)
		effort_anal=0;

	qsort(set->blocks, set->blocks_count, sizeof(int), comp_int_asc);

	printf("settings\tmode(greed);lax(%u);analysis_comp(%s);analysis_apod(%s);output_comp(%s);output_apod(%s);tweak_after(%u);tweak(%u);tweak_early_exit(%u);merge_after(%u);merge(%u);"
		"blocksize_limit_lower(%u);blocksize_limit_upper(%u);analysis_blocksizes(%u", set->lax, set->comp_anal, set->apod_anal, set->comp_output, set->apod_output, set->tweak_after, set->tweak, set->tweak_early_exit, set->merge_after, set->merge, set->blocksize_limit_lower, set->blocksize_limit_upper, set->blocks[0]);
	for(i=1;i<set->blocks_count;++i)
		printf(",%u", set->blocks[i]);
	cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	printf(")\teffort\tanalysis(%.3f);tweak(%.3f);merge(%.3f);output(%.3f)", effort_anal, effort_tweak, effort_merge, effort_output);
	printf("\tsubtiming\tanalysis(%.5f);tweak(%.5f);merge(%.5f)", anal_time, tweak_time, merge_time);
	printf("\tsize\t%zu\tcpu_time\t%.5f\n", outsize, cpu_time);

	return 0;
}
/* end of greed mode code */

/* peak mode code */
typedef struct{
	FLAC__StaticEncoder *enc;
	void *outbuf;
} peak_hunter;



int peak_main(void *input, size_t input_size, FILE *fout, flac_settings *set){
	int k;
	size_t effort=0, i, j, *step, step_count;
	size_t *frame_results, *running_results;
	uint8_t *running_step;
	size_t window_size, window_size_check=0;
	size_t outsize=42;
	size_t tot_samples=input_size/(set->channels*(set->bps==16?2:4));
	size_t effort_print=0;
	flist *frame=NULL, *frame_curr;
	peak_hunter *work;
	FLAC__StaticEncoder *encout;
	double effort_anal, effort_output=0, effort_tweak=0, effort_merge=0;
	clock_t cstart, cstart_sub;
	double cpu_time, anal_time=0, tweak_time=0, merge_time=0;
	cstart=clock();

	if(set->blocks_count==1)
		goodbye("Error: At least two blocksizes must be available\n");
	if(set->blocks_count>255)
		goodbye("Error: Implementation limited to up to 255 blocksizes. This is also just an insane heat-death-of-the-universe setting\n");

	for(i=1;i<set->blocks_count;++i){
		if(set->blocks[i]%set->blocks[0])
			goodbye("Error: All blocksizes must be a multiple of the minimum blocksize\n");
	}
	window_size=tot_samples/set->blocks[0];
	if(!window_size)
		goodbye("Error: Input too small\n");

	step=malloc(sizeof(size_t)*set->blocks_count);
	step_count=set->blocks_count;
	for(i=0;i<set->blocks_count;++i)
		step[i]=set->blocks[i]/set->blocks[0];

	for(i=1;i<step_count;++i){
		if(step[i]==step[i-1])
			goodbye("Error: Duplicate blocksizes in argument\n");
	}

	for(i=0;i<step_count;++i)
		effort+=step[i];

	fprintf(stderr, "anal %s comp %s tweak %d merge %d\n", set->comp_anal, set->comp_output, set->tweak, set->merge);

	frame_results=malloc(sizeof(size_t)*step_count*(window_size+1));
	running_results=malloc(sizeof(size_t)*(window_size+1));
	running_step=malloc(sizeof(size_t)*(window_size+1));


	cstart_sub=clock();
	/* process frames for stats */
	omp_set_num_threads(set->work_count);
	work=malloc(sizeof(peak_hunter)*set->work_count);
	for(j=0;j<step_count;++j){
		for(k=0;k<set->work_count;++k)
			work[k].enc=init_static_encoder(set, set->blocks[j], set->comp_anal, set->apod_anal);
		#pragma omp parallel for num_threads(set->work_count)
		for(i=0;i<window_size-(step[j]-1);++i){
			set->encode_func(work[omp_get_thread_num()].enc,
				input+(set->blocksize_min*i*set->channels*(set->bps==16?2:4)),
				set->blocks[j],
				set->blocksize_min*i,
				&(work[omp_get_thread_num()].outbuf),
				frame_results+(i*step_count)+j
			);
		}
		#pragma omp barrier
		for(i=window_size-(step[j]-1);i<window_size;++i)
			frame_results[(i*step_count)+j]=SIZE_MAX;
		for(k=0;k<set->work_count;++k)
			FLAC__static_encoder_delete(work[k].enc);
		effort_print+=step[j];
		fprintf(stderr, "Processed %zu/%zu\n", effort_print, effort);
	}

	/* analyse stats */
	running_results[0]=0;
	for(i=1;i<=window_size;++i){
		size_t curr_run=SIZE_MAX, curr_step=step_count;
		for(j=0;j<step_count;++j){
			if(step[j]>i)//near the beginning we need to ensure we don't go beyond window start
				break;
			if(curr_run>(running_results[i-step[j]]+frame_results[((i-step[j])*step_count)+j])){
				assert(frame_results[((i-step[j])*step_count)+j]!=SIZE_MAX);
				curr_run=(running_results[i-step[j]]+frame_results[((i-step[j])*step_count)+j]);
				curr_step=j;
			}
		}
		assert(curr_run!=SIZE_MAX);
		running_results[i]=curr_run;
		running_step[i]=curr_step;
	}
	anal_time=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;

	/* traverse optimal result */
	for(i=window_size;i>0;){
		size_t frame_at=i-step[running_step[i]];
		frame_curr=frame;
		frame=malloc(sizeof(flist));
		frame->is_outbuf_alloc=0;
		frame->merge_tried=0;
		frame->curr_sample=frame_at*set->blocksize_min;
		frame->blocksize=set->blocks[running_step[i]];
		frame->outbuf=NULL;
		frame->outbuf_size=frame_results[(frame_at*step_count)+running_step[i]];
		frame->next=frame_curr;
		frame->prev=NULL;
		if(frame_curr)
			frame_curr->prev=frame;
		window_size_check+=step[running_step[i]];
		i-=step[running_step[i]];
	}
	assert(i==0);
	assert(window_size_check==window_size);

	if(!set->merge_after && set->merge){
		size_t teff=0, tsaved=0;
		cstart_sub=clock();
		merge_pass_mt(frame, set, set->comp_anal, set->apod_anal, &teff, &tsaved, input);
		effort_merge=teff;
		effort_merge/=tot_samples;
		merge_time=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
	}

	if(!set->tweak_after && set->tweak){
		size_t teff=0, tsaved=0;
		cstart_sub=clock();
		tweak_pass_mt(frame, set, set->comp_anal, set->apod_anal, &teff, &tsaved, input);
		effort_tweak=teff;
		effort_tweak/=tot_samples;
		tweak_time=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
	}

	//if used simpler settings for analysis, encode properly here to get comp_output sizes
	//store encoded frames as most/all of these are likely to be used depending on how much tweaking is done
	flist_initial_output_encode(frame, set, input);

	if(set->merge_after && set->merge){
		size_t teff=0, tsaved=0;
		cstart_sub=clock();
		merge_pass_mt(frame, set, set->comp_output, set->apod_output, &teff, &tsaved, input);
		effort_merge=teff;
		effort_merge/=tot_samples;
		merge_time=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
	}

	if(set->tweak_after && set->tweak){
		size_t teff=0, tsaved=0;
		cstart_sub=clock();
		tweak_pass_mt(frame, set, set->comp_output, set->apod_output, &teff, &tsaved, input);
		effort_tweak=teff;
		effort_tweak/=tot_samples;
		tweak_time=((double)(clock()-cstart_sub))/CLOCKS_PER_SEC;
	}

	/* Write optimal result */
	flist_write(frame, set, input, &outsize, fout);
	if(!set->diff_comp_settings && !set->tweak && !set->merge)
		assert(outsize==(running_results[window_size]+42));

	if(tot_samples-(window_size*set->blocks[0])){/* partial end frame */
		void *partial_outbuf;
		size_t partial_outbuf_size;
		encout=init_static_encoder(set, set->blocksize_max, set->comp_output, set->apod_output);
		set->encode_func(encout,
			input+((window_size*set->blocks[0])*set->channels*(set->bps==16?2:4)),
			tot_samples-(window_size*set->blocks[0]),
			(window_size*set->blocks[0]),
			&(partial_outbuf),
			&(partial_outbuf_size)
		);
		outsize+=fwrite_framestat(partial_outbuf, partial_outbuf_size, fout, &(set->minf), &(set->maxf));
		FLAC__static_encoder_delete(encout);
	}
	if(set->bps==16)//non-16 bit TODO
		MD5(((void*)input), input_size, set->hash);

	effort_anal=0;
	for(i=0;i<set->blocks_count;++i)//analysis effort approaches the sum of the normalised blocksizes as window_size approaches infinity
		effort_anal+=step[i];
	effort_output+=1;

	printf("settings\tmode(peakset);lax(%u);analysis_comp(%s);analysis_apod(%s);output_comp(%s);output_apod(%s);tweak_after(%u);tweak(%u);tweak_early_exit(%u);merge_after(%u);merge(%u);"
		"blocksize_limit_lower(%u);blocksize_limit_upper(%u);analysis_blocksizes(%u", set->lax, set->comp_anal, set->apod_anal, set->comp_output, set->apod_output, set->tweak_after, set->tweak, set->tweak_early_exit, set->merge_after, set->merge, set->blocksize_limit_lower, set->blocksize_limit_upper, set->blocks[0]);
	for(i=1;i<set->blocks_count;++i)
		printf(",%u", set->blocks[i]);
	cpu_time=((double)(clock()-cstart))/CLOCKS_PER_SEC;
	printf(")\teffort\tanalysis(%.3f);tweak(%.3f);merge(%.3f);output(%.3f)", effort_anal, effort_tweak, effort_merge, effort_output);
	printf("\tsubtiming\tanalysis(%.5f);tweak(%.5f);merge(%.5f)", anal_time, tweak_time, merge_time);
	printf("\tsize\t%zu\tcpu_time\t%.5f\n", outsize, cpu_time);

	return 0;
}
/* end of peak mode code */
