/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * fbplay: a small player for this V4L2 stack. FFmpeg demuxes; H.264 and MPEG-2 are decoded on the video engine
 * through the v4l2-request hwaccel (DRM_PRIME frames, no CPU copy) and shown on the KMS screen exactly the way
 * ffkms does it (EGLImage NV12 import on lima, gbm scanout surface, bzkms page flips); the audio track (AAC, MP3,
 * MP2, AC-3, FLAC, Vorbis, PCM) is decoded by libavcodec, converted by libswresample to 16-bit stereo and played
 * through OSS (/dev/dsp). The audio is the master clock.
 *
 *   demux thread --> video packets --> decode thread --> frames --> main thread: wait for the frame's time, draw,
 *                \-> audio packets --> audio thread: decode, resample, write(2) to the sound device (blocks:
 *                                      that is the real-time pacing); after every write the audio clock is
 *                                      "end of the written samples minus what the device still holds" (GETODELAY).
 *
 * Video timing: the clock says when a frame is due; the page flip lands on a vblank, so the main thread works out
 * the vblank grid from the flip events it has seen, picks the vblank nearest to the due time, starts the draw a
 * little (measured render time + margin) before it and sleeps until then (clock_nanosleep, no spinning). A frame
 * whose vblank has already passed by more than half a refresh is dropped if a later frame is ready.
 *
 *   fbplay [options] file
 *     -s sec   stop after sec seconds           -l n    play the file n more times (loop);  -L loop forever
 *     -n       decode and draw, no page flip    -r fps  frame rate for raw streams (.264/.m2v), default 25
 *     -A null  audio to a dummy sink (real-time paced, no device)       -a dev  OSS device (default /dev/dsp0)
 *     -N       ignore the audio track           -q n    decoded frames queued ahead (default 2)
 *     -H file  write "pts md5" of every presented frame (NV12, cropped) -D file  dump every presented frame, raw NV12
 *     -k x     the sound device really runs at x times its nominal rate (measured; this board: 0.9616), the
 *              audio is resampled to that rate so pitch and clock are right
 *     -v       log every dropped frame
 * SIGUSR1 seeks back to the start (the clock goes on), SIGINT/SIGTERM stop.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/soundcard.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/md5.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define	EGL_PLATFORM_GBM_MESA	0x31D7
#define	NIMG			64
#define	MAXERR			400000

/* ------------------------------------------------------------------------------------------------------------ */
/* common */

static volatile sig_atomic_t quit, seek_req;

static void
die(const char *m)
{
	fprintf(stderr, "fbplay: %s\n", m);
	exit(1);
}

static double
mono(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec * 1e-9);
}

static void
sleep_until(double t)
{
	struct timespec ts = { .tv_sec = (time_t)t, .tv_nsec = (long)((t - floor(t)) * 1e9) };

	clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

static double
cpu_s(void)
{
	struct rusage ru;

	getrusage(RUSAGE_SELF, &ru);
	return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* bounded queue; an item is a packet or a frame (p), a marker (kind) and the timeline offset of its pass (off) */

enum { K_DATA, K_LOOP, K_SEEK, K_EOS };

struct qi {
	struct qi	*next;
	void		*p;
	int		kind;
	double		off, pts;
};
struct q {
	pthread_mutex_t	m;
	pthread_cond_t	nonempty, nonfull;
	struct qi	*h, *t;
	int		n, max;
};

static void
q_init(struct q *q, int max)
{
	memset(q, 0, sizeof(*q));
	pthread_mutex_init(&q->m, NULL);
	pthread_cond_init(&q->nonempty, NULL);
	pthread_cond_init(&q->nonfull, NULL);
	q->max = max;
}

static int
q_push(struct q *q, void *p, int kind, double off, double pts)
{
	struct qi *i = calloc(1, sizeof(*i));
	struct timespec ts;

	i->p = p; i->kind = kind; i->off = off; i->pts = pts;
	pthread_mutex_lock(&q->m);
	while (q->n >= q->max && !quit) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50000000;
		if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&q->nonfull, &q->m, &ts);
	}
	if (quit) {
		pthread_mutex_unlock(&q->m);
		free(i);
		return (-1);
	}
	if (q->t != NULL)
		q->t->next = i;
	else
		q->h = i;
	q->t = i;
	q->n++;
	pthread_cond_signal(&q->nonempty);
	pthread_mutex_unlock(&q->m);
	return (0);
}

/* block != 0: wait for an item (NULL only on quit); otherwise NULL if the queue is empty */
static struct qi *
q_pop(struct q *q, int block)
{
	struct qi *i;
	struct timespec ts;

	pthread_mutex_lock(&q->m);
	while (q->h == NULL && block && !quit) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50000000;
		if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&q->nonempty, &q->m, &ts);
	}
	if ((i = q->h) != NULL) {
		q->h = i->next;
		if (q->h == NULL)
			q->t = NULL;
		q->n--;
		pthread_cond_signal(&q->nonfull);
	}
	pthread_mutex_unlock(&q->m);
	return (i);
}

static void
q_clear(struct q *q, int frames)
{
	struct qi *i;

	while ((i = q_pop(q, 0)) != NULL) {
		if (i->p != NULL) {
			if (frames) {
				AVFrame *f = i->p;
				av_frame_free(&f);
			} else {
				AVPacket *pk = i->p;
				av_packet_free(&pk);
			}
		}
		free(i);
	}
}

