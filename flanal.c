#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void _(char *err){
	fprintf(stderr, "%s", err);
	exit(EXIT_FAILURE);
}

FILE *fopen_chk(const char *pathname, const char *mode){
	FILE *r=fopen(pathname, mode);
	if(!r){
		printf("Could not open '%s'\n", pathname);
		_("Err: fopen failed\n");
	}
	return r;
}

size_t fread_chk(void *ptr, size_t size, size_t nmemb, FILE *stream){
	if(fread(ptr, size, nmemb, stream)!=nmemb)
		_("Err: fread failed\n");
	return nmemb;
}

int fseek_chk(FILE *stream, off_t offset, int whence){
	if(fseeko(stream, offset, whence))
		_("Err: fseek failed\n");
	return 0;
}

off_t ftell_chk(FILE *stream){
	off_t r;
	if(-1==(r=ftello(stream)))
		_("Err: ftell failed\n");
	return r;
}

void *realloc_chk(void *ptr, size_t size, size_t prevsize){
	void *r=realloc(ptr, size);
	if(size && !r)
		_("Err: realloc failed\n");
	return r;
}

typedef struct bitfile{
	FILE *fp;
	size_t bitloc, limit;
	uint8_t buf;
	uint64_t build;
} bitfile;

void bitfile_open(bitfile *b, const char *pathname, const char *mode){
	if(strchr(mode, 'w'))
		_("Err: bitfile writing not implemented\n");
	b->fp=fopen_chk(pathname, mode);
	b->bitloc=0;
}

int bitfile_close(bitfile *b){
	return fclose(b->fp);
}

static size_t bitfile_read_chunk(bitfile *b, size_t bits){
	size_t cnt=(8-(b->bitloc%8))<bits?(8-(b->bitloc%8)):bits;
	assert(b&&bits);
	if(!(b->bitloc%8))
		fread_chk(&(b->buf), 1, 1, b->fp);
	b->build<<=cnt;
	b->build|= (b->buf>>(8-((b->bitloc%8)+cnt)))&((1<<cnt)-1);
	b->bitloc+=cnt;
	return cnt;
}

uint64_t bitfile_read(bitfile *b, size_t bits, size_t *counter){
	size_t i=0;
	assert(b&&(bits<65));
	for(b->build=0;i<bits;i+=bitfile_read_chunk(b, bits-i));
	if(counter)
		*counter+=bits;
	return b->build;
}

typedef struct{
	//working memory
	bitfile *b;
	char *file;
	uint8_t *crcwindow, md5[16], *copy;
	uint32_t *residual;
	size_t crcwindow_cnt, curr_sample, is_variable_blockstream_size, frame_count, first_frame_loc, residual_alloc, residual_loc;

	size_t metablocks[8];//metadata block sizes
	size_t blocksize_min, blocksize_max, frame_size_min, frame_size_max, sample_rate, channels, bits_per_sample, samples_in_stream, actual_samples_in_stream;//streaminfo elements
	size_t cframe_sync, cframe_reserved, cframe_blockstrat, cframe_blocksize, cframe_samplerate, cframe_channelassignment, cframe_samplesize, cframe_utf8, cframe_crc8, cframe_pad, cframe_crc16;//frame element bitcounts
	size_t csubframe_reserved, csubframe_type, csubframe_wastedbits;//subframe element bitcounts
	size_t bitlen_constant, bitlen_verbatim, bitlen_fixed, bitlen_lpc;//size of models
	size_t subframe_type[4];//subtype presence counts
	size_t res_escape, res_rice;//number of escaped/rice encodings
	size_t res_reserved, res_type, res_encoding;
} flanal;

enum{STREAMINFO=0, PADDING, APPLICATION, SEEKTABLE, VORBIS_COMMENT, CUESHEET, PICTURE, INVALID=127};

