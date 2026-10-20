# LZ4 HC in OpenZFS

`lib/` holds the LZ4 HC (high compression) compressor from the upstream
LZ4 project (https://github.com/lz4/lz4), BSD 2-Clause licensed. Apart
from the SPDX tag on the first line of each file, the files are
unmodified. `zfs_lz4hc.c` is the OpenZFS glue and builds the library
in that single translation unit.

LZ4 HC writes standard LZ4 blocks. Its output is therefore stored as
`ZIO_COMPRESS_LZ4` in the block pointer and is decompressed by the
existing LZ4 decoder in `module/zfs/lz4.c`. Only the compressor and
the `lz4hc` property values are new.

## Updating

The exact compressed bytes may change between LZ4 releases, which
affects deduplication, NOP writes and L2ARC recompression, so update
under the same rules as `module/zstd/README.md`:

1. Copy `lz4.c`, `lz4.h`, `lz4hc.c`, `lz4hc.h` and `LICENSE` from the
   release's `lib/` directory, unmodified, in a commit of their own.
2. Add the SPDX line back in a follow-up commit.
3. Check that the glue still builds without warnings.