static int
q_len(struct q *q)
{
	return (q->n);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* global state */

static struct {
	AVFormatContext	*fc;
	int		vs, as;
	AVStream	*vst, *ast;
	AVCodecContext	*vcc, *acc;
	struct q	vq, aq, fq;
	int		loops;			/* further passes; -1: forever */
	int		noaudio, anull, noflip;
	const char	*adev;
	double		fps;
	char		*rawfps;
	FILE		*hashf, *dumpf;
	int		verbose;
	/* the clock */
	pthread_mutex_t	cm;
	pthread_cond_t	cc;
	int		clk_valid, gate;
	double		clk_pts, clk_t;		/* the audio clock was clk_pts at monotonic time clk_t */
	/* audio stats */
	unsigned long	a_frames, a_underruns, a_samples;
	double		a_clk_back;		/* largest backwards step of the clock at an update */
	int		a_done, a_rate;
	double		a_k;			/* measured device speed / nominal speed (-k) */
	double		a_eff;			/* samples the device really plays per second */
	double		a_odelay_max;
	/* demux stats */
	int		passes;
	/* video stats */
	unsigned long	decoded;
} G;

/* master clock in timeline seconds, NAN until the audio has started */
static double
clock_now(void)
{
	double c;

	pthread_mutex_lock(&G.cm);
	c = G.clk_valid ? G.clk_pts + (mono() - G.clk_t) : NAN;
	pthread_mutex_unlock(&G.cm);
	return (c);
}

static void
clock_set(double pts)
{
	double t = mono();

	pthread_mutex_lock(&G.cm);
	if (G.clk_valid) {
		double old = G.clk_pts + (t - G.clk_t);

		if (old - pts > G.a_clk_back)
			G.a_clk_back = old - pts;
	}
	G.clk_pts = pts;
	G.clk_t = t;
	G.clk_valid = 1;
	pthread_mutex_unlock(&G.cm);
}

static void
gate_open(void)
{
	pthread_mutex_lock(&G.cm);
	G.gate = 1;
	pthread_cond_broadcast(&G.cc);
	pthread_mutex_unlock(&G.cm);
}

static void
gate_wait(void)
{
	struct timespec ts;

	pthread_mutex_lock(&G.cm);
	while (!G.gate && !quit) {
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50000000;
		if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&G.cc, &G.cm, &ts);
	}
	pthread_mutex_unlock(&G.cm);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* demux thread */

static double
pkt_end(const AVPacket *p, const AVStream *st, double off)
{
	int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;

	if (ts == AV_NOPTS_VALUE)
		return (-1e30);
	return ((ts + (p->duration > 0 ? p->duration : 0)) * av_q2d(st->time_base) + off);
}

static int
rewind_input(void)
{
	int64_t st = G.fc->start_time == AV_NOPTS_VALUE ? 0 : G.fc->start_time;

	if (avformat_seek_file(G.fc, -1, INT64_MIN, st, st, 0) >= 0)
		return (0);
	return (av_seek_frame(G.fc, -1, 0, AVSEEK_FLAG_BYTE));
}

static void *
demux_main(void *arg)
{
	AVPacket *pkt = av_packet_alloc();
	double start = G.fc->start_time == AV_NOPTS_VALUE ? 0 : G.fc->start_time * 1e-6;
	double off, vend = -1e30, aend = -1e30, e;
	int loops = G.loops, ret;

	off = 0;
	while (!quit) {
		if (seek_req) {
			seek_req = 0;
			q_clear(&G.vq, 0);
			q_clear(&G.aq, 0);
			off = clock_now();
			if (isnan(off))
				off = 0;
			off += 0.1 - start;
			vend = aend = -1e30;
			if (rewind_input() < 0)
				fprintf(stderr, "fbplay: seek to start failed\n");
			q_push(&G.vq, NULL, K_SEEK, off, 0);
			if (G.as >= 0)
				q_push(&G.aq, NULL, K_SEEK, off, 0);
			continue;
		}
		ret = av_read_frame(G.fc, pkt);
		if (ret < 0) {
			if (ret != AVERROR_EOF)
				fprintf(stderr, "fbplay: read error %d, treating as the end\n", ret);
			if (loops != 0) {
				if (loops > 0)
					loops--;
				off = (vend > aend ? vend : aend) - start;
				if (off < -1e29)
					off = 0;
				if (rewind_input() < 0) {
					fprintf(stderr, "fbplay: cannot loop: seek to start failed\n");
					loops = 0;
					continue;
				}
				G.passes++;
				q_push(&G.vq, NULL, K_LOOP, off, 0);
				if (G.as >= 0)
					q_push(&G.aq, NULL, K_LOOP, off, 0);
				continue;
			}
			q_push(&G.vq, NULL, K_EOS, 0, 0);
			if (G.as >= 0)
				q_push(&G.aq, NULL, K_EOS, 0, 0);
			break;
		}
		if (pkt->stream_index == G.vs) {
			if ((e = pkt_end(pkt, G.vst, off)) > vend)
				vend = e;
			if (q_push(&G.vq, pkt, K_DATA, off, 0) < 0)
				break;
			pkt = av_packet_alloc();
		} else if (pkt->stream_index == G.as) {
			if ((e = pkt_end(pkt, G.ast, off)) > aend)
				aend = e;
			if (q_push(&G.aq, pkt, K_DATA, off, 0) < 0)
				break;
			pkt = av_packet_alloc();
		} else {
			av_packet_unref(pkt);
		}
	}
	return (NULL);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* video decode thread */

static int
frame_out(AVFrame *fr, double *off, double *last, double dur)
{
	int64_t ts = fr->best_effort_timestamp;
	double pts;

	if (ts == AV_NOPTS_VALUE)
		ts = fr->pts;
	pts = ts == AV_NOPTS_VALUE ? *last + dur : ts * av_q2d(G.vst->time_base) + *off;
	*last = pts;
	G.decoded++;
	if (fr->format != AV_PIX_FMT_DRM_PRIME)
		die("the decoder did not give DRM_PRIME frames");
	return (q_push(&G.fq, fr, K_DATA, 0, pts));
}

static int
drain_frames(double *off, double *last, double dur)
{
	AVFrame *fr;
	int ret;

	for (;;) {
		fr = av_frame_alloc();
		ret = avcodec_receive_frame(G.vcc, fr);
		if (ret < 0) {
			av_frame_free(&fr);
			return (ret);
		}
		if (frame_out(fr, off, last, dur) < 0)
			return (-1);
	}
}

static void *
vdec_main(void *arg)
{
	struct qi *i;
	double off = 0, last = -1.0 / G.fps, dur = 1.0 / G.fps;
	int ret;

	while ((i = q_pop(&G.vq, 1)) != NULL) {
		if (i->kind == K_DATA) {
			AVPacket *pk = i->p;

			for (;;) {
				ret = avcodec_send_packet(G.vcc, pk);
				if (ret == AVERROR(EAGAIN)) {
					if (drain_frames(&off, &last, dur) < 0 && quit)
						break;
					continue;
				}
				break;
			}
			if (ret < 0 && ret != AVERROR(EAGAIN))
				fprintf(stderr, "fbplay: send_packet: %d\n", ret);
			av_packet_free(&pk);
			if (drain_frames(&off, &last, dur) == -1)
				break;
		} else if (i->kind == K_SEEK) {
			avcodec_flush_buffers(G.vcc);
			q_clear(&G.fq, 1);
			off = i->off;
			last = off - dur;
		} else if (i->kind == K_LOOP || i->kind == K_EOS) {
			avcodec_send_packet(G.vcc, NULL);
			if (drain_frames(&off, &last, dur) == -1)
				break;
			if (i->kind == K_EOS) {
				q_push(&G.fq, NULL, K_EOS, 0, 0);
				free(i);
				break;
			}
			avcodec_flush_buffers(G.vcc);
			off = i->off;
			last = off - dur;
		}
		free(i);
	}
	return (NULL);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* audio thread: decode, resample to S16 stereo, write to OSS (or a paced dummy), keep the clock */

static int adevfd = -1;
static double a_vend;		/* dummy sink: monotonic time at which everything written has been played */

static int
audio_open(void)
{
	int v;

	G.a_rate = 48000;
	if (G.anull) {
		G.a_eff = G.a_rate;
		return (0);
	}
	if ((adevfd = open(G.adev, O_WRONLY)) < 0) {
		fprintf(stderr, "fbplay: open %s: %s\n", G.adev, strerror(errno));
		return (-1);
	}
	v = AFMT_S16_LE;
	if (ioctl(adevfd, SNDCTL_DSP_SETFMT, &v) < 0 || v != AFMT_S16_LE)
		fprintf(stderr, "fbplay: SETFMT: got %d\n", v);
	v = 2;
	if (ioctl(adevfd, SNDCTL_DSP_CHANNELS, &v) < 0 || v != 2)
		fprintf(stderr, "fbplay: CHANNELS: got %d\n", v);
	v = G.a_rate;
	if (ioctl(adevfd, SNDCTL_DSP_SPEED, &v) < 0)
		fprintf(stderr, "fbplay: SPEED failed\n");
	else
		G.a_rate = v;
	G.a_eff = G.a_rate * G.a_k;
	return (0);
}

/* seconds of sound the device still holds */
static double
audio_delay(void)
{
	int bytes = 0;

	if (G.anull) {
		double d = a_vend - mono();

		return (d > 0 ? d : 0);
	}
	if (ioctl(adevfd, SNDCTL_DSP_GETODELAY, &bytes) < 0)
		return (0);
	return ((double)bytes / (G.a_eff * 4.0));
}

static void
audio_write(const uint8_t *b, size_t n)
{
	if (G.anull) {
		double d = n / (G.a_eff * 4.0), now = mono();

		if (a_vend < now) {
			if (G.clk_valid)
				G.a_underruns++;
			a_vend = now;
		}
		if (a_vend - now > 0.25 - d)
			sleep_until(a_vend - (0.25 - d));
		a_vend += d;
		return;
	}
	while (n > 0 && !quit) {
		ssize_t w = write(adevfd, b, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "fbplay: audio write: %s\n", strerror(errno));
			return;
		}
		b += w;
		n -= w;
	}
}

static void *
audio_main(void *arg)
{
	struct qi *i;
	AVFrame *fr = av_frame_alloc();
	SwrContext *swr = NULL;
	AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO, in_cl = { 0 };
	int in_fmt = -1, in_rate = 0, ret, started = 0;
	double off = 0, a_end = 0;
	uint8_t *buf = NULL;
	int bufn = 0;

	if (audio_open() < 0) {
		G.a_done = 1;
		return (NULL);
	}
	while ((i = q_pop(&G.aq, 1)) != NULL) {
		if (i->kind == K_EOS) {
			free(i);
			break;
		}
		if (i->kind == K_LOOP || i->kind == K_SEEK) {
			avcodec_flush_buffers(G.acc);
			if (swr != NULL)
				swr_close(swr), swr_init(swr);
			if (i->kind == K_SEEK && adevfd >= 0)
				ioctl(adevfd, SNDCTL_DSP_RESET, 0);
			if (i->kind == K_SEEK)
				a_vend = 0;
			off = i->off;
			free(i);
			continue;
		}
		AVPacket *pk = i->p;

		ret = avcodec_send_packet(G.acc, pk);
		av_packet_free(&pk);
		free(i);
		if (ret < 0 && ret != AVERROR(EAGAIN))
			continue;
		while (avcodec_receive_frame(G.acc, fr) >= 0) {
			int64_t ts = fr->best_effort_timestamp;
			int outmax, n;
			double pts, delay_out;

			if (swr == NULL || fr->format != in_fmt || fr->sample_rate != in_rate ||
			    av_channel_layout_compare(&fr->ch_layout, &in_cl) != 0) {
				swr_free(&swr);
				av_channel_layout_uninit(&in_cl);
				av_channel_layout_copy(&in_cl, &fr->ch_layout);
				in_fmt = fr->format;
				in_rate = fr->sample_rate;
				if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, (int)(G.a_eff + 0.5), &in_cl, in_fmt, in_rate,
				    0, NULL) < 0 || swr_init(swr) < 0)
					die("swresample init");
			}
			outmax = (int)av_rescale_rnd(swr_get_delay(swr, in_rate) + fr->nb_samples, (int)(G.a_eff + 0.5), in_rate,
			    AV_ROUND_UP) + 64;
			if (outmax * 4 > bufn) {
				bufn = outmax * 4;
				buf = realloc(buf, bufn);
			}
			n = swr_convert(swr, &buf, outmax, (const uint8_t **)fr->extended_data, fr->nb_samples);
			if (n <= 0)
				continue;
			pts = ts == AV_NOPTS_VALUE ? a_end : ts * av_q2d(G.ast->time_base) + off;
			delay_out = swr_get_delay(swr, (int)(G.a_eff + 0.5)) / G.a_eff;
			a_end = pts + fr->nb_samples / (double)in_rate;
			if (!started) {
				gate_wait();		/* the first picture is decoded: start the sound and the clock together */
				started = 1;
			}
			{
				double before = audio_delay();

				if (started == 1 && before <= 0)
					started = 2;
				else if (before <= 0 && G.clk_valid)
					G.a_underruns++;
			}
			audio_write(buf, (size_t)n * 4);
			{
				double d = audio_delay();

				if (d > G.a_odelay_max)
					G.a_odelay_max = d;
				clock_set(a_end - delay_out - d);
			}
			G.a_frames++;
			G.a_samples += n;
			av_frame_unref(fr);
		}
	}
	/* end of the stream: flush the resampler, let the device play out */
	if (swr != NULL && !quit) {
		int n = swr_convert(swr, &buf, bufn / 4, NULL, 0);

		if (n > 0)
			audio_write(buf, (size_t)n * 4);
	}
	if (!quit) {
		if (G.anull)
			sleep_until(a_vend);
		else if (adevfd >= 0)
			ioctl(adevfd, SNDCTL_DSP_SYNC, 0);
	}
	G.a_done = 1;
	return (NULL);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the screen: the same path as ffkms */

static struct {
	int		kmsfd, mode_set;
	drmModeModeInfo	mode;
	uint32_t	crtc, conn;
	struct gbm_device *gbm;
	struct gbm_surface *surf;
	EGLDisplay	dpy;
	EGLContext	ctx;
	EGLSurface	esurf;
	GLuint		prog;
	struct { int fd; EGLImageKHR img; GLuint tex; } im[NIMG];
	int		nim;
	uint32_t	prev_fb, prev_handle;
	struct gbm_bo	*prev_bo;
	unsigned long	presented, refused;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
} K;

static void
flip_done(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{

	*(int *)data = 0;
}

static GLuint
mkshader(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok)
		die("shader");
	return (sh);
}

static int
open_kms(void)
{
	char path[32];
	int i, fd;

	for (i = 0; i < 8; i++) {
		drmModeRes *res;
		int j, found = 0;

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		if ((fd = open(path, O_RDWR)) < 0)
			continue;
		if ((res = drmModeGetResources(fd)) != NULL) {
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
		if (found)
			return (fd);
		close(fd);
	}
	return (-1);
}

static void
kms_init(void)
{
	static const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
	    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	drmModeRes *res;
	drmModeConnector *cn = NULL;
	EGLConfig cfg = NULL;
	EGLint n = 0, vmaj, vmin;
	int i, drifd;

	K.kmsfd = open_kms();
	if (K.kmsfd < 0)
		die("no KMS node with a connector (is bzkms loaded?)");
	drmSetClientCap(K.kmsfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	res = drmModeGetResources(K.kmsfd);
	for (i = 0; res != NULL && i < res->count_connectors && cn == NULL; i++) {
		drmModeConnector *c = drmModeGetConnector(K.kmsfd, res->connectors[i]);

		if (c != NULL && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
			cn = c;
		else if (c != NULL)
			drmModeFreeConnector(c);
	}
	if (cn == NULL || res->count_crtcs < 1)
		die("no connector or CRTC");
	K.conn = cn->connector_id;
	K.mode = cn->modes[0];
	K.crtc = res->crtcs[0];
	printf("KMS: connector %u, crtc %u, mode %ux%u@%u\n", K.conn, K.crtc, K.mode.hdisplay, K.mode.vdisplay,
	    K.mode.vrefresh);
	if ((drifd = open("/dev/dri/renderD128", O_RDWR)) < 0)
		die("open renderD128");
	K.gbm = gbm_create_device(drifd);
	K.surf = K.gbm ? gbm_surface_create(K.gbm, K.mode.hdisplay, K.mode.vdisplay, GBM_FORMAT_XRGB8888,
	    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING) : NULL;
	if (K.surf == NULL)
		die("gbm surface");
	K.dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, K.gbm, NULL);
	if (K.dpy == EGL_NO_DISPLAY || !eglInitialize(K.dpy, &vmaj, &vmin) ||
	    !eglChooseConfig(K.dpy, cfg_attr, &cfg, 1, &n) || n < 1)
		die("EGL setup");
	K.esurf = eglCreateWindowSurface(K.dpy, cfg, (EGLNativeWindowType)K.surf, NULL);
	eglBindAPI(EGL_OPENGL_ES_API);
	K.ctx = eglCreateContext(K.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (K.esurf == EGL_NO_SURFACE || K.ctx == EGL_NO_CONTEXT || !eglMakeCurrent(K.dpy, K.esurf, K.esurf, K.ctx))
		die("EGL surface/context");
	printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
	K.create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	K.image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (K.create_image == NULL || K.image_target == NULL)
		die("no EGL image import");
	K.prog = glCreateProgram();
	glAttachShader(K.prog, mkshader(GL_VERTEX_SHADER,
	    "attribute vec2 pos; attribute vec2 uv; varying vec2 v;\n"
	    "void main() { gl_Position = vec4(pos, 0.0, 1.0); v = uv; }\n"));
	glAttachShader(K.prog, mkshader(GL_FRAGMENT_SHADER,
	    "#extension GL_OES_EGL_image_external : require\n"
	    "precision mediump float; varying vec2 v; uniform samplerExternalOES tex;\n"
	    "void main() { gl_FragColor = vec4(texture2D(tex, v).rgb, 1.0); }\n"));
	glBindAttribLocation(K.prog, 0, "pos");
	glBindAttribLocation(K.prog, 1, "uv");
	glLinkProgram(K.prog);
	glUseProgram(K.prog);
	glUniform1i(glGetUniformLocation(K.prog, "tex"), 0);
	for (i = 0; i < NIMG; i++)
		K.im[i].fd = -1;
}

/* The frame's dma-buf as an external texture, imported once per file descriptor. */
static GLuint
frame_texture(const AVDRMFrameDescriptor *d, unsigned w, unsigned h)
{
	const AVDRMLayerDescriptor *l = &d->layers[0];
	int fd = d->objects[l->planes[0].object_index].fd, i;

	for (i = 0; i < NIMG && K.im[i].fd != -1; i++)
		if (K.im[i].fd == fd)
			return (K.im[i].tex);
	if (i == NIMG)
		die("too many distinct frame buffers");
	{
		EGLint attr[] = {
			EGL_WIDTH, (EGLint)w, EGL_HEIGHT, (EGLint)h,
			EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_NV12,
			EGL_DMA_BUF_PLANE0_FD_EXT, fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)l->planes[0].offset,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)l->planes[0].pitch,
			EGL_DMA_BUF_PLANE1_FD_EXT, d->objects[l->planes[1].object_index].fd,
			EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)l->planes[1].offset,
			EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)l->planes[1].pitch,
			EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT,
			EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
			EGL_NONE
		};

		K.im[i].img = K.create_image(K.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	}
	if (K.im[i].img == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "NV12 import of dma-buf %d failed: %#x\n", fd, eglGetError());
		exit(1);
	}
	K.im[i].fd = fd;
	K.nim++;
	glGenTextures(1, &K.im[i].tex);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, K.im[i].tex);
	K.image_target(GL_TEXTURE_EXTERNAL_OES, K.im[i].img);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return (K.im[i].tex);
}

