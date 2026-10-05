/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * cedrus_gpu: the video engine decodes a picture, the GPU draws it from the
 * decoder's own buffer, no copy in between.
 *
 * cedrus_mpeg2.c decodes the first picture of a stream through V4L2 into an
 * NV12 capture buffer. That buffer is exported with VIDIOC_EXPBUF, Mesa imports
 * the descriptor as an EGLImage (EGL_LINUX_DMA_BUF_EXT, NV12, two planes of one
 * buffer), a textured quad converts and draws it into a render target, and the
 * read-back is compared with a CPU conversion of the same NV12 data. Every step
 * prints before it runs, so a hung GPU shows up as the last line.
 *

 *   cc -DCEDRUS_GPU -I<shim> -I<uapi> -I/usr/local/include -L/usr/local/lib \
 *       -o cedrus_gpu cedrus_gpu.c -lgbm -lEGL -lGLESv2
 *   ./cedrus_gpu -n 1 stream.m2v reference.yuv
 */
#include "cedrus_mpeg2.c"

#include <errno.h>
#include <drm.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

static void
step(const char *what)
{

	printf("  -> %s\n", what);
}

static GLuint
shader(GLenum type, const char *src)
{
	GLuint sh = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];

		glGetShaderInfoLog(sh, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return (sh);
}

/* BT.601, limited range: what the GPU path is asked for by default. */
static void
yuv2rgb(int y, int u, int v, int *rgb)
{
	int c = 298 * (y - 16), d = u - 128, e = v - 128, k;

	rgb[0] = (c + 409 * e + 128) >> 8;
	rgb[1] = (c - 100 * d - 208 * e + 128) >> 8;
	rgb[2] = (c + 516 * d + 128) >> 8;
	for (k = 0; k < 3; k++)
		rgb[k] = rgb[k] < 0 ? 0 : rgb[k] > 255 ? 255 : rgb[k];
}

