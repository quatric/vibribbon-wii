/*
 * usbcd - USB Mass Storage (Bulk-Only Transport) driver for optical drives,
 * speaking SCSI/MMC directly so we can read CD-DA sectors.
 */
#ifndef USBCD_H
#define USBCD_H

#include <gctypes.h>
#include <stdbool.h>

#define CD_FRAME_BYTES      2352
#define CD_FRAMES_PER_SEC   75
#define CD_MAX_TRACKS       99

/* Largest single READ CD request (sectors). Bounded by the MEM2 bounce buffer. */
#define USBCD_MAX_READ_SECTORS 16

/* usbcd_scsi() return codes */
#define USBCD_OK            0
#define USBCD_CHECK         1    /* CHECK CONDITION; see usbcd_last_sense() */
#define USBCD_ENODEV       -1    /* no drive open */
#define USBCD_ETIMEOUT     -2
#define USBCD_EIO          -3    /* transport / protocol failure */
#define USBCD_EINVAL       -4
#define USBCD_EPHASE       -5

typedef struct {
	u8 key, asc, ascq;
} cd_sense_t;

typedef struct {
	u8  number;
	u8  control;          /* bit 2 set = data track */
	u32 lba;              /* start sector */
	u32 length;           /* sectors (playable) */
} cd_track_t;

typedef struct {
	int first, last, count;
	cd_track_t tracks[CD_MAX_TRACKS];
	u32 leadout;
	int audio_count;
} cd_toc_t;

typedef struct {
	char album[64];
	char artist[64];
	char title[CD_MAX_TRACKS][48];
	char performer[CD_MAX_TRACKS][48];
	bool present;
} cd_text_t;

#define TRACK_IS_DATA(t) (((t)->control & 0x04) != 0)

int  usbcd_init(void);
int  usbcd_probe(void);            /* 1 = drive opened, 0 = none found */
bool usbcd_is_open(void);
void usbcd_close(void);
int  usbcd_reset(void);
const char *usbcd_name(void);
u16  usbcd_vid(void);
u16  usbcd_pid(void);
bool usbcd_highspeed(void);

/* Generic SCSI passthrough. len must be <= bounce buffer size. */
int  usbcd_scsi(const u8 *cdb, int cdblen, void *data, u32 len, bool data_in,
                u32 timeout_s, u32 *xferred);
cd_sense_t usbcd_last_sense(void);
u32  usbcd_bounce_size(void);

int  usbcd_test_unit_ready(void);
int  usbcd_inquiry(u8 *peripheral_type);
int  usbcd_read_toc(cd_toc_t *toc);
int  usbcd_read_cdtext(cd_text_t *txt);
int  usbcd_read_audio(u32 lba, u32 count, void *buf);
int  usbcd_start_stop(bool loej, bool start);
int  usbcd_allow_removal(bool allow);
int  usbcd_set_speed(u16 read_kbps);

const char *usbcd_sense_str(cd_sense_t s);

#endif
