#include "FLAC/stream_encoder.h"

#include "chunk.h"
#include "common.h"
#include "fixed.h"
#include "gasc.h"
#include "gset.h"
#include "load.h"
#include "peakset.h"
#include "seektable.h"

#include <getopt.h>
#include <stdlib.h>
#include <string.h>

char *help=
	"Usage: flaccid [options]\n"
	"\n  Note: There's two ways to define the compression settings used, either\n"
	"  using the simple interface (--preset and possibly --preset-apod), or the\n"
	"  complex interface (numerous settings allowing full customisation)\n"
	"\nOptions:\n"
	"  [General]\n"
	" --in infile : Source. Use - to specify piping from stdin. Valid extensions are\n"
	"               .wav for wav format, .flac for flac format, .bin for raw CDDA\n"
	" --input-format format : Force input to be treated as a particular format.\n"
	"                         Valid options are: flac wav cdda\n"
	" --lax : Allow non-subset settings\n"
	" --no-md5 : Disable MD5 generation\n"
	" --no-seek : Disable seeking of the output stream, meaning the header cannot be\n"
	"             updated at the end of the encode. Requires --no-md5 to also be set\n"
	"             to ensure the user knows that disabling seek disables MD5\n"
	" --out outfile : Destination. Use - to specify piping to stdout. By default the\n"
	"                 output pipe caches the entire output to RAM allowing the\n"
	"                 header to be updated before writing to pipe. Using --no-seek\n"
	"                 allows the output pipe to write output frames as soon as they\n"
	"                 are available\n"
	" --peakset-window size : Maximum window size in millions of samples (default 26\n"
	"                         for 26 million samples, ~10 minutes of 44.1KHz input).\n"
	"                         This is settable from simple or complex interface as\n"
	"                         it mainly allows RAM usage to be customised\n"
	" --preserve-flac-metadata: Preserve metadata from flac input, excluding padding\n"
	" --queue size : Number of frames in output queue (default 16), when output\n"
	"                queue is full it gets flushed. Tweak/merge acting on the output\n"
	"                queue and batching of output encoding allows multithreading\n"
	"                even if the mode used is single-threaded. This is settable from\n"
	"                simple or complex interface as it mainly allows RAM usage to be\n"
	"                customised\n"
	" --seektable val : Defines if and how a seektable is generated:\n"
	"                  -1 (default): Adapt to input if input size is known (most\n"
	"                                flac/wav has total_sample_cnt in the header),\n"
	"                                else use 100 seekpoints\n"
	"                   0          : No seektable\n"
	"                   n          : n seekpoints\n"
	" --workers integer : The maximum number of threads to use\n"
	"\n  [Simple interface]\n"
	" --preset num[extra] : A preset optionally appended with extra flac settings\n"
	"                       (supported settings e/l/m/p/q/r see ./flac for details).\n"
	"                       Presets 0..8 match those in ./flac and use a fixed\n"
	"                       blocking strategy (the only caveat being that -M\n"
	"                       adaptive mid-side is not supported by flaccid, so -1 and\n"
	"                       -4 don't enable it. Presets 9 and up use variable\n"
	"                       blocking strategies\n"
	" --preset-apod apod : Apodization settings to overwrite those set by preset.\n"
	"                      A single string semi-colon delimited for multiple apod\n"
	"                      options\n"
	"\n [Complex interface]\n"
	"    [Complex flac settings]\n"
	" --analysis-apod apod_string : Apodization settings to use during analysis. If\n"
	"                               supplied this overwrites the apod settings\n"
	"                               defined by the flac preset\n"
	" --analysis-comp comp_string : Compression settings to use during analysis.\n"
	"                               Depending on mode these settings could be used\n"
	"                               dozens or even hundreds of times per input, so\n"
	"                               might want to be relatively weak\n"
	" --output-apod apod_string : Apodization settings to use during output. If\n"
	"                             supplied this overwrites the apod settings\n"
	"                             defined by the flac preset\n"
	" --output-comp comp_string : Compression settings to use during output. These\n"
	"                             settings will be used at most once so can be\n"
	"                             relatively strong\n"
	" --outperc num : 1-100%, frequency of normal output settings (default 100%)\n"
	"                 A value of 1 to 99 enables output-alt-* to be used, a\n"
	"                 way to try and hit a compression target between two others\n"
	"                 without necessarily requiring knowledge of flac internals\n"
	"                 For example --outperc 70 --output-comp 8 --outputalt-comp 8p\n"
	"                 uses -8 70%% of the time and -8p 30%% of the time for output,\n"
	"                 aiming for compression slightly better than -8\n"
	" --outputalt-apod apod_string : Alternate output apod settings\n"
	" --outputalt-comp comp_string : Alternate output compression settings\n"
	"    [Complex flaccid settings]\n"
	" --mode mode : Which variable-blocksize algorithm to use for analysis. Valid\n"
	"               modes: fixed, peakset, gasc, chunk, gset\n"
	" --blocksize-list block,list : Blocksizes that a mode is allowed to use for\n"
	"                               analysis. Different modes have different\n"
	"                               constraints on valid combinations\n"
	" --blocksize-limit-lower limit : Minimum blocksize a frame can be\n"
	" --blocksize-limit-upper limit : Maximum blocksize a frame can be\n"
	" --merge threshold : If >0 enables merge passes, iterates until a pass saves\n"
	"                     less than threshold bytes. 0 disables merging\n"
	" --tweak threshold : If >0 enables tweak passes, iterates until a pass saves\n"
	"                     less than threshold bytes. 0 disables tweaking\n"
	"\nModes:\n"
	" fixed: A fixed blocking strategy like the reference encoder. Must use only one\n"
	"        blocksize, cannot use tweak or merge passes, analysis settings unused\n"
	" peakset: Find the optimal permutation of frames for a given blocksize list.\n"
	"          Truly optimal if analysis settings are the same as output settings.\n"
	"          Tweak/merge passes can still be a benefit as they can use blocksizes\n"
	"          not on the list\n"
	" gasc:  To find the next frame, test larger and larger blocksizes until\n"
	"        efficiency drops (then pick previous). Typically better than gset\n"
	" chunk: Process input as chunks, a chunk evenly subdivides the input by building\n"
	"        a tree, the children of a node subdivide the input range of the parent.\n"
	"        The root has a range of the maximum blocksize in the list\n"
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
	"\nMode complexity:\n"
	" O(1): Fixed mode encodes every sample once\n"
	" O(blocksize_count): chunk, gset, gasc (upper bound)\n"
	" O(blocksize_count^2): peakset (when blocksizes are contiguous multiples of the\n"
	"                       smallest blocksize. (n*(n+1))/2\n"
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
		_if(((*res)[*res_cnt]<16), "Blocksize must be at least 16");
		_if(((*res)[*res_cnt]>65535), "Blocksize must be at most 65535");
		*res_cnt=*res_cnt+1;
	}while((cptr=strchr(cptr+1, ',')));

	qsort(*res, *res_cnt, sizeof(int), comp_int_asc);
	for(i=1;i<*res_cnt;++i)
		_if(((*res)[i]==(*res)[i-1]), "Duplicate blocksizes in list");
}

