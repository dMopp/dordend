#pragma once

// The wuffs modules rummage compiles: PNG, JPEG and GIF with what they need, and the C++
// convenience API (wuffs_aux). Included before wuffs-v0.4.c everywhere it is used.

// NOLINTBEGIN(bugprone-reserved-identifier): the names wuffs reads
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__AUX__BASE
#define WUFFS_CONFIG__MODULE__AUX__IMAGE
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__ZLIB
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__JPEG
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__GIF
// NOLINTEND(bugprone-reserved-identifier)
