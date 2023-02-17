#include "seektable.h"

#include <stdlib.h>
#include <string.h>

void seektable_init(seektable_t *seektable, flac_settings *set, uint8_t *header){
	size_t seconds;
	if(set->seektable!=0){
		if(set->seektable==-1 && set->input_tot_samples){//we know sample count from input
			seconds=set->input_tot_samples/set->sample_rate;
			//arbitrarily scale seekpoint count for the hell of it
			if(seconds<180)
				set->seektable=seconds/3;//could be 0
			else if(seconds<600)
				set->seektable=seconds/6;
			else if(seconds<600*80)
				set->seektable=seconds/10;
			else
				set->seektable=seconds/15;
			set->seektable=(set->seektable>932067)?932067:set->seektable;//it would take weird input to trigger this
		}
		else if(set->seektable==-1)
			set->seektable=100;//hope THIS is enough. All players probably use sync anyway so even worst case this is probably fine
		else if(set->input_tot_samples && ((set->input_tot_samples/set->blocks[0])<set->seektable)){//save users from themselves
			fprintf(stderr, "Warning, overriding user-chosen seektable size to %zu, it was way too big\n", (set->input_tot_samples/set->blocks[0]));
			set->seektable=(set->input_tot_samples/set->blocks[0]);
		}
	}
	if(set->seektable!=0)//may have changed
		header[4]-=(header[4]>=0x80)?0x80:0;//streaminfo not last metadata block
	seektable->write_cnt=set->seektable;
}

//should be a even number
#define SEEKTABLE_GROWTH (58254)
//store a seekpoint for every frame. Even for sample counts of 2^36 this
//takes <1GiB of RAM when blocksize averages >1152. Worst case is ~72GiB
//RAM with 2^36 samples and blocksize 16, if someone wants to encode that
//madness they can do it with lots of RAM
//typical input will use <1MiB per hour of input
void seektable_add(seektable_t *seektable, uint64_t sample_num, uint64_t offset, uint16_t frame_sample_cnt){
	if(seektable->cnt==seektable->alloc){
			seektable->set=realloc(seektable->set, (seektable->alloc+SEEKTABLE_GROWTH)*18);
			seektable->alloc+=SEEKTABLE_GROWTH;
	}
	seektable->set[seektable->cnt].sample_num=sample_num;
	seektable->set[seektable->cnt].offset=offset;
	seektable->set[seektable->cnt].frame_sample_cnt=frame_sample_cnt;
	++seektable->cnt;
}

void seektable_write_dummy(seektable_t *seektable, flac_settings *set, output *out){
	size_t i;
	uint8_t mhead[4]={0x83, 0, 0, 0};//seektable is last metadata block
	uint8_t dummy[18]={255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	if(set->seektable){
		mhead[1]=((18*set->seektable)>>16)&255;
		mhead[2]=((18*set->seektable)>> 8)&255;
		mhead[3]=((18*set->seektable)    )&255;
		out_write(out, mhead, 4);
		seektable->seektable_loc=out->outloc;
		for(i=0;i<set->seektable;++i)
			out_write(out, dummy, 18);
		seektable->firstframe_loc=out->outloc;
	}
}

static size_t bwrite_u16be(uint64_t n, uint8_t *data){
	data[1]=n&255;
	data[0]=(n>>8)&255;
	return 2;
}
static size_t bwrite_u64be(uint64_t n, uint8_t *data){
	data[7]=n&255;
	data[6]=(n>>8)&255;
	data[5]=(n>>16)&255;
	data[4]=(n>>24)&255;
	data[3]=(n>>32)&255;
	data[2]=(n>>40)&255;
	data[1]=(n>>48)&255;
	data[0]=(n>>56)&255;
	return 8;
}
static void seekpoint_build(seekpoint_t *seekpoint, uint8_t *build){
	bwrite_u64be(seekpoint->sample_num, build);
	bwrite_u64be(seekpoint->offset, build+8);
	bwrite_u16be(seekpoint->frame_sample_cnt, build+16);
}

void seektable_write(seektable_t *seektable, output *out){
	size_t i, ideal_seek_step, next_step=0, curr_index=0;
	uint8_t seek_out[18];
	if(seektable->write_cnt==0)
		return;

	seektable_add(seektable, UINT64_MAX, 0, 0);//add dummy seekpoint so we don't overshoot

	if(out->fout!=stdout){
			fflush(out->fout);
			fseek(out->fout, seektable->seektable_loc, SEEK_SET);
	}
	ideal_seek_step=out->sampleloc/(seektable->write_cnt+1);
	for(i=0;i<seektable->write_cnt;++i){
		next_step+=ideal_seek_step;
		++curr_index;//skip first seekpoint or the one we just emitted
		while(seektable->set[curr_index].sample_num<next_step)
			++curr_index;
		if(seektable->set[curr_index].sample_num==UINT64_MAX)
			break;
		seekpoint_build(seektable->set+curr_index, seek_out);
		if(out->fout==stdout)//instead of implementing proper seek for typedef output
			memcpy(out->cache+seektable->seektable_loc+(i*18), seek_out, 18);
		else
			fwrite(seek_out, 1, 18, out->fout);
	}

	free(seektable->set);
}