static void preset_check(flac_settings *set, char *setting){
	if(set->ui_type==UI_PRESET){
		fprintf(stderr, "Cannot mix %s with --preset\n", setting);
		_("Simple/Complex UI mixed");
	}
	set->ui_type=UI_MANUAL;
}

int main(int argc, char *argv[]){
	int (*encoder[6])(input*, output*, flac_settings*)={chunk_main, gset_main, peak_main, gasc_main, fixed_main, NULL};
	char *blocklist_str=NULL, *ipath=NULL, *opath=NULL;
	flac_settings set={0};
	input in={0};
	output out={0};

	uint8_t header[42]={
		0x66, 0x4C, 0x61, 0x43,//magic
		0x00, 0, 0, 0x22,//streaminfo header
		0, 0, 0, 0,//min/max blocksize TBD
		0, 0, 0, 0, 0, 0,//min/max frame size TBD
		0x0a, 0xc4, 0x42,//44.1khz, 2 channel
		0xf0,//16 bps, upper 4 bits of total samples
		0, 0, 0, 0, //lower 32 bits of total samples
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,//md5
	};

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
		{"no-seek", no_argument, 0, 274},
		{"input-format", required_argument, 0, 275},
		{"out", required_argument, 0, 'o'},
		{"output-apod", required_argument, 0, 260},
		{"output-comp", required_argument, 0, 257},
		{"outperc", required_argument, 0, 269},
		{"outputalt-apod", required_argument, 0, 267},
		{"outputalt-comp", required_argument, 0, 268},
		{"peakset-window", required_argument, 0, 273},
		{"preserve-flac-metadata", no_argument, 0, 279},
		{"preset", required_argument, 0, 276},
		{"preset-apod", required_argument, 0, 277},
		{"queue", required_argument, 0, 270},
		{"seektable", required_argument, 0, 278},
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
	set.blocksize_limit_upper=0;
	set.blocksize_max=4096;
	set.blocksize_min=4096;
	set.bps=16;
	set.channels=2;
	set.comp_anal="6";
	set.comp_output="6";
	set.comp_outputalt="6";
	set.diff_comp_settings=0;
	set.lax=0;
	set.merge=0;
	set.minf=UINT32_MAX;
	set.maxf=0;
	set.md5=1;
	set.mode=-1;
	set.outperc=100;
	set.peakset_window=26;
	set.preserve_flac_metadata=0;
	set.queue_size=16;
	set.sample_rate=44100;
	set.seek=1;
	set.seektable=-1;
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
				fprintf(stderr, "%s", help);
				return 0;

			case 'i':
				ipath=optarg;
				break;

			case 'm':
				preset_check(&set, "--mode");
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
					_("Unknown mode");
				break;

			case 'o':
				opath=optarg;
				break;

			case 'w':
				_if((atoi(optarg)<1), "Worker count must be >=1, it is the number of encoder cores to use");
				set.work_count=atoi(optarg);
				break;

			case 256:
				preset_check(&set, "--analysis-comp");
				set.comp_anal=optarg;
				break;

			case 257:
				preset_check(&set, "--output-comp");
				set.comp_output=optarg;
				break;

			case 258:
				preset_check(&set, "--blocksize-list");
				blocklist_str=optarg;
				break;

			case 259:
				preset_check(&set, "--analysis-apod");
				set.apod_anal=optarg;
				break;

			case 260:
				preset_check(&set, "--output-apod");
				set.apod_output=optarg;
				break;

			case 261:
				preset_check(&set, "--tweak");
				_if((atoi(optarg)<0), "Invalid tweak setting");
				set.tweak=atoi(optarg);
				break;

			case 263:
				preset_check(&set, "--blocksize-limit-lower");
				_if((atoi(optarg)>65535 || atoi(optarg)<16), "Invalid lower limit blocksize");
				set.blocksize_limit_lower=atoi(optarg);
				break;

			case 264:
				preset_check(&set, "--blocksize-limit-upper");
				_if((atoi(optarg)>65535 || atoi(optarg)<16), "Invalid upper limit blocksize");
				set.blocksize_limit_upper=atoi(optarg);
				break;

			case 265:
				preset_check(&set, "--merge");
				_if((atoi(optarg)<0), "Invalid merge setting");
				set.merge=atoi(optarg);
				break;

			case 266:
				set.wildcard=atoi(optarg);
				break;

			case 267:
				preset_check(&set, "--outputalt-apod");
				set.apod_outputalt=optarg;
				break;

			case 268:
				preset_check(&set, "--outputalt-comp");
				set.comp_outputalt=optarg;
				break;

			case 269:
				preset_check(&set, "--outperc");
				_if((atoi(optarg)<1 || atoi(optarg)>100), "Invalid --outperc setting (must be in integer between 1 and 100 inclusive)");
				set.outperc=atoi(optarg);
				break;

			case 270:
				_if((atoi(optarg)<=0), "Queue size cannot be negative");
				set.queue_size=atoi(optarg);
				break;

			case 271:
				set.md5=0;
				break;

			case 272:
				set.lax=1;
				break;

			case 273:
				_if((atoi(optarg)<1), "Invalid --peakset-window setting");
				set.peakset_window=atoi(optarg);
				break;

			case 274:
				set.seek=0;
				break;

			case 275:
				_if((strcmp(optarg, "wav")!=0 && strcmp(optarg, "flac")!=0 && strcmp(optarg, "cdda")!=0), "--input-format must be one of flac/wav/cdda");
				set.input_format=optarg;
				break;

			case 276:
				_if((set.ui_type==UI_MANUAL), "Cannot mix manual settings with --preset");
				_if((atoi(optarg)<0), "Cannot have a negative preset");
				_if((strchr(optarg, 'M')?1:0), "-M is an unsupported ./flac setting");
				set.ui_type=UI_PRESET;

				if(atoi(optarg)<=8){
					set.mode=MODE_FIXED;
					if(strchr(optarg, 'b') && atoi(strchr(optarg, 'b')+1)>15){
						blocklist_str=malloc(32);
						sprintf(blocklist_str, "%u", atoi(strchr(optarg, 'b')+1));
					}
					else
						blocklist_str="4096";
					set.merge=0;
					set.tweak=0;
					set.comp_output=optarg;
					set.comp_anal=optarg;
				}
				else{
					_if((strchr(optarg, 'b')?1:0), "-b is an unsupported ./flac setting for variable presets 9+");
					switch(atoi(optarg)){
						case 9:
							set.mode=MODE_GASC;
							blocklist_str="1536";
							if(strlen(optarg)>1){
								set.comp_anal=malloc(2+strlen(optarg));
								set.comp_output=malloc(1+strlen(optarg));
								strcpy(set.comp_anal+1, optarg);
								strcpy(set.comp_output, optarg);
								set.comp_anal[0]='3';
								set.comp_anal[1]='m';
								set.comp_output[0]='8';
							}
							else{
								set.comp_anal="3m";
								set.comp_output="8";
							}
							set.merge=0;
							set.tweak=0;
							break;

						case 10:
							set.mode=MODE_PEAKSET;
							blocklist_str="1152,2304,3456,4608";
							if(strlen(optarg)>1){
								set.comp_anal=malloc(3+strlen(optarg));
								set.comp_output=malloc(2+strlen(optarg));
								strcpy(set.comp_anal, optarg);
								strcpy(set.comp_output, optarg+1);
								set.comp_anal[0]='3';
								set.comp_anal[1]='m';
								set.comp_output[0]='8';
							}
							else{
								set.comp_output="8";
								set.comp_anal="3m";
							}
							set.merge=0;
							set.tweak=0;
							break;

						default:
							_("Unknown preset");
					}
				}
				break;

			case 277:
				_if((set.ui_type==UI_MANUAL), "Cannot mix manual settings with --preset");
				if(set.ui_type==UI_UNDEFINED){//set a default in case --preset is never defined
					set.mode=MODE_FIXED;
					blocklist_str="4096";
					set.merge=0;
					set.tweak=0;
					set.comp_output="6";
					set.comp_anal="6";
				}
				set.ui_type=UI_PRESET;
				set.apod_output=optarg;
				set.apod_outputalt=optarg;
				set.apod_anal=optarg;
				break;

			case 278:
				_if((atoi(optarg)<-1), "--seektable option invalid");
				_if((atoi(optarg)>932067), "Too many seekpoints, max format can handle is 932067");
				set.seektable=atoi(optarg);
				break;

			case 279:
				set.preserve_flac_metadata=1;
				break;

			case '?':
				_("Unknown option");
				break;
		}
	}

	_if((!ipath), "No input");
	_if((!opath), "No output");
	_if((set.mode==-1), "No mode set, either set a mode manually or choose a preset");
	_if((!set.seek && set.md5), "Cannot use MD5 if seek is disabled");
	_if((set.seektable!=0 && !set.seek), "Cannot add a seektable if seek is disabled");

	if(!blocklist_str){//valid defaults for the different modes
		if(set.mode==MODE_GASC)
			blocklist_str="1536";
		else if(set.mode==MODE_FIXED)
			blocklist_str="4096";
		else
			blocklist_str="1152,2304,4608";
	}
	parse_blocksize_list(blocklist_str, &(set.blocks), &(set.blocks_count));
	set.blocksize_min=set.blocks[0];
	set.blocksize_max=set.blocks[set.blocks_count-1];

	prepare_io(&in, ipath, &out, opath, header, &set);

	set.diff_comp_settings=strcmp(set.comp_anal, set.comp_output)!=0;
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && !set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(!set.apod_anal && set.apod_output);
	set.diff_comp_settings=set.diff_comp_settings?set.diff_comp_settings:(set.apod_anal && set.apod_output && strcmp(set.apod_anal, set.apod_output)!=0);

	encoder[set.mode](&in, &out, &set);
	fprintf(stderr, "\t%s\n", ipath);

	if(set.seek){
		//write finished header
		if(set.mode!=MODE_FIXED && set.blocksize_min==set.blocksize_max){//rare input can appear to be fixed when it should be variable
			if(set.blocksize_min==16){
				header[ 8]=(set.blocksize_min>>8)&255;
				header[ 9]=(set.blocksize_min>>0)&255;
				header[10]=((set.blocksize_max+1)>>8)&255;
				header[11]=((set.blocksize_max+1)>>0)&255;
			}
			else{
				header[ 8]=((set.blocksize_min-1)>>8)&255;
				header[ 9]=((set.blocksize_min-1)>>0)&255;
				header[10]=(set.blocksize_max>>8)&255;
				header[11]=(set.blocksize_max>>0)&255;
			}
		}
		else{
			header[ 8]=(set.blocksize_min>>8)&255;
			header[ 9]=(set.blocksize_min>>0)&255;
			header[10]=(set.blocksize_max>>8)&255;
			header[11]=(set.blocksize_max>>0)&255;
		}
		header[12]=(set.minf>>16)&255;
		header[13]=(set.minf>> 8)&255;
		header[14]=(set.minf>> 0)&255;
		header[15]=(set.maxf>>16)&255;
		header[16]=(set.maxf>> 8)&255;
		header[17]=(set.maxf>> 0)&255;
		header[21]|=((in.loc_analysis>>32)&15);
		header[22]=(in.loc_analysis>>24)&255;
		header[23]=(in.loc_analysis>>16)&255;
		header[24]=(in.loc_analysis>> 8)&255;
		header[25]=(in.loc_analysis>> 0)&255;
		memcpy(header+26, set.hash, 16);

		if(out.fout==stdout)//instead of full seek implementation for typedef output
			memcpy(out.cache, header, 42);
		else{
			fflush(out.fout);
			fseek(out.fout, 0, SEEK_SET);
			fwrite(header, 1, 42, out.fout);
		}
	}

	seektable_write(&(out.seektable), &out);

	out_close(&out);
}
