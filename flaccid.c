#include "FLAC/stream_encoder.h"

#include "chunk.h"
#include "common.h"
#include "fixed.h"
#include "gasc.h"
#include "gset.h"
#include "load.h"
#include "peakset.h"

#include <getopt.h>
#include <stdlib.h>
#include <string.h>

char *help=
	"Usage: flaccid [options]\n"
	"\nModes:\n"
	" fixed: A fixed blocking strategy like the reference encoder. Must use only one\n"
	"        blocksize, cannot use tweak or merge passes, analysis settings unused\n"
	"        Effort O(1)\n"
	" peakset: Find the optimal permutation of frames for a given blocksize list.\n"
	"          Truly optimal if analysis settings are the same as output settings.\n"
	"          Tweak/merge passes can still be a benefit as they can use blocksizes\n"
	"          not on the list\n"
	"          Effort O(blocksize_count^2) when blocksizes are contiguous multiples\n"
	"          of the smallest blocksize. (n*(n+1))/2\n"
	" gasc:  To find the next frame, test larger and larger blocksizes until\n"
	"        efficiency drops (then pick previous). Typically better than gset\n"
	" chunk: Process input as chunks, a chunk evenly subdivides the input by building\n"
	"        a tree, the children of a node subdivide the input range of the parent.\n"
	"        The root has a range of the maximum blocksize in the list\n"
	"        Effort O(blocksize_count)\n"
	" gset:  Test all from a set of blocksizes and greedily pick the most efficient\n"
	"        as the next frame\n"
	"\nAdditional passes:\n"
	" tweak: Adjusts where adjacent frames are split to look for a more efficient\n"
	"        encoding. Every pass uses a smaller and smaller offset as we try and\n"
	"        get closer to optimal. Multithreaded, acts on the output queue and can\n"
	"        be sped up at a minor efficiency loss by using a smaller queue\n"
	" merge: Merges adjacent frames to see if the result is more efficient. Best used\n"
	"        with --lax for lots of merging headroom, a sane subset encoding is\n"
	"        unlikely to see much if any benefit as subset is limited to a blocksize\n"
	"        of 4608. Multithreaded, acts on the output queue and can be sped up at a\n"
	"        minor efficiency loss by using a smaller queue\n"
	"\nOptions:\n"
	" --analysis-apod apod_string : Apodization settings to use during analysis. If\n"
	"                               supplied this overwrites the apod settings\n"
	"                               defined by the flac preset\n"
	" --analysis-comp comp_string : Compression settings to use during analysis\n"
	" --blocksize-list block,list : Blocksizes that a mode is allowed to use for\n"
	"                               analysis. Different modes have different\n"
	"                               constraints on valid combinations\n"
	" --blocksize-limit-lower limit : Minimum blocksize a frame can be\n"
	" --blocksize-limit-upper limit : Maximum blocksize a frame can be\n"
	" --in infile : Source, pipe unsupported\n"
	" --lax : Allow non-subset settings\n"
	" --merge threshold : If set enables merge passes, iterates until a pass saves\n"
	"                     less than threshold bytes\n"
	" --mode mode : Which variable-blocksize algorithm to use for analysis. Valid\n"
	"               modes: fixed, peakset, gasc, chunk, gset\n"
	" --no-md5 : Disable MD5 generation\n"
	" --out outfile : Destination\n"
	" --output-apod apod_string : Apodization settings to use during output. If\n"
	"                             supplied this overwrites the apod settings\n"
	"                             defined by the flac preset\n"
	" --output-comp comp_string : Compression settings to use during output\n"
	" --outperc num : 1-100%%, frequency of normal output settings (default 100%%)\n"
	" --outputalt-apod apod_string : Alt apod settings to use if outperc not 100%%.\n"
	"                                If supplied this overwrites the apod settings\n"
	"                                defined by the flac preset\n"
	" --outputalt-comp comp_string : Alt output settings to use if outperc not 100%%\n"
	" --queue size : Number of frames in output queue (default 8192), when output\n"
	"                queue is full it gets flushed. Tweak/merge acting on the output\n"
	"                queue and batching of output encoding allows multithreading\n"
	"                even if the mode used is single-threaded\n"
	" --sample-rate num : Set sample rate for raw input\n"
	" --tweak threshold : If set enables tweak passes, iterates until a pass saves\n"
	"                     less than threshold bytes\n"
	" --workers integer : The maximum number of threads to use\n"
	"\nCompression settings format:\n"
	" * Mostly follows ./flac interface but requires settings to be in single string\n"
	" * Compression level must be the first element\n"
	" * Supported settings: e, m, l, p, q, r (see ./flac -h)\n"
	" * Adaptive mid-side from ./flac is not supported (-M), affects compression\n"
	"   levels 1 and 4\n"
	" * ie \"5er4\" defines compression level 5, exhaustive model search, max rice\n"
	"   partition order up to 4\n"
	"\nApodization settings format:\n"
	" * All apodization settings in a single semi-colon-delimited string\n"
	" * ie tukey(0.5);partial_tukey(2);punchout_tukey(3)\n\n";

