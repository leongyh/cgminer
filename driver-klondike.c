/*
 * Copyright 2013 Andrew Smith
 * Copyright 2013 Con Kolivas
 * Copyright 2013 Chris Savery
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include <float.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
#include <math.h>

#include "config.h"

#ifdef WIN32
#include <windows.h>
#endif

#include "compat.h"
#include "miner.h"
#include "usbutils.h"

#define K1 "K1"
#define K16 "K16"
#define K64 "K64"

#define MIDSTATE_BYTES 32
#define MERKLE_OFFSET 64
#define MERKLE_BYTES 12

#define REPLY_SIZE		15	// adequate for all types of replies
#define MAX_KLINES		1024	// unhandled reply limit
#define REPLY_WAIT_TIME		100 	// poll interval for a cmd waiting it's reply
#define CMD_REPLY_RETRIES	8	// how many retries for cmds
#define MAX_WORK_COUNT		4	// for now, must be binary multiple and match firmware
#define TACH_FACTOR		87890	// fan rpm divisor

struct device_drv klondike_drv;

typedef struct klondike_header {
	uint8_t cmd;
	uint8_t dev;
	uint8_t buf[REPLY_SIZE-2];
} HEADER;

#define K_2(_bytes) ((int)(_bytes[0]) + \
			((int)(_bytes[1]) << 8))

#define K_4(_bytes) ((uint64_t)(_bytes[0]) + \
			((uint64_t)(_bytes[1]) << 8) + \
			((uint64_t)(_bytes[2]) << 16) + \
			((uint64_t)(_bytes[3]) << 24))

#define K_SERIAL(_serial) K_4(_serial)
#define K_HASHCOUNT(_hashcount) K_2(_hashcount)
#define K_MAXCOUNT(_maxcount) K_2(_maxcount)
#define K_NONCE(_nonce) K_4(_nonce)
#define K_HASHCLOCK(_hashclock) K_2(_hashclock)

#define SET_HASHCLOCK(_hashclock, _value) do { \
						(_hashclock)[0] = (uint8_t)((_value) & 0xff); \
						(_hashclock)[1] = (uint8_t)(((_value) >> 8) & 0xff); \
					  } while(0)

#define KSENDHD(_add) (sizeof(char) + sizeof(uint8_t) + _add)

typedef struct klondike_id {
	uint8_t cmd;
	uint8_t dev;
	uint8_t version;
	uint8_t product[7];
	uint8_t serial[4];
} IDENTITY;

typedef struct klondike_status {
	uint8_t cmd;
	uint8_t dev;
	uint8_t state;
	uint8_t chipcount;
	uint8_t slavecount;
	uint8_t workqc;
	uint8_t workid;
	uint8_t temp;
	uint8_t fanspeed;
	uint8_t errorcount;
	uint8_t hashcount[2];
	uint8_t maxcount[2];
	uint8_t noise;
} WORKSTATUS;

typedef struct _worktask {
	uint8_t cmd;
	uint8_t dev;
	uint8_t workid;
	uint8_t midstate[32];
	uint8_t merkle[12];
} WORKTASK;

typedef struct _workresult {
	uint8_t cmd;
	uint8_t dev;
	uint8_t workid;
	uint8_t nonce[4];
} WORKRESULT;

typedef struct klondike_cfg {
	uint8_t cmd;
	uint8_t dev;
	uint8_t hashclock[2];
	uint8_t temptarget;
	uint8_t tempcritical;
	uint8_t fantarget;
	uint8_t pad2;
} WORKCFG;

typedef struct kline {
	union {
		HEADER hd;
		IDENTITY id;
		WORKSTATUS ws;
		WORKTASK wt;
		WORKRESULT wr;
		WORKCFG cfg;
	};
} KLINE;

typedef struct device_info {
	uint32_t noncecount;
	uint32_t nextworkid;
	uint16_t lasthashcount;
	uint64_t totalhashcount;
	uint32_t rangesize;
	uint32_t *chipstats;
} DEVINFO;

typedef struct klist {
	struct klist *prev;
	struct klist *next;
	KLINE kline;
	struct timeval tv_when;
	int block_seq;
	bool ready;
	bool working;
} KLIST;

struct klondike_info {
	bool shutdown;
	pthread_rwlock_t stat_lock;
	struct thr_info replies_thr;
	cglock_t klist_lock;
	KLIST *used;
	KLIST *free;
	int kline_count;
	int used_count;
	int block_seq;
	KLIST *status;
	DEVINFO *devinfo;
	KLIST *cfg;
	int noncecount;
	uint64_t hashcount;
	uint64_t errorcount;
	uint64_t noisecount;

	// us Delay from USB reply to being processed
	double delay_count;
	double delay_total;
	double delay_min;
	double delay_max;

	struct timeval tv_last_nonce_received;

	// Time from recieving one nonce to the next
	double nonce_count;
	double nonce_total;
	double nonce_min;
	double nonce_max;
};

static KLIST *new_klist_set(struct cgpu_info *klncgpu)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	KLIST *klist = NULL;
	int i;

	klist = calloc(MAX_KLINES, sizeof(*klist));
	if (!klist)
		quit(1, "Failed to calloc klist - when old count=%d", klninfo->kline_count);

	klninfo->kline_count += MAX_KLINES;

	klist[0].prev = NULL;
	klist[0].next = &(klist[1]);
	for (i = 1; i < MAX_KLINES-1; i++) {
		klist[i].prev = &klist[i-1];
		klist[i].next = &klist[i+1];
	}
	klist[MAX_KLINES-1].prev = &(klist[MAX_KLINES-2]);
	klist[MAX_KLINES-1].next = NULL;

	return klist;
}

static KLIST *allocate_kitem(struct cgpu_info *klncgpu)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	KLIST *kitem = NULL;
	int ran_out = 0;
	char errbuf[1024];

	cg_wlock(&klninfo->klist_lock);

	if (klninfo->free == NULL) {
		ran_out = klninfo->kline_count;
		klninfo->free = new_klist_set(klncgpu);
		snprintf(errbuf, sizeof(errbuf),
				 "%s%i: KLINE count exceeded %d, now %d",
				 klncgpu->drv->name, klncgpu->device_id,
				 ran_out, klninfo->kline_count);
	}

	kitem = klninfo->free;

	klninfo->free = klninfo->free->next;
	if (klninfo->free)
		klninfo->free->prev = NULL;

	kitem->next = klninfo->used;
	kitem->prev = NULL;
	if (kitem->next)
		kitem->next->prev = kitem;
	klninfo->used = kitem;

	kitem->ready = false;
	kitem->working = false;

	memset((void *)&(kitem->kline), 0, sizeof(kitem->kline));

	klninfo->used_count++;

	cg_wunlock(&klninfo->klist_lock);

	if (ran_out > 0)
		applog(LOG_ERR, "%s", errbuf);

	return kitem;
}

static void release_kitem(struct cgpu_info *klncgpu, KLIST *kitem)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);

	cg_wlock(&klninfo->klist_lock);

	if (kitem == klninfo->used)
		klninfo->used = kitem->next;

	if (kitem->next)
		kitem->next->prev = kitem->prev;
	if (kitem->prev)
		kitem->prev->next = kitem->next;

	kitem->next = klninfo->free;
	if (klninfo->free)
		klninfo->free->prev = kitem;

	kitem->prev = NULL;

	klninfo->free = kitem;

	klninfo->used_count--;

	cg_wunlock(&klninfo->klist_lock);
}

static double cvtKlnToC(uint8_t temp)
{
	double Rt, stein, celsius;

	if (temp == 0)
		return 0.0;

	Rt = 1000.0 * 255.0 / (double)temp - 1000.0;

	stein = log(Rt / 2200.0) / 3987.0;

	stein += 1.0 / (double)(25.0 + 273.15);

	celsius = (1.0 / stein) - 273.15;

	// For display of bad data
	if (celsius < 0.0)
		celsius = 0.0;
	if (celsius > 200.0)
		celsius = 200.0;

	return celsius;
}

static int cvtCToKln(double deg)
{
	double Rt, stein, temp;

	if (deg < 0.0)
		deg = 0.0;

	stein = 1.0 / (deg + 273.15);

	stein -= 1.0 / (double)(25.0 + 273.15);

	Rt = exp(stein * 3987.0) * 2200.0;

	if (Rt == -1000.0)
		Rt++;

	temp = 1000.0 * 256.0 / (Rt + 1000.0);

	if (temp > 255)
		temp = 255;
	if (temp < 0)
		temp = 0;

	return (int)temp;
}

// Change this to LOG_WARNING if you wish to always see the replies
#define READ_DEBUG LOG_DEBUG
//#define READ_DEBUG LOG_ERR

static void display_kline(struct cgpu_info *klncgpu, KLINE *kline)
{
	char *hexdata;

	switch (kline->hd.cmd) {
		case '=':
			applog(READ_DEBUG,
				"%s (%s) work [%c] dev=%d workid=%d"
				" nonce=0x%08x",
				klncgpu->drv->dname, klncgpu->device_path,
				kline->wr.cmd,
				(int)(kline->wr.dev),
				(int)(kline->wr.workid),
				(unsigned int)K_NONCE(kline->wr.nonce));
			break;
		case 'S':
		case 'W':
		case 'A':
		case 'E':
			applog(READ_DEBUG,
				"%s (%s) status [%c] dev=%d chips=%d"
				" slaves=%d workcq=%d workid=%d temp=%d fan=%d"
				" errors=%d hashes=%d max=%d noise=%d",
				klncgpu->drv->dname, klncgpu->device_path,
				kline->ws.cmd,
				(int)(kline->ws.dev),
				(int)(kline->ws.chipcount),
				(int)(kline->ws.slavecount),
				(int)(kline->ws.workqc),
				(int)(kline->ws.workid),
				(int)(kline->ws.temp),
				(int)(kline->ws.fanspeed),
				(int)(kline->ws.errorcount),
				K_HASHCOUNT(kline->ws.hashcount),
				K_MAXCOUNT(kline->ws.maxcount),
				(int)(kline->ws.noise));
			break;
		case 'C':
			applog(READ_DEBUG,
				"%s (%s) config [%c] dev=%d clock=%d"
				" temptarget=%d tempcrit=%d fan=%d",
				klncgpu->drv->dname, klncgpu->device_path,
				kline->cfg.cmd,
				(int)(kline->cfg.dev),
				K_HASHCLOCK(kline->cfg.hashclock),
				(int)(kline->cfg.temptarget),
				(int)(kline->cfg.tempcritical),
				(int)(kline->cfg.fantarget));
			break;
		case 'I':
			applog(READ_DEBUG,
				"%s (%s) info [%c] version=0x%02x prod=%.7s"
				" serial=0x%08x",
				klncgpu->drv->dname, klncgpu->device_path,
				kline->hd.cmd,
				(int)(kline->id.version),
				kline->id.product,
				(unsigned int)K_SERIAL(kline->id.serial));
			break;
		default:
			hexdata = bin2hex((unsigned char *)&(kline->hd.dev), REPLY_SIZE - 1);
			applog(LOG_ERR,
				"%s (%s) [%c:%s] unknown and ignored",
				klncgpu->drv->dname, klncgpu->device_path,
				kline->hd.cmd, hexdata);
			free(hexdata);
			break;
	}
}

static KLIST *SendCmdGetReply(struct cgpu_info *klncgpu, KLINE *kline, int datalen)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	KLIST *kitem;
	int retries = CMD_REPLY_RETRIES;
	int err, amt, writ;

	if (klncgpu->usbinfo.nodev)
		return NULL;

	writ = KSENDHD(datalen);
	err = usb_write(klncgpu, (char *)kline, writ, &amt, C_REQUESTRESULTS);
	if (err < 0 || amt != writ) {
		applog(LOG_ERR, "%s (%s) Cmd:%c Dev:%d, write failed (%d:%d:%d)",
				klncgpu->drv->dname, klncgpu->device_path,
				kline->hd.cmd, (int)kline->hd.dev,
				writ, amt, err);
	}

	while (retries-- > 0 && klninfo->shutdown == false) {
		cgsleep_ms(REPLY_WAIT_TIME);
		cg_rlock(&klninfo->klist_lock);
		kitem = klninfo->used;
		while (kitem) {
			if (kitem->kline.hd.cmd == kline->hd.cmd &&
			    kitem->kline.hd.dev == kline->hd.dev &&
			    kitem->ready == true && kitem->working == false) {
				kitem->working = true;
				cg_runlock(&klninfo->klist_lock);
				return kitem;
			}
			kitem = kitem->next;
		}
		cg_runlock(&klninfo->klist_lock);
	}
	return NULL;
}

static bool klondike_get_stats(struct cgpu_info *klncgpu)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	KLIST *kitem;
	KLINE kline;
	int slaves, dev;

	if (klncgpu->usbinfo.nodev || klninfo->status == NULL)
		return false;

	applog(LOG_DEBUG, "Klondike getting status");

	slaves = klninfo->status[0].kline.ws.slavecount;

	// loop thru devices and get status for each
	for (dev = 0; dev <= slaves; dev++) {
		kline.hd.cmd = 'S';
		kline.hd.dev = dev;
		kitem = SendCmdGetReply(klncgpu, &kline, 0);
		if (kitem != NULL) {
			wr_lock(&(klninfo->stat_lock));
			memcpy((void *)(&(klninfo->status[dev])), (void *)kitem, sizeof(*kitem));
			wr_unlock(&(klninfo->stat_lock));
			release_kitem(klncgpu, kitem);
			kitem = NULL;
		}
	}

	// todo: detect slavecount change and realloc space

	return true;
}

static bool klondike_init(struct cgpu_info *klncgpu)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	KLIST *kitem;
	KLINE kline;
	int slaves, dev;

	kline.hd.cmd = 'S';
	kline.hd.dev = 0;
	kitem = SendCmdGetReply(klncgpu, &kline, 0);
	if (kitem == NULL)
		return false;

	slaves = kitem->kline.ws.slavecount;
	release_kitem(klncgpu, kitem);
	kitem = NULL;
	if (klninfo->status == NULL) {
		applog(LOG_DEBUG, "Klondike initializing data");

		// alloc space for status, devinfo and cfg for master and slaves
		klninfo->status = calloc(slaves+1, sizeof(KLIST));
		if (unlikely(!klninfo->status))
			quit(1, "Failed to calloc status array in klondke_get_stats");
		klninfo->devinfo = calloc(slaves+1, sizeof(DEVINFO));
		if (unlikely(!klninfo->devinfo))
			quit(1, "Failed to calloc devinfo array in klondke_get_stats");
		klninfo->cfg = calloc(slaves+1, sizeof(KLIST));
		if (unlikely(!klninfo->cfg))
			quit(1, "Failed to calloc cfg array in klondke_get_stats");
	}

	// zero init triggers read back only
	memset(&(kline.cfg), 0, sizeof(kline.cfg));
	kline.cfg.cmd = 'C';

	int size = 2;

	// boundaries are checked by device, with valid values returned
	if (opt_klondike_options != NULL) {
		int hashclock;
		double temp1, temp2;

		sscanf(opt_klondike_options, "%d:%lf:%lf:%"SCNu8,
						&hashclock,
						&temp1, &temp2,
						&kline.cfg.fantarget);
		SET_HASHCLOCK(kline.cfg.hashclock, hashclock);
		kline.cfg.temptarget = cvtCToKln(temp1);
		kline.cfg.tempcritical = cvtCToKln(temp2);
		kline.cfg.fantarget = (int)255*kline.cfg.fantarget/100;
		size = sizeof(kline.cfg) - 2;
	}

	for (dev = 0; dev <= slaves; dev++) {
		kline.cfg.dev = dev;
		kitem = SendCmdGetReply(klncgpu, &kline, size);
		if (kitem != NULL) {
			memcpy((void *)&(klninfo->cfg[dev]), kitem, sizeof(*kitem));
			applog(LOG_WARNING, "Klondike config (%d: Clk: %d, T:%.0lf, C:%.0lf, F:%d)",
				dev, K_HASHCLOCK(klninfo->cfg[dev].kline.cfg.hashclock),
				cvtKlnToC(klninfo->cfg[dev].kline.cfg.temptarget),
				cvtKlnToC(klninfo->cfg[dev].kline.cfg.tempcritical),
				(int)100*klninfo->cfg[dev].kline.cfg.fantarget/256);
			release_kitem(klncgpu, kitem);
			kitem = NULL;
		}
	}
	klondike_get_stats(klncgpu);
	for (dev = 0; dev <= slaves; dev++) {
		klninfo->devinfo[dev].rangesize = ((uint64_t)1<<32) / klninfo->status[dev].kline.ws.chipcount;
		klninfo->devinfo[dev].chipstats = calloc(klninfo->status[dev].kline.ws.chipcount*2 , sizeof(uint32_t));
	}

	int tries = 2;
	bool ok = false;

	kline.hd.cmd = 'E';
	kline.hd.dev = 0;
	kline.hd.buf[0] = '1';
	
	while (tries-- > 0) {
		kitem = SendCmdGetReply(klncgpu, &kline, 1);
		if (kitem) {
			release_kitem(klncgpu, kitem);
			kitem = NULL;
			ok = true;
			break;
		}
	}

	if (!ok)
		applog(LOG_ERR, "%s%i: failed to enable", klncgpu->drv->name, klncgpu->device_id);

	return ok;
}

static bool klondike_detect_one(struct libusb_device *dev, struct usb_find_devices *found)
{
	struct cgpu_info *klncgpu = usb_alloc_cgpu(&klondike_drv, 1);
	struct klondike_info *klninfo = NULL;

	if (unlikely(!klncgpu))
		quit(1, "Failed to calloc klncgpu in klondike_detect_one");

	klninfo = calloc(1, sizeof(*klninfo));
	if (unlikely(!klninfo))
		quit(1, "Failed to calloc klninfo in klondke_detect_one");
	klncgpu->device_data = (void *)klninfo;

	klninfo->free = new_klist_set(klncgpu);

	if (usb_init(klncgpu, dev, found)) {
		int sent, recd, err;
		KLIST kitem;
		int attempts = 0;

		while (attempts++ < 3) {
			err = usb_write(klncgpu, "I", 2, &sent, C_REQUESTRESULTS);
			if (err < 0 || sent != 2) {
				applog(LOG_ERR, "%s (%s) detect write failed (%d:%d)",
						klncgpu->drv->dname,
						klncgpu->device_path,
						sent, err);
			}
			cgsleep_ms(REPLY_WAIT_TIME*10);
			err = usb_read(klncgpu, (char *)&(kitem.kline), REPLY_SIZE, &recd, C_GETRESULTS);
			if (err < 0) {
				applog(LOG_ERR, "%s (%s) detect read failed (%d:%d)",
						klncgpu->drv->dname,
						klncgpu->device_path,
						recd, err);
			} else if (recd < 1) {
				applog(LOG_ERR, "%s (%s) detect empty reply (%d)",
						klncgpu->drv->dname,
						klncgpu->device_path,
						recd);
			} else if (kitem.kline.hd.cmd == 'I' && kitem.kline.hd.dev == 0) {
display_kline(klncgpu, &kitem.kline);
				applog(LOG_DEBUG, "%s (%s) detect successful",
						  klncgpu->drv->dname,
						  klncgpu->device_path);
				if (!add_cgpu(klncgpu))
					break;
				update_usb_stats(klncgpu);
				applog(LOG_DEBUG, "Klondike cgpu added");
				cglock_init(&klninfo->klist_lock);
				return true;
			}
		}
		usb_uninit(klncgpu);
	}
	free(klninfo->free);
	free(klninfo);
	free(klncgpu);
	return false;
}

static void klondike_detect(bool __maybe_unused hotplug)
{
	usb_detect(&klondike_drv, klondike_detect_one);
}

static void klondike_identify(__maybe_unused struct cgpu_info *klncgpu)
{
/*
	KLINE kline;

	kline.hd.cmd = 'I';
	kline.hd.dev = 0;
	SendCmdGetReply(klncgpu, &kline, KSENDHD(0));
*/
}