/*
 * Draw the frame, queue the page flip and wait for it. *t_issue is the monotonic time at which the flip was
 * requested (the draw is done by then), *t_ev the time the flip event arrived (the picture is on the screen).
 */
static int
show(const AVFrame *f, double *t_issue, double *t_ev)
{
	const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f->data[0];
	GLfloat quad[16] = { -1, -1, 0, 1,  1, -1, 1, 1,  -1, 1, 0, 0,  1, 1, 1, 0 };
	struct gbm_bo *bo;
	int dmafd;
	float aw, x0;
	uint32_t handle = 0, fb = 0, handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, frame_texture(d, f->width, f->height));
	glViewport(0, 0, K.mode.hdisplay, K.mode.vdisplay);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	aw = (float)K.mode.vdisplay * f->width / f->height;
	if (aw > K.mode.hdisplay)
		aw = K.mode.hdisplay;
	x0 = (K.mode.hdisplay - aw) / 2;
	glViewport((GLint)x0, 0, (GLsizei)aw, K.mode.vdisplay);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (!eglSwapBuffers(K.dpy, K.esurf)) {
		fprintf(stderr, "eglSwapBuffers: %#x\n", eglGetError());
		return (1);
	}
	bo = gbm_surface_lock_front_buffer(K.surf);
	if (bo == NULL || gbm_bo_get_stride(bo) != K.mode.hdisplay * 4u)
		die("front buffer unusable");
	dmafd = gbm_bo_get_fd(bo);
	if (dmafd < 0 || drmPrimeFDToHandle(K.kmsfd, dmafd, &handle) != 0)
		die("drmPrimeFDToHandle");
	close(dmafd);
	handles[0] = handle;
	pitches[0] = K.mode.hdisplay * 4;
	if (drmModeAddFB2(K.kmsfd, K.mode.hdisplay, K.mode.vdisplay, DRM_FORMAT_XRGB8888, handles, pitches, offsets,
	    &fb, 0) != 0)
		die("drmModeAddFB2");
	*t_issue = mono();
	if (G.noflip) {
		K.presented++;
		*t_ev = mono();
	} else if (!K.mode_set) {
		if (drmModeSetCrtc(K.kmsfd, K.crtc, fb, 0, 0, &K.conn, 1, &K.mode) != 0)
			die("drmModeSetCrtc");
		K.mode_set = 1;
		K.presented++;
		*t_ev = mono();
	} else {
		int waiting = 1;

		if (drmModePageFlip(K.kmsfd, K.crtc, fb, DRM_MODE_PAGE_FLIP_EVENT, &waiting) != 0) {
			K.refused++;
			*t_ev = mono();
		} else {
			struct pollfd pfd = { .fd = K.kmsfd, .events = POLLIN };
			drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };

			while (waiting) {
				if (poll(&pfd, 1, 1000) <= 0)
					break;
				drmHandleEvent(K.kmsfd, &ev);
			}
			*t_ev = mono();
			K.presented++;
		}
	}
	if (K.prev_fb != 0) {
		struct drm_gem_close gc = { .handle = K.prev_handle };

		drmModeRmFB(K.kmsfd, K.prev_fb);
		drmIoctl(K.kmsfd, DRM_IOCTL_GEM_CLOSE, &gc);
	}
	if (K.prev_bo != NULL)
		gbm_surface_release_buffer(K.surf, K.prev_bo);
	K.prev_fb = fb;
	K.prev_handle = handle;
	K.prev_bo = bo;
	return (0);
}

