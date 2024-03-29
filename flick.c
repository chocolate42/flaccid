/* quick flac - Try to convert input to the quickest valid flac file
	* Outputs verbatim subset frames only
	* Avoids bit readers and writers
	* Theoretically can work with any multiple of 8 input bit depth
	  (Currently a Big old hack, raw CDDA only so only stereo 16 bit LE 44100Hz)
	  (more input types TODO maybe)
	* Theoretically frame locations are knowable ahead of time so a seektable
	  could be implemented cleanly before frame generation (without seeks or padding)
	  (seektable gen TODO maybe)
	* For (almost) apples-to-apples comparison with reference encoder use these
	  in reference encoder:
	  --no-md5 -l0 -b4608 --no-padding --no-seektable --no-adaptive-mid-side --no-mid-side --totally-silent
	  But even then reference encoder generates a vorbis comment with libflac version
	  and sets frame size info in header so output won't be binarily identical
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void _(char *s){
	fprintf(stderr, "Error: %s\n", s);
	exit(1);
}

void _if(int goodbye, char *s){
	if(goodbye)
		_(s);
}

//crc adapted from ffmpeg
void calc_crc16(uint8_t *b, size_t cnt, uint8_t *ret){
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
	ret[0]=crc&0xFF;
	ret[1]=(crc>>8)&0xFF;
}

void crc16_update(uint8_t *b, size_t cnt, uint16_t *crc){
	const uint8_t *end=b+cnt;
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
		*crc=ctx[((uint8_t)*crc)^*b++]^(*crc>>8);
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

uint8_t write_utf8(uint32_t n, uint8_t *ret){
	if(n<128){
		ret[0]=n;
		return 1;
	}
	else if(n<2048){
		ret[0]=0xC0|(n>>6);
		ret[1]=0x80|(n&63);
		return 2;
	}
	else if(n<65536){
		ret[0]=0xE0|(n>>12);
		ret[1]=0x80|((n>>6)&63);
		ret[2]=0x80|(n&63);
		return 3;
	}
	else{//assume n< 2^21, works for cdda ~60 hours, hack
		ret[0]=0xF0|(n>>18);
		ret[1]=0x80|((n>>12)&63);
		ret[2]=0x80|((n>>6)&63);
		ret[3]=0x80|(n&63);
		return 4;
	}
}

int encode(char* ip, char *op){
	FILE *fi, *fo;
	size_t read;
	static uint32_t blocksize_reverse_lookup[16]={0, 192, 576, 1152, 2304, 4608, 0, 0, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
	uint8_t header[42]={
		0x66, 0x4C, 0x61, 0x43,//magic
		0x80, 0, 0, 0x22,//streaminfo header, is_last=true
		18, 0, 18, 0,//min/max blocksize 4608 (TBD if necessary)
		0, 0, 0, 0, 0, 0,//min/max frame size (TBD optional)
		0x0a, 0xc4, 0x42,//44.1khz, 2 channel (TBD is necessary)
		0xf0,//16 bps, upper 4 bits of total samples
		0, 0, 0, 0, //lower 32 bits of total samples (TBD optional)
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,//md5 (TBD optional)
	};
	uint8_t frame_header[4]={0xFF, 0xF8, 0x59, 0x18};
	uint8_t subframe_header=0x02;
	uint8_t in[4608*4], out[12+2+(4608*4)+1+2];
	uint32_t frame=0, loc, channel, i, tot_samples=0;

	fi=fopen(ip, "rb");
	fo=fopen(op, "wb");

	//adapt stuff for different input types TODO

	fwrite(header, 1, 42, fo);//cdda header
	while((4608*4)==(read=fread(in, 1, 4608*4, fi))){
		memcpy(out, frame_header, 4);//frame header
		loc=4+write_utf8(frame++, out+4);//utf8
		out[loc++]=calc_crc8(out, loc);//crc8
		for(channel=0;channel<2;++channel){
			out[loc++]=subframe_header;
			for(i=0;i<4608;++i){
				out[loc++]=in[(i*4)+(channel*2)+1];
				out[loc++]=in[(i*4)+(channel*2)+0];
			}
		}
		calc_crc16(out, loc, out+loc);
		fwrite(out, 1, loc+2, fo);
	}

	if(read&&(read!=4608*4)){//end frame
		uint32_t samples=read/4;
		for(i=0;i<16;++i){
			if(samples==blocksize_reverse_lookup[i])
				break;
		}
		//set samplecount flag
		if(i<16)
			frame_header[2]=((i&15)<<4)|(frame_header[2]&15);//common
		else if(samples<=256)
			frame_header[2]=0x60|(frame_header[2]&15);//8 bit
		else
			frame_header[2]=0x70|(frame_header[2]&15);//16 bit
		memcpy(out, frame_header, 4);//frame header
		loc=4+write_utf8(frame++, out+4);//utf8
		//write samplecount
		if(i<16);
		else if(samples<=256)
			out[loc++]=(samples-1)&0xFF;
		else{
			out[loc++]=((samples-1)>>8)&0xFF;
			out[loc++]=(samples-1)&0xFF;
		}
		out[loc++]=calc_crc8(out, loc);//crc8
		for(channel=0;channel<2;++channel){
			out[loc++]=subframe_header;
			for(i=0;i<samples;++i){
				out[loc++]=in[(i*4)+(channel*2)+1];
				out[loc++]=in[(i*4)+(channel*2)+0];
			}
		}
		calc_crc16(out, loc, out+loc);
		fwrite(out, 1, loc+2, fo);
	}

	fclose(fi);
	fclose(fo);
	return 0;
}

int decode(char *ip, char *op){
	FILE *fi, *fo;
	uint8_t header[42], *magic="fLaC", *out, *planar, si_len[3]={0,0,34};
	uint32_t channels, blocksize, blocksize_actual, samples_actual, bps, islast, metasize, framecounter=0, i, j, k, loc, fhloc;
	uint8_t fh[24];
	uint32_t utflen=1, utfresize[6]={0, 1<<7, 1<<11, 1<<16, 1<<21, 1<<26};
	static uint32_t blocksize_lookup[16]={0, 192, 576, 1152, 2304, 4608, 8, 16, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
	static uint32_t samplerate_lookup[16]={UINT32_MAX, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000, 8, 16, 16, 0};
	uint16_t crc16;

	fi=fopen(ip, "rb");
	fo=fopen(op, "wb");

	fread(header, 1, 42, fi);
	_if(memcmp(header, magic, 4)!=0, "magic mismatch");
	_if(header[4]&127, "streaminfo missing");
	islast=header[4]>>7;
	_if(memcmp(header+5, si_len,3)!=0, "streaminfo mis-sized");
	_if(memcmp(header+8, header+10, 2)!=0, "Must be fixed blocksize stream");
	blocksize=(header[8]<<8)|header[9];
	channels=((header[20]>>1)&7)+1;
	bps=(((header[20]&1)<<4)|(header[21]>>4))+1;
	_if(bps%8, "bps must be multiple of 8");

	while(!islast){//skip metadata
		_if(4!=fread(header, 1, 4, fi), "unexpected EOF");
		islast=header[0]>>7;
		metasize=(header[1]<<16)|(header[2]<<8)|header[3];
		fseek(fi, metasize, SEEK_CUR);
	}

	out=malloc(channels*blocksize*(bps>>3));
	planar=malloc(channels*blocksize*(bps>>3));
	while(1){
		crc16=0;
		fhloc=4+utflen;//fh+utf
		if(fhloc!=fread(fh, 1, fhloc, fi))
			break;//assumed EOF
		blocksize_actual=blocksize_lookup[fh[2]>>4];
		if(blocksize_actual==8){
			fhloc+=fread(fh+fhloc, 1, 1, fi);
			blocksize_actual=fh[fhloc-1]+1;
		}
		else if(blocksize_actual==16){
			fhloc+=fread(fh+fhloc, 1, 2, fi);
			blocksize_actual=(fh[fhloc-2]<<8)+fh[fhloc-1]+1;
		}//ignore case==0, flick encode won't use it
		samples_actual=samplerate_lookup[fh[2]&15];
		if(samples_actual==8)
			fhloc+=fread(fh+fhloc, 1, 1, fi);
		else if(samples_actual==16)
			fhloc+=fread(fh+fhloc, 1, 2, fi);
		//ignore case==0||15, flick encode won't use it
		fhloc+=fread(fh+fhloc, 1, 1, fi);
		_if(calc_crc8(fh, fhloc-1)!=fh[fhloc-1], "crc8 failed");
		crc16_update(fh, fhloc, &crc16);

		++framecounter;
		if(utfresize[utflen]==framecounter)
			++utflen;

		loc=0;
		for(i=0;i<channels;++i){
			fread(fh, 1, 1, fi);//skip subframeheader
			crc16_update(fh, 1, &crc16);
			loc+=fread(planar+loc, 1, blocksize_actual*(bps/8), fi);//read planes
			crc16_update(planar+loc-(blocksize_actual*(bps/8)), blocksize_actual*(bps/8), &crc16);
		}
		fread(fh, 1, 2, fi);//crc16
		_if(crc16!=((fh[1]<<8)|fh[0]), "crc16 failed");
		//shuffle into interleaved LE TODO
		loc=0;
		for(i=0;i<blocksize_actual;++i){
			for(j=0;j<channels;++j){
				for(k=bps/8;k;--k){
					out[loc++]=planar[(j*blocksize_actual*(bps/8))+(i*(bps/8))+(k-1)];
				}
			}
		}
		fwrite(out, 1, loc, fo);
	}
	fclose(fi);
	fclose(fo);
	return 0;
}

int decode_quick(char *ip, char *op){
	FILE *fi, *fo;
	uint8_t header[42], *magic="fLaC", *out, *planar, si_len[3]={0,0,34};
	uint32_t channels, blocksize, blocksize_actual, samples_actual, bps, islast, metasize, framecounter=0, i, j, k, loc, fhloc;
	uint8_t fh[24];
	uint32_t utflen=1, utfresize[6]={0, 1<<7, 1<<11, 1<<16, 1<<21, 1<<26};
	static uint32_t blocksize_lookup[16]={0, 192, 576, 1152, 2304, 4608, 8, 16, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
	static uint32_t samplerate_lookup[16]={UINT32_MAX, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000, 8, 16, 16, 0};

	fi=fopen(ip, "rb");
	fo=fopen(op, "wb");

	fread(header, 1, 42, fi);
	_if(memcmp(header, magic, 4)!=0, "magic mismatch");
	_if(header[4]&127, "streaminfo missing");
	islast=header[4]>>7;
	_if(memcmp(header+5, si_len,3)!=0, "streaminfo mis-sized");
	_if(memcmp(header+8, header+10, 2)!=0, "Must be fixed blocksize stream");
	blocksize=(header[8]<<8)|header[9];
	channels=((header[20]>>1)&7)+1;
	bps=(((header[20]&1)<<4)|(header[21]>>4))+1;
	_if(bps%8, "bps must be multiple of 8");

	while(!islast){//skip metadata
		_if(4!=fread(header, 1, 4, fi), "unexpected EOF");
		islast=header[0]>>7;
		metasize=(header[1]<<16)|(header[2]<<8)|header[3];
		fseek(fi, metasize, SEEK_CUR);
	}

	out=malloc(channels*blocksize*(bps>>3));
	planar=malloc(channels*blocksize*(bps>>3));
	while(1){
		fhloc=4+utflen;//fh+utf
		if(fhloc!=fread(fh, 1, fhloc, fi))
			break;//assumed EOF
		blocksize_actual=blocksize_lookup[fh[2]>>4];
		if(blocksize_actual==8){
			fhloc+=fread(fh+fhloc, 1, 1, fi);
			blocksize_actual=fh[fhloc-1]+1;
		}
		else if(blocksize_actual==16){
			fhloc+=fread(fh+fhloc, 1, 2, fi);
			blocksize_actual=(fh[fhloc-2]<<8)+fh[fhloc-1]+1;
		}//ignore case==0, flick encode won't use it
		samples_actual=samplerate_lookup[fh[2]&15];
		if(samples_actual==8)
			fhloc+=fread(fh+fhloc, 1, 1, fi);
		else if(samples_actual==16)
			fhloc+=fread(fh+fhloc, 1, 2, fi);
		//ignore case==0||15, flick encode won't use it
		fhloc+=fread(fh+fhloc, 1, 1, fi);

		++framecounter;
		if(utfresize[utflen]==framecounter)
			++utflen;

		loc=0;
		for(i=0;i<channels;++i){
			fread(fh, 1, 1, fi);//skip subframeheader
			loc+=fread(planar+loc, 1, blocksize_actual*(bps/8), fi);//read planes
		}
		fread(fh, 1, 2, fi);//crc16
		//shuffle into interleaved LE TODO
		loc=0;
		for(i=0;i<blocksize_actual;++i){
			for(j=0;j<channels;++j){
				for(k=bps/8;k;--k){
					out[loc++]=planar[(j*blocksize_actual*(bps/8))+(i*(bps/8))+(k-1)];
				}
			}
		}
		fwrite(out, 1, loc, fo);
	}
	fclose(fi);
	fclose(fo);
	return 0;
}

int main(int argc, char *argv[]){
	if(argc!=4)
		return printf("Usage: flick [e/d/q] in out\nEncoder input must be raw CDDA (16 bit LE 2 channel)\nDecoder input must be flick encoded or kaboom\n");
	if(strcmp(argv[1], "e")==0)
		return encode(argv[2], argv[3]);
	else if(strcmp(argv[1], "d")==0)
		return decode(argv[2], argv[3]);
	else if(strcmp(argv[1], "q")==0)
		return decode_quick(argv[2], argv[3]);
	return printf("Mode unsupported\n");
}