static void klondike_check_nonce(struct cgpu_info *klncgpu, KLIST *kitem)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	struct work *work, *tmp;
	KLINE *kline = &(kitem->kline);
	struct timeval tv_now;
	double us_diff;
	uint32_t nonce = K_NONCE(kline->wr.nonce) - 0xC0;

	applog(LOG_DEBUG, "Klondike FOUND NONCE (%02x:%08x)",
			  kline->wr.workid, (unsigned int)nonce);

	HASH_ITER(hh, klncgpu->queued_work, work, tmp) {
		if (work->queued && (work->subid == (kline->wr.dev*256 + kline->wr.workid))) {

			wr_lock(&(klninfo->stat_lock));
			klninfo->devinfo[kline->wr.dev].noncecount++;
			klninfo->noncecount++;
			wr_unlock(&(klninfo->stat_lock));

//			kline->wr.nonce = le32toh(kline->wr.nonce - 0xC0);
			applog(LOG_DEBUG, "Klondike SUBMIT NONCE (%02x:%08x)",
					  kline->wr.workid, (unsigned int)nonce);

			cgtime(&tv_now);
			bool ok = submit_nonce(klncgpu->thr[0], work, nonce);

			applog(LOG_DEBUG, "Klondike chip stats %d, %08x, %d, %d",
					  kline->wr.dev, (unsigned int)nonce,
					  klninfo->devinfo[kline->wr.dev].rangesize,
					  klninfo->status[kline->wr.dev].kline.ws.chipcount);

			klninfo->devinfo[kline->wr.dev].chipstats[(nonce / klninfo->devinfo[kline->wr.dev].rangesize) + (ok ? 0 : klninfo->status[kline->wr.dev].kline.ws.chipcount)]++;

			us_diff = us_tdiff(&tv_now, &(kitem->tv_when));
			if (klninfo->delay_count == 0) {
				klninfo->delay_min = us_diff;
				klninfo->delay_max = us_diff;
			} else {
				if (klninfo->delay_min > us_diff)
					klninfo->delay_min = us_diff;
				if (klninfo->delay_max < us_diff)
					klninfo->delay_max = us_diff;
			}
			klninfo->delay_count++;
			klninfo->delay_total += us_diff;

			if (klninfo->nonce_count > 0) {
				us_diff = us_tdiff(&(kitem->tv_when), &(klninfo->tv_last_nonce_received));
				if (klninfo->nonce_count == 1) {
					klninfo->nonce_min = us_diff;
					klninfo->nonce_max = us_diff;
				} else {
					if (klninfo->nonce_min > us_diff)
						klninfo->nonce_min = us_diff;
					if (klninfo->nonce_max < us_diff)
						klninfo->nonce_max = us_diff;
				}
				klninfo->nonce_total += us_diff;
			}
			klninfo->nonce_count++;

			memcpy(&(klninfo->tv_last_nonce_received), &(kitem->tv_when),
				sizeof(klninfo->tv_last_nonce_received));

			return;
		}
	}

	applog(LOG_ERR, "%s%i:%d unknown work (%02x:%08x) - ignored",
			klncgpu->drv->name, klncgpu->device_id,
			kline->wr.dev, kline->wr.workid, (unsigned int)nonce);

	//inc_hw_errors(klncgpu->thr[0]);
}

