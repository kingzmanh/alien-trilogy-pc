#pragma once
#include "setup.h"

/* extract every entry whose name (without its path) want() accepts into dstdir;
   returns the number of files written, -1 if the archive can't be read */
int zip_extract(const char *zip, const char *dstdir, int (*want)(const char *name, void *ud), void *ud,
                void (*progress)(u64 bytes, void *ud));
/* the same, want() gets the whole path inside the archive */
int zip_extract_path(const char *zip, const char *dstdir, int (*want)(const char *path, void *ud), void *ud,
                     void (*progress)(u64 bytes, void *ud));
