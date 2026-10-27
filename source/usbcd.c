/*
 * usbcd - USB Mass Storage (Bulk-Only Transport) driver for optical drives.
 *
 * libogc's usbstorage only knows READ(10)/WRITE(10) on 512/2048-byte blocks
 * and clamps the CBW CDB length to 10 bytes. Audio CDs need 12-byte MMC
 * commands (READ CD 0xBE, SET CD SPEED 0xBB) and 2352-byte raw frames, so this
 * is a small standalone BOT implementation on top of libogc's USB_* IOS calls.
 */
#include <gccore.h>
#include <ogc/usb.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "usbcd.h"
#include "netlog.h"

#define USB_CLASS_MASS_STORAGE   0x08
#define MASS_STORAGE_BULK_ONLY   0x50
#define USB_ENDPOINT_BULK        0x02

#define BOT_GET_MAX_LUN          0xFE
#define BOT_RESET                0xFF

#define CBW_SIGNATURE            0x43425355
#define CSW_SIGNATURE            0x53425355
#define CBW_SIZE                 31
#define CSW_SIZE                 13
#define CBW_IN                   0x80

#define BOUNCE_DATA_SIZE         (CD_FRAME_BYTES * USBCD_MAX_READ_SECTORS)
#define ARENA_SIZE               (BOUNCE_DATA_SIZE + 256)

#define DEVLIST_MAX              8

/* MEM2 DMA-safe scratch: [0..63] CBW, [64..95] CSW, [96..127] misc, [128..255] sense, [256..] data */
static u8 *arena = NULL;
static u8 *cbw_buf, *csw_buf, *misc_buf, *sense_buf, *data_buf;

static mutex_t lock = LWP_MUTEX_NULL;

/* USBv5 device handles can be negative, so openness is tracked separately */
static s32  usb_fd = -1;
static bool dev_open;
static u16  dev_vid, dev_pid;
static u8   ep_in, ep_out;
static u8   iface;
static u8   subclass;
static u8   lun;
static u8   max_lun;
static bool hs;
static u32  tag = 0x0BADC0DE;
static volatile bool removed = false;
static cd_sense_t last_sense;
static char devname[48] = "";

static volatile u32 cur_gen = 0;
static volatile bool xfer_done;
static volatile s32 xfer_result;

