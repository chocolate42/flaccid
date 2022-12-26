## Flaccid

A proof-of-concept multi-threaded variable-blocksize flac encoder frontend using a custom libFLAC as a backend ( https://github.com/chocolate42/flac )

Currently just a toy for benchmarking, there are many missing features that would be necessary for a user-ready version. Input is limited to raw or flac, doesn't do seektables or anything non-essential, reads input fully to RAM before encoding, probably doesn't compile on windows, IO and MD5 hashing may be non-optimal or not implemented for some input, etc.

## Build

First build libFLAC from this repo, it adds a few functions to tap into libFLAC's encoder in a non-streaming way: https://github.com/chocolate42/flac

Then to build flaccid on Linux do something like this:

gcc -oflaccid chunk.c common.c flaccid.c gset.c load.c merge.c peakset.c tweak.c -I<PATH_TO_LIBFLAC_INCLUDE> <PATH_TO_libFLAC-static.a> -lcrypto -lm -logg -fopenmp -Wall -O3 -funroll-loops  -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Waggregate-return -Wcast-align -Wnested-externs -Wshadow -Wundef -Wmissing-declarations -Winline  -Wdeclaration-after-statement -fvisibility=hidden -fstack-protector-strong

This is just a copy of the default flags used to compile libFLAC, plus OpenMP for coarse multithreading and OpenSSL for MD5.

## Static API

The changes boil down to:

* Leaving the existing API untouched
* Adding a new type FLAC__StaticEncoder which simply wraps FLAC__StreamEncoder, allowing stream functions to be used internally but separating the interface externally
* A few necessary functions to create and destroy the new type
* The user encodes per-frame, by feeding an entire frame of input along with the frame/sample index and providing a valid static encoder instance. Instead of callbacks the function returns a buffer containing the encoded frame, valid until the static encoder instance is re-used
* To reduce needlessly copying data there's a variant with int16_t[] input. There's still a copy from input to internal buffer but it eliminates the intermediate external int32_t[] buffer
* The frame encoders do not do MD5 hashing, hashing is an in-order operation and we cannot guarantee that (in fact the major use case of the API is to allow things to be done out-of-order)

## flaccid options

There's some options exposed to modify how variable blocksizes are chosen, they all boil down to brute force testing which blocks are more efficient in some way. Some options are better than others, it's a WIP to find which levers to pull to quickly increase efficiency:

* peakset mode is optimal for a given blocksize list and encode settings when the analysis settings are the same as the encode settings
* chunk mode takes much less effort for worse efficiency, but is still reasonable for quick encodes
* gset mode greedily picks the best option for the next frame from a fixed set of block sizes. It has relatively weak efficiency because it doesn't take into account local frames, and thread occupancy is lower than peakset and chunk as every test for the next frame has to complete before moving on to the next frame

To speed things up and improve space-efficiency there's some additional options:

* 1 worker by default, all other things equal a near-linear speedup can be gained by changing the number of workers to the number of cores on the system
* --lax allows the encoder to use large blocksizes and other non-subset settings
* Analysis can use different compression settings to the final encode. -5 reasonably approximates the behaviour of -8, and as analysis takes the majority of effort this can save a lot of time for a small loss in efficiency
* Merge passes combine adjacent frames and store them as one frame if the result is smaller
* Tweak passes adjust where adjacent frames are split, storing frames with a new split if the result is smaller
* Merge and tweak are defined with pass thresholds, multiple passes are done until a pass doesn't meet the threshold
* Merge and tweak use analysis settings by default, but they can use encode settings if desired (probably shouldn't be used, at least not until very slow settings are used)
* Tweak and merge behaviour can be fine-tuned by limiting min and max blocksize. It mostly doesn't do much but some input may benefit
* Every tweak test tries above and below the current best, it can be told to early exit if the first test saves bytes. This probably shouldn't be set, it can save time but also loses quite a lot of efficiency for general input as often both going above and below saves bytes, it's pot-luck if the first test is the best of the two