static enum AVPixelFormat
get_format(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
	for (; *fmts != AV_PIX_FMT_NONE; fmts++)
		if (*fmts == AV_PIX_FMT_DRM_PRIME)
			return (*fmts);
	return (AV_PIX_FMT_NONE);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* frame check: md5 / raw dump of what is about to be submitted, read back from the decoder's dma-buf */

static void
check_frame(const AVFrame *f, double pts)
{
	AVFrame *t = av_frame_alloc();
	struct AVMD5 *md5 = av_md5_alloc();
	uint8_t sum[16];
	int y, w, h;

	if (av_hwframe_transfer_data(t, f, 0) < 0 || t->format != AV_PIX_FMT_NV12) {
		fprintf(stderr, "fbplay: frame readback failed\n");
		av_frame_free(&t);
		av_free(md5);
		return;
	}
	w = f->width;
	h = f->height;
	av_md5_init(md5);
	for (y = 0; y < h; y++)
		av_md5_update(md5, t->data[0] + (size_t)y * t->linesize[0], w);
	for (y = 0; y < (h + 1) / 2; y++)
		av_md5_update(md5, t->data[1] + (size_t)y * t->linesize[1], (w + 1) / 2 * 2);
	av_md5_final(md5, sum);
	if (G.hashf != NULL) {
		fprintf(G.hashf, "%.6f ", pts);
		for (y = 0; y < 16; y++)
			fprintf(G.hashf, "%02x", sum[y]);
		fprintf(G.hashf, "\n");
	}
	if (G.dumpf != NULL) {
		for (y = 0; y < h; y++)
			fwrite(t->data[0] + (size_t)y * t->linesize[0], 1, w, G.dumpf);
		for (y = 0; y < (h + 1) / 2; y++)
			fwrite(t->data[1] + (size_t)y * t->linesize[1], 1, (w + 1) / 2 * 2, G.dumpf);
	}
	av_frame_free(&t);
	av_free(md5);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* the presenter (main thread) */

static double err_a[MAXERR];
static unsigned nerr;

static int
dcmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x < y ? -1 : x > y);
}