// thread to keep looking for replies
static void *klondike_get_replies(void *userdata)
{
	struct cgpu_info *klncgpu = (struct cgpu_info *)userdata;
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	KLIST *kitem = NULL;
	char *hexdata;
	int err, recd;

	applog(LOG_ERR, "Klondike listening for replies");

	while (klninfo->shutdown == false) {
		if (klncgpu->usbinfo.nodev)
			return NULL;

		if (kitem == NULL)
			kitem = allocate_kitem(klncgpu);
		else
			memset((void *)&(kitem->kline), 0, sizeof(kitem->kline));

		err = usb_read(klncgpu, (char *)&(kitem->kline), REPLY_SIZE, &recd, C_GETRESULTS);
		if (!err && recd == REPLY_SIZE) {
			cgtime(&(kitem->tv_when));
			kitem->block_seq = klninfo->block_seq;
			if (opt_log_level <= READ_DEBUG) {
				hexdata = bin2hex((unsigned char *)&(kitem->kline.hd.dev), recd-1);
				applog(READ_DEBUG, "%s (%s) reply [%c:%s]",
					klncgpu->drv->dname, klncgpu->device_path,
					kitem->kline.hd.cmd, hexdata);
				free(hexdata);
			}

			switch (kitem->kline.hd.cmd) {
				case '=':
					klondike_check_nonce(klncgpu, kitem);
					display_kline(klncgpu, &kitem->kline);
					break;
				case 'S':
				case 'W':
				case 'A':
				case 'E':
					wr_lock(&(klninfo->stat_lock));
					klninfo->errorcount += kitem->kline.ws.errorcount;
					klninfo->noisecount += kitem->kline.ws.noise;
					wr_unlock(&(klninfo->stat_lock));
					display_kline(klncgpu, &kitem->kline);
					kitem->ready = true;
					kitem = NULL;
					break;
				case 'C':
					display_kline(klncgpu, &kitem->kline);
					kitem->ready = true;
					kitem = NULL;
					break;
				case 'I':
					display_kline(klncgpu, &kitem->kline);
					kitem->ready = true;
					kitem = NULL;
					break;
				default:
					display_kline(klncgpu, &kitem->kline);
					break;
			}
		}
	}
	return NULL;
}