size_t read_utf8(bitfile *b, uint64_t *ret, size_t *counter){
	int i, j;
	uint8_t t;
	if((t=bitfile_read(b, 8, counter))&0x80){//multi-byte
		for(i=5;i>=0;--i){
			if(!(t&(1<<i))){//52 43 34 25 16 07
				*ret=t&((1<<i)-1);
				for(j=1;j<(7-i);++j)
					*ret=(*ret<<6)+(bitfile_read(b, 8, counter)&0x3F);
				return 7-i;
			}
		}
		_("Err: Invalid \"UTF8\" number\n");
	}
	else{
		*ret=t;
		return 1;
	}
	return 0;
}

//crc adapted from ffmpeg
uint16_t calc_crc16(uint8_t *b, size_t cnt){
	const uint8_t *end=b+cnt;
	uint16_t crc=0;
	static const uint16_t ctx[257]={
		0x0000, 0x0580, 0x0F80, 0x0A00, 0x1B80, 0x1E00, 0x1400, 0x1180,
		0x3380, 0x3600, 0x3C00, 0x3980, 0x2800, 0x2D80, 0x2780, 0x2200,
		0x6380, 0x6600, 0x6C00, 0x6980, 0x7800, 0x7D80, 0x7780, 0x7200,
		0x5000, 0x5580, 0x5F80, 0x5A00, 0x4B80, 0x4E00, 0x4400, 0x4180,
		0xC380, 0xC600, 0xCC00, 0xC980, 0xD800, 0xDD80, 0xD780, 0xD200,
		0xF000, 0xF580, 0xFF80, 0xFA00, 0xEB80, 0xEE00, 0xE400, 0xE180,
		0xA000, 0xA580, 0xAF80, 0xAA00, 0xBB80, 0xBE00, 0xB400, 0xB180,
		0x9380, 0x9600, 0x9C00, 0x9980, 0x8800, 0x8D80, 0x8780, 0x8200,
		0x8381, 0x8601, 0x8C01, 0x8981, 0x9801, 0x9D81, 0x9781, 0x9201,
		0xB001, 0xB581, 0xBF81, 0xBA01, 0xAB81, 0xAE01, 0xA401, 0xA181,
		0xE001, 0xE581, 0xEF81, 0xEA01, 0xFB81, 0xFE01, 0xF401, 0xF181,
		0xD381, 0xD601, 0xDC01, 0xD981, 0xC801, 0xCD81, 0xC781, 0xC201,
		0x4001, 0x4581, 0x4F81, 0x4A01, 0x5B81, 0x5E01, 0x5401, 0x5181,
		0x7381, 0x7601, 0x7C01, 0x7981, 0x6801, 0x6D81, 0x6781, 0x6201,
		0x2381, 0x2601, 0x2C01, 0x2981, 0x3801, 0x3D81, 0x3781, 0x3201,
		0x1001, 0x1581, 0x1F81, 0x1A01, 0x0B81, 0x0E01, 0x0401, 0x0181,
		0x0383, 0x0603, 0x0C03, 0x0983, 0x1803, 0x1D83, 0x1783, 0x1203,
		0x3003, 0x3583, 0x3F83, 0x3A03, 0x2B83, 0x2E03, 0x2403, 0x2183,
		0x6003, 0x6583, 0x6F83, 0x6A03, 0x7B83, 0x7E03, 0x7403, 0x7183,
		0x5383, 0x5603, 0x5C03, 0x5983, 0x4803, 0x4D83, 0x4783, 0x4203,
		0xC003, 0xC583, 0xCF83, 0xCA03, 0xDB83, 0xDE03, 0xD403, 0xD183,
		0xF383, 0xF603, 0xFC03, 0xF983, 0xE803, 0xED83, 0xE783, 0xE203,
		0xA383, 0xA603, 0xAC03, 0xA983, 0xB803, 0xBD83, 0xB783, 0xB203,
		0x9003, 0x9583, 0x9F83, 0x9A03, 0x8B83, 0x8E03, 0x8403, 0x8183,
		0x8002, 0x8582, 0x8F82, 0x8A02, 0x9B82, 0x9E02, 0x9402, 0x9182,
		0xB382, 0xB602, 0xBC02, 0xB982, 0xA802, 0xAD82, 0xA782, 0xA202,
		0xE382, 0xE602, 0xEC02, 0xE982, 0xF802, 0xFD82, 0xF782, 0xF202,
		0xD002, 0xD582, 0xDF82, 0xDA02, 0xCB82, 0xCE02, 0xC402, 0xC182,
		0x4382, 0x4602, 0x4C02, 0x4982, 0x5802, 0x5D82, 0x5782, 0x5202,
		0x7002, 0x7582, 0x7F82, 0x7A02, 0x6B82, 0x6E02, 0x6402, 0x6182,
		0x2002, 0x2582, 0x2F82, 0x2A02, 0x3B82, 0x3E02, 0x3402, 0x3182,
		0x1382, 0x1602, 0x1C02, 0x1982, 0x0802, 0x0D82, 0x0782, 0x0202, 0x0001};
	while(b<end)
		crc=ctx[((uint8_t)crc)^*b++]^(crc>>8);
	return crc;
}

