// SPDX-License-Identifier: CDDL-1.0
/*
 * hddsim: a ublk server that presents a memory-backed block device with
 * the service times of a 7200 rpm SATA hard disk, for ZFS measurements on
 * machines without spare disks.  Test rig only; never point it at data.
 *
 *   hddsim run ID SIZE_MIB PARAMS STATS   serve /dev/ublkbID until SIGTERM
 *   hddsim del ID                         stop and delete a left-over ID
 *   hddsim selftest PARAMS [SECONDS]      the model alone, in virtual time
 *   hddsim print PARAMS                   the parameters as a server logs them
 *
 * PARAMS is a file of key=value lines, read again on SIGHUP (see
 * params_set() for the keys).  STATS gets one line per second.
 *
 * The model (struct hdd):
 *  - one actuator: one media operation at a time, costing a command
 *    overhead, a positioning time (none for a forward gap below seq_kb,
 *    else seek(distance) plus a uniform rotational latency) and a transfer
 *    at a zoned media rate;
 *  - an NCQ of `ncq` commands; the media serves the queued command with the
 *    shortest estimated positioning time, unless the oldest has waited
 *    `age_ms`;
 *  - a volatile write cache of `cache_mb`: a write completes once it has
 *    crossed the link, and is destaged in elevator order when the media is
 *    idle for `idle_ms`, or at once above `cache_hi` of the cache;
 *  - FLUSH as on SATA (flush_blocks=1): a non-queued command, so it waits
 *    for the NCQ to drain, holds off every later command, and completes
 *    once the whole cache is on the media.
 * Data are kept per 4 KiB page: a page that repeats its first 8 bytes is
 * kept as those 8 bytes, any other page in full.  Pools on hddsim must use
 * compression=off and write such patterned data, or memory runs out.
 */

#define	_GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>

#ifndef PR_SET_IO_FLUSHER
#define	PR_SET_IO_FLUSHER	57
#endif

/* ublk UAPI (include/uapi/linux/ublk_cmd.h), stable since Linux 6.2 */
struct ublk_ctrl_cmd {
	uint32_t dev_id;
	uint16_t queue_id;
	uint16_t len;
	uint64_t addr;
	uint64_t data[1];
	uint16_t dev_path_len;
	uint16_t pad;
	uint32_t reserved;
};
struct ublk_dev_info {
	uint16_t nr_hw_queues;
	uint16_t queue_depth;
	uint16_t state;
	uint16_t pad0;
	uint32_t max_io_buf_bytes;
	uint32_t dev_id;
	int32_t ublksrv_pid;
	uint32_t pad1;
	uint64_t flags;
	uint64_t ublksrv_flags;
	uint32_t owner_uid;
	uint32_t owner_gid;
	uint64_t reserved1;
	uint64_t reserved2;
};
struct ublk_io_desc {
	uint32_t op_flags;
	uint32_t nr_sectors;
	uint64_t start_sector;
	uint64_t addr;
};
struct ublk_io_cmd {
	uint16_t q_id;
	uint16_t tag;
	int32_t result;
	uint64_t addr;
};
struct ublk_param_basic {
	uint32_t attrs;
	uint8_t logical_bs_shift;
	uint8_t physical_bs_shift;
	uint8_t io_opt_shift;
	uint8_t io_min_shift;
	uint32_t max_sectors;
	uint32_t chunk_sectors;
	uint64_t dev_sectors;
	uint64_t virt_boundary_mask;
};
struct ublk_params {
	uint32_t len;
	uint32_t types;
	struct ublk_param_basic basic;
};
_Static_assert(sizeof (struct ublk_ctrl_cmd) == 32, "ctrl cmd");
_Static_assert(sizeof (struct ublk_dev_info) == 64, "dev info");
_Static_assert(sizeof (struct ublk_io_desc) == 24, "io desc");
_Static_assert(sizeof (struct ublk_io_cmd) == 16, "io cmd");
_Static_assert(sizeof (struct ublk_param_basic) == 32, "basic");

#define	U_ADD_DEV	_IOWR('u', 0x04, struct ublk_ctrl_cmd)
#define	U_DEL_DEV	_IOWR('u', 0x05, struct ublk_ctrl_cmd)
#define	U_START_DEV	_IOWR('u', 0x06, struct ublk_ctrl_cmd)
#define	U_STOP_DEV	_IOWR('u', 0x07, struct ublk_ctrl_cmd)
#define	U_SET_PARAMS	_IOWR('u', 0x08, struct ublk_ctrl_cmd)
#define	U_FETCH_REQ	_IOWR('u', 0x20, struct ublk_io_cmd)
#define	U_COMMIT_FETCH	_IOWR('u', 0x21, struct ublk_io_cmd)
#define	UBLK_F_CMD_IOCTL_ENCODE	(1ULL << 6)
#define	UBLK_ATTR_ROTATIONAL	(1U << 1)
#define	UBLK_ATTR_VOLATILE_CACHE	(1U << 2)
#define	UBLK_PARAM_TYPE_BASIC	(1U << 0)
#define	UBLK_IO_OP_READ		0
#define	UBLK_IO_OP_WRITE	1
#define	UBLK_IO_OP_FLUSH	2
#define	UBLK_IO_OP_DISCARD	3
#define	UBLK_IO_OP_WRITE_ZEROES	5
#define	UBLK_IO_F_FUA		(1U << 13)
#define	UBLK_IO_RES_ABORT	(-ENODEV)

#define	QDEPTH		64		/* tags the block layer may use */
#define	MAX_IO		(1U << 20)	/* bytes per request */
#define	PAGE		4096ULL
#define	NBUCKET		26		/* log2 microsecond buckets */
#define	MS		1000000ULL
#define	SEC		1000000000ULL

static void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void) fprintf(stderr, "hddsim: ");
	(void) vfprintf(stderr, fmt, ap);
	(void) fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static uint64_t
mono_ns(void)
{
	struct timespec ts;

	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * SEC + (uint64_t)ts.tv_nsec);
}

/* ------------------------------------------------------------------ */
/* Parameters */

struct params {
	int model;		/* 0: complete at once, 1: HDD model */
	int ncq;		/* commands the drive holds (1..QDEPTH) */
	double age_ms;		/* oldest queued command served past this */
	double seek_min_ms;	/* track to track, with settle */
	double seek_max_ms;	/* full stroke */
	double rpm;
	double rate_outer;	/* media rate at offset 0, MB/s (1e6) */
	double rate_inner;	/* media rate at the end of the device */
	double stroke;		/* fraction of the stroke the device spans */
	double overhead_us;	/* per command */
	double iface_mbs;	/* link rate, MB/s */
	double cache_mb;	/* volatile write cache, 0 = write through */
	double cache_hi;	/* destage first above this fraction */
	double idle_ms;		/* idle media time before destaging */
	double destage_kb;	/* largest destage operation */
	double seq_kb;		/* forward gap read through, no seek */
	int flush_blocks;	/* 1: SATA non-queued flush */
	unsigned seed;
	double store_mb;	/* memory cap for non-pattern pages */
};

static void
params_default(struct params *p)
{
	p->model = 1;
	p->ncq = 32;
	p->age_ms = 1000;
	p->seek_min_ms = 0.8;
	p->seek_max_ms = 16;
	p->rpm = 7200;
	p->rate_outer = 200;
	p->rate_inner = 100;
	p->stroke = 1.0;
	p->overhead_us = 100;
	p->iface_mbs = 550;
	p->cache_mb = 128;
	p->cache_hi = 0.75;
	p->idle_ms = 5;
	p->destage_kb = 1024;
	p->seq_kb = 1024;
	p->flush_blocks = 1;
	p->seed = 1;
	p->store_mb = 2048;
}

static int
params_set(struct params *p, const char *k, const char *v)
{
	char *end;
	double d = strtod(v, &end);

	if (end == v || (*end != '\0' && *end != '\n' && *end != ' '))
		return (-1);
#define	PD(name)	if (strcmp(k, #name) == 0) { p->name = d; return (0); }
#define	PI(name)	if (strcmp(k, #name) == 0) { \
		p->name = (int)d; return (0); }
	PI(model) PI(ncq) PD(age_ms) PD(seek_min_ms) PD(seek_max_ms) PD(rpm)
	PD(rate_outer) PD(rate_inner) PD(stroke) PD(overhead_us) PD(iface_mbs)
	PD(cache_mb) PD(cache_hi) PD(idle_ms) PD(destage_kb) PD(seq_kb)
	PI(flush_blocks) PD(store_mb)
	if (strcmp(k, "seed") == 0) {
		p->seed = (unsigned)d;
		return (0);
	}