static void klondike_flush_work(struct cgpu_info *klncgpu)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	KLIST *kitem;
	KLINE kline;
	int slaves, dev;

	klninfo->block_seq++;

	applog(LOG_DEBUG, "Klondike flushing work");
	slaves = klninfo->status[0].kline.ws.slavecount;
	kline.hd.cmd = 'A';
	for (dev = 0; dev <= slaves; dev++) {
		kline.hd.dev = dev;
		kitem = SendCmdGetReply(klncgpu, &kline, KSENDHD(0));
		if (kitem != NULL) {
			wr_lock(&(klninfo->stat_lock));
			memcpy((void *)&(klninfo->status[dev]), kitem, sizeof(*kitem));
			wr_unlock(&(klninfo->stat_lock));
			release_kitem(klncgpu, kitem);
			kitem = NULL;
		}
	}
}

static bool klondike_thread_prepare(struct thr_info *thr)
{
	struct cgpu_info *klncgpu = thr->cgpu;
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);

	if (thr_info_create(&(klninfo->replies_thr), NULL, klondike_get_replies, (void *)klncgpu)) {
		applog(LOG_ERR, "%s%i: thread create failed", klncgpu->drv->name, klncgpu->device_id);
		return false;
	}
	pthread_detach(klninfo->replies_thr.pth);

	// let the listening get started
	cgsleep_ms(100);

	return klondike_init(klncgpu);
}

