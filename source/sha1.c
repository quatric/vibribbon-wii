/* Minimal SHA-1 (FIPS 180-1), enough for MusicBrainz disc IDs. */
#include <string.h>
#include "sha1.h"

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_block(u32 h[5], const u8 *p)
{
	u32 w[80], a, b, c, d, e, f, k, t;
	int i;

	for (i = 0; i < 16; i++)
		w[i] = ((u32)p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
	for (i = 16; i < 80; i++)
		w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
		else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
		else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
		t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = c; c = ROL(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void sha1_digest(const void *data, u32 len, u8 out[20])
{
	u32 h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
	const u8 *p = data;
	u8 tail[128];
	u32 rem = len % 64, n, i;
	u64 bits = (u64)len * 8;

	for (i = 0; i + 64 <= len; i += 64)
		sha1_block(h, p + i);

	memset(tail, 0, sizeof(tail));
	memcpy(tail, p + len - rem, rem);
	tail[rem] = 0x80;
	n = rem < 56 ? 64 : 128;
	for (i = 0; i < 8; i++)
		tail[n - 1 - i] = bits >> (i * 8);
	sha1_block(h, tail);
	if (n == 128)
		sha1_block(h, tail + 64);

	for (i = 0; i < 5; i++) {
		out[i * 4] = h[i] >> 24;
		out[i * 4 + 1] = h[i] >> 16;
		out[i * 4 + 2] = h[i] >> 8;
		out[i * 4 + 3] = h[i];
	}
}
