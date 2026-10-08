/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * A tiny libudev for FFmpeg's V4L2 Request hwaccel on FreeBSD.
 *
 * The hwaccel (Kwiboo's v4l2-request branch) finds its decoder by enumerating the "media" udev subsystem
 * and mapping the media interfaces' major/minor to device nodes. FreeBSD has no sysfs; devfs has the
 * nodes. This implements just the calls it makes: media devices are /dev/media*, a device number is
 * mapped by stat()ing /dev/video* and /dev/media*. Build: cc -shared -fPIC -o libudev.so.1 libudev-shim.c
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct udev { int ref; };
struct udev_device { struct udev *u; char path[64]; };
struct udev_list_entry { struct udev_list_entry *next; char name[64]; };
struct udev_enumerate { struct udev *u; char prefix[16]; struct udev_list_entry *head; };

struct udev *
udev_new(void)
{
	struct udev *u = calloc(1, sizeof(*u));

	if (u != NULL)
		u->ref = 1;
	return (u);
}

struct udev *
udev_ref(struct udev *u)
{
	u->ref++;
	return (u);
}

struct udev *
udev_unref(struct udev *u)
{
	if (u != NULL && --u->ref == 0) {
		free(u);
		return (NULL);
	}
	return (u);
}

struct udev_enumerate *
udev_enumerate_new(struct udev *u)
{
	struct udev_enumerate *e = calloc(1, sizeof(*e));

	if (e != NULL)
		e->u = u;
	return (e);
}

int
udev_enumerate_add_match_subsystem(struct udev_enumerate *e, const char *sub)
{
	if (strcmp(sub, "media") == 0)
		strlcpy(e->prefix, "media", sizeof(e->prefix));
	else if (strcmp(sub, "video4linux") == 0)
		strlcpy(e->prefix, "video", sizeof(e->prefix));
	else
		return (-1);
	return (0);
}

static int
cmp_names(const void *a, const void *b)
{
	return (strcmp(*(char *const *)a, *(char *const *)b));
}

int
udev_enumerate_scan_devices(struct udev_enumerate *e)
{
	DIR *d = opendir("/dev");
	struct dirent *de;
	char *names[64];
	int n = 0, i;
	size_t pl = strlen(e->prefix);

	if (d == NULL)
		return (-1);
	while ((de = readdir(d)) != NULL && n < 64)
		if (pl != 0 && strncmp(de->d_name, e->prefix, pl) == 0 && de->d_name[pl] >= '0' && de->d_name[pl] <= '9')
			names[n++] = strdup(de->d_name);
	closedir(d);
	qsort(names, n, sizeof(names[0]), cmp_names);
	for (i = n - 1; i >= 0; i--) {		/* build the list in order */
		struct udev_list_entry *le = calloc(1, sizeof(*le));

		snprintf(le->name, sizeof(le->name), "/dev/%s", names[i]);
		le->next = e->head;
		e->head = le;
		free(names[i]);
	}
	return (0);
}

struct udev_list_entry *
udev_enumerate_get_list_entry(struct udev_enumerate *e)
{
	return (e->head);
}

struct udev_enumerate *
udev_enumerate_unref(struct udev_enumerate *e)
{
	struct udev_list_entry *le, *nx;

	for (le = e->head; le != NULL; le = nx) {
		nx = le->next;
		free(le);
	}
	free(e);
	return (NULL);
}

struct udev_list_entry *
udev_list_entry_get_next(struct udev_list_entry *le)
{
	return (le->next);
}

const char *
udev_list_entry_get_name(struct udev_list_entry *le)
{
	return (le->name);
}

struct udev_device *
udev_device_new_from_syspath(struct udev *u, const char *path)
{
	struct udev_device *d;

	if (strncmp(path, "/dev/", 5) != 0)
		return (NULL);
	d = calloc(1, sizeof(*d));
	d->u = u;
	strlcpy(d->path, path, sizeof(d->path));
	return (d);
}

struct udev_device *
udev_device_new_from_devnum(struct udev *u, char type, dev_t devnum)
{
	static const char *const pre[] = { "video", "media" };
	DIR *dir;
	struct dirent *de;
	struct udev_device *d = NULL;
	unsigned k;

	if (type != 'c' || (dir = opendir("/dev")) == NULL)
		return (NULL);
	while (d == NULL && (de = readdir(dir)) != NULL) {
		for (k = 0; k < 2; k++) {
			size_t pl = strlen(pre[k]);
			char p[64];
			struct stat st;

			if (strncmp(de->d_name, pre[k], pl) != 0)
				continue;
			snprintf(p, sizeof(p), "/dev/%s", de->d_name);
			if (stat(p, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == devnum) {
				d = calloc(1, sizeof(*d));
				d->u = u;
				strlcpy(d->path, p, sizeof(d->path));
				break;
			}
		}
	}
	closedir(dir);
	return (d);
}

const char *
udev_device_get_devnode(struct udev_device *d)
{
	return (d->path);
}

struct udev *
udev_device_get_udev(struct udev_device *d)
{
	return (d->u);
}

struct udev_device *
udev_device_unref(struct udev_device *d)
{
	free(d);
	return (NULL);
}