#undef PD
#undef PI
	return (-1);
}

static int
params_read(struct params *p, const char *path, char *err, size_t errlen)
{
	FILE *f = fopen(path, "r");
	char line[256];
	int n = 0;

	params_default(p);
	if (f == NULL) {
		(void) snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return (-1);
	}
	while (fgets(line, sizeof (line), f) != NULL) {
		char *s = line, *eq;

		n++;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '#' || *s == '\n' || *s == '\0')
			continue;
		eq = strchr(s, '=');
		if (eq == NULL)
			goto bad;
		*eq = '\0';
		if (params_set(p, s, eq + 1) != 0)
			goto bad;
	}
	(void) fclose(f);
	if (p->ncq < 1 || p->ncq > QDEPTH || p->rpm <= 0 ||
	    p->rate_outer <= 0 || p->rate_inner <= 0 || p->stroke <= 0 ||
	    p->stroke > 1 || p->iface_mbs <= 0 || p->cache_mb < 0 ||
	    p->cache_hi <= 0 || p->cache_hi > 1 || p->destage_kb < 4 ||
	    p->store_mb < 16) {
		(void) snprintf(err, errlen, "%s: a value is out of range",
		    path);
		return (-1);
	}
	return (0);
bad:
	(void) fclose(f);
	(void) snprintf(err, errlen, "%s:%d: bad line", path, n);
	return (-1);
}

static void
params_print(FILE *f, const struct params *p)
{
	(void) fprintf(f, "model=%d ncq=%d age_ms=%g seek_min_ms=%g "
	    "seek_max_ms=%g rpm=%g rate_outer=%g rate_inner=%g stroke=%g "
	    "overhead_us=%g iface_mbs=%g cache_mb=%g cache_hi=%g idle_ms=%g "
	    "destage_kb=%g seq_kb=%g flush_blocks=%d seed=%u store_mb=%g",
	    p->model, p->ncq, p->age_ms, p->seek_min_ms, p->seek_max_ms,
	    p->rpm, p->rate_outer, p->rate_inner, p->stroke, p->overhead_us,
	    p->iface_mbs, p->cache_mb, p->cache_hi, p->idle_ms,
	    p->destage_kb, p->seq_kb, p->flush_blocks, p->seed, p->store_mb);
}

/* ------------------------------------------------------------------ */
/* The drive model */

enum { OP_R, OP_W, OP_F, OP_O, NOPS };
enum { S_HOST, S_LINK, S_MEDIA, S_SPACE, S_ACTIVE, S_FLUSH, S_DONE };
static const char opname[NOPS] = { 'r', 'w', 'f', 'o' };

struct hreq {
	int tag;
	int op;
	int fua;
	int state;
	int32_t result;
	uint64_t off, len;	/* bytes */
	uint64_t t_arr, t_adm, t_done;
	struct hreq *next;
};

struct ext {
	uint64_t off, len;
};

struct stats {
	uint64_t n[NOPS], bytes[NOPS];
	uint64_t lat_sum[NOPS], lat_max[NOPS];
	uint64_t hist[NOPS][NBUCKET];
	uint64_t busy_ns, dest_bytes, dest_ops, dirty_max;
	uint64_t ncq_int, hq_int, last_t;
	uint64_t fl_exec_sum, fl_exec_max;
	uint64_t late_max, late_hist[NBUCKET];
};

struct hdd {
	struct params p;
	uint64_t size;
	uint64_t now;
	uint64_t rng;
	struct hreq *hq_head, *hq_tail;
	int hq_n;
	struct hreq *ncq[QDEPTH];
	int ncq_n;
	struct hreq *flush;		/* the flush in progress */
	uint64_t flush_target;		/* queued flush: destaged bytes */
	uint64_t link_free;
	int m_busy;
	uint64_t m_start, m_end;
	struct hreq *m_req;		/* NULL: a destage */
	uint64_t m_doff, m_dlen;
	uint64_t head;
	uint64_t idle_t;
	struct ext *ex;
	int ex_n, ex_cap;
	uint64_t dirty, destaged;
	struct stats st;
	void (*done)(void *, struct hreq *);
	void *arg;
};

static uint64_t
rnd(struct hdd *m)
{
	uint64_t x = m->rng;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	m->rng = x;
	return (x * 0x2545F4914F6CDD1DULL);
}

static double
rnd01(struct hdd *m)
{
	return ((double)(rnd(m) >> 11) / 9007199254740992.0);
}

static int
bucket(uint64_t ns)
{
	uint64_t us = ns / 1000;
	int b = 0;

	while (us > 1 && b < NBUCKET - 1) {
		us >>= 1;
		b++;
	}
	return (b);
}

static uint64_t
cache_cap(const struct hdd *m)
{
	return ((uint64_t)(m->p.cache_mb * 1048576.0));
}

static double
rate_at(const struct hdd *m, uint64_t off)
{
	double f = (double)off / (double)m->size * m->p.stroke;

	return ((m->p.rate_outer - (m->p.rate_outer - m->p.rate_inner) * f) *
	    1e6);
}

static uint64_t
xfer_ns(const struct hdd *m, uint64_t off, uint64_t len)
{
	return ((uint64_t)((double)len / rate_at(m, off) * 1e9));
}

static uint64_t
seek_ns(const struct hdd *m, uint64_t dist)
{
	double f = (double)dist / (double)m->size * m->p.stroke;

	if (f > 1)
		f = 1;
	return ((uint64_t)((m->p.seek_min_ms + (m->p.seek_max_ms -
	    m->p.seek_min_ms) * sqrt(f)) * 1e6));
}

static uint64_t
rev_ns(const struct hdd *m)
{
	return ((uint64_t)(60e9 / m->p.rpm));
}

/* Positioning from the head to off; random rotation unless expected. */
static uint64_t
pos_ns(struct hdd *m, uint64_t off, int expected)
{
	uint64_t seq = (uint64_t)(m->p.seq_kb * 1024);

	if (off >= m->head && off - m->head <= seq)
		return (xfer_ns(m, m->head, off - m->head));
	if (expected)
		return (seek_ns(m, off > m->head ? off - m->head :
		    m->head - off) + rev_ns(m) / 2);
	return (seek_ns(m, off > m->head ? off - m->head : m->head - off) +
	    (uint64_t)(rnd01(m) * (double)rev_ns(m)));
}

static uint64_t
ovh_ns(const struct hdd *m)
{
	return ((uint64_t)(m->p.overhead_us * 1000));
}

static void
integ(struct hdd *m)
{
	if (m->now > m->st.last_t) {
		uint64_t dt = m->now - m->st.last_t;

		m->st.ncq_int += dt * (uint64_t)m->ncq_n;
		m->st.hq_int += dt * (uint64_t)m->hq_n;
		m->st.last_t = m->now;
	}
}

/* Sorted, non-overlapping extents of dirty data. */
static int
ex_find(const struct hdd *m, uint64_t off)
{
	/* first extent whose end is above off */
	int lo = 0, hi = m->ex_n;

	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (m->ex[mid].off + m->ex[mid].len <= off)
			lo = mid + 1;
		else
			hi = mid;
	}
	return (lo);
}

static void
ex_insert(struct hdd *m, uint64_t off, uint64_t len)
{
	uint64_t end = off + len, covered = 0;
	int i = ex_find(m, off), j = i;

	/* merge every extent touching [off, end] */
	if (i > 0 && m->ex[i - 1].off + m->ex[i - 1].len == off)
		i--;
	j = i;
	while (j < m->ex_n && m->ex[j].off <= end) {
		uint64_t eo = m->ex[j].off, ee = eo + m->ex[j].len;
		uint64_t lo = eo > off ? eo : off, hi = ee < end ? ee : end;

		if (hi > lo)
			covered += hi - lo;
		if (eo < off)
			off = eo;
		if (ee > end)
			end = ee;
		j++;
	}
	m->dirty += len - covered;
	if (j == i) {
		if (m->ex_n == m->ex_cap) {
			m->ex_cap = m->ex_cap ? m->ex_cap * 2 : 1024;
			m->ex = realloc(m->ex, sizeof (struct ext) *
			    (size_t)m->ex_cap);
			if (m->ex == NULL)
				die("out of memory (extents)");
		}
		(void) memmove(&m->ex[i + 1], &m->ex[i],
		    sizeof (struct ext) * (size_t)(m->ex_n - i));
		m->ex_n++;
	} else if (j - i > 1) {
		(void) memmove(&m->ex[i + 1], &m->ex[j],
		    sizeof (struct ext) * (size_t)(m->ex_n - j));
		m->ex_n -= j - i - 1;
	}
	m->ex[i].off = off;
	m->ex[i].len = end - off;
	if (m->dirty > m->st.dirty_max)
		m->st.dirty_max = m->dirty;
}