static bool klondike_thread_init(struct thr_info *thr)
{
	struct cgpu_info *klncgpu = thr->cgpu;

	if (klncgpu->usbinfo.nodev)
		return false;

	klondike_flush_work(klncgpu);

	return true;
}

static void klondike_shutdown(struct thr_info *thr)
{
	struct cgpu_info *klncgpu = thr->cgpu;
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	KLIST *kitem;
	KLINE kline;
	int dev;

	applog(LOG_DEBUG, "Klondike shutting down work");
	kline.hd.cmd = 'E';
	for (dev = 0; dev <= klninfo->status[0].kline.ws.slavecount; dev++) {
		kline.hd.dev = dev;
		kline.hd.buf[0] = '0';
		kitem = SendCmdGetReply(klncgpu, &kline, KSENDHD(1));
		if (kitem)
			release_kitem(klncgpu, kitem);
	}
	klncgpu->shutdown = klninfo->shutdown = true;
}

static void klondike_thread_enable(struct thr_info *thr)
{
	struct cgpu_info *klncgpu = thr->cgpu;

	if (klncgpu->usbinfo.nodev)
		return;

/*
	KLINE kline;

	kline.hd.cmd = 'E';
	kline.hd.dev = dev;
	kline.hd.buf[0] = '0';
	kitem = SendCmdGetReply(klncgpu, &kline, KSENDHD(1));
*/

}

