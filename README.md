## Flaccid

A proof-of-concept multi-threaded flac encoder frontend

This is just a toy to test the static encoder additions made to the libFLAC API here ( https://github.com/chocolate42/flac ). It only accepts raw CDDA, doesn't do seektables or anything non-essential, used a fixed blocksize of 4096, has a horrible UI, uses POSIX mmap for input so probably doesn't compile on windows, has barely been tested, etc.

## Build

First build libFLAC from this repo, it adds a few functions to tap into libFLAC's encoder in a non-streaming way (allowing for among other things SMT): https://github.com/chocolate42/flac

Then to build flaccid on Linux do something like this:

gcc -oflaccid flaccid.c -I<PATH_TO_LIBFLAC_INCLUDE> <PATH_TO_libFLAC-static.a> -lcrypto -lm -logg -fopenmp -Wall -O3 -funroll-loops  -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Waggregate-return -Wcast-align -Wnested-externs -Wshadow -Wundef -Wmissing-declarations -Winline  -Wdeclaration-after-statement -fvisibility=hidden -fstack-protector-strong

This is just a copy of the flags used to compile libFLAC, plus OpenMP for SMT and OpenSSL for MD5.

## Static API

The changes boil down to:

* Leaving the existing API untouched
* Adding a new type FLAC__StaticEncoder which simply wraps FLAC__StreamEncoder, allowing stream functions to be used internally but separating the interface externally
* A few necessary functions to create and destroy the new type
* The user encodes per-frame, by feeding an entire frame of input along with the frame index and static encoder instance to a function. Instead of callbacks the function returns a buffer containing the encoded frame
* To reduce needlessly copying data there's a variant with int16_t[] input. There's still a copy from input to internal buffer but it eliminates the intermediate external int32_t[] buffer
* The frame encoders do not do MD5 hashing, as hashing is an in-order operation and we cannot guarantee that (in fact the main point of the API is to allow things to be done out-of-order)

## flaccid jank

* Worker count is the maximum number of encode threads to use simultaneouusly, but depending on the mode it's not necessarily the total number of simultaneous threads. I/O and MD5 may have their own threads and how they act may not be optimal, it's implemented however was convenient
* Raw CDDA input only just because it was convenient