/* Remove [off, off + len) from the dirty set; returns the bytes removed. */
static uint64_t
ex_remove(struct hdd *m, uint64_t off, uint64_t len)
{
	uint64_t end = off + len, removed = 0;
	int i = ex_find(m, off);

	while (i < m->ex_n && m->ex[i].off < end) {
		uint64_t eo = m->ex[i].off, ee = eo + m->ex[i].len;
		uint64_t lo = eo > off ? eo : off, hi = ee < end ? ee : end;

		removed += hi - lo;
		if (eo < off && ee > end) {
			/* split */
			if (m->ex_n == m->ex_cap) {
				m->ex_cap *= 2;
				m->ex = realloc(m->ex, sizeof (struct ext) *
				    (size_t)m->ex_cap);
				if (m->ex == NULL)
					die("out of memory (extents)");
			}
			(void) memmove(&m->ex[i + 2], &m->ex[i + 1],
			    sizeof (struct ext) * (size_t)(m->ex_n - i - 1));
			m->ex_n++;
			m->ex[i].len = off - eo;
			m->ex[i + 1].off = end;
			m->ex[i + 1].len = ee - end;
			break;
		} else if (eo < off) {
			m->ex[i].len = off - eo;
			i++;
		} else if (ee > end) {
			m->ex[i].off = end;
			m->ex[i].len = ee - end;
			break;
		} else {
			(void) memmove(&m->ex[i], &m->ex[i + 1],
			    sizeof (struct ext) * (size_t)(m->ex_n - i - 1));
			m->ex_n--;
		}
	}
	m->dirty -= removed;
	return (removed);
}

static int
ex_covers(const struct hdd *m, uint64_t off, uint64_t len)
{
	int i = ex_find(m, off);

	return (i < m->ex_n && m->ex[i].off <= off &&
	    m->ex[i].off + m->ex[i].len >= off + len);
}

static void
complete(struct hdd *m, struct hreq *r, uint64_t t)
{
	int i, b;
	uint64_t lat;

	for (i = 0; i < m->ncq_n; i++) {
		if (m->ncq[i] == r) {
			m->ncq[i] = m->ncq[--m->ncq_n];
			break;
		}
	}
	r->t_done = t;
	r->state = S_DONE;
	lat = t - r->t_arr;
	m->st.n[r->op]++;
	m->st.bytes[r->op] += r->len;
	m->st.lat_sum[r->op] += lat;
	if (lat > m->st.lat_max[r->op])
		m->st.lat_max[r->op] = lat;
	b = bucket(lat);
	m->st.hist[r->op][b]++;
	if (r->op == OP_F) {
		uint64_t ex = t - r->t_adm;

		m->st.fl_exec_sum += ex;
		if (ex > m->st.fl_exec_max)
			m->st.fl_exec_max = ex;
	}
	m->done(m->arg, r);
}

static void
link_xfer(struct hdd *m, struct hreq *r)
{
	uint64_t t = m->link_free > m->now ? m->link_free : m->now;

	t += ovh_ns(m) + (uint64_t)((double)r->len / (m->p.iface_mbs * 1e6) *
	    1e9);
	m->link_free = t;
	r->state = S_LINK;
	r->t_done = t;
}

static void
admit_one(struct hdd *m, struct hreq *r)
{
	r->t_adm = m->now;
	m->ncq[m->ncq_n++] = r;
	switch (r->op) {
	case OP_R:
		if (ex_covers(m, r->off, r->len))
			link_xfer(m, r);
		else
			r->state = S_MEDIA;
		break;
	case OP_W:
		if (cache_cap(m) > 0 && !r->fua) {
			if (m->dirty + r->len <= cache_cap(m)) {
				ex_insert(m, r->off, r->len);
				link_xfer(m, r);
			} else {
				r->state = S_SPACE;
			}
		} else {
			r->state = S_MEDIA;
		}
		break;
	default:
		r->state = S_LINK;
		r->t_done = m->now + ovh_ns(m);
		break;
	}
}

static int
admit(struct hdd *m)
{
	int n = 0;

	while (m->hq_head != NULL) {
		struct hreq *r = m->hq_head;

		if (m->flush != NULL && m->p.flush_blocks)
			break;
		if (r->op == OP_F) {
			if (m->flush != NULL)
				break;
			if (m->p.flush_blocks && m->ncq_n > 0)
				break;
			m->hq_head = r->next;
			if (m->hq_head == NULL)
				m->hq_tail = NULL;
			m->hq_n--;
			r->t_adm = m->now;
			r->state = S_FLUSH;
			m->flush = r;
			m->flush_target = m->destaged + m->dirty;
			n++;
			continue;
		}
		if (m->ncq_n >= m->p.ncq)
			break;
		m->hq_head = r->next;
		if (m->hq_head == NULL)
			m->hq_tail = NULL;
		m->hq_n--;
		admit_one(m, r);
		n++;
	}
	return (n);
}

/* Writes that waited for cache space, in arrival order. */
static int
space(struct hdd *m)
{
	int n = 0;

	for (;;) {
		struct hreq *best = NULL;
		int i;

		for (i = 0; i < m->ncq_n; i++) {
			struct hreq *r = m->ncq[i];

			if (r->state == S_SPACE &&
			    (best == NULL || r->t_arr < best->t_arr))
				best = r;
		}
		if (best == NULL || m->dirty + best->len > cache_cap(m))
			return (n);
		ex_insert(m, best->off, best->len);
		link_xfer(m, best);
		n++;
	}
}

static int
has_state(const struct hdd *m, int state)
{
	int i;

	for (i = 0; i < m->ncq_n; i++)
		if (m->ncq[i]->state == state)
			return (1);
	return (0);
}

static int
urgent(const struct hdd *m)
{
	if (m->dirty == 0)
		return (0);
	if (m->flush != NULL)
		return (1);
	return ((double)m->dirty >= m->p.cache_hi * (double)cache_cap(m) ||
	    has_state(m, S_SPACE));
}

static struct hreq *
pick(struct hdd *m)
{
	struct hreq *best = NULL, *old = NULL;
	uint64_t bc = UINT64_MAX;
	int i;

	for (i = 0; i < m->ncq_n; i++) {
		struct hreq *r = m->ncq[i];
		uint64_t c;

		if (r->state != S_MEDIA)
			continue;
		if (old == NULL || r->t_adm < old->t_adm)
			old = r;
		c = pos_ns(m, r->off, 1);
		if (c < bc || (c == bc && r->t_adm < best->t_adm)) {
			bc = c;
			best = r;
		}
	}
	if (old != NULL &&
	    m->now - old->t_adm >= (uint64_t)(m->p.age_ms * 1e6))
		return (old);
	return (best);
}

static void
media_start(struct hdd *m, uint64_t off, uint64_t len, struct hreq *r)
{
	uint64_t d = ovh_ns(m) + pos_ns(m, off, 0) + xfer_ns(m, off, len);

	m->m_busy = 1;
	m->m_start = m->now;
	m->m_end = m->now + d;
	m->m_req = r;
	m->m_doff = off;
	m->m_dlen = len;
	m->head = off + len;
	if (r != NULL)
		r->state = S_ACTIVE;
}

static void
destage(struct hdd *m)
{
	int i = ex_find(m, m->head);
	uint64_t max = (uint64_t)(m->p.destage_kb * 1024), len;

	if (i >= m->ex_n)
		i = 0;	/* C-LOOK: back to the lowest dirty extent */
	if (m->ex[i].off < m->head && m->ex[i].off + m->ex[i].len > m->head) {
		/* the head is inside this extent: write from the head */
		len = m->ex[i].off + m->ex[i].len - m->head;
		media_start(m, m->head, len < max ? len : max, NULL);
		return;
	}
	len = m->ex[i].len < max ? m->ex[i].len : max;
	media_start(m, m->ex[i].off, len, NULL);
}