uint8_t calc_crc8(uint8_t *b, size_t cnt){
	uint8_t crc=0;
	const uint8_t *end=b+cnt;
	static const uint8_t ctx[257] = {
		0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31,
		0x24, 0x23, 0x2A, 0x2D, 0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65,
		0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D, 0xE0, 0xE7, 0xEE, 0xE9,
		0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
		0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1,
		0xB4, 0xB3, 0xBA, 0xBD, 0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2,
		0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA, 0xB7, 0xB0, 0xB9, 0xBE,
		0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
		0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16,
		0x03, 0x04, 0x0D, 0x0A, 0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42,
		0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A, 0x89, 0x8E, 0x87, 0x80,
		0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
		0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8,
		0xDD, 0xDA, 0xD3, 0xD4, 0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C,
		0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44, 0x19, 0x1E, 0x17, 0x10,
		0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
		0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F,
		0x6A, 0x6D, 0x64, 0x63, 0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B,
		0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13, 0xAE, 0xA9, 0xA0, 0xA7,
		0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
		0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF,
		0xFA, 0xFD, 0xF4, 0xF3, 0x01};
	while(b<end)
		crc=ctx[((uint8_t)crc)^*b++]^(crc>>8);
	return crc;
}

void read_residual(flanal *s, int blocksize, uint32_t predictor_order){
	uint32_t u32, partition_order, partition_cnt, i, j;
	uint32_t num_samples, unencoded=0, rice_param, is_rice5, escape_code;

	if(bitfile_read(s->b, 1, &s->res_reserved))
		_("Err: Residual reserved\n");
	is_rice5=bitfile_read(s->b, 1, &s->res_type);
	escape_code=is_rice5?31:15;

	if((blocksize-predictor_order)>s->residual_alloc){
		s->residual=realloc_chk(s->residual, sizeof(uint32_t)*blocksize,	sizeof(uint32_t)*s->residual_alloc);
		s->copy=realloc_chk(s->copy, sizeof(uint32_t)*blocksize,	sizeof(uint32_t)*s->residual_alloc);
		s->residual_alloc=blocksize-predictor_order;
	}

	s->residual_loc=0;
	partition_order=bitfile_read(s->b, 4, &(s->res_encoding));
	partition_cnt=1<<partition_order;
	for(i=0;i<partition_cnt;++i){
		rice_param=bitfile_read(s->b, is_rice5?5:4, &(s->res_encoding));
		if(rice_param==escape_code)
			unencoded=bitfile_read(s->b, 5, &(s->res_encoding));

		if(!partition_order)
			num_samples=blocksize-predictor_order;
		else if(i)
			num_samples=blocksize/partition_cnt;
		else
			num_samples=(blocksize/partition_cnt)-predictor_order;

		if(rice_param==escape_code){
			++s->res_escape;
			if(unencoded){
				for(j=0;j<num_samples;++j)
					s->residual[s->residual_loc++]=bitfile_read(s->b, unencoded, &(s->res_encoding));
			}
			else{
				for(j=0;j<num_samples;++j)
					s->residual[s->residual_loc++]=0;
			}
		}
		else{//rice coding
			++s->res_rice;
			for(j=0;j<num_samples;++j){
				uint32_t q;
				u32=bitfile_read(s->b, 1, &(s->res_encoding));
				for(q=0;!u32;++q)//unary quotient
					u32=bitfile_read(s->b, 1, &(s->res_encoding));
				u32=bitfile_read(s->b, rice_param, &(s->res_encoding));
				s->residual[s->residual_loc++]=q*(1<<rice_param)+u32;
			}
		}
	}
	assert(s->residual_loc==(blocksize-predictor_order));
}