static void
present_all(double t_begin, double maxsecs)
{
	struct qi *cur = NULL, *nxt = NULL;
	AVFrame *prevf = NULL;
	double P = 1.0 / (K.mode.vrefresh ? K.mode.vrefresh : 60), last_ev = mono(), r_est = 0.006, tpts;
	double next_log = 0, last_shown_pts = NAN, last_shown_t = 0, max_gap = 0, play0 = 0, c = NAN, freerun_off = NAN;
	unsigned long shown = 0, dropped = 0, late = 0, early = 0, gaps = 0, vmiss = 0;
	double pe_sum = 0;
	int first = 1;

	for (;;) {
		double now, V, S;
		int k;

		if (quit || (maxsecs > 0 && mono() - t_begin > maxsecs))
			break;
		if (cur == NULL) {
			if ((cur = q_pop(&G.fq, 1)) == NULL)
				break;
			if (cur->kind == K_EOS) {
				free(cur);
				cur = NULL;
				break;
			}
		}
		if (first) {
			gate_open();		/* the audio may start now */
			first = 0;
		}
		/* the clock: audio, or free-running from the first frame if the audio does not come */
		c = clock_now();
		if (isnan(c)) {
			if (G.as >= 0 && mono() - t_begin < 5) {
				usleep(2000);
				continue;
			}
			if (isnan(freerun_off)) {
				freerun_off = mono() - cur->pts;
				fprintf(stderr, "fbplay: no audio clock, free-running\n");
			}
			pthread_mutex_lock(&G.cm);
			G.clk_pts = cur->pts;
			G.clk_t = mono();
			G.clk_valid = 1;
			pthread_mutex_unlock(&G.cm);
			continue;
		}
		now = mono();
		if (play0 == 0) {
			play0 = now;
			next_log = now + 1;
		}
		/* log once a second: the audio clock minus the pts of the picture on the screen */
		if (now >= next_log) {
			double cpu = cpu_s();
			static double cpu_prev, t_prev;

			printf("t=%5.1f clk=%8.3f vpts=%8.3f clk-vpts=%+7.3f shown=%lu drop=%lu late=%lu aund=%lu fq=%d vq=%d "
			    "aq=%d cpu=%.0f%%\n", now - play0, c, last_shown_pts, c - last_shown_pts, shown, dropped, late,
			    G.a_underruns, q_len(&G.fq), q_len(&G.vq), q_len(&G.aq),
			    t_prev ? 100 * (cpu - cpu_prev) / (now - t_prev) : 0.0);
			fflush(stdout);
			cpu_prev = cpu;
			t_prev = now;
			next_log += 1;
			if (next_log < now)
				next_log = now + 1;
		}
		if (nxt == NULL) {
			nxt = q_pop(&G.fq, 0);
			if (nxt != NULL && nxt->kind == K_EOS) {
				free(nxt);
				nxt = NULL;
			}
		}
		tpts = now + (cur->pts - c);			/* monotonic time at which the clock reaches this frame */
		k = (int)ceil((tpts - 0.5 * P - last_ev) / P);
		if (k < 1)
			k = 1;
		V = last_ev + k * P;				/* the vblank this frame should land on */
		S = V - (r_est + 0.004);			/* when to start drawing it */
		if (S < now - 0.002) {			/* too late for this vblank (a wake-up a little after S is fine) */
			k += (int)ceil((now - 0.002 - S) / P);
			V = last_ev + k * P;
			S = V - (r_est + 0.004);
		}
		/* a later frame that is already due by then replaces this one */
		if (nxt != NULL && V > now + (nxt->pts - c) - 0.5 * P + 1e-4 && !G.noflip) {
			AVFrame *df = cur->p;

			if (G.verbose)
				printf("drop pts %.3f (clock %.3f, vblank in %.1f ms, next %.3f)\n", cur->pts, c,
				    (V - now) * 1e3, nxt->pts);
			av_frame_free(&df);
			free(cur);
			cur = nxt;
			nxt = NULL;
			dropped++;
			continue;
		}
		if (S - now > 0.0004) {
			double w = S - now > 0.05 ? now + 0.05 : S;

			sleep_until(w);
			continue;
		}
		/* draw, flip, wait for the vblank */
		{
			AVFrame *f = cur->p;
			double ti, te, err;

			if (G.hashf != NULL || G.dumpf != NULL)
				check_frame(f, cur->pts);
			if (show(f, &ti, &te))
				break;
			r_est = 0.9 * r_est + 0.1 * (ti - now);
			if (te > V + 0.5 * P)
				vmiss++;
			if (!G.noflip)
				last_ev = te;
			err = clock_now() - cur->pts;		/* how late the picture reached the screen, against the clock */
			if (nerr < MAXERR)
				err_a[nerr++] = err;
			pe_sum += err;
			if (err > P * 1.25)
				late++;
			else if (err < -P * 1.25)
				early++;
			if (!isnan(last_shown_pts) && te - last_shown_t > max_gap)
				max_gap = te - last_shown_t;
			if (!isnan(last_shown_pts) && te - last_shown_t > 3.5 * P &&
			    cur->pts - last_shown_pts < 1.5 * (te - last_shown_t))
				gaps++;
			last_shown_pts = cur->pts;
			last_shown_t = te;
			shown++;
			if (prevf != NULL)
				av_frame_free(&prevf);
			prevf = f;
			free(cur);
			cur = nxt;
			nxt = NULL;
		}
	}
	{
		double t_end = mono(), s2 = 0, mean = nerr ? pe_sum / nerr : 0;
		unsigned i;

		for (i = 0; i < nerr; i++)
			s2 += (err_a[i] - mean) * (err_a[i] - mean);
		qsort(err_a, nerr, sizeof(double), dcmp);
		printf("PLAYED %.2f s: decoded %lu, presented %lu, dropped %lu, flips refused %lu, passes %d\n",
		    t_end - play0, G.decoded, shown, dropped, K.refused, G.passes + 1);
		if (nerr)
			printf("FLIP-ERR (clock at the flip event minus frame pts, ms): mean %+.2f sd %.2f min %+.2f p5 %+.2f "
			    "p50 %+.2f p95 %+.2f max %+.2f; late(>1.25 refresh) %lu early %lu vblank-missed %lu "
			    "max-interval-between-flips %.1f ms (%lu intervals >3.5 refresh); refresh %.2f ms\n",
			    mean * 1e3, sqrt(s2 / nerr) * 1e3, err_a[0] * 1e3, err_a[nerr / 20] * 1e3, err_a[nerr / 2] * 1e3,
			    err_a[nerr - 1 - nerr / 20] * 1e3, err_a[nerr - 1] * 1e3, late, early, vmiss, max_gap * 1e3,
			    gaps, P * 1e3);
		printf("AUDIO: %lu frames, %lu samples (%.2f s at %d Hz), underruns %lu, largest backward clock step %.1f ms, "
		    "largest device delay %.1f ms\n", G.a_frames, G.a_samples, G.a_samples / G.a_eff, (int)(G.a_eff + 0.5),
		    G.a_underruns, G.a_clk_back * 1e3, G.a_odelay_max * 1e3);
		printf("EGL images: %d  CPU total %.2f s over %.2f s wall (%.0f%% of one core)\n", K.nim, cpu_s(),
		    t_end - t_begin, 100 * cpu_s() / (t_end - t_begin));
	}
}