static void
dispatch(struct hdd *m)
{
	struct hreq *r;

	if (m->m_busy)
		return;
	if (m->flush != NULL && m->p.flush_blocks && m->ncq_n == 0 &&
	    m->dirty == 0) {
		/* the flush command itself */
		m->m_busy = 1;
		m->m_start = m->now;
		m->m_end = m->now + ovh_ns(m);
		m->m_req = m->flush;
		m->m_dlen = 0;
		return;
	}
	if (urgent(m)) {
		destage(m);
		return;
	}
	r = pick(m);
	if (r != NULL) {
		media_start(m, r->off, r->len, r);
		return;
	}
	if (m->dirty > 0 && m->now >= m->idle_t +
	    (uint64_t)(m->p.idle_ms * 1e6))
		destage(m);
}

static uint64_t
next_event(const struct hdd *m)
{
	uint64_t e = UINT64_MAX;
	int i;

	if (m->m_busy)
		e = m->m_end;
	for (i = 0; i < m->ncq_n; i++)
		if (m->ncq[i]->state == S_LINK && m->ncq[i]->t_done < e)
			e = m->ncq[i]->t_done;
	if (!m->m_busy && m->dirty > 0 && !urgent(m) &&
	    !has_state(m, S_MEDIA)) {
		uint64_t t = m->idle_t + (uint64_t)(m->p.idle_ms * 1e6);

		if (t < e)
			e = t;
	}
	return (e);
}

static void
flush_check(struct hdd *m)
{
	/* a queued (non-blocking) flush completes once its data are down */
	if (m->flush != NULL && !m->p.flush_blocks &&
	    m->destaged >= m->flush_target) {
		struct hreq *f = m->flush;

		m->flush = NULL;
		complete(m, f, m->now);
	}
}

/* Everything due at m->now, then admission and dispatch. */
static void
step(struct hdd *m)
{
	int i, again;

	integ(m);
	if (m->m_busy && m->m_end <= m->now) {
		struct hreq *r = m->m_req;

		m->m_busy = 0;
		m->idle_t = m->m_end;
		m->st.busy_ns += m->m_end - m->m_start;
		if (r == NULL) {
			uint64_t n = ex_remove(m, m->m_doff, m->m_dlen);

			m->destaged += n;
			m->st.dest_bytes += n;
			m->st.dest_ops++;
		} else if (r == m->flush) {
			m->flush = NULL;
			complete(m, r, m->m_end);
		} else {
			complete(m, r, m->m_end);
		}
	}
	do {
		again = 0;
		for (i = 0; i < m->ncq_n; i++) {
			struct hreq *r = m->ncq[i];

			if (r->state == S_LINK && r->t_done <= m->now) {
				complete(m, r, r->t_done);
				again = 1;
				break;
			}
		}
	} while (again);
	do {
		flush_check(m);
		again = space(m);
		again += admit(m);
	} while (again);
	flush_check(m);
	integ(m);
	dispatch(m);
}

static void
advance(struct hdd *m, uint64_t t)
{
	int stuck = 0;

	for (;;) {
		uint64_t e = next_event(m);

		if (e > t)
			break;
		if (e > m->now)
			m->now = e;
		step(m);
		/* an event that stays due without progress: never spin */
		if (next_event(m) == e) {
			if (++stuck > 3)
				break;
		} else {
			stuck = 0;
		}
	}
	if (t > m->now)
		m->now = t;
	integ(m);
}

static void
submit(struct hdd *m, struct hreq *r)
{
	r->t_arr = m->now;
	r->next = NULL;
	if (!m->p.model) {
		r->t_adm = m->now;
		m->ncq[m->ncq_n++] = r;
		complete(m, r, m->now);
		return;
	}
	r->state = S_HOST;
	if (m->hq_tail != NULL)
		m->hq_tail->next = r;
	else
		m->hq_head = r;
	m->hq_tail = r;
	m->hq_n++;
	step(m);
}

static void
hdd_init(struct hdd *m, const struct params *p, uint64_t size,
    void (*done)(void *, struct hreq *), void *arg)
{
	(void) memset(m, 0, sizeof (*m));
	m->p = *p;
	m->size = size;
	m->rng = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)p->seed << 1);
	m->done = done;
	m->arg = arg;
}

/* New parameters; leaving the model completes everything it holds. */
static void
hdd_params(struct hdd *m, const struct params *p)
{
	int was = m->p.model;

	m->p = *p;
	if (was && !p->model) {
		while (m->hq_head != NULL) {
			struct hreq *r = m->hq_head;

			m->hq_head = r->next;
			m->hq_n--;
			r->t_adm = m->now;
			m->ncq[m->ncq_n++] = r;
		}
		m->hq_tail = NULL;
		if (m->flush != NULL) {
			struct hreq *f = m->flush;

			m->flush = NULL;
			if (m->m_req == f)
				m->m_busy = 0;
			f->t_adm = m->now;
			m->ncq[m->ncq_n++] = f;
		}
		while (m->ncq_n > 0)
			complete(m, m->ncq[0], m->now);
		m->m_busy = 0;
		m->ex_n = 0;
		m->dirty = 0;
	}
	if (!was && p->model) {
		m->idle_t = m->now;
		m->link_free = m->now;
	}
}

/* ------------------------------------------------------------------ */
/* Pattern store */

#define	CHUNK_PAGES	4096U

struct store {
	uint64_t npages;
	uint64_t *ent;
	uint64_t *bits;
	char **chunk;
	uint32_t nchunk, chunk_cap;
	uint32_t *freel;
	uint32_t nfree, free_cap;
	uint64_t nfull, max_full;
	uint64_t errors;
};

static int
isfull(const struct store *s, uint64_t pg)
{
	return ((s->bits[pg >> 6] >> (pg & 63)) & 1);
}

static void
store_init(struct store *s, uint64_t size, double max_mb)
{
	(void) memset(s, 0, sizeof (*s));
	s->npages = size / PAGE;
	s->ent = calloc(s->npages, sizeof (uint64_t));
	s->bits = calloc((s->npages + 63) / 64, sizeof (uint64_t));
	if (s->ent == NULL || s->bits == NULL)
		die("out of memory (store index)");
	s->max_full = (uint64_t)(max_mb * 256.0);
}

static char *
slot_ptr(const struct store *s, uint64_t slot)
{
	return (s->chunk[slot / CHUNK_PAGES] + (slot % CHUNK_PAGES) * PAGE);
}