void read_subframe(flanal *s, int bitspersample, int blocksize, int is_side_subframe){
	uint32_t subframe_type, u32, k, wasted_bits_per_sample=0;
	if(bitfile_read(s->b, 1, &s->csubframe_reserved))
		_("Err: SubFrameHeader start padding non-zero\n");

	subframe_type=bitfile_read(s->b, 6, &s->csubframe_type);
	if(bitfile_read(s->b, 1, &s->csubframe_wastedbits)){
		wasted_bits_per_sample=1;
		u32=bitfile_read(s->b, 1, &s->csubframe_wastedbits);
		while(!u32){
			++wasted_bits_per_sample;
			u32=bitfile_read(s->b, 1, &s->csubframe_wastedbits);
		}
	}

	if(!subframe_type){//SUBFRAME_CONSTANT
		s->subframe_type[0]++;
		bitfile_read(s->b, (bitspersample+is_side_subframe)-wasted_bits_per_sample, &s->bitlen_constant);
	}
	else if(subframe_type==1){//SUBFRAME_VERBATIM
		s->subframe_type[1]++;
		for(k=0;k<blocksize;++k)
			bitfile_read(s->b, (bitspersample+is_side_subframe)-wasted_bits_per_sample, &s->bitlen_verbatim);
	}
	else if(subframe_type<8)
		_("Err: SubFrameHeader type reservedA\n");
	else if(subframe_type<13){//SUBFRAME_FIXED
		int order=(subframe_type&7);
		s->subframe_type[2]++;
		for(k=0;k<order;++k)
			bitfile_read(s->b, (bitspersample+is_side_subframe)-wasted_bits_per_sample, &s->bitlen_fixed);
		read_residual(s, blocksize, order);
	}
	else if(subframe_type<32)
		_("Err: SubFrameHeader type reservedB\n");
	else{//SUBFRAME_LPC
		int order=(subframe_type&31)+1;
		s->subframe_type[3]++;
		for(k=0;k<order;++k)
			bitfile_read(s->b, (bitspersample+is_side_subframe)-wasted_bits_per_sample, &s->bitlen_lpc);
		uint32_t quant_predictor_precision;
		quant_predictor_precision=bitfile_read(s->b, 4, &s->bitlen_lpc)+1;
		if(quant_predictor_precision==16)
			_("Err: SubFrameLPC quant linear predictor coeef precision invalid\n");
		uint32_t quant_predictor_shift;
		quant_predictor_shift=bitfile_read(s->b, 5, &s->bitlen_lpc);
		for(k=0;k<order;++k)
			bitfile_read(s->b, quant_predictor_precision, &s->bitlen_lpc);
		read_residual(s, blocksize, order);
	}
}

