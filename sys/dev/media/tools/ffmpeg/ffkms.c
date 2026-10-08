/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ffkms: FFmpeg decodes on the video engine through our V4L2 driver and the picture goes to the screen
 * without the CPU touching it.
 *
 * The decoder (Kwiboo's v4l2-request hwaccel in libavcodec) hands out AV_PIX_FMT_DRM_PRIME frames: an
 * AVDRMFrameDescriptor naming the dma-buf of the capture buffer and the layout of its two NV12 planes. Each
 * dma-buf is imported once as an NV12 EGLImage (Mesa on lima), a textured quad converts and scales it into a
 * gbm scanout surface, and the finished buffer is page-flipped on the KMS node (bzkms); every flip is waited
 * for. Nothing reads a picture back. Same display path as tests/cedrus_kms.c, with FFmpeg in front.
 *
 *   ffkms [-s seconds] [-n] stream.264        (-n: decode and draw, but do not flip: GPU cost only)
 * Runs the stream in a loop for `seconds` (default 10) and prints the frame rate and the CPU time used.
 */
#include <sys/types.h>
#include <sys/resource.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define	EGL_PLATFORM_GBM_MESA	0x31D7
#define	NIMG			32

static struct {
	int		kmsfd, mode_set, noflip;
	drmModeModeInfo	mode;
	uint32_t	crtc, conn;
	struct gbm_device *gbm;
	struct gbm_surface *surf;
	EGLDisplay	dpy;
	EGLContext	ctx;
	EGLSurface	esurf;
	GLuint		prog;
	struct { int fd; EGLImageKHR img; GLuint tex; } im[NIMG];
	uint32_t	prev_fb, prev_handle;
	struct gbm_bo	*prev_bo;
	unsigned long	frames, presented, refused;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
} K;

static void
die(const char *m)
{
	fprintf(stderr, "ffkms: %s\n", m);
	exit(1);
}

static double
now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (ts.tv_sec + ts.tv_nsec * 1e-9);
}

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
	glGenTextures(1, &K.im[i].tex);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, K.im[i].tex);
	K.image_target(GL_TEXTURE_EXTERNAL_OES, K.im[i].img);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return (K.im[i].tex);
}

static int
show(const AVFrame *f)
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
	K.frames++;
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
	if (K.noflip) {
		K.presented++;
	} else if (!K.mode_set) {
		if (drmModeSetCrtc(K.kmsfd, K.crtc, fb, 0, 0, &K.conn, 1, &K.mode) != 0)
			die("drmModeSetCrtc");
		K.mode_set = 1;
		K.presented++;
	} else {
		int waiting = 1;

		if (drmModePageFlip(K.kmsfd, K.crtc, fb, DRM_MODE_PAGE_FLIP_EVENT, &waiting) != 0) {
			K.refused++;
		} else {
			struct pollfd pfd = { .fd = K.kmsfd, .events = POLLIN };
			drmEventContext ev = { .version = 2, .page_flip_handler = flip_done };

			while (waiting) {
				if (poll(&pfd, 1, 1000) <= 0)
					break;
				drmHandleEvent(K.kmsfd, &ev);
			}
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

int
main(int argc, char **argv)
{
	AVFormatContext *fc = NULL;
	AVCodecContext *cc;
	const AVCodec *codec;
	AVBufferRef *dev = NULL;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	double secs = 10, t0, dt;
	struct rusage ru0, ru1;
	int ch, vs, ret;
	unsigned long decoded = 0;

	while ((ch = getopt(argc, argv, "s:n")) != -1) {
		if (ch == 's')
			secs = atof(optarg);
		else if (ch == 'n')
			K.noflip = 1;
		else
			return (2);
	}
	if (argc - optind != 1)
		die("usage: ffkms [-s seconds] [-n] stream");
	kms_init();
	if (avformat_open_input(&fc, argv[optind], NULL, NULL) < 0 || avformat_find_stream_info(fc, NULL) < 0)
		die("cannot open the stream");
	vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
	if (vs < 0)
		die("no video stream");
	cc = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(cc, fc->streams[vs]->codecpar);
	if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_V4L2REQUEST, NULL, NULL, 0) < 0)
		die("no V4L2 Request device");
	cc->hw_device_ctx = av_buffer_ref(dev);
	cc->get_format = get_format;
	if (avcodec_open2(cc, codec, NULL) < 0)
		die("cannot open the decoder");
	printf("%s %dx%d\n", codec->name, cc->width, cc->height);
	getrusage(RUSAGE_SELF, &ru0);
	t0 = now_s();
	while ((dt = now_s() - t0) < secs) {
		ret = av_read_frame(fc, pkt);
		if (ret == AVERROR_EOF) {
			avcodec_flush_buffers(cc);
			av_seek_frame(fc, vs, 0, AVSEEK_FLAG_BYTE);
			continue;
		}
		if (ret < 0)
			break;
		if (pkt->stream_index == vs && avcodec_send_packet(cc, pkt) >= 0)
			while (avcodec_receive_frame(cc, fr) >= 0) {
				decoded++;
				if (fr->format != AV_PIX_FMT_DRM_PRIME)
					die("the decoder did not give DRM_PRIME frames");
				if (show(fr))
					return (1);
				av_frame_unref(fr);
			}
		av_packet_unref(pkt);
	}
	dt = now_s() - t0;
	getrusage(RUSAGE_SELF, &ru1);
	printf("%lu frames decoded, %lu shown (%lu flips refused) in %.2f s = %.1f fps; CPU user %.2f s, system %.2f s "
	    "(%.0f%% of one core); %s on a %ux%u screen\n", decoded, K.presented, K.refused, dt, K.presented / dt,
	    (ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) * 1e-6,
	    (ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) * 1e-6,
	    100 * (((ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec) + (ru1.ru_utime.tv_usec - ru0.ru_utime.tv_usec) * 1e-6) +
	    ((ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec) + (ru1.ru_stime.tv_usec - ru0.ru_stime.tv_usec) * 1e-6)) / dt,
	    K.noflip ? "no flips" : "page flips", K.mode.hdisplay, K.mode.vdisplay);
	return (0);
}