static int
slot_alloc(struct store *s, uint64_t *slot)
{
	if (s->nfree > 0) {
		*slot = s->freel[--s->nfree];
		return (0);
	}
	if (s->nfull >= s->max_full)
		return (-1);
	if ((uint64_t)s->nchunk * CHUNK_PAGES <= s->nfull) {
		char *c;

		if (s->nchunk == s->chunk_cap) {
			s->chunk_cap = s->chunk_cap ? s->chunk_cap * 2 : 16;
			s->chunk = realloc(s->chunk, sizeof (char *) *
			    s->chunk_cap);
			if (s->chunk == NULL)
				return (-1);
		}
		c = mmap(NULL, CHUNK_PAGES * PAGE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (c == MAP_FAILED)
			return (-1);
		s->chunk[s->nchunk++] = c;
	}
	*slot = s->nfull++;
	return (0);
}

static void
slot_free(struct store *s, uint64_t slot)
{
	if (s->nfree == s->free_cap) {
		s->free_cap = s->free_cap ? s->free_cap * 2 : 4096;
		s->freel = realloc(s->freel, sizeof (uint32_t) * s->free_cap);
		if (s->freel == NULL)
			die("out of memory (free list)");
	}
	s->freel[s->nfree++] = (uint32_t)slot;
}

static int
store_write(struct store *s, uint64_t off, const char *buf, uint64_t len)
{
	uint64_t pg = off / PAGE, i;

	for (i = 0; i < len / PAGE; i++, pg++) {
		const char *p = buf + i * PAGE;

		if (memcmp(p, p + 8, PAGE - 8) == 0) {
			uint64_t pat;

			(void) memcpy(&pat, p, 8);
			if (isfull(s, pg)) {
				slot_free(s, s->ent[pg]);
				s->bits[pg >> 6] &= ~(1ULL << (pg & 63));
			}
			s->ent[pg] = pat;
		} else {
			uint64_t slot;

			if (!isfull(s, pg)) {
				if (slot_alloc(s, &slot) != 0) {
					s->errors++;
					return (-ENOSPC);
				}
				s->ent[pg] = slot;
				s->bits[pg >> 6] |= 1ULL << (pg & 63);
			}
			(void) memcpy(slot_ptr(s, s->ent[pg]), p, PAGE);
		}
	}
	return (0);
}

static void
store_zero(struct store *s, uint64_t off, uint64_t len)
{
	uint64_t pg = off / PAGE, i;

	for (i = 0; i < len / PAGE; i++, pg++) {
		if (isfull(s, pg)) {
			slot_free(s, s->ent[pg]);
			s->bits[pg >> 6] &= ~(1ULL << (pg & 63));
		}
		s->ent[pg] = 0;
	}
}

static void
store_read(const struct store *s, uint64_t off, char *buf, uint64_t len)
{
	uint64_t pg = off / PAGE, i, j;

	for (i = 0; i < len / PAGE; i++, pg++) {
		char *p = buf + i * PAGE;

		if (isfull(s, pg)) {
			(void) memcpy(p, slot_ptr(s, s->ent[pg]), PAGE);
		} else if (s->ent[pg] == 0) {
			(void) memset(p, 0, PAGE);
		} else {
			uint64_t *q = (uint64_t *)(void *)p;

			for (j = 0; j < PAGE / 8; j++)
				q[j] = s->ent[pg];
		}
	}
}

/* ------------------------------------------------------------------ */
/* io_uring without liburing */

struct ring {
	int fd;
	unsigned sqe_size;
	unsigned *sq_head, *sq_tail, *sq_mask, *sq_array;
	unsigned *cq_head, *cq_tail, *cq_mask;
	struct io_uring_cqe *cqes;
	char *sqes;
	unsigned tail;		/* local SQ tail */
	void *sq_ptr, *cq_ptr;
	size_t sq_sz, cq_sz, sqes_sz;
};

struct geta {			/* struct io_uring_getevents_arg */
	uint64_t sigmask;
	uint32_t sigmask_sz;
	uint32_t min_wait_usec;
	uint64_t ts;
};

static int
ring_init(struct ring *r, unsigned entries, int big)
{
	struct io_uring_params p;

	(void) memset(&p, 0, sizeof (p));
	(void) memset(r, 0, sizeof (*r));
	if (big)
		p.flags = IORING_SETUP_SQE128;
	r->fd = (int)syscall(__NR_io_uring_setup, entries, &p);
	if (r->fd < 0)
		return (-errno);
	if (!(p.features & IORING_FEAT_EXT_ARG))
		return (-ENOTSUP);
	r->sqe_size = big ? 128 : 64;
	r->sq_sz = p.sq_off.array + p.sq_entries * sizeof (unsigned);
	r->cq_sz = p.cq_off.cqes + p.cq_entries *
	    sizeof (struct io_uring_cqe);
	r->sq_ptr = mmap(NULL, r->sq_sz, PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQ_RING);
	if (r->sq_ptr == MAP_FAILED)
		return (-errno);
	r->cq_ptr = mmap(NULL, r->cq_sz, PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_CQ_RING);
	if (r->cq_ptr == MAP_FAILED)
		return (-errno);
	r->sqes_sz = (size_t)p.sq_entries * r->sqe_size;
	r->sqes = mmap(NULL, r->sqes_sz, PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_POPULATE, r->fd, IORING_OFF_SQES);
	if (r->sqes == MAP_FAILED)
		return (-errno);
	r->sq_head = (unsigned *)((char *)r->sq_ptr + p.sq_off.head);
	r->sq_tail = (unsigned *)((char *)r->sq_ptr + p.sq_off.tail);
	r->sq_mask = (unsigned *)((char *)r->sq_ptr + p.sq_off.ring_mask);
	r->sq_array = (unsigned *)((char *)r->sq_ptr + p.sq_off.array);
	r->cq_head = (unsigned *)((char *)r->cq_ptr + p.cq_off.head);
	r->cq_tail = (unsigned *)((char *)r->cq_ptr + p.cq_off.tail);
	r->cq_mask = (unsigned *)((char *)r->cq_ptr + p.cq_off.ring_mask);
	r->cqes = (struct io_uring_cqe *)((char *)r->cq_ptr + p.cq_off.cqes);
	r->tail = *r->sq_tail;
	return (0);
}

static void
ring_close(struct ring *r)
{
	if (r->sqes != NULL && r->sqes != MAP_FAILED)
		(void) munmap(r->sqes, r->sqes_sz);
	if (r->cq_ptr != NULL && r->cq_ptr != MAP_FAILED)
		(void) munmap(r->cq_ptr, r->cq_sz);
	if (r->sq_ptr != NULL && r->sq_ptr != MAP_FAILED)
		(void) munmap(r->sq_ptr, r->sq_sz);
	if (r->fd >= 0)
		(void) close(r->fd);
	r->fd = -1;
}

static struct io_uring_sqe *
ring_sqe(struct ring *r)
{
	unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
	unsigned idx;
	struct io_uring_sqe *sqe;

	if (r->tail - head > *r->sq_mask)
		return (NULL);
	idx = r->tail & *r->sq_mask;
	sqe = (struct io_uring_sqe *)(void *)(r->sqes +
	    (size_t)idx * r->sqe_size);
	(void) memset(sqe, 0, r->sqe_size);
	r->sq_array[idx] = idx;
	r->tail++;
	return (sqe);
}

/* Submit what is queued; wait for one completion up to wait_ns (0: none) */
static int
ring_enter(struct ring *r, uint64_t wait_ns, int wait)
{
	struct __kernel_timespec ts;
	struct geta ga;
	unsigned flags = 0, min = 0, n;
	int ret;

	__atomic_store_n(r->sq_tail, r->tail, __ATOMIC_RELEASE);
	n = r->tail - __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
	(void) memset(&ga, 0, sizeof (ga));
	if (wait) {
		ts.tv_sec = (long long)(wait_ns / SEC);
		ts.tv_nsec = (long long)(wait_ns % SEC);
		ga.ts = (uint64_t)(uintptr_t)&ts;
		flags = IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG;
		min = 1;
	}
	ret = (int)syscall(__NR_io_uring_enter, r->fd, n, min, flags,
	    wait ? &ga : NULL, wait ? sizeof (ga) : 0);
	if (ret < 0 && errno != ETIME && errno != EINTR && errno != EAGAIN &&
	    errno != EBUSY)
		return (-errno);
	return (0);
}

/* ------------------------------------------------------------------ */
/* ublk control */

static int
ctrl(struct ring *r, int fd, unsigned op, uint32_t dev_id, void *addr,
    uint16_t len, uint64_t data0)
{
	struct io_uring_sqe *sqe = ring_sqe(r);
	struct ublk_ctrl_cmd c;
	unsigned head;
	int res;

	if (sqe == NULL)
		return (-EBUSY);
	(void) memset(&c, 0, sizeof (c));
	c.dev_id = dev_id;
	c.queue_id = 0xffff;
	c.addr = (uint64_t)(uintptr_t)addr;
	c.len = len;
	c.data[0] = data0;
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = fd;
	sqe->cmd_op = op;
	sqe->user_data = op;
	(void) memcpy(sqe->cmd, &c, sizeof (c));
	for (;;) {
		int ret = ring_enter(r, 60 * SEC, 1);

		if (ret < 0)
			return (ret);
		head = *r->cq_head;
		if (head != __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE))
			break;
	}
	res = r->cqes[head & *r->cq_mask].res;
	__atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
	return (res);
}

/* ------------------------------------------------------------------ */
/* The server */

struct srv {
	int id;
	uint64_t size;
	const char *params_path, *stats_path;
	struct params p;
	struct hdd m;
	struct store st;
	struct ring ring;
	int cdev;
	struct ublk_io_desc *desc;
	size_t desc_sz;
	char *bufs;
	struct hreq req[QDEPTH];
	int dead[QDEPTH];
	struct hreq *done[QDEPTH];
	int ndone;
	FILE *stats;
	atomic_int reload, stop;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int fetched;
	int io_err;
};

static void
srv_done(void *arg, struct hreq *r)
{
	struct srv *s = arg;

	s->done[s->ndone++] = r;
}

static int
queue_cmd(struct srv *s, unsigned op, int tag, int32_t result)
{
	struct io_uring_sqe *sqe = ring_sqe(&s->ring);
	struct ublk_io_cmd c;

	if (sqe == NULL)
		return (-EBUSY);
	c.q_id = 0;
	c.tag = (uint16_t)tag;
	c.result = result;
	c.addr = (uint64_t)(uintptr_t)(s->bufs + (size_t)tag * MAX_IO);
	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = s->cdev;
	sqe->cmd_op = op;
	sqe->user_data = (uint64_t)tag;
	(void) memcpy(sqe->cmd, &c, sizeof (c));
	return (0);
}