int read_frame(flanal *s){
	size_t frame_start=s->b->bitloc/8;
	uint8_t channel_assignment, samplesize, crc8, k;
	uint16_t crc16, crc16_chk;
	static uint32_t blocksize_lookup[16]={0, 192, 576, 1152, 2304, 4608, 8, 16, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
	static uint32_t samplerate_lookup[16]={UINT32_MAX, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000, 8, 16, 160, 0};
	uint8_t samplesize_lookup[8]={255, 8, 12, 0, 16, 20, 24, 32};
	uint32_t blocksize, samplerate;
	uint64_t sample_number=0;

	assert(!(s->b->bitloc%8));
	if(ftell(s->b->fp)==s->b->limit)//EOF
		return 1;
	if(bitfile_read(s->b, 14, &s->cframe_sync)!=16382)
		_("Err: FrameHeader sync code failed\n");
	if(bitfile_read(s->b, 1, &s->cframe_reserved))
		_("Err: FrameHeader reservedA failed\n");
	if(bitfile_read(s->b, 1, &s->cframe_blockstrat)!=s->is_variable_blockstream_size)
		_("Err: FrameHeader BlockingStrategy doesn't match StreamInfo block\n");
	if(!(blocksize=bitfile_read(s->b, 4, &s->cframe_blocksize)))
		_("Err: FrameHeader BlockSize reserved\n");
	if((samplerate=bitfile_read(s->b, 4, &s->cframe_samplerate))==15)
		_("Err: FrameHeader SampleRate reserved\n");
	if((channel_assignment=bitfile_read(s->b, 4, &s->cframe_channelassignment))>10)
		_("Err: FrameHeader ChannelAssignment reserved\n");
	if((samplesize=bitfile_read(s->b, 3, &s->cframe_samplesize))==3)
		_("Err: FrameHeader SampleSize invalid\n");
	samplesize=samplesize_lookup[samplesize];
	if(samplesize==255)
		samplesize=s->bits_per_sample;
	if(bitfile_read(s->b, 1, &s->cframe_reserved))
		_("Err: FrameHeader reservedB failed\n");

	read_utf8(s->b, &sample_number, &s->cframe_utf8);
	if(!(s->is_variable_blockstream_size))
		sample_number*=s->blocksize_min;
	if(sample_number!=s->curr_sample){
		fprintf(stderr, "computed %"PRIu64" expected %"PRIu64"\n", sample_number, s->curr_sample);
		_("Err: Computed sample number doesn't match expected\n");
	}

	blocksize=blocksize_lookup[blocksize];
	if((blocksize==8) || (blocksize==16))
		blocksize=bitfile_read(s->b, blocksize, &s->cframe_blocksize)+1;
	s->actual_samples_in_stream+=blocksize;

	samplerate=samplerate_lookup[samplerate];
	if(samplerate==8)
		samplerate=bitfile_read(s->b, 8, &s->cframe_samplerate)*1000;
	else if(samplerate==16)
		samplerate=bitfile_read(s->b, 16, &s->cframe_samplerate);
	else if(samplerate==160)
		samplerate=bitfile_read(s->b, 16, &s->cframe_samplerate)*10;
	else if(samplerate==UINT32_MAX)
		samplerate=s->sample_rate;
	if(samplerate!=s->sample_rate)
		_("Err: SampleRate doesn't match STREAMINFO\n");

	assert(!(s->b->bitloc%8));
	if(s->crcwindow_cnt<((s->b->bitloc/8)-frame_start)){
		s->crcwindow=realloc_chk(s->crcwindow, ((s->b->bitloc/8)-frame_start), s->crcwindow_cnt);
		s->crcwindow_cnt=((s->b->bitloc/8)-frame_start);
	}
	fseek_chk(s->b->fp, frame_start, SEEK_SET);
	fread_chk(s->crcwindow, 1, ((s->b->bitloc/8)-frame_start), s->b->fp);
	crc8=calc_crc8(s->crcwindow, ((s->b->bitloc/8)-frame_start));
	if(crc8!=bitfile_read(s->b, 8, &s->cframe_crc8))
		_("Err: FrameHeader failed crc8 check\n");

	//subframes
	for(k=0;k<s->channels;++k){
		if((k==0&&channel_assignment==9)||(k==1&&channel_assignment==8)||(k==1&&channel_assignment==10))
			read_subframe(s, samplesize, blocksize, 1);
		else
			read_subframe(s, samplesize, blocksize, 0);
	}

	//frame footer
	if(s->b->bitloc%8){
		s->cframe_pad+=8-(s->b->bitloc%8);//padding to byte boundary
		if(bitfile_read(s->b, 8-(s->b->bitloc%8), NULL))
			_("Err: FrameFooter padding non-zero\n");
	}

	if(s->crcwindow_cnt<((s->b->bitloc/8)-frame_start)){
		s->crcwindow=realloc_chk(s->crcwindow, ((s->b->bitloc/8)-frame_start), s->crcwindow_cnt);
		s->crcwindow_cnt=((s->b->bitloc/8)-frame_start);
	}
	fseek_chk(s->b->fp, frame_start, SEEK_SET);
	fread_chk(s->crcwindow, 1, ((s->b->bitloc/8)-frame_start), s->b->fp);
	assert(ftell(s->b->fp)==(s->b->bitloc/8));
	crc16=calc_crc16(s->crcwindow, ((s->b->bitloc/8)-frame_start));
	crc16_chk=bitfile_read(s->b, 8, &s->cframe_crc16);
	crc16_chk|=bitfile_read(s->b, 8, &s->cframe_crc16)<<8;
	if(crc16!=crc16_chk){
		fprintf(stderr, "crc16 expected %"PRIu16" actual %"PRIu16"\n", crc16_chk, crc16);
		_("Err: FrameFooter failed crc16 check\n");
	}

	s->curr_sample+=blocksize;
	++s->frame_count;
	return 0;
}

int read_metadata_block(flanal *s){
	uint8_t blocktype, last;
	uint32_t k, blocklen;
	last=bitfile_read(s->b, 1, NULL);
	blocktype=bitfile_read(s->b, 7, NULL);
	blocklen=bitfile_read(s->b, 24, NULL);
	s->metablocks[blocktype<7?blocktype:7]+=(8*(4+blocklen));
	switch(blocktype){
		case STREAMINFO:
			s->blocksize_min=bitfile_read(s->b, 16, NULL);
			s->blocksize_max=bitfile_read(s->b, 16, NULL);
			//apparently it can be
			//if(s->blocksize_min && s->blocksize_min<16)
			//	_("Err: StreamInfo BlockSizeMin cannot be less than 16\n");
			if(s->blocksize_max && s->blocksize_max<16)
				_("Err: StreamInfo BlockSizeMax cannot be less than 16\n");
			if(s->blocksize_min>s->blocksize_max)
				_("Err: StreamInfo BlockSizeMin cannot be greater than BlockSizeMax\n");
			s->is_variable_blockstream_size=s->blocksize_min!=s->blocksize_max;
			s->frame_size_min=bitfile_read(s->b, 24, NULL);
			s->frame_size_max=bitfile_read(s->b, 24, NULL);
			if(s->frame_size_min&&s->frame_size_max&&(s->frame_size_min>s->frame_size_max))
				_("Err: StreamInfo FrameSizeMin cannot be greater than FrameSizeMax\n");
			s->sample_rate=bitfile_read(s->b, 20, NULL);
			if(!s->sample_rate)
				_("Err: StreamInfo SampleRate cannot be 0\n");
			if(s->sample_rate>655350)
				_("Err: StreamInfo SampleRate too large\n");
			s->channels=bitfile_read(s->b, 3, NULL)+1;
			s->bits_per_sample=bitfile_read(s->b, 5, NULL)+1;
			if(s->bits_per_sample<4)
				_("Err: StreamInfo BitsPerSample cannot be less than 4\n");
			if(s->bits_per_sample>32)
				_("Err: StreamInfo BitsPerSample cannot be more than 32\n");
			s->samples_in_stream=bitfile_read(s->b, 36, NULL);
			for(k=0;k<16;++k)
				s->md5[k]=bitfile_read(s->b, 8, NULL);
			return last;
		case PADDING://TODO
			for(k=0;k<blocklen;++k){
				if(bitfile_read(s->b, 8, NULL))
					_("Err: Paddingblock contains non-zero values\n");
			}
			return last;
		case APPLICATION:
			break;
		case SEEKTABLE://TODO
			break;
		case VORBIS_COMMENT://TODO
			break;
		case CUESHEET://TODO
			break;
		case PICTURE://TODO
			break;
		case INVALID:
			_("Err: Invalid BLOCK_TYPE in METADATA_BLOCK_HEADER\n");
		default:
			printf("Warning: Reserved BLOCK_TYPE in METADATA_BLOCK_HEADER, unknown contents preserved verbatim\n");
	}
	//fall through
	for(k=0;k<blocklen;++k)
		bitfile_read(s->b, 8, NULL);
	return last;
}

void printstat(size_t bits, size_t totbits, char *msg){
	double perc=100;
	perc*=bits;
	perc/=totbits;
	printf("%6zu (%f%%) %s\n", bits, perc, msg);
}

size_t flanal_play(char *path, int print_bitstats){
	char *meta_names[]={"STREAMINFO", "PADDING", "APPLICATION", "SEEKTABLE", "VORBIS_COMMENT", "CUESHEET", "PICTURE", "RESERVED", NULL};
	char *model_names[]={"subframes used constant modelling", "subframes used verbatim modelling", "subframes used fixed modelling", "subframes used lpc modelling", NULL};
	bitfile b;
	flanal s={0};
	size_t framebits_tot, i;

	memset(&b, 0, sizeof(bitfile));
	memset(&s, 0, sizeof(flanal));
	s.b=&b;
	bitfile_open(s.b, path, "rb");
	s.file=path;

	//if tot_samples not in header
	fseek(s.b->fp, 0, SEEK_END);
	s.b->limit=ftell(s.b->fp);
	rewind(s.b->fp);

	if(bitfile_read(s.b, 32, NULL)!=1716281667)
		_("Err: Failed magic word check, not a flac file\n");
	while(!read_metadata_block(&s));

	printf("'%s' STREAMINFO{\n blocksize min %zu max %zu\n framesize min %zu max %zu\n samplerate %zu\n channels %zu bits_per_sample %zu total samples %zu\n Blocking strategy: %s\n MD5: ", s.file, s.blocksize_min, s.blocksize_max, s.frame_size_min, s.frame_size_max, s.sample_rate, s.channels, s.bits_per_sample, s.samples_in_stream, s.is_variable_blockstream_size?"Variable":"Fixed");
	for(i=0;i<16;++i)
		printf("%02x ", s.md5[i]);
	printf("\n");
	//frames
	s.first_frame_loc=(s.b->bitloc/8);
	while(s.samples_in_stream==0 || s.curr_sample<s.samples_in_stream){
		if(read_frame(&s))
			break;
	}
	framebits_tot=s.b->bitloc-(s.first_frame_loc*8);
	if(!s.samples_in_stream)
		printf("Samples seen: %zu\n", s.actual_samples_in_stream);
	//anything after last frame is ignored
	if(print_bitstats){
		printf("\nMetadata bit stats (%% including bitstream):\n");
		for(i=0;meta_names[i];++i){
			if(s.metablocks[i])
				printstat(s.metablocks[i], framebits_tot, meta_names[i]);
		}

		printf("\nFrame header stats (%% excluding metadata):\n");
		printstat(s.cframe_sync, framebits_tot, "bits spent on sync codes");
		printstat(s.cframe_reserved, framebits_tot, "bits spent on frame reservations to maintain syncability");
		printstat(s.cframe_blockstrat, framebits_tot, "bits spent on block strategy bit");
		printstat(s.cframe_blocksize, framebits_tot, "bits spent encoding blocksize");
		printstat(s.cframe_samplerate, framebits_tot, "bits spent encoding samplerate");
		printstat(s.cframe_channelassignment, framebits_tot, "bits spent encoding channel assignment");
		printstat(s.cframe_samplesize, framebits_tot, "bits spent encoding sample size");
		printstat(s.cframe_utf8, framebits_tot, "bits spent encoding current frame/sample index with UTF8");
		printstat(s.cframe_crc8, framebits_tot, "bits spent encoding frame header crc8");

		printf("\nSubframe header stats (%% excluding metadata)\n");
		printstat(s.csubframe_reserved, framebits_tot, "bits spent on subframe reservations to maintain syncability");
		printstat(s.csubframe_type, framebits_tot, "bits spent encoding model type");
		printstat(s.csubframe_wastedbits, framebits_tot, "bits spent on wasted bits flag");

		printf("\nModelling stats (bit %% excluding metadata) (excluding residual bits)\n");
		for(i=0;model_names[i];++i)
			printstat(s.subframe_type[i], s.subframe_type[0]+s.subframe_type[1]+s.subframe_type[2]+s.subframe_type[3], model_names[i]);
		printstat(s.bitlen_constant, framebits_tot, "bits spent on constant");
		printstat(s.bitlen_verbatim, framebits_tot, "bits spent on verbatim");
		printstat(s.bitlen_fixed, framebits_tot, "bits spent on fixed");
		printstat(s.bitlen_lpc, framebits_tot, "bits spent on LPC");

		printf("\nResidual stats (%% excluding metadata):\n");
		printstat(s.res_reserved, framebits_tot, "bits spent on residual reservations to maintain syncability");
		printstat(s.res_type, framebits_tot, "bits spent on residual type (4 or 5 bit rice parameter)");
		printstat(s.res_encoding, framebits_tot, "bits spent on residual encoding");

		printf("\nFrame footer stats (%% excluding metadata):\n");
		printstat(s.cframe_crc16, framebits_tot, "bits spent encoding frame footer crc16");
		printstat(s.cframe_pad, framebits_tot, "bits spent on frame padding for byte alignment");

		printf("\nCombined stats (%% excluding metadata)\n");
		printstat(s.bitlen_constant+s.bitlen_verbatim+s.bitlen_fixed+s.bitlen_lpc, framebits_tot, "total bits spent on modelling");
		printstat(s.res_encoding+s.res_type+s.res_reserved, framebits_tot, "total bits spent on residual");
		printstat(s.cframe_sync+s.cframe_reserved+s.cframe_blockstrat+s.cframe_blocksize+s.cframe_samplerate+s.cframe_channelassignment+s.cframe_samplesize+s.cframe_utf8+s.cframe_crc8+s.csubframe_reserved+s.csubframe_type+s.csubframe_wastedbits+s.cframe_crc16+s.cframe_pad, framebits_tot, "total bits spent on overhead (frame_header+subframe_header+footer");

		printf("\nMiscellaneous stats:\n");
		printstat(s.res_rice, s.res_escape+s.res_rice, "of residual partitions stored rice-encoded");
		printstat(s.res_escape, s.res_escape+s.res_rice, "of residual partitions stored verbatim");
		printstat(s.cframe_reserved+s.csubframe_reserved+s.res_reserved, framebits_tot, "total bits spent on pure reservations to maintain syncability (not including the many reserved values in used elements or end-of-frame padding)");
	}
	return 0;
}

uint8_t *metacomp_process(char *inpath, size_t *outsize){
	FILE *af;
	uint8_t *a, *aout;
	size_t acnt, aloc=4;
	size_t cnt;
	af=fopen(inpath, "rb");
	assert(af);
	fseek(af, 0, SEEK_END);
	acnt=ftell(af);
	rewind(af);
	a=malloc(acnt);
	aout=malloc(acnt);
	assert(a);
	if(fread(a, 1, acnt, af)!=acnt)
		exit(1);
	fclose(af);
	*outsize=0;
	while(1){
		cnt=(a[aloc+1]<<16)+(a[aloc+2]<<8)+a[aloc+3];
		if((a[aloc]&127)!=0 && (a[aloc]&127)!=1 && (a[aloc]&127)!=3){
			memcpy(aout+*outsize, a+aloc+4, cnt);
			*outsize+=cnt;
		}
		if(a[aloc]>>7)
			break;
		aloc+=(4+cnt);
	}
	free(a);
	return aout;
}

int metacomp(char *ap, char *bp){
	uint8_t *ameta, *bmeta;
	size_t ameta_size, bmeta_size;
	ameta=metacomp_process(ap, &ameta_size);
	bmeta=metacomp_process(bp, &bmeta_size);
	if(ameta_size!=bmeta_size){
		fprintf(stderr, "Err: meta sizes do not match: %zu %zu\n", ameta_size, bmeta_size);
		return 1;
	}
	if(ameta_size==0)
		return 0;
	if(memcmp(ameta, bmeta, ameta_size)!=0){
		fprintf(stderr, "Err: --metacomp metadata does not match\n");
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[]){
	if(argc>=3 && strcmp(argv[1], "--bitstat")==0)
		return flanal_play(argv[2], argc==4?1:0);
	else if(argc==4 && strcmp(argv[1], "--metacomp")==0)
		return metacomp(argv[2], argv[3]);
	else{
		printf("Usage:\n\n"
		"Process file collecting bit stats\n"
		"flanal --bitstat file.flac\n\n"
		"Compare two flac files for metadata equality\n"
		"flanal --metacomp a.flac b.flac\n");
	}
}
