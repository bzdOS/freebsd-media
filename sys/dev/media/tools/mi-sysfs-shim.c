/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * LD_PRELOAD shim for v4l-utils on FreeBSD.
 *
 * libv4l's media-info finds the media device of a video node by listing
 * /sys/dev/char/<major>:<minor>/device, which is Linux sysfs. This answers
 * that one directory listing: "media<N>" for the media device whose topology
 * has an interface with that major:minor, and the DEVNAME= line of
 * /sys/dev/char/<major>:<minor>/uevent, which names the /dev entry. Nothing
 * else is changed.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/media.h>

#define	NFAKE	8

static struct fake {
	DIR		*dir;
	struct dirent	ent;
	int		done;
	char		name[16];
} fakes[NFAKE];

static int
has_interface(int fd, unsigned maj, unsigned min)
{
	struct media_v2_topology t;
	struct media_v2_interface *ifs;
	unsigned i;
	int found = 0;

	memset(&t, 0, sizeof(t));
	if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &t) != 0 || t.num_interfaces == 0)
		return (0);
	ifs = calloc(t.num_interfaces, sizeof(*ifs));
	t.num_entities = t.num_pads = t.num_links = 0;
	t.ptr_interfaces = (uintptr_t)ifs;
	if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &t) == 0)
		for (i = 0; i < t.num_interfaces; i++)
			if (ifs[i].devnode.major == maj && ifs[i].devnode.minor == min)
				found = 1;
	free(ifs);
	return (found);
}

static int
find_media(unsigned maj, unsigned min, char *out, size_t n)
{
	char path[32];
	int i, fd;

	for (i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/media%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		if (has_interface(fd, maj, min)) {
			close(fd);
			snprintf(out, n, "media%d", i);
			return (1);
		}
		close(fd);
	}
	return (0);
}

DIR *
opendir(const char *path)
{
	static DIR *(*real)(const char *);
	unsigned maj, min;
	char name[16];
	DIR *d;
	int i;

	if (real == NULL)
		real = dlsym(RTLD_NEXT, "opendir");
	if (sscanf(path, "/sys/dev/char/%u:%u/device", &maj, &min) == 2) {
		if (!find_media(maj, min, name, sizeof(name)))
			return (real(path));
		d = real("/");
		for (i = 0; d != NULL && i < NFAKE; i++) {
			if (fakes[i].dir == NULL) {
				fakes[i].dir = d;
				fakes[i].done = 0;
				strlcpy(fakes[i].name, name, sizeof(fakes[i].name));
				break;
			}
		}
		return (d);
	}
	return (real(path));
}

struct dirent *
readdir(DIR *d)
{
	static struct dirent *(*real)(DIR *);
	int i;

	if (real == NULL)
		real = dlsym(RTLD_NEXT, "readdir");
	for (i = 0; i < NFAKE; i++) {
		if (fakes[i].dir == d && d != NULL) {
			if (fakes[i].done)
				return (NULL);
			fakes[i].done = 1;
			memset(&fakes[i].ent, 0, sizeof(fakes[i].ent));
			fakes[i].ent.d_type = DT_DIR;
			strlcpy(fakes[i].ent.d_name, fakes[i].name,
			    sizeof(fakes[i].ent.d_name));
			fakes[i].ent.d_namlen = strlen(fakes[i].ent.d_name);
			fakes[i].ent.d_reclen = sizeof(fakes[i].ent);
			return (&fakes[i].ent);
		}
	}
	return (real(d));
}

int
closedir(DIR *d)
{
	static int (*real)(DIR *);
	int i;

	if (real == NULL)
		real = dlsym(RTLD_NEXT, "closedir");
	for (i = 0; i < NFAKE; i++)
		if (fakes[i].dir == d && d != NULL)
			fakes[i].dir = NULL;
	return (real(d));
}

/* "video3" for the character device major:minor, if a /dev/video or /dev/media has it. */
static int
devname_of(unsigned maj, unsigned min, char *out, size_t n)
{
	static const char *const pfx[] = { "video", "media" };
	char path[32];
	struct stat sb;
	int i, k;

	for (k = 0; k < 2; k++) {
		for (i = 0; i < 32; i++) {
			snprintf(path, sizeof(path), "/dev/%s%d", pfx[k], i);
			if (stat(path, &sb) == 0 && S_ISCHR(sb.st_mode) &&
			    major(sb.st_rdev) == maj && minor(sb.st_rdev) == min) {
				snprintf(out, n, "%s%d", pfx[k], i);
				return (1);
			}
		}
	}
	return (0);
}

FILE *
fopen(const char *path, const char *mode)
{
	static FILE *(*real)(const char *, const char *);
	unsigned maj, min;
	char name[16];
	FILE *f;

	if (real == NULL)
		real = dlsym(RTLD_NEXT, "fopen");
	if (sscanf(path, "/sys/dev/char/%u:%u/uevent", &maj, &min) == 2 &&
	    devname_of(maj, min, name, sizeof(name)) && (f = tmpfile()) != NULL) {
		fprintf(f, "DEVNAME=%s\n", name);
		rewind(f);
		return (f);
	}
	return (real(path, mode));
}
