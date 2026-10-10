/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * cedrus_plane: video from the engine to the screen on the display engine's video
 * plane, no copy and no GPU.
 *
 * cedrus_mpeg2.c decodes the stream over and over through V4L2; for every decoded
 * frame it calls kms_frame() below. The capture buffer (NV12, linear, from the
 * engine's reserved pool) is exported (VIDIOC_EXPBUF), imported into the KMS node
 * (bzkms) with drmPrimeFDToHandle(), wrapped in an NV12 framebuffer once and then
 * put on the hypervisor's video plane (DE2 VI channel: scaled by the VSU, converted
 * to RGB by the channel's CSC) with one atomic commit per frame. The commit stages
 * the plane's registers and EL2 latches them at the next vblank, so the picture
 * never tears; the commit's flip event says the vblank has come. Nothing reads or
 * writes a picture, no render node is opened (lima may be absent altogether).
 *
 * A capture buffer is never handed back to the decoder while it is on the screen or
 * staged for the next vblank: plane_hold() below keeps it out of the decoder's reach
 * until a further vblank after the flip that replaced it. With 8 capture buffers and
 * at most 2 reference pictures that leaves the decoder 3 or more to work in.
 *
 * With /dev/mem readable (root) the test also reads the hypervisor's scanout
 * registers (SCANOUT_R_VID_*, 0x0A002000): after every flip the staged addresses,
 * pitches, geometry and CTRL must be the ones of that frame's buffer, the applied
 * count must have advanced by exactly one and VID_REJECT must still be zero. The
 * physical address of every capture buffer is also read back independently: a marker
 * written through the user mapping is read at the address EL2 was given with the
 * engine driver's physical peek (dev.cedrus.0.peek).
 *
 *   cc -DCEDRUS_PLANE -DCEDRUS_KMS -I<shim> -I<uapi> -I/usr/local/include \
 *       -I/usr/local/include/libdrm -L/usr/local/lib -o cedrus_plane cedrus_plane.c -ldrm
 *   CEDRUS_PLANE_SECONDS=10 ./cedrus_plane stream.m2v reference.yuv
 *
 * Environment: CEDRUS_PLANE_SECONDS (10), CEDRUS_PLANE_AREA = "full" (default, the
 * whole screen), "window" (the hypervisor's guest window area, 16,66 1120x276) or
 * "x,y,w,h" in screen pixels; CEDRUS_PLANE_STRETCH=1 fills the area ignoring the
 * picture's aspect (the default keeps it, centred); CEDRUS_PLANE_BT709 = 0/1
 * (default: BT.709 from 720 lines up); CEDRUS_PLANE_NOREGS=1 skips the register checks.
 * CEDRUS_PLANE_TRACE=1 keeps a per-frame trace (entry, end of the wait, commit and event
 * times, EL2's vblank count at each) and lists every flip that was not exactly one vblank
 * after the previous one; CEDRUS_PLANE_DUMP=<file> writes the trace and the DRM event
 * stamps there, and with CEDRUS_PLANE_SAMPLER=1 also the changes of EL2's vblank and
 * VID_COUNT registers seen by a thread that does nothing else (a stalled flip event can
 * then be told from a stalled vblank or a late apply).
 * The hypervisor refuses a downscale beyond 2x, so a destination smaller than half the
 * picture is enlarged to that.
 *
 * The stream must have no B pictures (frames are shown in decoding order).
 */
#define	CEDRUS_KMS_BUILD
#ifndef CEDRUS_KMS
#define	CEDRUS_KMS
#endif
#ifndef CEDRUS_PLANE
#define	CEDRUS_PLANE
#endif
#include "cedrus_mpeg2.c"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

/* microkernel/scanout.h */
#define	SCANOUT_MMIO_BASE	0x0A002000UL
#define	SC_MAGIC		0x00
#define	SC_VERSION		0x04
#define	SC_WIDTH		0x10
#define	SC_HEIGHT		0x14
#define	SC_VBLANK_COUNT		0x40
#define	SC_VID_ADDR0		0xA0
#define	SC_VID_ADDR1		0xA4
#define	SC_VID_PITCH0		0xA8
#define	SC_VID_PITCH1		0xAC
#define	SC_VID_SRC_SIZE		0xB0
#define	SC_VID_DST_COORD	0xB4
#define	SC_VID_DST_SIZE		0xB8
#define	SC_VID_FORMAT		0xBC
#define	SC_VID_CTRL		0xC0
#define	SC_VID_COUNT		0xC4
#define	SC_VID_REJECT		0xC8

#define	WIN_X	16		/* the guest window on the screen (hdmi.h) */
#define	WIN_Y	66
#define	WIN_W	1120
#define	WIN_H	276

static struct {
	int		ready, fd;
	drmModeModeInfo	mode;
	uint32_t	crtc, conn, plane;
	uint32_t	dumb_handle, dumb_fb;
	uint32_t	p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_dx, p_dy, p_dw, p_dh, p_enc;
	uint64_t	enc_val;
	volatile uint32_t *reg;
	int		have_regs;
	/* geometry */
	unsigned	sw, sh, stride, ha;
	int		dx, dy;
	unsigned	dw, dh, bt709;
	unsigned	scrw, scrh;
	/* per capture buffer */
	unsigned	ncap;
	uint32_t	handle[NCAP], fb[NCAP];
	uint32_t	pa_y[NCAP], pa_uv[NCAP];
	int		state[NCAP];	/* 0 free, 1 staged, 2 on screen, 3 retiring */
	uint32_t	retire_vbl[NCAP];
	uint32_t	sum[NCAP];	/* checksum of a buffer when it was staged */
	int		check_sums;
	unsigned long	changed;	/* buffers that changed while on screen or staged */
	double		retire_t[NCAP];
	int		pending;	/* commit not yet confirmed by its event, or -1 */
	int		front;		/* confirmed on screen, or -1 */
	int		waiting;
	/* statistics */
	double		t0, t_last_ev, seconds, t_stop;
	uint32_t	vb0, vb_last_ev, vid_count0, vid_reject0, vid_count_expect;
	unsigned long	frames, events, mism, miss_vbl, viol, ev_gaps_gt;
	double		gap_sum, gap_min, gap_max;
	unsigned long	vb_hist[4];	/* vblanks since the previous event: 1, 2, 3+ ; [0]=0 */
	int		finished;
	/* per-frame trace, for the stall hunt */
	struct ftrace {
		double t_in, t_waited, t_commit, t_ev;
		uint32_t vb_in, vb_commit, vb_ev;
		int dv;
	} *tr;
	unsigned long	ntr;
} K = { .pending = -1, .front = -1, .gap_min = 1e9 };

static double
now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec * 1e-9);
}

static inline uint32_t
rd(unsigned off)
{

	return (K.reg[off / 4]);
}

static uint32_t
vblanks(void)
{

	return (K.have_regs ? rd(SC_VBLANK_COUNT) : 0);
}

struct evstamp { double rx; unsigned seq, sec, usec; };
static struct evstamp *EVS;
static unsigned long nevs;
static double ev_t0;

static void
flip_done(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{

	if (EVS != NULL && nevs < 400000) {
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		EVS[nevs].rx = ts.tv_sec + ts.tv_nsec * 1e-9;
		EVS[nevs].seq = seq;
		EVS[nevs].sec = sec;
		EVS[nevs].usec = usec;
		nevs++;
	}
	*(int *)data = 0;
}

/* A thread that does nothing but watch EL2's counters and note when they change. */
struct smp { double t; uint32_t vbl, vid; int gap; };
static struct smp *SM;
static unsigned long nsm;
static volatile int sm_stop;

static void *
sampler(void *arg)
{
	extern uint32_t *sm_regs;

	volatile uint32_t *r = (volatile uint32_t *)sm_regs;
	uint32_t lv = ~0u, ld = ~0u;
	struct timespec ts;
	double last = 0;

	while (!sm_stop && nsm < 2000000) {
		uint32_t v = r[SC_VBLANK_COUNT / 4], d = r[SC_VID_COUNT / 4];
		double t;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		t = ts.tv_sec + ts.tv_nsec * 1e-9;
		if (v != lv || d != ld || t - last > 0.002) {
			SM[nsm].gap = 0;
			SM[nsm].t = t; SM[nsm].vbl = v; SM[nsm].vid = d;
			SM[nsm].gap = (t - last > 0.003) ? 2 : (v == lv && d == ld);
			nsm++;
			lv = v; ld = d;
		}
		last = t;
	}
	return (NULL);
}
uint32_t *sm_regs;

/* The first /dev/dri/cardN whose driver is bzkms (it has a connected connector). */
static int
open_kms(void)
{
	char path[32];
	int i, fd;

	for (i = 0; i < 8; i++) {
		drmModeRes *res;
		int j, found = 0;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		res = drmModeGetResources(fd);
		if (res != NULL) {
			for (j = 0; j < res->count_connectors; j++) {
				drmModeConnector *cn = drmModeGetConnector(fd, res->connectors[j]);

				if (cn != NULL) {
					if (cn->connection == DRM_MODE_CONNECTED && cn->count_modes > 0)
						found = 1;
					drmModeFreeConnector(cn);
				}
			}
			drmModeFreeResources(res);
		}
		if (found) {
			printf("KMS node %s\n", path);
			return (fd);
		}
		close(fd);
	}
	return (-1);
}

/* The id of property `name` of a KMS object; if `e` is given, its enum value `val_name`. */
static uint32_t
prop_id(uint32_t obj, uint32_t type, const char *name, const char *val_name, uint64_t *val)
{
	drmModeObjectProperties *pr = drmModeObjectGetProperties(K.fd, obj, type);
	uint32_t id = 0;
	unsigned i;

	if (pr == NULL)
		return (0);
	for (i = 0; i < pr->count_props && id == 0; i++) {
		drmModePropertyRes *p = drmModeGetProperty(K.fd, pr->props[i]);

		if (p != NULL && strcmp(p->name, name) == 0) {
			id = p->prop_id;
			if (val_name != NULL && val != NULL) {
				int k;

				for (k = 0; k < p->count_enums; k++)
					if (strcmp(p->enums[k].name, val_name) == 0)
						*val = p->enums[k].value;
			}
		}
		if (p != NULL)
			drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(pr);
	return (id);
}

static void
parse_area(int *x, int *y, unsigned *w, unsigned *h)
{
	const char *a = getenv("CEDRUS_PLANE_AREA");
	int ax, ay;
	unsigned aw, ah;

	if (a == NULL || strcmp(a, "full") == 0) {
		*x = 0; *y = 0; *w = K.scrw; *h = K.scrh;
	} else if (strcmp(a, "window") == 0) {
		*x = WIN_X; *y = WIN_Y; *w = WIN_W; *h = WIN_H;
	} else if (sscanf(a, "%d,%d,%u,%u", &ax, &ay, &aw, &ah) == 4) {
		*x = ax; *y = ay; *w = aw; *h = ah;
	} else {
		fprintf(stderr, "bad CEDRUS_PLANE_AREA '%s'\n", a);
		exit(2);
	}
}

/* Where the picture goes: the area, aspect kept unless stretching, at least half the picture. */
static void
place(void)
{
	int ax, ay;
	unsigned aw, ah, dw, dh;
	int stretch = getenv("CEDRUS_PLANE_STRETCH") != NULL && atoi(getenv("CEDRUS_PLANE_STRETCH"));

	parse_area(&ax, &ay, &aw, &ah);
	dw = aw;
	dh = ah;
	if (!stretch) {
		if ((uint64_t)aw * K.sh > (uint64_t)ah * K.sw)
			dw = (unsigned)((uint64_t)ah * K.sw / K.sh);
		else
			dh = (unsigned)((uint64_t)aw * K.sh / K.sw);
	}
	if (K.sw > 2 * dw || K.sh > 2 * dh) {
		unsigned nw = dw < (K.sw + 1) / 2 ? (K.sw + 1) / 2 : dw;
		unsigned nh = dh < (K.sh + 1) / 2 ? (K.sh + 1) / 2 : dh;

		printf("destination %ux%u is below half the %ux%u picture (the hypervisor refuses "
		    "that): enlarged to %ux%u\n", dw, dh, K.sw, K.sh, nw, nh);
		dw = nw;
		dh = nh;
	}
	K.dx = ax + (int)(aw - dw) / 2;
	K.dy = ay + (int)(ah - dh) / 2;
	K.dw = dw;
	K.dh = dh;
	if (K.dx < 0)
		K.dx = 0;
	if (K.dy < 0)
		K.dy = 0;
}

static int
commit(int cap, int on, uint32_t flags, void *ud)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int r;

	drmModeAtomicAddProperty(req, K.plane, K.p_fb, on ? K.fb[cap] : 0);
	drmModeAtomicAddProperty(req, K.plane, K.p_crtc, on ? K.crtc : 0);
	if (on) {
		drmModeAtomicAddProperty(req, K.plane, K.p_sx, 0);
		drmModeAtomicAddProperty(req, K.plane, K.p_sy, 0);
		drmModeAtomicAddProperty(req, K.plane, K.p_sw, (uint64_t)K.sw << 16);
		drmModeAtomicAddProperty(req, K.plane, K.p_sh, (uint64_t)K.sh << 16);
		drmModeAtomicAddProperty(req, K.plane, K.p_dx, K.dx);
		drmModeAtomicAddProperty(req, K.plane, K.p_dy, K.dy);
		drmModeAtomicAddProperty(req, K.plane, K.p_dw, K.dw);
		drmModeAtomicAddProperty(req, K.plane, K.p_dh, K.dh);
		if (K.p_enc != 0)
			drmModeAtomicAddProperty(req, K.plane, K.p_enc, K.enc_val);
	}
	r = drmModeAtomicCommit(K.fd, req, flags, ud);
	drmModeAtomicFree(req);
	return (r);
}

/* Wait for the flip event of the commit in flight. */
static void
wait_event(void)
{
	struct pollfd pfd = { .fd = K.fd, .events = POLLIN };
	drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };
	double t0 = now_s();

	while (K.waiting) {
		if (poll(&pfd, 1, 1000) <= 0 || now_s() - t0 > 3) {
			fprintf(stderr, "flip event did not arrive\n");
			K.waiting = 0;
			K.mism++;
			return;
		}
		drmHandleEvent(K.fd, &ev);
	}
}

/* A flip has completed: check the registers against the frame, retire the previous one. */
static void
confirmed(int cap)
{
	uint32_t vb = vblanks();
	double t = now_s();
	int old = K.front, bad = 0;

	if (K.have_regs) {
		K.vid_count_expect++;
		if (rd(SC_VID_ADDR0) != K.pa_y[cap] || rd(SC_VID_ADDR1) != K.pa_uv[cap] ||
		    rd(SC_VID_PITCH0) != K.stride || rd(SC_VID_PITCH1) != K.stride ||
		    rd(SC_VID_SRC_SIZE) != (K.sw | K.sh << 16) ||
		    rd(SC_VID_DST_COORD) != ((uint32_t)K.dx | (uint32_t)K.dy << 16) ||
		    rd(SC_VID_DST_SIZE) != (K.dw | K.dh << 16) || rd(SC_VID_FORMAT) != 0 ||
		    rd(SC_VID_CTRL) != (1u | (K.bt709 ? 2u : 0u)))
			bad = 1;
		if (rd(SC_VID_COUNT) - K.vid_count0 != K.vid_count_expect ||
		    rd(SC_VID_REJECT) != K.vid_reject0)
			bad = 1;
		if (bad && K.mism++ < 5)
			fprintf(stderr, "frame %lu cap %d: registers addr0 %#x addr1 %#x ctrl %#x "
			    "count %u (expected %u) reject %u; wanted %#x %#x\n", K.frames, cap,
			    rd(SC_VID_ADDR0), rd(SC_VID_ADDR1), rd(SC_VID_CTRL),
			    rd(SC_VID_COUNT) - K.vid_count0, K.vid_count_expect,
			    rd(SC_VID_REJECT) - K.vid_reject0, K.pa_y[cap], K.pa_uv[cap]);
	}
	if (K.events > 0) {
		double gap = (t - K.t_last_ev) * 1000;
		uint32_t dv = vb - K.vb_last_ev;

		K.gap_sum += gap;
		if (gap < K.gap_min)
			K.gap_min = gap;
		if (gap > K.gap_max)
			K.gap_max = gap;
		if (K.have_regs) {
			K.vb_hist[dv > 3 ? 3 : dv]++;
			if (dv > 1)
				K.miss_vbl += dv - 1;
		}
	}
	if (K.tr != NULL && K.frames > 0 && K.frames <= 400000) {
		struct ftrace *f = &K.tr[K.frames - 1];

		f->t_ev = t - K.t0;
		f->vb_ev = vb;
		f->dv = K.events > 0 ? (int)(vb - K.vb_last_ev) : 0;
	}
	K.t_last_ev = t;
	K.vb_last_ev = vb;
	K.events++;
	K.state[cap] = 2;
	K.front = cap;
	K.pending = -1;
	if (old >= 0) {
		K.state[old] = 3;
		K.retire_vbl[old] = vb + 1;
		K.retire_t[old] = t + 0.040;
	}
}

/* A checksum of the picture in capture buffer `cap` (read through the uncached user mapping). */
static uint32_t
bufsum(unsigned cap)
{
	const volatile uint32_t *p = (const volatile uint32_t *)cbuf[cap];
	size_t i, n = (size_t)K.stride * K.ha / 4 + (size_t)K.stride * K.sh / 8;
	uint32_t h = 2166136261u;

	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return (h);
}

/* Is capture buffer `cap` off limits to the decoder? */
int
plane_hold(unsigned cap)
{

	if (!K.ready)
		return (0);
	if (K.state[cap] == 3) {
		if (K.have_regs ? (int32_t)(vblanks() - K.retire_vbl[cap]) >= 0 : now_s() > K.retire_t[cap]) {
			/* Back to the decoder: it must be the picture that was staged. */
			if (K.check_sums && bufsum(cap) != K.sum[cap])
				K.changed++;
			K.state[cap] = 0;
		}
	}
	return (K.state[cap] != 0);
}

/* The physical word at `pa`, through the engine driver's lab peek (it prints to the log). */
static int
peek(uint64_t pa, uint32_t *val)
{
	char cmd[160], line[200];
	FILE *p;
	int ok = 0;

	if (sysctlbyname("dev.cedrus.0.peek", NULL, NULL, &pa, sizeof(pa)) != 0)
		return (-1);
	snprintf(cmd, sizeof(cmd), "dmesg | grep 'peek %08jx = ' | tail -1", (uintmax_t)pa);
	p = popen(cmd, "r");
	if (p == NULL)
		return (-1);
	if (fgets(line, sizeof(line), p) != NULL) {
		char *e = strstr(line, " = ");

		if (e != NULL) {
			*val = (uint32_t)strtoul(e + 3, NULL, 16);
			ok = 1;
		}
	}
	pclose(p);
	return (ok ? 0 : -1);
}

static void
cleanup(void)
{
	double dt, cpu;
	struct rusage ru;

	if (!K.ready || K.finished)
		return;
	K.finished = 1;
	if (K.waiting)
		wait_event();
	if (K.pending >= 0)
		confirmed(K.pending);
	K.t_stop = now_s();
	sm_stop = 1;
	if (K.tr != NULL && getenv("CEDRUS_PLANE_DUMP") != NULL) {
		FILE *fp = fopen(getenv("CEDRUS_PLANE_DUMP"), "w");
		unsigned long i;

		if (fp != NULL) {
			fprintf(fp, "T0 %.6f\n", K.t0);
			for (i = 0; i < K.frames; i++)
				fprintf(fp, "F %lu %.6f %.6f %.6f %.6f %u %u %u %d\n", i, K.tr[i].t_in, K.tr[i].t_waited,
				    K.tr[i].t_commit, K.tr[i].t_ev, K.tr[i].vb_in, K.tr[i].vb_commit, K.tr[i].vb_ev, K.tr[i].dv);
			for (i = 0; i < nevs; i++)
				fprintf(fp, "E %lu %.6f %u %u %u\n", i, EVS[i].rx, EVS[i].seq, EVS[i].sec, EVS[i].usec);
			for (i = 0; i < nsm; i++)
				fprintf(fp, "S %.6f %u %u %d\n", SM[i].t, SM[i].vbl, SM[i].vid, SM[i].gap);
			fclose(fp);
		}
	}
	if (K.tr != NULL) {
		unsigned long i;
		double sd = 0, sd2 = 0, sw = 0, sc = 0, se = 0, mx = 0;

		for (i = 1; i < K.frames; i++) {
			struct ftrace *f = &K.tr[i], *pf = &K.tr[i - 1];
			double d = (f->t_in - pf->t_ev) * 1000;

			sd += d; sd2 += d * d;
			if (d > mx) mx = d;
			sw += (f->t_waited - f->t_in) * 1000;
			sc += (f->t_commit - f->t_waited) * 1000;
			se += (f->t_ev - f->t_commit) * 1000;
		}
		printf("TRACE means ms: ev->next entry (decode) %.2f (max %.2f), wait_event %.2f, commit %.2f, commit->event %.2f\n",
		    sd / (K.frames - 1), mx, sw / (K.frames - 1), sc / (K.frames - 1), se / (K.frames - 1));
		for (i = 1; i < K.frames; i++) {
			struct ftrace *f = &K.tr[i], *pf = &K.tr[i - 1];

			if (f->dv == 1)
				continue;
			printf("STALL frame %lu dv %d t %.3f: gap %.1f ms | prev ev->entry %.1f, wait %.1f, commit %.2f, "
			    "commit->ev %.1f | vb: prev_ev %u entry %u commit %u ev %u\n", i, f->dv, f->t_ev,
			    (f->t_ev - pf->t_ev) * 1000, (f->t_in - pf->t_ev) * 1000,
			    (f->t_waited - f->t_in) * 1000, (f->t_commit - f->t_waited) * 1000,
			    (f->t_ev - f->t_commit) * 1000, pf->vb_ev, f->vb_in, f->vb_commit, f->vb_ev);
		}
	}
	dt = K.t_stop - K.t0;
	getrusage(RUSAGE_SELF, &ru);
	cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6 + ru.ru_stime.tv_sec +
	    ru.ru_stime.tv_usec * 1e-6;
	printf("%lu frames in %.2f s = %.2f fps (%ux%u -> %ux%u at %d,%d on a %ux%u screen, BT.%s), "
	    "flip events %lu\n", K.frames, dt, K.frames / dt, K.sw, K.sh, K.dw, K.dh, K.dx, K.dy,
	    K.scrw, K.scrh, K.bt709 ? "709" : "601", K.events);
	printf("process CPU %.3f s of %.2f s = %.1f%% of one core (user %.3f, system %.3f)\n", cpu, dt,
	    100 * cpu / dt, ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6,
	    ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6);
	if (K.events > 1)
		printf("flip interval ms: mean %.2f min %.2f max %.2f\n", K.gap_sum / (K.events - 1),
		    K.gap_min, K.gap_max);
	if (K.have_regs) {
		printf("EL2: %u vblanks in the run, VID applied %u (expected %u), VID_REJECT %u, "
		    "register mismatches %lu\n", vblanks() - K.vb0, rd(SC_VID_COUNT) - K.vid_count0,
		    K.vid_count_expect, rd(SC_VID_REJECT) - K.vid_reject0, K.mism);
		printf("vblanks between consecutive flips: 1 x %lu, 2 x %lu, 3+ x %lu (0 x %lu); "
		    "vblanks skipped %lu\n", K.vb_hist[1], K.vb_hist[2], K.vb_hist[3], K.vb_hist[0],
		    K.miss_vbl);
	}
	printf("decoder handed a buffer that was on screen or staged: %lu times\n", K.viol);
	if (K.check_sums)
		printf("pictures whose checksum changed between staging and release: %lu\n", K.changed);
	/* Plane off, the window back to the hypervisor's own buffer. */
	commit(-1, 0, 0, NULL);
	drmModeSetCrtc(K.fd, K.crtc, 0, 0, 0, NULL, 0, NULL);
	if (K.have_regs)
		printf("after cleanup VID_CTRL %#x\n", rd(SC_VID_CTRL));
	{
		unsigned k;

		for (k = 0; k < K.ncap; k++)
			if (K.fb[k] != 0)
				drmModeRmFB(K.fd, K.fb[k]);
	}
	fflush(stdout);
}

static void
onsig(int s)
{

	exit(130);
}

static void
plane_init(int vfd, unsigned stride, unsigned ha, unsigned w, unsigned h)
{
	drmModeRes *res;
	drmModeConnector *cn = NULL;
	drmModePlaneRes *pres;
	struct drm_mode_create_dumb cd;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	const char *e;
	unsigned i;
	int mfd;

	setvbuf(stdout, NULL, _IOLBF, 0);
	K.seconds = (e = getenv("CEDRUS_PLANE_SECONDS")) != NULL ? atof(e) : 10;
	K.sw = w;
	K.sh = h;
	K.stride = stride;
	K.ha = ha;
	K.check_sums = (e = getenv("CEDRUS_PLANE_SUMS")) != NULL && atoi(e) != 0;
	K.bt709 = (e = getenv("CEDRUS_PLANE_BT709")) != NULL ? atoi(e) != 0 : h >= 720;

	K.fd = open_kms();
	if (K.fd < 0) {
		fprintf(stderr, "no KMS node with a connector (is bzkms loaded?)\n");
		exit(1);
	}
	drmSetClientCap(K.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (drmSetClientCap(K.fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
		fprintf(stderr, "no atomic KMS\n");
		exit(1);
	}
	res = drmModeGetResources(K.fd);
	for (i = 0; res != NULL && i < (unsigned)res->count_connectors && cn == NULL; i++) {
		drmModeConnector *c = drmModeGetConnector(K.fd, res->connectors[i]);

		if (c != NULL && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
			cn = c;
		else if (c != NULL)
			drmModeFreeConnector(c);
	}
	if (cn == NULL || res->count_crtcs < 1) {
		fprintf(stderr, "no connector or CRTC\n");
		exit(1);
	}
	K.conn = cn->connector_id;
	K.mode = cn->modes[0];
	K.crtc = res->crtcs[0];
	printf("KMS: connector %u, crtc %u, mode %ux%u@%u\n", K.conn, K.crtc, K.mode.hdisplay,
	    K.mode.vdisplay, K.mode.vrefresh);

	/* The video plane: the overlay plane that takes NV12. */
	pres = drmModeGetPlaneResources(K.fd);
	for (i = 0; pres != NULL && i < pres->count_planes && K.plane == 0; i++) {
		drmModePlane *pl = drmModeGetPlane(K.fd, pres->planes[i]);
		unsigned f;

		if (pl == NULL)
			continue;
		for (f = 0; f < pl->count_formats; f++)
			if (pl->formats[f] == DRM_FORMAT_NV12)
				K.plane = pl->plane_id;
		drmModeFreePlane(pl);
	}
	if (K.plane == 0) {
		fprintf(stderr, "no plane takes NV12 (bzkms without the video plane?)\n");
		exit(1);
	}
	K.p_fb = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL, NULL);
	K.p_crtc = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL, NULL);
	K.p_sx = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "SRC_X", NULL, NULL);
	K.p_sy = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL, NULL);
	K.p_sw = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "SRC_W", NULL, NULL);
	K.p_sh = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "SRC_H", NULL, NULL);
	K.p_dx = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL, NULL);
	K.p_dy = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL, NULL);
	K.p_dw = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL, NULL);
	K.p_dh = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL, NULL);
	K.p_enc = prop_id(K.plane, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING",
	    K.bt709 ? "ITU-R BT.709 YCbCr" : "ITU-R BT.601 YCbCr", &K.enc_val);
	printf("video plane %u\n", K.plane);

	/* The scanout registers, for the checks. */
	K.scrw = 1920;
	K.scrh = 1080;
	if (getenv("CEDRUS_PLANE_NOREGS") == NULL && (mfd = open("/dev/mem", O_RDWR)) >= 0) {
		void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mfd, SCANOUT_MMIO_BASE);

		if (m != MAP_FAILED && ((volatile uint32_t *)m)[0] == 0x53434e41u) {
			K.reg = m;
			K.have_regs = 1;
			K.scrw = rd(SC_WIDTH);
			K.scrh = rd(SC_HEIGHT);
			printf("scanout device version %u, screen %ux%u\n", rd(SC_VERSION), K.scrw, K.scrh);
		}
	}
	if (!K.have_regs)
		printf("no scanout registers: register checks off\n");
	place();

	/* The window: a black dumb buffer, so the picture is the only thing that moves. */
	memset(&cd, 0, sizeof(cd));
	cd.width = K.mode.hdisplay;
	cd.height = K.mode.vdisplay;
	cd.bpp = 32;
	if (drmIoctl(K.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) != 0)
		die("CREATE_DUMB");
	K.dumb_handle = cd.handle;
	handles[0] = cd.handle;
	pitches[0] = cd.pitch;
	if (drmModeAddFB2(K.fd, K.mode.hdisplay, K.mode.vdisplay, DRM_FORMAT_XRGB8888, handles, pitches,
	    offsets, &K.dumb_fb, 0) != 0)
		die("AddFB2 window");
	if (drmModeSetCrtc(K.fd, K.crtc, K.dumb_fb, 0, 0, &K.conn, 1, &K.mode) != 0)
		die("SetCrtc");

	/* Every capture buffer: dma-buf -> GEM handle -> NV12 framebuffer. */
	for (i = 0; i < NCAP; i++) {
		struct v4l2_exportbuffer x;
		uint32_t hs[4] = { 0 }, ps[4] = { 0 }, os[4] = { 0 };

		memset(&x, 0, sizeof(x));
		x.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		x.index = i;
		x.flags = O_RDWR | O_CLOEXEC;
		if (ioctl(vfd, VIDIOC_EXPBUF, &x) != 0)
			break;
		if (drmPrimeFDToHandle(K.fd, x.fd, &K.handle[i]) != 0) {
			fprintf(stderr, "drmPrimeFDToHandle(cap %u): %s\n", i, strerror(errno));
			exit(1);
		}
		close(x.fd);
		hs[0] = hs[1] = K.handle[i];
		ps[0] = ps[1] = stride;
		os[0] = 0;
		os[1] = stride * ha;
		if (drmModeAddFB2(K.fd, w, h, DRM_FORMAT_NV12, hs, ps, os, &K.fb[i], 0) != 0) {
			fprintf(stderr, "AddFB2 NV12 (cap %u, %ux%u pitch %u uv at %u): %s\n", i, w, h,
			    stride, os[1], strerror(errno));
			exit(1);
		}
		K.ncap = i + 1;
	}
	printf("%u capture buffers imported as NV12 framebuffers (pitch %u, chroma at +%u)\n", K.ncap,
	    stride, stride * ha);
	K.ready = 1;
	atexit(cleanup);
	signal(SIGINT, onsig);
	signal(SIGTERM, onsig);

	/*
	 * Probe: put each buffer on the plane once, with a marker in its first luma and
	 * chroma words, read the address EL2 holds for it and check it with the driver's
	 * physical peek.
	 */
	if (K.have_regs) {
		K.vid_count0 = rd(SC_VID_COUNT);
		K.vid_reject0 = rd(SC_VID_REJECT);
		K.vb0 = vblanks();
	}
	for (i = 0; i < K.ncap; i++) {
		volatile uint32_t *y = (volatile uint32_t *)cbuf[i];
		volatile uint32_t *uv = (volatile uint32_t *)(cbuf[i] + (size_t)stride * ha);
		uint32_t vy = 0, vuv = 0;
		int okp = -1;

		*y = 0x59000000u | i;
		*uv = 0x55000000u | i;
		K.waiting = 1;
		if (commit(i, 1, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, &K.waiting) != 0) {
			fprintf(stderr, "probe commit of buffer %u: %s\n", i, strerror(errno));
			K.waiting = 0;
			K.t0 = now_s();
			exit(1);
		}
		wait_event();
		K.state[i] = 1;
		K.pending = i;
		if (K.have_regs) {
			K.pa_y[i] = rd(SC_VID_ADDR0);
			K.pa_uv[i] = rd(SC_VID_ADDR1);
			if (K.pa_uv[i] - K.pa_y[i] != stride * ha) {
				fprintf(stderr, "buffer %u: chroma plane not at +%u\n", i, stride * ha);
				K.mism++;
			}
			okp = (peek(K.pa_y[i], &vy) == 0 && peek(K.pa_uv[i], &vuv) == 0) ?
			    (vy == (0x59000000u | i) && vuv == (0x55000000u | i)) : -1;
			printf("  cap %u: luma at %#x, chroma at %#x; physical read %s (%#x, %#x)\n", i,
			    K.pa_y[i], K.pa_uv[i], okp == 1 ? "matches the markers" :
			    okp == 0 ? "DIFFERS from the markers" : "unavailable", vy, vuv);
			if (okp == 0)
				K.mism++;
		}
		confirmed(i);
	}
	/* Everything but the last probed buffer (on the screen now) is free again two vblanks on. */
	usleep(60000);
	for (i = 0; i < K.ncap; i++)
		if (K.state[i] == 3)
			K.state[i] = 0;
	K.frames = 0;
	K.events = 0;
	K.gap_sum = 0;
	K.gap_min = 1e9;
	K.gap_max = 0;
	K.miss_vbl = 0;
	memset(K.vb_hist, 0, sizeof(K.vb_hist));
	if (K.have_regs) {
		K.vid_count0 = rd(SC_VID_COUNT);
		K.vid_count_expect = 0;
		K.vb0 = vblanks();
	}
	K.t0 = now_s();
}

