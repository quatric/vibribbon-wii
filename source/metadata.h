/*
 * metadata - album/track names from MusicBrainz (via libcurl), cached on SD.
 */
#ifndef METADATA_H
#define METADATA_H

#include <stdbool.h>
#include "usbcd.h"

typedef enum {
	META_NONE,        /* no disc */
	META_WAIT_NET,    /* lookup queued until the network is up */
	META_LOOKUP,      /* request in flight */
	META_FOUND,       /* from MusicBrainz */
	META_CACHED,      /* from sd cache */
	META_NOT_FOUND,
	META_ERROR,
} meta_state_t;

#define DISCID_LEN 29     /* 28 chars + NUL */

void meta_init(void);
void meta_shutdown(void);
/* new disc (toc) or disc gone (NULL) */
void meta_request(const cd_toc_t *toc);
/* force a network lookup for the current disc, ignoring the cache */
void meta_refresh(void);
/* copies current results; returns state. txt is valid for FOUND/CACHED. */
meta_state_t meta_get(cd_text_t *txt, char discid[DISCID_LEN], char *source, int source_len);
const char *meta_state_str(meta_state_t s);

/* Disc ID per https://musicbrainz.org/doc/Disc_ID_Calculation. offsets are
 * absolute (LBA + 150); leadout likewise. */
void meta_discid(int first, int last, u32 leadout, const u32 *offsets, char out[DISCID_LEN]);
/* Synchronous lookup, used by the debug shell. Returns META_FOUND/NOT_FOUND/ERROR. */
meta_state_t meta_lookup(int first, int last, u32 leadout, const u32 *offsets, cd_text_t *txt,
                         char *source, int source_len);

#endif
