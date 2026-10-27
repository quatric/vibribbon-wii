#ifndef SHA1_H
#define SHA1_H

#include <gctypes.h>

void sha1_digest(const void *data, u32 len, u8 out[20]);

#endif