int
kms_frame(int vfd, unsigned cap, unsigned stride, unsigned ha, unsigned w, unsigned h)
{

	if (!K.ready)
		plane_init(vfd, stride, ha, w, h);
	if (cap >= K.ncap) {
		fprintf(stderr, "capture index %u\n", cap);
		return (1);
	}
	if (K.state[cap] != 0)
		K.viol++;		/* the decoder was given a buffer that is not free */
	/* The previous frame must have reached the screen before this one is staged. */
	if (K.tr == NULL && getenv("CEDRUS_PLANE_TRACE") != NULL) {
		pthread_t th;

		K.tr = calloc(400000, sizeof(*K.tr));
		EVS = calloc(400000, sizeof(*EVS));
		if (K.have_regs && getenv("CEDRUS_PLANE_SAMPLER") != NULL) {
			SM = calloc(2000000, sizeof(*SM));
			sm_regs = (uint32_t *)K.reg;
			pthread_create(&th, NULL, sampler, NULL);
		}
	}
	if (K.tr != NULL && K.frames < 400000) {
		K.tr[K.frames].t_in = now_s() - K.t0;
		K.tr[K.frames].vb_in = vblanks();
	}
	if (K.waiting)
		wait_event();
	if (K.tr != NULL && K.frames < 400000)
		K.tr[K.frames].t_waited = now_s() - K.t0;
	if (K.pending >= 0)
		confirmed(K.pending);
	if (K.check_sums)
		K.sum[cap] = bufsum(cap);
	K.waiting = 1;
	if (commit(cap, 1, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, &K.waiting) != 0) {
		fprintf(stderr, "atomic commit of buffer %u: %s\n", cap, strerror(errno));
		K.waiting = 0;
		return (1);
	}
	if (K.tr != NULL && K.frames < 400000) {
		K.tr[K.frames].t_commit = now_s() - K.t0;
		K.tr[K.frames].vb_commit = vblanks();
	}
	K.state[cap] = 1;
	K.pending = cap;
	K.frames++;
	return (now_s() - K.t0 >= K.seconds);
}