static inline void put_le32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline u32  get_le32(const u8 *p)  { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static inline u32  get_be32(const u8 *p)  { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline u16  get_be16(const u8 *p)  { return (p[0] << 8) | p[1]; }

static s32 xfer_cb(s32 result, void *usr)
{
	if ((u32)usr == cur_gen) {
		xfer_result = result;
		xfer_done = true;
	}
	return 0;
}

static s32 removal_cb(s32 result, void *usr)
{
	removed = true;
	return 0;
}

/* Wait for the in-flight async request by polling: simple, and the latency
 * (a few hundred microseconds) is irrelevant at CD audio rates. */
static s32 wait_xfer(u32 timeout_ms)
{
	u64 start = gettime();
	while (!xfer_done) {
		if (removed)
			return USBCD_ENODEV;
		if (ticks_to_millisecs(diff_ticks(start, gettime())) > timeout_ms)
			return USBCD_ETIMEOUT;
		usleep(200);
	}
	return xfer_result;
}

static s32 bulk(u8 ep, void *buf, u16 len, u32 timeout_ms)
{
	s32 r;
	u32 gen;

	xfer_done = false;
	gen = ++cur_gen;
	r = USB_WriteBlkMsgAsync(usb_fd, ep, len, buf, xfer_cb, (void *)gen);
	if (r < 0)
		return r;
	return wait_xfer(timeout_ms);
}

static s32 ctrl(u8 reqtype, u8 req, u16 value, u16 index, u16 len, void *buf, u32 timeout_ms)
{
	s32 r;
	u32 gen;

	xfer_done = false;
	gen = ++cur_gen;
	r = USB_WriteCtrlMsgAsync(usb_fd, reqtype, req, value, index, len, buf, xfer_cb, (void *)gen);
	if (r < 0)
		return r;
	return wait_xfer(timeout_ms);
}

static void reset_recovery(void)
{
	if (!dev_open)
		return;
	ctrl(USB_CTRLTYPE_DIR_HOST2DEVICE | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE,
	     BOT_RESET, 0, iface, 0, NULL, 1000);
	usleep(60 * 1000);
	USB_ClearHalt(usb_fd, ep_in);
	usleep(10 * 1000);
	USB_ClearHalt(usb_fd, ep_out);
	usleep(10 * 1000);
}

int usbcd_init(void)
{
	u32 level;
	u8 *hi;

	if (arena)
		return 0;

	_CPU_ISR_Disable(level);
	hi = (u8 *)SYS_GetArena2Hi();
	arena = (u8 *)(((u32)hi - ARENA_SIZE) & ~31);
	if ((u32)arena < (u32)SYS_GetArena2Lo()) {
		arena = NULL;
		_CPU_ISR_Restore(level);
		return -1;
	}
	SYS_SetArena2Hi(arena);
	_CPU_ISR_Restore(level);

	cbw_buf   = arena;
	csw_buf   = arena + 64;
	misc_buf  = arena + 96;
	sense_buf = arena + 128;
	data_buf  = arena + 256;

	LWP_MutexInit(&lock, true);
	return USB_Initialize();
}

u32 usbcd_bounce_size(void) { return BOUNCE_DATA_SIZE; }
bool usbcd_is_open(void) { return dev_open && !removed; }
const char *usbcd_name(void) { return devname; }
u16 usbcd_vid(void) { return dev_vid; }
u16 usbcd_pid(void) { return dev_pid; }
bool usbcd_highspeed(void) { return hs; }
cd_sense_t usbcd_last_sense(void) { return last_sense; }

void usbcd_close(void)
{
	LWP_MutexLock(lock);
	if (dev_open) {
		USB_CloseDevice(&usb_fd);
		dlog("usbcd: closed %04x:%04x", dev_vid, dev_pid);
	}
	usb_fd = -1;
	dev_open = false;
	removed = false;
	devname[0] = 0;
	LWP_MutexUnlock(lock);
}

int usbcd_reset(void)
{
	if (!dev_open)
		return USBCD_ENODEV;
	LWP_MutexLock(lock);
	reset_recovery();
	LWP_MutexUnlock(lock);
	return 0;
}

/* One full CBW -> data -> CSW cycle. Caller holds the lock. Returns
 * <0 transport error, otherwise the CSW status (0 pass, 1 fail, 2 phase). */
static int bot_cycle(u8 l, const u8 *cdb, int cdblen, u8 *buf, u32 len, bool in,
                     u32 timeout_ms, u32 *xferred)
{
	s32 r;
	u32 done = 0, maxchunk = hs ? 16384 : 4096;
	u8 ep = in ? ep_in : ep_out;
	u32 sig, rtag;

	memset(cbw_buf, 0, CBW_SIZE);
	put_le32(cbw_buf + 0, CBW_SIGNATURE);
	put_le32(cbw_buf + 4, ++tag);
	put_le32(cbw_buf + 8, len);
	cbw_buf[12] = (in && len) ? CBW_IN : 0;
	cbw_buf[13] = l;
	cbw_buf[14] = cdblen;
	memcpy(cbw_buf + 15, cdb, cdblen);

	r = bulk(ep_out, cbw_buf, CBW_SIZE, 2000);
	if (r != CBW_SIZE)
		return r < 0 ? r : USBCD_EIO;

	while (done < len) {
		u32 chunk = len - done;
		if (chunk > maxchunk)
			chunk = maxchunk;
		r = bulk(ep, buf + done, chunk, timeout_ms);
		if (r < 0) {
			if (r == USBCD_ETIMEOUT || r == USBCD_ENODEV)
				return r;
			/* Device stalled the data pipe (e.g. less data than asked);
			 * clear it and go fetch the CSW per the BOT spec. */
			USB_ClearHalt(usb_fd, ep);
			break;
		}
		done += r;
		if ((u32)r < chunk)
			break;   /* short packet: end of data phase */
	}
	if (xferred)
		*xferred = done;

	r = bulk(ep_in, csw_buf, CSW_SIZE, timeout_ms);
	if (r < 0 && r != USBCD_ETIMEOUT && r != USBCD_ENODEV) {
		USB_ClearHalt(usb_fd, ep_in);
		r = bulk(ep_in, csw_buf, CSW_SIZE, timeout_ms);
	}
	if (r != CSW_SIZE)
		return r < 0 ? r : USBCD_EIO;

	sig  = get_le32(csw_buf);
	rtag = get_le32(csw_buf + 4);
	if (sig != CSW_SIGNATURE || rtag != tag)
		return USBCD_EIO;
	return csw_buf[12];
}

int usbcd_scsi(const u8 *cdb, int cdblen, void *data, u32 len, bool data_in,
               u32 timeout_s, u32 *xferred)
{
	int r, attempt;
	u32 got = 0;

	if (cdblen < 1 || cdblen > 16 || len > BOUNCE_DATA_SIZE)
		return USBCD_EINVAL;

	LWP_MutexLock(lock);
	if (!dev_open || removed) {
		LWP_MutexUnlock(lock);
		return USBCD_ENODEV;
	}

	memset(&last_sense, 0, sizeof(last_sense));
	if (!data_in && len)
		memcpy(data_buf, data, len);

	for (attempt = 0; attempt < 2; attempt++) {
		r = bot_cycle(lun, cdb, cdblen, data_buf, len, data_in, timeout_s * 1000, &got);
		if (r == USBCD_ENODEV)
			break;
		if (r >= 0 && r != 2)
			break;
		dlog("usbcd: cmd %02x transport error %d, reset recovery", cdb[0], r);
		reset_recovery();
		if (r == 2)
			r = USBCD_EPHASE;
	}

	if (r == 1) {
		/* CHECK CONDITION: fetch sense */
		u8 rs[6] = { 0x03, 0, 0, 0, 18, 0 };
		u32 sl = 0;
		memset(sense_buf, 0, 18);
		if (bot_cycle(lun, rs, 6, sense_buf, 18, true, 2000, &sl) == 0 && sl >= 14) {
			last_sense.key  = sense_buf[2] & 0x0F;
			last_sense.asc  = sense_buf[12];
			last_sense.ascq = sense_buf[13];
		}
		r = USBCD_CHECK;
	}

	if (data_in && got && data)
		memcpy(data, data_buf, got);
	if (xferred)
		*xferred = got;
	LWP_MutexUnlock(lock);
	return r;
}

static u8 inq_raw[8];

int usbcd_inquiry(u8 *ptype)
{
	u8 cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
	u8 resp[36];
	u32 got = 0;
	int r;
	char vendor[9], product[17], rev[5];

	memset(resp, 0, sizeof(resp));
	r = usbcd_scsi(cdb, 6, resp, 36, true, 3, &got);
	if (r != USBCD_OK)
		return r;
	memcpy(inq_raw, resp, sizeof(inq_raw));
	if (got < 36)
		dlog("usbcd: INQUIRY returned only %u bytes", got);
	if (ptype)
		*ptype = resp[0] & 0x1F;
	memcpy(vendor, resp + 8, 8);   vendor[8] = 0;
	memcpy(product, resp + 16, 16); product[16] = 0;
	memcpy(rev, resp + 32, 4);      rev[4] = 0;
	snprintf(devname, sizeof(devname), "%s %s %s", vendor, product, rev);
	return USBCD_OK;
}

/* GET CONFIGURATION (MMC). Only optical drives implement it. */
static int mmc_get_configuration(u16 *profile)
{
	u8 cdb[10] = { 0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0 };
	u8 resp[8];
	u32 got = 0;
	int r;

	memset(resp, 0, sizeof(resp));
	r = usbcd_scsi(cdb, 10, resp, 8, true, 3, &got);
	if (r == USBCD_OK && profile)
		*profile = got >= 8 ? get_be16(resp + 6) : 0;
	return r;
}

/* Decide whether LUN l is an optical drive. Returns the reason, or NULL.
 * INQUIRY type 5 is the normal answer, but USB-ATAPI bridges are not always
 * truthful (or answer INQUIRY late after power-up), so an MMC GET
 * CONFIGURATION reply or an ATAPI interface subclass also counts. */
static const char *probe_lun(u8 l)
{
	u8 ptype = 0xFF;
	u16 profile = 0;
	int r = USBCD_EIO, attempt, g;
	cd_sense_t s;

	lun = l;
	for (attempt = 0; attempt < 3; attempt++) {
		r = usbcd_inquiry(&ptype);
		if (r == USBCD_OK)
			break;
		s = usbcd_last_sense();
		dlog("usbcd: LUN %u INQUIRY try %d failed: %d (sense %x/%02x/%02x)", l, attempt + 1, r,
		     s.key, s.asc, s.ascq);
		if (r == USBCD_ENODEV)
			return NULL;
		if (r < 0)
			usbcd_reset();
		usleep(300 * 1000);
	}
	if (r == USBCD_OK)
		dlog("usbcd: LUN %u INQUIRY type %u [%02x %02x %02x %02x %02x %02x %02x %02x] %s", l, ptype,
		     inq_raw[0], inq_raw[1], inq_raw[2], inq_raw[3], inq_raw[4], inq_raw[5], inq_raw[6], inq_raw[7],
		     devname);

	if (r == USBCD_OK && ptype == 5)
		return "INQUIRY type 5";

	g = mmc_get_configuration(&profile);
	s = usbcd_last_sense();
	dlog("usbcd: LUN %u GET CONFIGURATION -> %d profile %04x (sense %x/%02x/%02x)", l, g, profile,
	     s.key, s.asc, s.ascq);
	if (g == USBCD_OK)
		return "answers MMC GET CONFIGURATION";
	if (subclass == 0x02 || subclass == 0x05)
		return "ATAPI interface subclass";
	if (r != USBCD_OK && g >= 0)
		return "responds to SCSI, INQUIRY unusable";
	return NULL;
}

/* Find and open the first USB mass-storage device that is a CD/DVD drive. */
int usbcd_probe(void)
{
	usb_device_entry list[DEVLIST_MAX] ATTRIBUTE_ALIGN(32);
	u8 count = 0, i;

	if (usbcd_is_open())
		return 1;
	if (removed)
		usbcd_close();

	memset(list, 0, sizeof(list));
	if (USB_GetDeviceList(list, DEVLIST_MAX, USB_CLASS_MASS_STORAGE, &count) < 0)
		return 0;

	for (i = 0; i < count; i++) {
		usb_devdesc udd;
		u32 c, n, e;
		bool found = false;
		s32 fd = -1, r;
		u8 conf = 0, l;

		if (list[i].vid == 0 && list[i].pid == 0)
			continue;
		if (USB_OpenDevice(list[i].device_id, list[i].vid, list[i].pid, &fd) < 0)
			continue;
		if (USB_GetDescriptors(fd, &udd) < 0) {
			USB_CloseDevice(&fd);
			continue;
		}

		for (c = 0; c < udd.bNumConfigurations && !found; c++) {
			usb_configurationdesc *ucd = &udd.configurations[c];
			for (n = 0; n < ucd->bNumInterfaces && !found; n++) {
				usb_interfacedesc *uid = &ucd->interfaces[n];
				if (uid->bInterfaceClass != USB_CLASS_MASS_STORAGE ||
				    uid->bInterfaceProtocol != MASS_STORAGE_BULK_ONLY)
					continue;
				ep_in = ep_out = 0;
				for (e = 0; e < uid->bNumEndpoints; e++) {
					usb_endpointdesc *ued = &uid->endpoints[e];
					if ((ued->bmAttributes & 3) != USB_ENDPOINT_BULK)
						continue;
					if (ued->bEndpointAddress & USB_ENDPOINT_IN)
						ep_in = ued->bEndpointAddress;
					else {
						ep_out = ued->bEndpointAddress;
						hs = ued->wMaxPacketSize > 64;
					}
				}
				if (ep_in && ep_out) {
					found = true;
					iface = uid->bInterfaceNumber;
					subclass = uid->bInterfaceSubClass;
					conf = ucd->bConfigurationValue;
				}
			}
		}
		USB_FreeDescriptors(&udd);

		if (!found) {
			USB_CloseDevice(&fd);
			continue;
		}

		LWP_MutexLock(lock);
		usb_fd = fd;
		dev_open = true;
		removed = false;
		dev_vid = list[i].vid;
		dev_pid = list[i].pid;
		{
			u8 cur = 0;
			USB_GetConfiguration(usb_fd, &cur);
			if (cur != conf)
				USB_SetConfiguration(usb_fd, conf);
		}
		misc_buf[0] = 0;
		r = ctrl(USB_CTRLTYPE_DIR_DEVICE2HOST | USB_CTRLTYPE_TYPE_CLASS | USB_CTRLTYPE_REC_INTERFACE,
		         BOT_GET_MAX_LUN, 0, iface, 1, misc_buf, 1000);
		max_lun = (r >= 1 && misc_buf[0] < 8) ? misc_buf[0] + 1 : 1;
		if (r < 0) {
			/* some bridges stall GET MAX LUN and leave the bulk pipes halted */
			USB_ClearHalt(usb_fd, ep_in);
			USB_ClearHalt(usb_fd, ep_out);
		}
		USB_DeviceRemovalNotifyAsync(usb_fd, removal_cb, NULL);
		LWP_MutexUnlock(lock);

		dlog("usbcd: %04x:%04x iface %u subclass %02x ep in %02x out %02x %s-speed",
		     dev_vid, dev_pid, iface, subclass, ep_in, ep_out, hs ? "high" : "full");
		dlog("usbcd: GET MAX LUN -> %d (byte %02x), using %u LUN(s)", r, misc_buf[0], max_lun);

		for (l = 0; l < max_lun; l++) {
			const char *why = probe_lun(l);
			if (why) {
				dlog("usbcd: using LUN %u (%s): %s", l, why, devname[0] ? devname : "unnamed device");
				usbcd_allow_removal(true);
				return 1;
			}
		}
		dlog("usbcd: %04x:%04x: no LUN looks like an optical drive, will retry", dev_vid, dev_pid);
		usbcd_close();
	}
	return 0;
}

int usbcd_test_unit_ready(void)
{
	u8 cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
	return usbcd_scsi(cdb, 6, NULL, 0, false, 5, NULL);
}

int usbcd_allow_removal(bool allow)
{
	u8 cdb[6] = { 0x1E, 0, 0, 0, allow ? 0 : 1, 0 };
	return usbcd_scsi(cdb, 6, NULL, 0, false, 3, NULL);
}

int usbcd_start_stop(bool loej, bool start)
{
	u8 cdb[6] = { 0x1B, 0, 0, 0, (loej ? 2 : 0) | (start ? 1 : 0), 0 };
	return usbcd_scsi(cdb, 6, NULL, 0, false, 20, NULL);
}

int usbcd_set_speed(u16 kbps)
{
	u8 cdb[12] = { 0xBB, 0, kbps >> 8, kbps & 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 };
	return usbcd_scsi(cdb, 12, NULL, 0, false, 5, NULL);
}

int usbcd_read_toc(cd_toc_t *toc)
{
	static u8 resp[804] ATTRIBUTE_ALIGN(32);
	u8 cdb[10] = { 0x43, 0, 0, 0, 0, 0, 0, sizeof(resp) >> 8, sizeof(resp) & 0xFF, 0 };
	u32 got = 0, n, i;
	int r;

	memset(toc, 0, sizeof(*toc));
	r = usbcd_scsi(cdb, 10, resp, sizeof(resp), true, 15, &got);
	if (r != USBCD_OK)
		return r;
	if (got < 4)
		return USBCD_EIO;

	n = (get_be16(resp) + 2 - 4) / 8;
	if (n > (got - 4) / 8)
		n = (got - 4) / 8;
	toc->first = resp[2];
	toc->last = resp[3];

	for (i = 0; i < n; i++) {
		const u8 *d = resp + 4 + i * 8;
		u8 num = d[2];
		u32 lba = get_be32(d + 4);
		if (num == 0xAA) {
			toc->leadout = lba;
		} else if (num >= 1 && num <= 99 && toc->count < CD_MAX_TRACKS) {
			cd_track_t *t = &toc->tracks[toc->count++];
			t->number = num;
			t->control = d[1] & 0x0F;
			t->lba = lba;
		}
	}

	for (i = 0; i < (u32)toc->count; i++) {
		cd_track_t *t = &toc->tracks[i];
		u32 end = (i + 1 < (u32)toc->count) ? toc->tracks[i + 1].lba : toc->leadout;
		/* CD-Extra (Blue Book): last audio track is followed by a session
		 * gap of 11400 sectors before the data track. */
		if (!TRACK_IS_DATA(t) && i + 1 < (u32)toc->count && TRACK_IS_DATA(&toc->tracks[i + 1]) &&
		    end - t->lba > 11400)
			end -= 11400;
		t->length = end > t->lba ? end - t->lba : 0;
		if (!TRACK_IS_DATA(t))
			toc->audio_count++;
	}
	return USBCD_OK;
}

static void cdtext_store(cd_text_t *txt, u8 type, int track, const char *s)
{
	char *dst = NULL;
	size_t cap = 0;

	if (track == 0) {
		if (type == 0x80) { dst = txt->album;  cap = sizeof(txt->album); }
		if (type == 0x81) { dst = txt->artist; cap = sizeof(txt->artist); }
	} else if (track >= 1 && track <= CD_MAX_TRACKS) {
		if (type == 0x80) { dst = txt->title[track - 1];     cap = sizeof(txt->title[0]); }
		if (type == 0x81) { dst = txt->performer[track - 1]; cap = sizeof(txt->performer[0]); }
	}
	if (dst && !dst[0]) {
		strncpy(dst, s, cap - 1);
		dst[cap - 1] = 0;
		txt->present = true;
	}
}

int usbcd_read_cdtext(cd_text_t *txt)
{
	static u8 buf[BOUNCE_DATA_SIZE] ATTRIBUTE_ALIGN(32);
	u16 alloc = BOUNCE_DATA_SIZE > 0xFFFF ? 0xFFFF : BOUNCE_DATA_SIZE;
	u8 cdb[10] = { 0x43, 0, 0x05, 0, 0, 0, 0, alloc >> 8, alloc & 0xFF, 0 };
	u32 got = 0, packs, p;
	int r, type;

	memset(txt, 0, sizeof(*txt));
	r = usbcd_scsi(cdb, 10, buf, alloc, true, 10, &got);
	if (r != USBCD_OK)
		return r;
	if (got < 4)
		return USBCD_EIO;

	packs = (get_be16(buf) + 2 - 4) / 18;
	if (packs > (got - 4) / 18)
		packs = (got - 4) / 18;

	for (type = 0x80; type <= 0x81; type++) {
		char acc[160];
		int alen = 0, track = -1;
		char prev[160] = "";

		for (p = 0; p < packs; p++) {
			const u8 *pk = buf + 4 + p * 18;
			int k;
			if (pk[0] != type || ((pk[3] >> 4) & 7) != 0)  /* block 0 only */
				continue;
			if (track < 0) {
				track = pk[1] & 0x7F;
				alen = 0;
			}
			for (k = 0; k < 12; k++) {
				char ch = pk[4 + k];
				if (ch == 0) {
					acc[alen] = 0;
					/* a lone TAB means "same as previous track" */
					if (!strcmp(acc, "\t"))
						strcpy(acc, prev);
					cdtext_store(txt, type, track, acc);
					strcpy(prev, acc);
					track++;
					alen = 0;
				} else if (alen < (int)sizeof(acc) - 1) {
					acc[alen++] = (ch >= 0x20 || ch == '\t') ? ch : '?';
				}
			}
		}
	}
	return USBCD_OK;
}

int usbcd_read_audio(u32 lba, u32 count, void *out)
{
	u8 cdb[12];
	u32 got = 0;
	int r;

	if (count == 0 || count > USBCD_MAX_READ_SECTORS)
		return USBCD_EINVAL;

	cdb[0] = 0xBE;               /* READ CD */
	cdb[1] = 0x04;               /* expected sector type: CD-DA */
	cdb[2] = lba >> 24; cdb[3] = lba >> 16; cdb[4] = lba >> 8; cdb[5] = lba;
	cdb[6] = count >> 16; cdb[7] = count >> 8; cdb[8] = count;
	cdb[9] = 0x10;               /* user data only (2352 bytes) */
	cdb[10] = 0;                 /* no sub-channel */
	cdb[11] = 0;

	r = usbcd_scsi(cdb, 12, out, count * CD_FRAME_BYTES, true, 15, &got);
	if (r == USBCD_OK && got != count * CD_FRAME_BYTES)
		return USBCD_EIO;
	return r;
}

const char *usbcd_sense_str(cd_sense_t s)
{
	if (s.key == 0x02 && s.asc == 0x3A && s.ascq == 0x02) return "tray open";
	if (s.key == 0x02 && s.asc == 0x3A) return "no disc";
	if (s.key == 0x02 && s.asc == 0x04) return "becoming ready";
	if (s.key == 0x06 && s.asc == 0x28) return "media changed";
	if (s.key == 0x06 && s.asc == 0x29) return "drive reset";
	if (s.key == 0x03) return "medium error";
	if (s.key == 0x04) return "hardware error";
	if (s.key == 0x05) return "illegal request";
	if (s.key == 0x02) return "not ready";
	if (s.key == 0x06) return "unit attention";
	return "error";
}
