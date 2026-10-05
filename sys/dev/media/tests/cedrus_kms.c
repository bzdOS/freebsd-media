/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * cedrus_kms: video from the engine to the screen, no copy on the way.
 *
 * cedrus_mpeg2.c decodes the stream over and over through V4L2; for every decoded
 * frame it calls kms_frame() below. The capture buffer is exported (VIDIOC_EXPBUF),
 * Mesa imports it as an NV12 EGLImage, a textured quad converts and draws it into a
 * gbm surface (lima, renderD128), and the finished buffer is handed to the KMS node
 * (bzkms) as a framebuffer and page-flipped, each flip waited for. Nothing reads the
 * picture back (a read-back of a scanout buffer hangs the GPU on this stack).
 *
 *   cc -DCEDRUS_KMS -I<shim> -I<uapi> -I/usr/local/include -I/usr/local/include/libdrm \
 *       -L/usr/local/lib -o cedrus_kms cedrus_kms.c -lgbm -lEGL -lGLESv2 -ldrm
 *   CEDRUS_KMS_SECONDS=10 ./cedrus_kms stream.m2v reference.yuv
 *
 * The stream must have no B pictures (frames are shown in decoding order).
 */
#define	CEDRUS_KMS_BUILD
#ifndef CEDRUS_KMS
#define	CEDRUS_KMS
#endif
#include "cedrus_mpeg2.c"

#include <errno.h>
#include <time.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define	EGL_PLATFORM_GBM_MESA	0x31D7

static struct {
	int		ready, kmsfd, mode_set;
	drmModeModeInfo	mode;
	uint32_t	crtc, conn;
	struct gbm_device *gbm;
	struct gbm_surface *surf;
	EGLDisplay	dpy;
	EGLContext	ctx;
	EGLSurface	esurf;
	GLuint		prog;
	EGLImageKHR	img[8];
	GLuint		tex[8];
	int		dmafd[8];
	uint32_t	prev_fb, prev_handle;
	struct gbm_bo	*prev_bo;
	double		t0;
	unsigned long	frames, presented, refused;
	double		seconds;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
} K;

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
	if (!ok) {
		char log[1024] = { 0 };

		glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return (sh);
}

/* The first /dev/dri/cardN with a connected connector: bzkms. */
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
		if (found)
			return (fd);
		close(fd);
	}
	return (-1);
}

static void
kms_init(void)
{
	static const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
	    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
	    EGL_BLUE_SIZE, 8, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	drmModeRes *res;
	drmModeConnector *cn = NULL;
	EGLConfig cfg;
	EGLint n = 0, vmaj, vmin;
	int i, drifd;

	setvbuf(stdout, NULL, _IOLBF, 0);
	K.seconds = getenv("CEDRUS_KMS_SECONDS") != NULL ? atof(getenv("CEDRUS_KMS_SECONDS")) : 10;
	K.kmsfd = open_kms();
	if (K.kmsfd < 0) {
		fprintf(stderr, "no KMS node with a connector (is bzkms loaded?)\n");
		exit(1);
	}
	drmSetClientCap(K.kmsfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	res = drmModeGetResources(K.kmsfd);
	for (i = 0; res != NULL && i < res->count_connectors && cn == NULL; i++) {
		drmModeConnector *c = drmModeGetConnector(K.kmsfd, res->connectors[i]);

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
	printf("KMS: connector %u, crtc %u, mode %ux%u@%u\n", K.conn, K.crtc,
	    K.mode.hdisplay, K.mode.vdisplay, K.mode.vrefresh);

	drifd = open("/dev/dri/renderD128", O_RDWR);
	if (drifd < 0)
		die("open renderD128");
	K.gbm = gbm_create_device(drifd);
	K.surf = K.gbm ? gbm_surface_create(K.gbm, K.mode.hdisplay, K.mode.vdisplay,
	    GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING) : NULL;
	if (K.surf == NULL) {
		fprintf(stderr, "gbm surface failed\n");
		exit(1);
	}
	K.dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, K.gbm, NULL);
	if (K.dpy == EGL_NO_DISPLAY || !eglInitialize(K.dpy, &vmaj, &vmin) ||
	    !eglChooseConfig(K.dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "EGL setup failed: %#x\n", eglGetError());
		exit(1);
	}
	K.esurf = eglCreateWindowSurface(K.dpy, cfg, (EGLNativeWindowType)K.surf, NULL);
	eglBindAPI(EGL_OPENGL_ES_API);
	K.ctx = eglCreateContext(K.dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (K.esurf == EGL_NO_SURFACE || K.ctx == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(K.dpy, K.esurf, K.esurf, K.ctx)) {
		fprintf(stderr, "EGL surface/context failed: %#x\n", eglGetError());
		exit(1);
	}
	printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
	K.create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	K.image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
	    eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (K.create_image == NULL || K.image_target == NULL) {
		fprintf(stderr, "no EGL image import\n");
		exit(1);
	}
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
	for (i = 0; i < 8; i++)
		K.dmafd[i] = -1;
	K.t0 = now_s();
	K.ready = 1;
}

/* The capture buffer `cap` as an external texture, imported once. */
static GLuint
cap_texture(int vfd, unsigned cap, unsigned stride, unsigned ha, unsigned w, unsigned h)
{
	struct v4l2_exportbuffer e;

	if (K.img[cap] != EGL_NO_IMAGE_KHR && K.img[cap] != NULL)
		return (K.tex[cap]);
	memset(&e, 0, sizeof(e));
	e.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	e.index = cap;
	e.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(vfd, VIDIOC_EXPBUF, &e) != 0)
		die("EXPBUF");
	K.dmafd[cap] = e.fd;
	{
		EGLint attr[] = {
			EGL_WIDTH, (EGLint)w, EGL_HEIGHT, (EGLint)h,
			EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_NV12,
			EGL_DMA_BUF_PLANE0_FD_EXT, e.fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
			EGL_DMA_BUF_PLANE1_FD_EXT, e.fd,
			EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)(stride * ha),
			EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)stride,
			EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT,
			EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
			EGL_NONE
		};

		K.img[cap] = K.create_image(K.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	}
	if (K.img[cap] == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "NV12 import of capture buffer %u failed: %#x\n", cap, eglGetError());
		exit(1);
	}
	glGenTextures(1, &K.tex[cap]);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, K.tex[cap]);
	K.image_target(GL_TEXTURE_EXTERNAL_OES, K.img[cap]);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return (K.tex[cap]);
}

