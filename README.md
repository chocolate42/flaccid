## Flaccid

A proof-of-concept multi-threaded variable-blocksize flac encoder frontend using a custom libFLAC as a backend ( https://github.com/chocolate42/flac )

Currently just a toy for benchmarking, there are many missing features that would be necessary for a user-ready version. Input is limited to raw or flac, nothing non-essential is generated not even a seektable, input is read fully to RAM before encoding, etc.

## Build

First build the custom libFLAC, it adds a few functions to tap into libFLAC's encoder in a non-streaming way: https://github.com/chocolate42/flac

Then to build flaccid on Linux do something like this:

gcc -oflaccid chunk.c common.c fixed.c flaccid.c gasc.c gset.c load.c peakset.c -I<PATH_TO_LIBFLAC_INCLUDE> <PATH_TO_libFLAC-static.a> -lcrypto -lm -logg -fopenmp -Wall -O3 -funroll-loops -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Waggregate-return -Wcast-align -Wnested-externs -Wshadow -Wundef -Wmissing-declarations -Winline  -Wdeclaration-after-statement -fvisibility=hidden -fstack-protector-strong

This is just a copy of the default flags used to compile libFLAC, plus OpenMP for coarse multithreading and OpenSSL for MD5.

## Static API

The changes boil down to:

* Leaving the existing API untouched
* Adding a new type FLAC__StaticEncoder which simply wraps FLAC__StreamEncoder, allowing stream functions to be used internally but separating the interface externally
* A few necessary functions to create and destroy the new type
* The user encodes per-frame, by feeding an entire frame of input along with the frame/sample index and providing a valid static encoder instance. Instead of callbacks the function returns a buffer containing the encoded frame, valid until the static encoder instance is re-used
* To reduce needlessly copying data there's a variant with int16_t[] input. There's still a copy from input to internal buffer but it eliminates the intermediate external int32_t[] buffer
* The frame encoders do not do MD5 hashing, hashing is an in-order operation and we cannot guarantee that (a major use case of the API is to allow things to be done out-of-order)

## Flaccid features

* Multithreading, of variable and fixed blocking strategy encodes
* Multiple analysis modes to choose variable blocksizes, with different complexities 
* Optional merge/tweak passes to refine frame permutation to be more efficient
* Optional analysis encode settings different to output encode settings

See ./flaccid -h for more details