static void
onsig(int s)
{

	if (s == SIGUSR1)
		seek_req = 1;
	else
		quit = 1;
}

int
main(int argc, char **argv)
{
	const AVCodec *vcodec = NULL, *acodec = NULL;
	AVBufferRef *dev = NULL;
	AVDictionary *opts = NULL;
	pthread_t td, tv, ta;
	double secs = 0, t_begin;
	int ch, qdepth = 2;

	G.fps = 25;
	G.a_k = 1.0;
	G.adev = "/dev/dsp0";
	while ((ch = getopt(argc, argv, "s:l:Lnr:A:a:Nq:H:D:vk:")) != -1) {
		switch (ch) {
		case 's': secs = atof(optarg); break;
		case 'l': G.loops = atoi(optarg); break;
		case 'L': G.loops = -1; break;
		case 'n': G.noflip = 1; break;
		case 'r': G.rawfps = optarg; G.fps = atof(optarg); break;
		case 'A': if (strcmp(optarg, "null") == 0) G.anull = 1; else G.adev = optarg; break;
		case 'a': G.adev = optarg; break;
		case 'N': G.noaudio = 1; break;
		case 'q': qdepth = atoi(optarg); break;
		case 'H': G.hashf = fopen(optarg, "w"); break;
		case 'D': G.dumpf = fopen(optarg, "w"); break;
		case 'v': G.verbose = 1; break;
		case 'k': G.a_k = atof(optarg); break;
		default: return (2);
		}
	}
	if (argc - optind != 1)
		die("usage: fbplay [-s sec] [-l n | -L] [-n] [-r fps] [-A null | -a dev] [-N] [-q n] [-H md5file] [-D rawfile] [-v] file");
	signal(SIGINT, onsig);
	signal(SIGTERM, onsig);
	signal(SIGUSR1, onsig);
	signal(SIGPIPE, SIG_IGN);
	pthread_mutex_init(&G.cm, NULL);
	pthread_cond_init(&G.cc, NULL);
	if (G.rawfps != NULL)
		av_dict_set(&opts, "framerate", G.rawfps, 0);
	if (avformat_open_input(&G.fc, argv[optind], NULL, &opts) < 0 || avformat_find_stream_info(G.fc, NULL) < 0)
		die("cannot open the file");
	G.vs = av_find_best_stream(G.fc, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
	G.as = G.noaudio ? -1 : av_find_best_stream(G.fc, AVMEDIA_TYPE_AUDIO, -1, G.vs, &acodec, 0);
	if (G.vs < 0 && G.as < 0)
		die("no video or audio stream");
	if (G.vs >= 0) {
		AVRational fr;

		G.vst = G.fc->streams[G.vs];
		G.vcc = avcodec_alloc_context3(vcodec);
		avcodec_parameters_to_context(G.vcc, G.vst->codecpar);
		if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_V4L2REQUEST, NULL, NULL, 0) < 0)
			die("no V4L2 Request device");
		G.vcc->hw_device_ctx = av_buffer_ref(dev);
		G.vcc->get_format = get_format;
		if (avcodec_open2(G.vcc, vcodec, NULL) < 0)
			die("cannot open the video decoder");
		fr = av_guess_frame_rate(G.fc, G.vst, NULL);
		if (fr.num > 0 && fr.den > 0)
			G.fps = av_q2d(fr);
		printf("video: %s %dx%d %.3f fps\n", vcodec->name, G.vcc->width, G.vcc->height, G.fps);
		kms_init();
	}
	if (G.as >= 0) {
		G.ast = G.fc->streams[G.as];
		G.acc = avcodec_alloc_context3(acodec);
		avcodec_parameters_to_context(G.acc, G.ast->codecpar);
		if (avcodec_open2(G.acc, acodec, NULL) < 0)
			die("cannot open the audio decoder");
		printf("audio: %s %d Hz %d ch, %s\n", acodec->name, G.acc->sample_rate, G.acc->ch_layout.nb_channels,
		    G.anull ? "dummy sink" : G.adev);
	}
	q_init(&G.vq, 256);
	q_init(&G.aq, 512);
	q_init(&G.fq, qdepth);
	if (G.vs < 0)
		G.gate = 1;
	t_begin = mono();
	pthread_create(&td, NULL, demux_main, NULL);
	if (G.vs >= 0)
		pthread_create(&tv, NULL, vdec_main, NULL);
	if (G.as >= 0)
		pthread_create(&ta, NULL, audio_main, NULL);
	if (G.vs >= 0) {
		present_all(t_begin, secs);
	} else {
		double next = mono() + 1;

		while (!quit && !G.a_done && (secs <= 0 || mono() - t_begin < secs)) {
			usleep(20000);
			if (mono() >= next) {
				printf("t=%5.1f clk=%.3f aund=%lu aq=%d\n", mono() - t_begin, clock_now(), G.a_underruns,
				    q_len(&G.aq));
				fflush(stdout);
				next += 1;
			}
		}
		printf("AUDIO: %lu frames, %.2f s, underruns %lu\n", G.a_frames, G.a_samples / (double)G.a_rate,
		    G.a_underruns);
	}
	/* the picture is done; wait for the sound to play out, unless stopped */
	if (G.as >= 0) {
		while (!G.a_done && !quit && (secs <= 0 || mono() - t_begin < secs))
			usleep(10000);
	}
	quit = 1;
	pthread_join(td, NULL);
	if (G.vs >= 0)
		pthread_join(tv, NULL);
	if (G.as >= 0)
		pthread_join(ta, NULL);
	if (G.hashf != NULL)
		fclose(G.hashf);
	if (G.dumpf != NULL)
		fclose(G.dumpf);
	return (0);
}