static bool klondike_send_work(struct cgpu_info *klncgpu, int dev, struct work *work)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	struct work *tmp;
	KLINE kline;

	if (klncgpu->usbinfo.nodev)
		return false;

	kline.wt.cmd = 'W';
	kline.wt.dev = dev;
	memcpy(kline.wt.midstate, work->midstate, MIDSTATE_BYTES);
	memcpy(kline.wt.merkle, work->data + MERKLE_OFFSET, MERKLE_BYTES);
	kline.wt.workid = (uint8_t)(klninfo->devinfo[dev].nextworkid++ & 0xFF);
	work->subid = dev*256 + kline.wt.workid;

	if (opt_log_level <= LOG_DEBUG) {
		char *hexdata = bin2hex((void *)&kline.wt, sizeof(kline.wt));
		applog(LOG_DEBUG, "WORKDATA: %s", hexdata);
		free(hexdata);
	}

	applog(LOG_DEBUG, "Klondike sending work (%d:%02x)", dev, kline.wt.workid);
	KLIST *kitem = SendCmdGetReply(klncgpu, &kline, sizeof(kline.wt));
	if (kitem != NULL) {
		wr_lock(&(klninfo->stat_lock));
		memcpy((void *)&(klninfo->status[dev]), kitem, sizeof(*kitem));
		wr_unlock(&(klninfo->stat_lock));
		release_kitem(klncgpu, kitem);
		kitem = NULL;

		// remove old work
		HASH_ITER(hh, klncgpu->queued_work, work, tmp) {
		if (work->queued && (work->subid == (int)(dev*256 + ((klninfo->devinfo[dev].nextworkid-2*MAX_WORK_COUNT) & 0xFF))))
			work_completed(klncgpu, work);
		}
		return true;
	}
	return false;
}