static void
stats_line(struct srv *s, uint64_t now)
{
	struct stats *t = &s->m.st;
	struct timespec wall;
	int o, b;

	(void) clock_gettime(CLOCK_REALTIME, &wall);
	(void) fprintf(s->stats, "S %.3f %.3f model %d", (double)now / 1e9,
	    (double)wall.tv_sec + (double)wall.tv_nsec / 1e9, s->m.p.model);
	for (o = 0; o < NOPS; o++)
		(void) fprintf(s->stats, " %c %" PRIu64 " %" PRIu64 " %" PRIu64
		    " %" PRIu64, opname[o], t->n[o], t->bytes[o],
		    t->lat_sum[o] / 1000, t->lat_max[o] / 1000);
	(void) fprintf(s->stats, " busy_us %" PRIu64 " dest %" PRIu64 " %"
	    PRIu64 " dirty %" PRIu64 " %" PRIu64 " ncq_us %" PRIu64 " hq_us %"
	    PRIu64 " flx %" PRIu64 " %" PRIu64 " late_max_us %" PRIu64
	    " full %" PRIu64 " err %" PRIu64,
	    t->busy_ns / 1000, t->dest_ops, t->dest_bytes, s->m.dirty,
	    t->dirty_max, t->ncq_int / 1000, t->hq_int / 1000,
	    t->fl_exec_sum / 1000, t->fl_exec_max / 1000, t->late_max / 1000,
	    s->st.nfull - s->st.nfree, s->st.errors);
	for (o = 0; o < NOPS; o++) {
		(void) fprintf(s->stats, " h%c", opname[o]);
		for (b = 0; b < NBUCKET; b++)
			if (t->hist[o][b] != 0)
				(void) fprintf(s->stats, " %d:%" PRIu64, b,
				    t->hist[o][b]);
	}
	(void) fprintf(s->stats, " hl");
	for (b = 0; b < NBUCKET; b++)
		if (t->late_hist[b] != 0)
			(void) fprintf(s->stats, " %d:%" PRIu64, b,
			    t->late_hist[b]);
	(void) fprintf(s->stats, "\n");
	(void) fflush(s->stats);
	(void) memset(t, 0, sizeof (*t));
	t->last_t = s->m.now;
	t->dirty_max = s->m.dirty;
}

static void
do_reload(struct srv *s)
{
	struct params p;
	char err[256];

	if (params_read(&p, s->params_path, err, sizeof (err)) != 0) {
		(void) fprintf(s->stats, "E reload failed: %s\n", err);
		(void) fflush(s->stats);
		return;
	}
	p.store_mb = s->p.store_mb;	/* fixed at start */
	s->p = p;
	hdd_params(&s->m, &p);
	(void) fprintf(s->stats, "P %.3f ", (double)mono_ns() / 1e9);
	params_print(s->stats, &p);
	(void) fprintf(s->stats, "\n");
	(void) fflush(s->stats);
}

static void
handle_cqe(struct srv *s, int tag, int res)
{
	const struct ublk_io_desc *d;
	struct hreq *r;
	unsigned op;

	if (tag < 0 || tag >= QDEPTH)
		return;
	if (res == UBLK_IO_RES_ABORT || res < 0) {
		if (res != UBLK_IO_RES_ABORT)
			s->io_err = res;
		s->dead[tag] = 1;
		return;
	}
	d = &s->desc[tag];
	r = &s->req[tag];
	op = d->op_flags & 0xff;
	r->tag = tag;
	r->off = d->start_sector << 9;
	r->len = (uint64_t)d->nr_sectors << 9;
	r->fua = (d->op_flags & UBLK_IO_F_FUA) != 0;
	r->result = 0;
	switch (op) {
	case UBLK_IO_OP_READ:
		r->op = OP_R;
		r->result = (int32_t)r->len;
		break;
	case UBLK_IO_OP_WRITE:
		r->op = OP_W;
		r->result = (int32_t)r->len;
		break;
	case UBLK_IO_OP_FLUSH:
		r->op = OP_F;
		r->len = 0;
		break;
	default:
		r->op = OP_O;
		r->len = 0;
		break;
	}
	if ((r->off % PAGE) != 0 || (r->len % PAGE) != 0 || r->len > MAX_IO ||
	    r->off + r->len > s->size)
		r->result = -EINVAL;
	else if (op == UBLK_IO_OP_WRITE)
		r->result = store_write(&s->st, r->off, s->bufs +
		    (size_t)tag * MAX_IO, r->len) == 0 ? (int32_t)r->len :
		    -ENOSPC;
	else if (op == UBLK_IO_OP_WRITE_ZEROES || op == UBLK_IO_OP_DISCARD)
		store_zero(&s->st, d->start_sector << 9,
		    (uint64_t)d->nr_sectors << 9);
	else if (op != UBLK_IO_OP_READ && op != UBLK_IO_OP_FLUSH)
		r->result = -EOPNOTSUPP;
	advance(&s->m, mono_ns());
	submit(&s->m, r);
}

static void *
io_thread(void *arg)
{
	struct srv *s = arg;
	sigset_t all;
	uint64_t tick;
	int i;

	(void) sigfillset(&all);
	(void) pthread_sigmask(SIG_BLOCK, &all, NULL);
	(void) prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
	for (i = 0; i < QDEPTH; i++)
		if (queue_cmd(s, U_FETCH_REQ, i, 0) != 0)
			s->io_err = -EBUSY;
	if (s->io_err == 0)
		s->io_err = ring_enter(&s->ring, 0, 0);
	(void) pthread_mutex_lock(&s->mu);
	s->fetched = s->io_err == 0 ? 1 : -1;
	(void) pthread_cond_signal(&s->cv);
	(void) pthread_mutex_unlock(&s->mu);
	if (s->io_err != 0)
		return (NULL);
	s->m.now = mono_ns();
	s->m.idle_t = s->m.now;
	s->m.st.last_t = s->m.now;
	tick = s->m.now + SEC;
	while (!atomic_load(&s->stop)) {
		uint64_t now = mono_ns(), e, wait;
		unsigned head, tail;

		if (atomic_exchange(&s->reload, 0))
			do_reload(s);
		advance(&s->m, now);
		for (i = 0; i < s->ndone; i++) {
			struct hreq *r = s->done[i];
			uint64_t late = now > r->t_done ? now - r->t_done : 0;

			if (r->op == OP_R && r->result > 0)
				store_read(&s->st, r->off, s->bufs +
				    (size_t)r->tag * MAX_IO, r->len);
			if (late > s->m.st.late_max)
				s->m.st.late_max = late;
			s->m.st.late_hist[bucket(late)]++;
			(void) queue_cmd(s, U_COMMIT_FETCH, r->tag, r->result);
		}
		s->ndone = 0;
		if (now >= tick) {
			stats_line(s, now);
			tick += SEC;
			if (tick < now)
				tick = now + SEC;
		}
		e = next_event(&s->m);
		wait = e <= now ? 0 : e - now;
		if (wait > 100 * MS)
			wait = 100 * MS;
		if (tick > now && tick - now < wait)
			wait = tick - now;
		if ((i = ring_enter(&s->ring, wait == 0 ? 1 : wait, 1)) < 0) {
			/* keep serving: a pool on this device would hang */
			if (s->io_err == 0) {
				(void) fprintf(s->stats, "E io_uring_enter: "
				    "%s\n", strerror(-i));
				(void) fflush(s->stats);
			}
			s->io_err = i;
			(void) usleep(1000);
		}
		head = *s->ring.cq_head;
		tail = __atomic_load_n(s->ring.cq_tail, __ATOMIC_ACQUIRE);
		while (head != tail) {
			struct io_uring_cqe *c = &s->ring.cqes[head &
			    *s->ring.cq_mask];

			handle_cqe(s, (int)c->user_data, c->res);
			head++;
		}
		__atomic_store_n(s->ring.cq_head, head, __ATOMIC_RELEASE);
	}
	return (NULL);
}

