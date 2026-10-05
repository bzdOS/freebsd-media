/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * prime_texture: the GPU draws a camera frame it never had a copy of.
 *
 * The camera (/dev/video0, vtest) produces XRGB8888 colour bars into a buffer
 * that is exported with VIDIOC_EXPBUF. Mesa imports that descriptor as an
 * EGLImage (EGL_LINUX_DMA_BUF_EXT: the PRIME import goes through drm-kmod's
 * dma_buf_get() and its bridge to the native dmabuf core), the image becomes a
 * texture, a textured quad is drawn into an ordinary render target, and the
 * result is read back and compared with the bars. Every step prints before it
 * runs, so a hung GPU shows up as the last line.
 *
 *   cc -I<uapi> -I<shim> -I/usr/local/include -L/usr/local/lib -o prime_texture \
 *       prime_texture.c -lgbm -lEGL -lGLESv2
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define	W	320
#define	H	240

static void
step(const char *what)
{

	printf("  -> %s\n", what);
}

static void
die(const char *what)
{

	perror(what);
	exit(1);
}

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return (s);
}

int
main(void)
{
	static const unsigned char bars[8][3] = {
		{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
		{255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
	};
	static const GLfloat quad[] = {	/* x y u v */
		-1, -1, 0, 1,
		 1, -1, 1, 1,
		-1,  1, 0, 0,
		 1,  1, 1, 0,
	};
	struct v4l2_requestbuffers rb;
	struct v4l2_exportbuffer e;
	struct v4l2_format f;
	struct v4l2_buffer b;
	struct pollfd p;
	struct gbm_device *gbm;
	EGLDisplay dpy;
	EGLContext ctx;
	EGLConfig cfg;
	EGLImageKHR img;
	EGLint n = 0, vmaj, vmin;
	GLuint src, dst, fbo, prog;
	unsigned char *px;
	unsigned type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	int cam, drifd, bad = 0, i, k;

	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
	PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

	setvbuf(stdout, NULL, _IOLBF, 0);

	step("camera: XRGB8888 320x240, two buffers, export buffer 0");
	cam = open("/dev/video0", O_RDWR);
	if (cam < 0)
		die("open /dev/video0");
	memset(&f, 0, sizeof(f));
	f.type = type;
	f.fmt.pix.width = W;
	f.fmt.pix.height = H;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_XBGR32;
	if (ioctl(cam, VIDIOC_S_FMT, &f) != 0 ||
	    f.fmt.pix.pixelformat != V4L2_PIX_FMT_XBGR32)
		die("S_FMT XBGR32");
	memset(&rb, 0, sizeof(rb));
	rb.count = 2;
	rb.type = type;
	rb.memory = V4L2_MEMORY_MMAP;
	if (ioctl(cam, VIDIOC_REQBUFS, &rb) != 0)
		die("REQBUFS");
	memset(&e, 0, sizeof(e));
	e.type = type;
	e.index = 0;
	e.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(cam, VIDIOC_EXPBUF, &e) != 0)
		die("EXPBUF");
	for (i = 0; i < 2; i++) {
		memset(&b, 0, sizeof(b));
		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (ioctl(cam, VIDIOC_QBUF, &b) != 0)
			die("QBUF");
	}
	if (ioctl(cam, VIDIOC_STREAMON, &type) != 0)
		die("STREAMON");
	p.fd = cam;
	p.events = POLLIN;
	if (poll(&p, 1, 5000) != 1)
		die("waiting for a frame");
	memset(&b, 0, sizeof(b));
	b.type = type;
	b.memory = V4L2_MEMORY_MMAP;
	if (ioctl(cam, VIDIOC_DQBUF, &b) != 0)
		die("DQBUF");
	printf("     frame %u is in buffer %u\n", b.sequence, b.index);
	if (b.index != 0) {	/* we exported buffer 0: wait for the other one to come round */
		struct v4l2_buffer q = b;

		ioctl(cam, VIDIOC_QBUF, &q);
		if (poll(&p, 1, 5000) != 1)
			die("waiting for buffer 0");
		memset(&b, 0, sizeof(b));
		b.type = type;
		b.memory = V4L2_MEMORY_MMAP;
		if (ioctl(cam, VIDIOC_DQBUF, &b) != 0 || b.index != 0)
			die("DQBUF of buffer 0");
	}

	step("GPU: open the render node, EGL on GBM");
	drifd = open("/dev/dri/renderD128", O_RDWR);
	if (drifd < 0)
		die("open renderD128");
	gbm = gbm_create_device(drifd);
	dpy = gbm ? eglGetDisplay((EGLNativeDisplayType)gbm) : EGL_NO_DISPLAY;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &vmaj, &vmin)) {
		fprintf(stderr, "EGL did not initialise: %#x\n", eglGetError());
		return 1;
	}
	{
		EGLint acfg[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
		    EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
		EGLint actx[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

		if (!eglChooseConfig(dpy, acfg, &cfg, 1, &n) || n < 1)
			die("eglChooseConfig");
		eglBindAPI(EGL_OPENGL_ES_API);
		ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, actx);
		if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE,
		    EGL_NO_SURFACE, ctx))
			die("EGL context");
	}
	eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
	    eglGetProcAddress("glEGLImageTargetTexture2DOES");
	if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
		fprintf(stderr, "no EGL image import\n");
		return 1;
	}

	step("eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) on the camera's descriptor -- the import");
	{
		EGLint attr[] = {
			EGL_WIDTH, W, EGL_HEIGHT, H,
			EGL_LINUX_DRM_FOURCC_EXT, (EGLint)GBM_FORMAT_XRGB8888,
			EGL_DMA_BUF_PLANE0_FD_EXT, e.fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, W * 4,
			EGL_NONE
		};

		img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
		if (img == EGL_NO_IMAGE_KHR) {
			fprintf(stderr, "eglCreateImageKHR failed: %#x\n", eglGetError());
			return 1;
		}
	}

	step("bind the import as a texture, make an ordinary render target");
	glGenTextures(1, &src);
	glBindTexture(GL_TEXTURE_2D, src);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenTextures(1, &dst);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, dst);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "render target incomplete\n");
		return 1;
	}
	glActiveTexture(GL_TEXTURE0);

	step("draw a textured quad that samples the camera's buffer");
	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER,
	    "attribute vec2 pos; attribute vec2 uv; varying vec2 v;\n"
	    "void main() { gl_Position = vec4(pos, 0.0, 1.0); v = uv; }\n"));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER,
	    "precision mediump float; varying vec2 v; uniform sampler2D tex;\n"
	    "void main() { gl_FragColor = vec4(texture2D(tex, v).rgb, 1.0); }\n"));
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "uv");
	glLinkProgram(prog);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "tex"), 0);
	glViewport(0, 0, W, H);
	glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	step("glFinish");
	glFinish();

	px = malloc((size_t)W * H * 4);
	if (px == NULL)
		die("malloc");
	step("glReadPixels -- if the GPU hangs, this is the last line");
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	step("glReadPixels returned");
	for (k = 0; k < 8; k++) {		/* the middle of each bar, mid height */
		const unsigned char *q = px + ((size_t)(H / 2) * W + (k * 2 + 1) * W / 16) * 4;

		if (abs(q[0] - bars[k][0]) > 6 || abs(q[1] - bars[k][1]) > 6 ||
		    abs(q[2] - bars[k][2]) > 6) {
			printf("     bar %d: got %u %u %u, want %u %u %u\n", k, q[0], q[1],
			    q[2], bars[k][0], bars[k][1], bars[k][2]);
			bad++;
		}
	}
	printf("RESULT: the GPU sampled the camera's buffer through a dmabuf: %s\n",
	    bad == 0 ? "all 8 bars correct" : "MISMATCH");

	eglDestroyImageKHR(dpy, img);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	gbm_device_destroy(gbm);
	return (bad != 0);
}