static int comp_int_asc(const void *aa, const void *bb){
	int *a=(int*)aa;
	int *b=(int*)bb;
	if(*a<*b)
		return -1;
	else
		return *a==*b?0:1;
}

static void parse_blocksize_list(char *list, int **res, size_t *res_cnt){
	size_t i;
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

	qsort(*res, *res_cnt, sizeof(int), comp_int_asc);
	for(i=1;i<*res_cnt;++i){
		if((*res)[i]==(*res)[i-1])
			goodbye("Error: Duplicate blocksizes in list\n");
	}
}

int main(int argc, char *argv[]){
	int (*encoder[6])(void*, size_t, FILE*, flac_settings*)={chunk_main, gset_main, peak_main, gasc_main, fixed_main, NULL};
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
		{"lax", no_argument, 0, 272},
		{"merge",	required_argument, 0, 265},
		{"mode", required_argument, 0, 'm'},
		{"no-md5", no_argument, 0, 271},
		{"out", required_argument, 0, 'o'},
		{"output-apod", required_argument, 0, 260},
		{"output-comp", required_argument, 0, 257},
		{"outperc", required_argument, 0, 269},
		{"outputalt-apod", required_argument, 0, 267},
		{"outputalt-comp", required_argument, 0, 268},
		{"queue", required_argument, 0, 270},
		{"sample-rate",	required_argument, 0, 262},
		{"tweak", required_argument, 0, 261},
		{"workers", required_argument, 0, 'w'},
		{"wildcard", required_argument, 0, 266},
		{0, 0, 0, 0}
	};

	memset(&set, 0, sizeof(flac_settings));
	set.apod_anal=NULL;
	set.apod_output=NULL;
	set.apod_outputalt=NULL;
	set.blocksize_limit_lower=256;
	set.blocksize_limit_upper=65535;
	set.blocksize_max=4096;
	set.blocksize_min=4096;
	set.bps=16;
	set.channels=2;
	set.comp_anal="5";
	set.comp_output="8p";
	set.comp_outputalt="8";
	set.diff_comp_settings=0;
	set.lax=0;
	set.merge=0;
	set.minf=UINT32_MAX;
	set.maxf=0;
	set.md5=1;
	set.mode=-1;
	set.outperc=100;
	set.queue_size=8192;
	set.sample_rate=44100;
	set.tweak=0;
	set.wildcard=0;
	set.work_count=1;
	set.lpc_order_limit=32;
	set.rice_order_limit=15;

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
				else if(strcmp(optarg, "gasc")==0)
					set.mode=3;
				else if(strcmp(optarg, "fixed")==0)
					set.mode=4;
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

			case 266:
				set.wildcard=atoi(optarg);
				break;

			case 267:
				set.apod_outputalt=optarg;
				break;

			case 268:
				set.comp_outputalt=optarg;
				break;

			case 269:
				set.outperc=atoi(optarg);
				if(atoi(optarg)<1 || atoi(optarg)>100)
					goodbye("Error: Invalid --outperc setting (must be in integer between 1 and 100 inclusive)\n");
				break;

			case 270:
				set.queue_size=atoi(optarg);
				if(set.queue_size<=0)
					goodbye("Error: Queue size cannot be negative\n");
				break;

			case 271:
				set.md5=0;
				break;

			case 272:
				set.lax=1;
				break;

			case '?':
				goodbye("");
				break;
		}
	}

	if(!ipath)
		goodbye("Error: No input\n");

	if(!opath)/* Add test option with no output TODO */
		goodbye("Error: No output\n");

	if(set.mode==-1)
		goodbye("Error: No mode\n");

	parse_blocksize_list(blocklist_str, &(set.blocks), &(set.blocks_count));
	set.blocksize_min=set.blocks[0];
	set.blocksize_max=set.blocks[set.blocks_count-1];

	if(!(fout=fopen(opath, "wb+")))
		goodbye("Error: fopen() output failed\n");
	fwrite(header, 1, 42, fout);

	input=load_input(ipath, &input_size, &set);

	if(!set.lax){
		set.blocksize_limit_upper=(set.sample_rate<=48000)?4608:16384;
		if(set.sample_rate<=48000)
			set.lpc_order_limit=12;
		set.rice_order_limit=8;
	}

	if(!set.lax && set.blocksize_limit_upper>4608)
		set.blocksize_limit_upper=4608;//<=48KHz assumed fix TODO


	set.diff_comp_settings=strcmp(set.comp_anal, set.comp_output)!=0;
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && !set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(!set.apod_anal && set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && set.apod_output && strcmp(set.apod_anal, set.apod_output)!=0);

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

	fclose(fout);
}