static bool klondike_queue_full(struct cgpu_info *klncgpu)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	struct work *work = NULL;
	int dev, queued, slaves;

	slaves = klninfo->status[0].kline.ws.slavecount;
	for (queued = 0; queued < MAX_WORK_COUNT-1; queued++)
		for (dev = 0; dev <= slaves; dev++)
			if (klninfo->status[dev].kline.ws.workqc <= queued) {
				if (!work)
					work = get_queued(klncgpu);
				if (unlikely(!work))
					return false;
				if (klondike_send_work(klncgpu, dev, work)) {
					work = NULL;
					break;
				}
			}

	return true;
}

static int64_t klondike_scanwork(struct thr_info *thr)
{
	struct cgpu_info *klncgpu = thr->cgpu;
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	int64_t newhashcount = 0;
	int dev, slaves;

	if (klncgpu->usbinfo.nodev)
		return -1;

	restart_wait(thr, 200);
	if (klninfo->status != NULL) {
		rd_lock(&(klninfo->stat_lock));
		slaves = klninfo->status[0].kline.ws.slavecount;
		for (dev = 0; dev <= slaves; dev++) {
			uint64_t newhashdev = 0, hashcount;
			int maxcount;

			hashcount = K_HASHCOUNT(klninfo->status[dev].kline.ws.hashcount);
			maxcount = K_MAXCOUNT(klninfo->status[dev].kline.ws.maxcount);
			if (klninfo->devinfo[dev].lasthashcount > hashcount) // todo: chg this to check workid for wrapped instead
				newhashdev += maxcount; // hash counter wrapped
			newhashdev += hashcount - klninfo->devinfo[dev].lasthashcount;
			klninfo->devinfo[dev].lasthashcount = hashcount;
			if (maxcount != 0)
				klninfo->hashcount += (newhashdev << 32) / maxcount;

			// todo: check stats for critical conditions
		}
		newhashcount += 0xffffffffull * (uint64_t)klninfo->noncecount;
		klninfo->noncecount = 0;
		rd_unlock(&(klninfo->stat_lock));
	}
	return newhashcount;
}


static void get_klondike_statline_before(char *buf, size_t siz, struct cgpu_info *klncgpu)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	uint8_t temp = 0xFF;
	uint16_t fan = 0;
	uint16_t clock = 0;
	int dev, slaves;
	char tmp[16];

	if (klninfo->status == NULL) {
		blank_get_statline_before(buf, siz, klncgpu);
		return;
	}

	rd_lock(&(klninfo->stat_lock));
	slaves = klninfo->status[0].kline.ws.slavecount;
	for (dev = 0; dev <= slaves; dev++) {
		if (klninfo->status[dev].kline.ws.temp < temp)
			temp = klninfo->status[dev].kline.ws.temp;
		fan += klninfo->cfg[dev].kline.cfg.fantarget;
		clock += (uint16_t)K_HASHCLOCK(klninfo->cfg[dev].kline.cfg.hashclock);
	}
	fan /= slaves + 1;
	clock /= slaves + 1;
	rd_unlock(&(klninfo->stat_lock));

	snprintf(tmp, sizeof(tmp), "%2.0fC", cvtKlnToC(temp));
	if (strlen(tmp) < 4)
		strcat(tmp, " ");

	tailsprintf(buf, siz, "%3dMHz %3d%% %s| ", (int)clock, fan*100/255, tmp);
}