static int
cmd_run(int id, uint64_t size_mib, const char *params_path,
    const char *stats_path)
{
	struct srv *s = calloc(1, sizeof (*s));
	struct ublk_dev_info info;
	struct ublk_params up;
	struct ring cr;
	pthread_t th;
	sigset_t set;
	char path[64], err[256];
	int cfd, ret, i, sig;

	if (s == NULL)
		die("out of memory");
	s->id = id;
	s->size = size_mib << 20;
	s->params_path = params_path;
	s->stats_path = stats_path;
	(void) pthread_mutex_init(&s->mu, NULL);
	(void) pthread_cond_init(&s->cv, NULL);
	if (params_read(&s->p, params_path, err, sizeof (err)) != 0)
		die("%s", err);
	s->stats = fopen(stats_path, "a");
	if (s->stats == NULL)
		die("%s: %s", stats_path, strerror(errno));
	/* a disposition inherited as ignored must not swallow them */
	(void) signal(SIGTERM, SIG_DFL);
	(void) signal(SIGINT, SIG_DFL);
	(void) signal(SIGHUP, SIG_DFL);
	(void) sigemptyset(&set);
	(void) sigaddset(&set, SIGTERM);
	(void) sigaddset(&set, SIGINT);
	(void) sigaddset(&set, SIGHUP);
	(void) pthread_sigmask(SIG_BLOCK, &set, NULL);
	store_init(&s->st, s->size, s->p.store_mb);
	hdd_init(&s->m, &s->p, s->size, srv_done, s);
	s->bufs = mmap(NULL, (size_t)QDEPTH * MAX_IO, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (s->bufs == MAP_FAILED)
		die("buffers: %s", strerror(errno));
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		(void) fprintf(stderr, "hddsim %d: mlockall: %s (going on)\n",
		    id, strerror(errno));

	cfd = open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
	if (cfd < 0)
		die("/dev/ublk-control: %s", strerror(errno));
	if ((ret = ring_init(&cr, 4, 1)) != 0)
		die("control ring: %s", strerror(-ret));
	(void) memset(&info, 0, sizeof (info));
	info.nr_hw_queues = 1;
	info.queue_depth = QDEPTH;
	info.max_io_buf_bytes = MAX_IO;
	info.dev_id = (uint32_t)id;
	info.flags = UBLK_F_CMD_IOCTL_ENCODE;
	ret = ctrl(&cr, cfd, U_ADD_DEV, (uint32_t)id, &info, sizeof (info), 0);
	if (ret != 0)
		die("ADD_DEV %d: %s", id, strerror(-ret));
	(void) memset(&up, 0, sizeof (up));
	up.len = sizeof (up);
	up.types = UBLK_PARAM_TYPE_BASIC;
	up.basic.attrs = UBLK_ATTR_ROTATIONAL | UBLK_ATTR_VOLATILE_CACHE;
	up.basic.logical_bs_shift = 12;
	up.basic.physical_bs_shift = 12;
	up.basic.io_opt_shift = 12;
	up.basic.io_min_shift = 12;
	up.basic.max_sectors = MAX_IO >> 9;
	up.basic.dev_sectors = s->size >> 9;
	ret = ctrl(&cr, cfd, U_SET_PARAMS, (uint32_t)id, &up, sizeof (up), 0);
	if (ret != 0) {
		(void) ctrl(&cr, cfd, U_DEL_DEV, (uint32_t)id, NULL, 0, 0);
		die("SET_PARAMS %d: %s", id, strerror(-ret));
	}
	(void) snprintf(path, sizeof (path), "/dev/ublkc%d", id);
	for (i = 0; i < 100; i++) {
		s->cdev = open(path, O_RDWR | O_CLOEXEC);
		if (s->cdev >= 0)
			break;
		(void) usleep(100000);
	}
	if (s->cdev < 0) {
		(void) ctrl(&cr, cfd, U_DEL_DEV, (uint32_t)id, NULL, 0, 0);
		die("%s: %s", path, strerror(errno));
	}
	s->desc_sz = ((QDEPTH * sizeof (struct ublk_io_desc)) + 4095) &
	    ~(size_t)4095;
	s->desc = mmap(NULL, s->desc_sz, PROT_READ, MAP_SHARED | MAP_POPULATE,
	    s->cdev, 0);
	if (s->desc == MAP_FAILED)
		die("mmap %s: %s", path, strerror(errno));
	if ((ret = ring_init(&s->ring, 2 * QDEPTH, 0)) != 0)
		die("io ring: %s", strerror(-ret));
	if (pthread_create(&th, NULL, io_thread, s) != 0)
		die("pthread_create");
	(void) pthread_mutex_lock(&s->mu);
	while (s->fetched == 0)
		(void) pthread_cond_wait(&s->cv, &s->mu);
	(void) pthread_mutex_unlock(&s->mu);
	if (s->fetched < 0)
		die("FETCH_REQ: %s", strerror(-s->io_err));
	ret = ctrl(&cr, cfd, U_START_DEV, (uint32_t)id, NULL, 0,
	    (uint64_t)getpid());
	if (ret != 0) {
		atomic_store(&s->stop, 1);
		(void) pthread_join(th, NULL);
		ring_close(&s->ring);
		(void) close(s->cdev);
		(void) ctrl(&cr, cfd, U_DEL_DEV, (uint32_t)id, NULL, 0, 0);
		die("START_DEV %d: %s", id, strerror(-ret));
	}
	(void) fprintf(s->stats, "I %.3f hddsim %d started /dev/ublkb%d "
	    "size_mib %" PRIu64 " pid %d\nP %.3f ", (double)mono_ns() / 1e9,
	    id, id, size_mib, (int)getpid(), (double)mono_ns() / 1e9);
	params_print(s->stats, &s->p);
	(void) fprintf(s->stats, "\n");
	(void) fflush(s->stats);
	(void) printf("hddsim %d: /dev/ublkb%d ready pid %d\n", id, id,
	    (int)getpid());
	(void) fflush(stdout);
	for (;;) {
		if (sigwait(&set, &sig) != 0)
			continue;
		if (sig == SIGHUP) {
			atomic_store(&s->reload, 1);
			continue;
		}
		break;
	}
	ret = ctrl(&cr, cfd, U_STOP_DEV, (uint32_t)id, NULL, 0, 0);
	atomic_store(&s->stop, 1);
	(void) pthread_join(th, NULL);
	ring_close(&s->ring);
	(void) munmap(s->desc, s->desc_sz);
	(void) close(s->cdev);
	i = ctrl(&cr, cfd, U_DEL_DEV, (uint32_t)id, NULL, 0, 0);
	(void) fprintf(s->stats, "I %.3f hddsim %d stopped (stop %d del %d "
	    "io_err %d)\n", (double)mono_ns() / 1e9, id, ret, i, s->io_err);
	(void) fclose(s->stats);
	ring_close(&cr);
	(void) close(cfd);
	return (ret == 0 && i == 0 ? 0 : 1);
}

static int
cmd_del(int id)
{
	struct ring cr;
	int cfd, a, b, ret;

	cfd = open("/dev/ublk-control", O_RDWR | O_CLOEXEC);
	if (cfd < 0)
		die("/dev/ublk-control: %s", strerror(errno));
	if ((ret = ring_init(&cr, 4, 1)) != 0)
		die("control ring: %s", strerror(-ret));
	a = ctrl(&cr, cfd, U_STOP_DEV, (uint32_t)id, NULL, 0, 0);
	b = ctrl(&cr, cfd, U_DEL_DEV, (uint32_t)id, NULL, 0, 0);
	(void) printf("hddsim del %d: stop %d del %d\n", id, a, b);
	ring_close(&cr);
	(void) close(cfd);
	return (b == 0 ? 0 : 1);
}

/* ------------------------------------------------------------------ */
/* Self-test: the model alone, in virtual time */

#define	ST_MAX	4096
#define	ST_LAT	131072

enum { C_OTHER, C_SYNC, C_ASYNC };

struct sim {
	struct hdd m;
	struct hreq pool[ST_MAX];
	int used[ST_MAX];
	int cls[ST_MAX];
	int nout;		/* requests outstanding */
	int ndone;		/* completions since the driver last looked */
	int out[3];		/* outstanding per class */
	uint64_t lat[NOPS][ST_LAT];
	int nlat[NOPS];
	uint64_t busy_t, busy_since;
	uint64_t wait[ST_LAT];	/* sync writes: time queued above the device */
	int nwait;
};

static void
sim_done(void *arg, struct hreq *r)
{
	struct sim *x = arg;
	uint64_t lat = r->t_done - r->t_arr;

	if (x->nlat[r->op] < ST_LAT)
		x->lat[r->op][x->nlat[r->op]++] = lat;
	x->used[r->tag] = 0;
	x->out[x->cls[r->tag]]--;
	x->nout--;
	x->ndone++;
	if (x->nout == 0)
		x->busy_t += r->t_done - x->busy_since;
}

static void
sim_issue(struct sim *x, int cls, int op, uint64_t off, uint64_t len)
{
	struct hreq *r = NULL;
	int i;

	for (i = 0; i < ST_MAX; i++) {
		if (!x->used[i]) {
			r = &x->pool[i];
			break;
		}
	}
	if (r == NULL)
		die("selftest: out of requests");
	(void) memset(r, 0, sizeof (*r));
	x->used[i] = 1;
	x->cls[i] = cls;
	r->tag = i;
	r->op = op;
	r->off = off;
	r->len = len;
	if (x->nout == 0)
		x->busy_since = x->m.now;
	x->nout++;
	x->out[cls]++;
	submit(&x->m, r);
}

static uint64_t
sim_off(struct sim *x, uint64_t span, uint64_t len)
{
	return ((rnd(&x->m) % ((span - len) / PAGE)) * PAGE);
}

static int
cmp64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x < y ? -1 : x > y);
}