int
gpu_stage(int vfd, unsigned stride, unsigned ha, unsigned w, unsigned h,
    const uint8_t *nv12)
{
	static const GLfloat quad[] = {	/* x y u v */
		-1, -1, 0, 1,
		 1, -1, 1, 1,
		-1,  1, 0, 0,
		 1,  1, 1, 0,
	};
	struct v4l2_exportbuffer e;
	struct gbm_device *gbm;
	EGLDisplay dpy;
	EGLContext ctx;
	EGLConfig cfg;
	EGLImageKHR img, img2 = EGL_NO_IMAGE_KHR;
	GLuint src2 = 0;
	int planar = 0;
	EGLint n = 0, vmaj, vmin;
	GLuint src, dst, fbo, prog;
	unsigned char *px;
	unsigned x, y, good = 0;
	int drifd;
	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
	PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

	setvbuf(stdout, NULL, _IOLBF, 0);
	step("export the decoded capture buffer as a dmabuf");
	memset(&e, 0, sizeof(e));
	e.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	e.index = 0;
	e.flags = O_RDWR | O_CLOEXEC;
	if (ioctl(vfd, VIDIOC_EXPBUF, &e) != 0)
		die("EXPBUF");

	step("GPU: open the render node, EGL on GBM");
	drifd = open("/dev/dri/renderD128", O_RDWR);
	if (drifd < 0)
		die("open renderD128");
	{
		struct drm_prime_handle ph;

		memset(&ph, 0, sizeof(ph));
		ph.fd = e.fd;
		ph.flags = DRM_CLOEXEC;
		if (ioctl(drifd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) != 0)
			printf("     PRIME_FD_TO_HANDLE on the decoder's buffer: errno %d\n", errno);
		else {
			struct drm_gem_close gc = { .handle = ph.handle };

			printf("     PRIME_FD_TO_HANDLE on the decoder's buffer: handle %u\n", ph.handle);
			ioctl(drifd, DRM_IOCTL_GEM_CLOSE, &gc);
		}
	}
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

	step("eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, NV12) on the decoder's descriptor");
	{
		EGLint attr[] = {
			EGL_WIDTH, w, EGL_HEIGHT, h,
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

		img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	}
	if (img == EGL_NO_IMAGE_KHR) {
		/*
		 * No YUV import in this driver (nor R8/GR88: the Mali-400 has no
		 * one- or two-channel formats here). Import each plane as a 32-bit
		 * RGBA image a quarter of the width: a texel holds four luma bytes,
		 * or two Cb/Cr pairs, and the shader picks the one it needs.
		 */
		EGLint a0[] = {
			EGL_WIDTH, stride / 4, EGL_HEIGHT, h,
			EGL_LINUX_DRM_FOURCC_EXT, (EGLint)0x34324241,	/* ABGR8888 */
			EGL_DMA_BUF_PLANE0_FD_EXT, e.fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
			EGL_NONE
		};
		EGLint a1[] = {
			EGL_WIDTH, stride / 4, EGL_HEIGHT, h / 2,
			EGL_LINUX_DRM_FOURCC_EXT, (EGLint)0x34324241,	/* ABGR8888 */
			EGL_DMA_BUF_PLANE0_FD_EXT, e.fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)(stride * ha),
			EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
			EGL_NONE
		};

		printf("     NV12 import refused (%#x): importing the planes separately\n",
		    eglGetError());
		step("eglCreateImageKHR twice: luma and chroma as RGBA images, same descriptor");
		img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a0);
		printf("     luma plane as RGBA: %s (%#x)\n", img == EGL_NO_IMAGE_KHR ? "refused" : "ok", eglGetError());
		img2 = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a1);
		printf("     chroma plane at offset %u as RGBA: %s (%#x)\n", stride * ha,
		    img2 == EGL_NO_IMAGE_KHR ? "refused" : "ok", eglGetError());
		if (img == EGL_NO_IMAGE_KHR || img2 == EGL_NO_IMAGE_KHR) {
			fprintf(stderr, "plane import failed: %#x\n", eglGetError());
			return 1;
		}
		planar = 1;
	}

	step("bind the import as an external texture, make an ordinary render target");
	{
		GLenum tgt = planar ? GL_TEXTURE_2D : GL_TEXTURE_EXTERNAL_OES;

		glGenTextures(1, &src);
		glBindTexture(tgt, src);
		glEGLImageTargetTexture2DOES(tgt, img);
		glTexParameteri(tgt, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(tgt, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(tgt, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(tgt, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (planar) {
			glGenTextures(1, &src2);
			glActiveTexture(GL_TEXTURE2);
			glBindTexture(GL_TEXTURE_2D, src2);
			glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img2);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glActiveTexture(GL_TEXTURE0);
		}
	}
	glGenTextures(1, &dst);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, dst);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
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

	step("draw a textured quad that samples the decoder's buffer");
	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER,
	    "attribute vec2 pos; attribute vec2 uv; varying vec2 v;\n"
	    "void main() { gl_Position = vec4(pos, 0.0, 1.0); v = uv; }\n"));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, planar ?
	    "precision mediump float; varying vec2 v; uniform sampler2D tex; uniform sampler2D tex2;\n"
	    "uniform float width;\n"
	    "void main() {\n"
	    "  float px = floor(v.x * width);\n"
	    "  float q = floor(px / 4.0), k = px - 4.0 * q;\n"
	    "  vec4 l = texture2D(tex, vec2((q + 0.5) / (width / 4.0), v.y));\n"
	    "  float yy = k < 0.5 ? l.r : (k < 1.5 ? l.g : (k < 2.5 ? l.b : l.a));\n"
	    "  float cx = floor(px / 2.0), cq = floor(cx / 2.0), odd = cx - 2.0 * cq;\n"
	    "  vec4 cc = texture2D(tex2, vec2((cq + 0.5) / (width / 4.0), v.y));\n"
	    "  float y = 1.164 * (yy - 16.0 / 255.0);\n"
	    "  vec2 c = (odd < 0.5 ? cc.rg : cc.ba) - 0.5;\n"
	    "  gl_FragColor = vec4(y + 1.596 * c.y, y - 0.392 * c.x - 0.813 * c.y, y + 2.017 * c.x, 1.0);\n"
	    "}\n" :
	    "#extension GL_OES_EGL_image_external : require\n"
	    "precision mediump float; varying vec2 v; uniform samplerExternalOES tex;\n"
	    "void main() { gl_FragColor = vec4(texture2D(tex, v).rgb, 1.0); }\n"));
	glBindAttribLocation(prog, 0, "pos");
	glBindAttribLocation(prog, 1, "uv");
	glLinkProgram(prog);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "tex"), 0);
	if (planar)
		glUniform1i(glGetUniformLocation(prog, "tex2"), 2);
	if (planar)
		glUniform1f(glGetUniformLocation(prog, "width"), (float)w);
	glViewport(0, 0, w, h);
	glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	step("glFinish");
	glFinish();
	px = malloc((size_t)w * h * 4);
	if (px == NULL)
		die("malloc");
	/* glReadPixels returns the bottom row first: row y of the picture is row h-1-y. */
	step("glReadPixels -- if the GPU hangs, this is the last line");
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	step("glReadPixels returned");

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			int rgb[3];
			const uint8_t *uv = nv12 + (size_t)stride * ha + (y / 2) * stride + (x / 2) * 2;
			const unsigned char *q = px + ((size_t)(h - 1 - y) * w + x) * 4;

			yuv2rgb(nv12[(size_t)y * stride + x], uv[0], uv[1], rgb);
			if (abs(q[0] - rgb[0]) <= 12 && abs(q[1] - rgb[1]) <= 12 &&
			    abs(q[2] - rgb[2]) <= 12)
				good++;
		}
	{
		/* Which conversion did the GPU apply? Compare against four. */
		static const struct { const char *name; int ky, kr, kgu, kgv, kb, off; } m[] = {
			{ "BT.601 limited", 298, 409, 100, 208, 516, 16 },
			{ "BT.601 full", 256, 359, 88, 183, 454, 0 },
			{ "BT.709 limited", 298, 459, 55, 136, 541, 16 },
			{ "BT.709 full", 256, 403, 48, 120, 475, 0 },
		};
		unsigned i, best = 0, bestn = 0;

		for (i = 0; i < 4; i++) {
			unsigned cnt = 0;

			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++) {
					const uint8_t *uv = nv12 + (size_t)stride * ha + (y / 2) * stride + (x / 2) * 2;
					const unsigned char *q = px + ((size_t)(h - 1 - y) * w + x) * 4;
					int c = m[i].ky * (nv12[(size_t)y * stride + x] - m[i].off), d = uv[0] - 128, ee = uv[1] - 128;
					int r = (c + m[i].kr * ee + 128) >> 8, g = (c - m[i].kgu * d - m[i].kgv * ee + 128) >> 8,
					    bb = (c + m[i].kb * d + 128) >> 8;

					r = r < 0 ? 0 : r > 255 ? 255 : r;
					g = g < 0 ? 0 : g > 255 ? 255 : g;
					bb = bb < 0 ? 0 : bb > 255 ? 255 : bb;
					if (abs(q[0] - r) <= 12 && abs(q[1] - g) <= 12 && abs(q[2] - bb) <= 12)
						cnt++;
				}
			printf("     %s: %u of %u pixels match\n", m[i].name, cnt, w * h);
			if (cnt > bestn) {
				bestn = cnt;
				best = i;
			}
		}
		printf("     closest: %s; sample pixel (10,10): gpu %u %u %u\n", m[best].name,
		    px[((h - 11) * w + 10) * 4], px[((h - 11) * w + 10) * 4 + 1], px[((h - 11) * w + 10) * 4 + 2]);
	}
	if (getenv("CEDRUS_GPU_PPM") != NULL) {
		FILE *g = fopen("gpu.ppm", "wb"), *c = fopen("cpu.ppm", "wb");

		fprintf(g, "P6 %u %u 255\n", w, h);
		fprintf(c, "P6 %u %u 255\n", w, h);
		for (y = 0; y < h; y++)
			for (x = 0; x < w; x++) {
				int rgb[3];
				const uint8_t *uv = nv12 + (size_t)stride * ha + (y / 2) * stride + (x / 2) * 2;

				yuv2rgb(nv12[(size_t)y * stride + x], uv[0], uv[1], rgb);
				fputc(rgb[0], c); fputc(rgb[1], c); fputc(rgb[2], c);
				fwrite(px + ((size_t)(h - 1 - y) * w + x) * 4, 1, 3, g);
			}
		fclose(g);
		fclose(c);
	}
	printf("RESULT: the GPU drew the decoder's own buffer: %u of %u pixels within 12 of the "
	    "CPU conversion: %s\n", good, w * h, good * 100 >= w * h * 97 ? "OK" : "MISMATCH");
	eglDestroyImageKHR(dpy, img);
	if (img2 != EGL_NO_IMAGE_KHR)
		eglDestroyImageKHR(dpy, img2);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	gbm_device_destroy(gbm);
	return (good * 100 < w * h * 97);
}