static struct api_data *klondike_api_stats(struct cgpu_info *klncgpu)
{
	struct klondike_info *klninfo = (struct klondike_info *)(klncgpu->device_data);
	struct api_data *root = NULL;
	char buf[32];
	int dev, slaves;

	if (klninfo->status == NULL)
		return NULL;

	rd_lock(&(klninfo->stat_lock));
	slaves = klninfo->status[0].kline.ws.slavecount;
	for (dev = 0; dev <= slaves; dev++) {

		float fTemp = cvtKlnToC(klninfo->status[dev].kline.ws.temp);
		sprintf(buf, "Temp %d", dev);
		root = api_add_temp(root, buf, &fTemp, true);

		double dClk = (double)K_HASHCLOCK(klninfo->cfg[dev].kline.cfg.hashclock);
		sprintf(buf, "Clock %d", dev);
		root = api_add_freq(root, buf, &dClk, true);

		unsigned int iFan = (unsigned int)100 * klninfo->cfg[dev].kline.cfg.fantarget / 255;
		sprintf(buf, "Fan Percent %d", dev);
		root = api_add_int(root, buf, (int *)(&iFan), true);

		iFan = 0;
		if (klninfo->status[dev].kline.ws.fanspeed > 0)
			iFan = (unsigned int)TACH_FACTOR / klninfo->status[dev].kline.ws.fanspeed;
		sprintf(buf, "Fan RPM %d", dev);
		root = api_add_int(root, buf, (int *)(&iFan), true);

		if (klninfo->devinfo[dev].chipstats != NULL) {
			char data[2048];
			char one[32];
			int n;

			sprintf(buf, "Nonces / Chip %d", dev);
			data[0] = '\0';
			for (n = 0; n < klninfo->status[dev].kline.ws.chipcount; n++) {
				snprintf(one, sizeof(one), "%07d ", klninfo->devinfo[dev].chipstats[n]);
				strcat(data, one);
			}
			root = api_add_string(root, buf, data, true);

			sprintf(buf, "Errors / Chip %d", dev);
			data[0] = '\0';
			for (n = 0; n < klninfo->status[dev].kline.ws.chipcount; n++) {
				snprintf(one, sizeof(one), "%07d ", klninfo->devinfo[dev].chipstats[n + klninfo->status[dev].kline.ws.chipcount]);
				strcat(data, one);
			}
			root = api_add_string(root, buf, data, true);
		}
	}

	root = api_add_uint64(root, "Hash Count", &(klninfo->hashcount), true);
	root = api_add_uint64(root, "Error Count", &(klninfo->errorcount), true);
	root = api_add_uint64(root, "Noise Count", &(klninfo->noisecount), true);

	root = api_add_int(root, "KLine Limit", &(klninfo->kline_count), true);
	root = api_add_int(root, "KLine Used", &(klninfo->used_count), true);

	root = api_add_elapsed(root, "KQue Delay Count", &(klninfo->delay_count), true);
	root = api_add_elapsed(root, "KQue Delay Total", &(klninfo->delay_total), true);
	root = api_add_elapsed(root, "KQue Delay Min", &(klninfo->delay_min), true);
	root = api_add_elapsed(root, "KQue Delay Max", &(klninfo->delay_max), true);
	double avg;
	if (klninfo->delay_count == 0)
		avg = 0;
	else
		avg = klninfo->delay_total / klninfo->delay_count;
	root = api_add_diff(root, "KQue Delay Avg", &avg, true);

	root = api_add_elapsed(root, "KQue Nonce Count", &(klninfo->nonce_count), true);
	root = api_add_elapsed(root, "KQue Nonce Total", &(klninfo->nonce_total), true);
	root = api_add_elapsed(root, "KQue Nonce Min", &(klninfo->nonce_min), true);
	root = api_add_elapsed(root, "KQue Nonce Max", &(klninfo->nonce_max), true);
	if (klninfo->nonce_count == 0)
		avg = 0;
	else
		avg = klninfo->nonce_total / klninfo->nonce_count;
	root = api_add_diff(root, "KQue Nonce Avg", &avg, true);

	rd_unlock(&(klninfo->stat_lock));

	return root;
}

struct device_drv klondike_drv = {
	.drv_id = DRIVER_klondike,
	.dname = "Klondike",
	.name = "KLN",
	.drv_detect = klondike_detect,
	.get_api_stats = klondike_api_stats,
	.get_statline_before = get_klondike_statline_before,
	.get_stats = klondike_get_stats,
	.identify_device = klondike_identify,
	.thread_prepare = klondike_thread_prepare,
	.thread_init = klondike_thread_init,
	.hash_work = hash_queued_work,
	.scanwork = klondike_scanwork,
	.queue_full = klondike_queue_full,
	.flush_work = klondike_flush_work,
	.thread_shutdown = klondike_shutdown,
	.thread_enable = klondike_thread_enable
};
