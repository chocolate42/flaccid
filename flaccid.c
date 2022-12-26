#include "FLAC/stream_encoder.h"

#include "chunk.h"
#include "common.h"
#include "gset.h"
#include "load.h"
#include "merge.h"
#include "peakset.h"
#include "tweak.h"

#include <getopt.h>
#include <stdlib.h>
#include <string.h>

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
	" --mode mode : Which variable-blocksize algorithm to use. Valid modes: chunk, gset, peakset\n"
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

int main(int argc, char *argv[]){
	int (*encoder[3])(void*, size_t, FILE*, flac_settings*)={chunk_main, gset_main, peak_main};
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

	int c, option_index;
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
	set.blocksize_limit_upper=65535;
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
	set.mode=-1;
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
					set.mode=0;
				else if(strcmp(optarg, "gset")==0)
					set.mode=1;
				else if(strcmp(optarg, "peakset")==0)
					set.mode=2;
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
	if(!set.lax && set.blocksize_limit_upper>4608)
		set.blocksize_limit_upper=4608;//<=48KHz assumed fix TODO

	set.diff_comp_settings=strcmp(set.comp_anal, set.comp_output)!=0;
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && !set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(!set.apod_anal && set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && set.apod_output && strcmp(set.apod_anal, set.apod_output)!=0);

	if(!ipath)
		goodbye("Error: No input\n");

	if(!opath)/* Add test option with no output TODO */
		goodbye("Error: No output\n");

	if(set.mode==-1)
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
	encoder[set.mode](input, input_size, fout, &set);

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