int
kms_frame(int vfd, unsigned cap, unsigned stride, unsigned ha, unsigned w, unsigned h)
{
	GLfloat quad[16];
	float aw, x0;
	struct gbm_bo *bo;
	int dmafd;
	uint32_t handle = 0, fb = 0, handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

	if (!K.ready)
		kms_init();
	if (cap >= 8) {
		fprintf(stderr, "capture index %u\n", cap);
		return (1);
	}
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, cap_texture(vfd, cap, stride, ha, w, h));
	/* Keep the picture's aspect: centred, the rest black. */
	glViewport(0, 0, K.mode.hdisplay, K.mode.vdisplay);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	aw = (float)K.mode.vdisplay * w / h;
	if (aw > K.mode.hdisplay)
		aw = K.mode.hdisplay;
	x0 = (K.mode.hdisplay - aw) / 2;
	glViewport((GLint)x0, 0, (GLsizei)aw, K.mode.vdisplay);
	{
		static const GLfloat q[16] = { -1, -1, 0, 1,  1, -1, 1, 1,  -1, 1, 0, 0,  1, 1, 1, 0 };

		memcpy(quad, q, sizeof(quad));
	}
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
	if (bo == NULL || gbm_bo_get_stride(bo) != K.mode.hdisplay * 4u) {
		fprintf(stderr, "front buffer unusable\n");
		return (1);
	}
	dmafd = gbm_bo_get_fd(bo);
	if (dmafd < 0 || drmPrimeFDToHandle(K.kmsfd, dmafd, &handle) != 0) {
		fprintf(stderr, "drmPrimeFDToHandle: %s\n", strerror(errno));
		return (1);
	}
	close(dmafd);
	handles[0] = handle;
	pitches[0] = K.mode.hdisplay * 4;
	if (drmModeAddFB2(K.kmsfd, K.mode.hdisplay, K.mode.vdisplay, DRM_FORMAT_XRGB8888,
	    handles, pitches, offsets, &fb, 0) != 0) {
		fprintf(stderr, "drmModeAddFB2: %s\n", strerror(errno));
		return (1);
	}
	if (!K.mode_set) {
		if (drmModeSetCrtc(K.kmsfd, K.crtc, fb, 0, 0, &K.conn, 1, &K.mode) != 0) {
			fprintf(stderr, "drmModeSetCrtc: %s\n", strerror(errno));
			return (1);
		}
		K.mode_set = 1;
		K.presented++;
	} else {
		int waiting = 1;

		if (drmModePageFlip(K.kmsfd, K.crtc, fb, DRM_MODE_PAGE_FLIP_EVENT, &waiting) != 0) {
			if (K.refused++ == 0)
				fprintf(stderr, "drmModePageFlip: %s\n", strerror(errno));
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
	/* Retire the frame that was on screen before this one, never the one just flipped to. */
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

	if (now_s() - K.t0 >= K.seconds) {
		double dt = now_s() - K.t0;

		printf("%lu frames in %.1f s = %.1f fps, %ux%u video on a %ux%u screen; presented %lu, "
		    "refused %lu\n", K.frames, dt, K.frames / dt, w, h, K.mode.hdisplay,
		    K.mode.vdisplay, K.presented, K.refused);
		return (1);
	}
	return (0);
}