static void
sim_dist(const char *name, uint64_t *v, int n)
{
	uint64_t sum = 0;
	int i;

	if (n == 0)
		return;
	qsort(v, (size_t)n, sizeof (uint64_t), cmp64);
	for (i = 0; i < n; i++)
		sum += v[i];
	(void) printf(" %s n %d mean %.1f p50 %.1f p99 %.1f max %.1f ms;",
	    name, n, (double)sum / n / 1e6, (double)v[n / 2] / 1e6,
	    (double)v[(int)((double)n * 0.99)] / 1e6, (double)v[n - 1] / 1e6);
}

static void
sim_report(struct sim *x, const char *name, uint64_t span)
{
	static const char *nm[NOPS] = { "read", "write", "flush", "other" };
	int o;

	(void) printf("%s:", name);
	for (o = 0; o < NOPS; o++)
		sim_dist(nm[o], x->lat[o], x->nlat[o]);
	if (x->nwait > 0)
		sim_dist("sync-write queue wait", x->wait, x->nwait);
	if (span > 0)
		(void) printf(" util %.1f %%, media busy %.1f %%, "
		    "IOPS %.0f\n", 100.0 * (double)x->busy_t / (double)span,
		    100.0 * (double)x->m.st.busy_ns / (double)span,
		    (double)(x->nlat[OP_R] + x->nlat[OP_W]) /
		    ((double)span / 1e9));
}

static struct sim *
sim_new(const struct params *p, uint64_t size)
{
	static struct sim x;

	(void) memset(&x, 0, sizeof (x));
	hdd_init(&x.m, p, size, sim_done, &x);
	return (&x);
}

/* qd operations outstanding until n are done, random or sequential. */
static void
sim_closed(const struct params *p, const char *name, int op, uint64_t len,
    int qd, int n, int seq)
{
	uint64_t size = 4ULL << 40, cur = 0;
	struct sim *x = sim_new(p, size);
	int issued = 0;

	while (issued < n || x->nout > 0) {
		uint64_t e;

		while (issued < n && x->nout < qd) {
			sim_issue(x, C_OTHER, op, seq ? cur : sim_off(x, size,
			    len), len);
			cur += len;
			issued++;
		}
		e = next_event(&x->m);
		if (e == UINT64_MAX)
			break;
		advance(&x->m, e);
	}
	sim_report(x, name, x->m.now);
}

/*
 * One leaf of the host's 5-wide raidz2, open loop, the vdev queue reduced
 * to its class limits: every 5 s a txg writes TXG_MB in 1 MiB writes, 2 in
 * flight, then flushes; ZIL commits arrive at 0.6/s, each writes 38 sync
 * writes of 342 KiB, 10 in flight, then flushes; reads of 342 KiB arrive
 * at 6.6/s, 10 in flight.  The reported sync-write queue wait is the time
 * from a commit's arrival to each of its writes' issue.
 */
#define	TXG_MB	75

static double
expo(struct sim *x, double rate)
{
	return (-log(1.0 - rnd01(&x->m)) / rate * 1e9);
}

static void
sim_host(const struct params *p, double secs)
{
	uint64_t size = 4ULL << 40, used = size / 3 * 2, t_end;
	uint64_t next_r, next_z, next_t, tcur = 0, zcur = used / 2;
	uint64_t zq_t[256];
	const uint64_t col = 342 * 1024;
	struct sim *x = sim_new(p, size);
	int zq = 0, tq = 0, rq = 0, zfl = 0, tfl = 0, zh = 0;

	t_end = (uint64_t)(secs * 1e9);
	next_r = (uint64_t)expo(x, 6.6);
	next_z = (uint64_t)expo(x, 0.6);
	next_t = 5 * SEC;
	for (;;) {
		uint64_t e = next_event(&x->m), a = UINT64_MAX;

		if (x->m.now < t_end) {
			a = next_r < next_z ? next_r : next_z;
			a = a < next_t ? a : next_t;
		}
		if (e == UINT64_MAX && a == UINT64_MAX && zq + tq + rq == 0 &&
		    !zfl && !tfl)
			break;
		if (e <= a) {
			advance(&x->m, e);
		} else {
			advance(&x->m, a);
			if (a == next_r) {
				rq++;
				next_r += (uint64_t)expo(x, 6.6);
			} else if (a == next_z) {
				int i;

				for (i = 0; i < 38 && zq < 256; i++)
					zq_t[(zh + zq++) % 256] = x->m.now;
				zfl = 1;
				next_z += (uint64_t)expo(x, 0.6);
			} else {
				tq += TXG_MB;
				tfl = 1;
				next_t += 5 * SEC;
			}
		}
		while (rq > 0 && x->out[C_OTHER] < 10) {
			rq--;
			sim_issue(x, C_OTHER, OP_R, sim_off(x, used, col), col);
		}
		while (zq > 0 && x->out[C_SYNC] < 10) {
			if (x->nwait < ST_LAT)
				x->wait[x->nwait++] = x->m.now - zq_t[zh];
			zh = (zh + 1) % 256;
			zq--;
			sim_issue(x, C_SYNC, OP_W, zcur, col);
			zcur += col + PAGE * (rnd(&x->m) % 64);
		}
		while (tq > 0 && x->out[C_ASYNC] < 2) {
			tq--;
			sim_issue(x, C_ASYNC, OP_W, tcur, 1 << 20);
			tcur += (1 << 20) + PAGE * (rnd(&x->m) % 16);
			if (tcur + (1 << 20) > used)
				tcur = 0;
		}
		if (zfl && zq == 0 && x->out[C_SYNC] == 0) {
			zfl = 0;
			sim_issue(x, C_OTHER, OP_F, 0, 0);
		}
		if (tfl && tq == 0 && x->out[C_ASYNC] == 0) {
			tfl = 0;
			sim_issue(x, C_OTHER, OP_F, 0, 0);
		}
	}
	sim_report(x, "host-like leaf", t_end);
}

static int
cmd_selftest(const char *path, double host_secs)
{
	struct params p;
	char err[256];

	if (params_read(&p, path, err, sizeof (err)) != 0)
		die("%s", err);
	p.model = 1;
	(void) printf("params: ");
	params_print(stdout, &p);
	(void) printf("\n");
	sim_closed(&p, "QD1 random 4K read", OP_R, 4096, 1, 3000, 0);
	sim_closed(&p, "QD32 random 4K read", OP_R, 4096, 32, 6000, 0);
	sim_closed(&p, "QD1 sequential 1M read", OP_R, 1 << 20, 1, 2000, 1);
	sim_closed(&p, "QD4 random 342K write, then nothing", OP_W,
	    342 * 1024, 4, 3000, 0);
	sim_host(&p, host_secs);
	return (0);
}

int
main(int argc, char **argv)
{
	if (argc == 6 && strcmp(argv[1], "run") == 0)
		return (cmd_run(atoi(argv[2]), strtoull(argv[3], NULL, 10),
		    argv[4], argv[5]));
	if (argc == 3 && strcmp(argv[1], "del") == 0)
		return (cmd_del(atoi(argv[2])));
	if (argc == 3 && strcmp(argv[1], "print") == 0) {
		struct params p;
		char err[256];

		if (params_read(&p, argv[2], err, sizeof (err)) != 0)
			die("%s", err);
		params_print(stdout, &p);
		(void) printf("\n");
		return (0);
	}
	if ((argc == 3 || argc == 4) && strcmp(argv[1], "selftest") == 0)
		return (cmd_selftest(argv[2], argc == 4 ? atof(argv[3]) :
		    600));
	(void) fprintf(stderr, "usage: hddsim run ID SIZE_MIB PARAMS STATS |"
	    " del ID | selftest PARAMS [SECONDS] | print PARAMS\n");
	return (2);
}
