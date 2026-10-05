/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * dmabuf: see dmabuf.h.
 *
 * The descriptor is a file of its own type. It can be passed between
 * processes, mapped with mmap(2) (the exporter makes the vm object), sized with
 * lseek(fd, 0, SEEK_END) and fstat(2)ed; it cannot be read or written.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/capsicum.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mman.h>
#include <sys/unistd.h>
#include <sys/proc.h>
#include <sys/refcount.h>
#include <sys/stat.h>
#include <sys/user.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_object.h>
#include <vm/vm_extern.h>

#include <dev/dmabuf/dmabuf.h>

/*
 * Not yet in sys/file.h. Local value, as drm-kmod's own dma-buf uses; the two
 * are told apart by their fileops, never by this number.
 */
#ifndef DTYPE_DMABUF
#define	DTYPE_DMABUF	100
#endif

static MALLOC_DEFINE(M_DMABUF, "dmabuf_base", "dmabuf structures");

static u_int dmabuf_live;		/* buffers that exist: the module cannot unload */

static fo_stat_t	dmabuf_stat;
static fo_close_t	dmabuf_close;
static fo_seek_t	dmabuf_seek;
static fo_mmap_t	dmabuf_mmap_f;
static fo_fill_kinfo_t	dmabuf_fill_kinfo;

static const struct fileops dmabuf_fileops = {
	.fo_read = invfo_rdwr,
	.fo_write = invfo_rdwr,
	.fo_truncate = invfo_truncate,
	.fo_ioctl = invfo_ioctl,
	.fo_poll = invfo_poll,
	.fo_kqfilter = invfo_kqfilter,
	.fo_stat = dmabuf_stat,
	.fo_close = dmabuf_close,
	.fo_chmod = invfo_chmod,
	.fo_chown = invfo_chown,
	.fo_sendfile = invfo_sendfile,
	.fo_seek = dmabuf_seek,
	.fo_fill_kinfo = dmabuf_fill_kinfo,
	.fo_mmap = dmabuf_mmap_f,
	.fo_cmp = file_kcmp_generic,
	.fo_flags = DFLAG_PASSABLE | DFLAG_SEEKABLE,
};

struct dmabuf *
dmabuf_alloc(const struct dmabuf_ops *ops, size_t size, void *priv)
{
	struct dmabuf *buf;

	buf = malloc(sizeof(*buf), M_DMABUF, M_WAITOK | M_ZERO);
	buf->ops = ops;
	buf->priv = priv;
	buf->size = size;
	refcount_init(&buf->refs, 1);
	atomic_add_int(&dmabuf_live, 1);
	return (buf);
}

void
dmabuf_hold(struct dmabuf *buf)
{

	refcount_acquire(&buf->refs);
}

void
dmabuf_put(struct dmabuf *buf)
{

	if (!refcount_release(&buf->refs))
		return;
	buf->ops->release(buf);		/* frees the exporter's side; we free ours */
	free(buf, M_DMABUF);
	atomic_subtract_int(&dmabuf_live, 1);
}

void *
dmabuf_vmap(struct dmabuf *buf)
{

	return (buf->ops->vmap != NULL ? buf->ops->vmap(buf) : NULL);
}

int
dmabuf_sglist(struct dmabuf *buf, struct sglist **sgp)
{

	return (buf->ops->sglist != NULL ? buf->ops->sglist(buf, sgp) : ENODEV);
}

/* ------------------------------------------------------------- descriptors */

int
dmabuf_to_fd(struct dmabuf *buf, int oflags, struct thread *td, int *fdp)
{
	struct file *fp;
	int error, fd, fflags;

	switch (oflags & O_ACCMODE) {
	case O_RDONLY:
		fflags = FREAD;
		break;
	case O_WRONLY:
		fflags = FWRITE;
		break;
	default:
		fflags = FREAD | FWRITE;
		break;
	}
	error = falloc(td, &fp, &fd, oflags & O_CLOEXEC);
	if (error != 0)
		return (error);
	dmabuf_hold(buf);			/* the file's reference */
	finit(fp, fflags, DTYPE_DMABUF, buf, &dmabuf_fileops);
	*fdp = fd;
	fdrop(fp, td);
	return (0);
}

int
dmabuf_from_fd(int fd, struct thread *td, struct dmabuf **bufp)
{
	struct file *fp;
	struct dmabuf *buf;
	int error;

	error = fget(td, fd, &cap_no_rights, &fp);
	if (error != 0)
		return (error);
	if (fp->f_ops != &dmabuf_fileops) {
		fdrop(fp, td);
		return (EINVAL);
	}
	buf = fp->f_data;
	dmabuf_hold(buf);
	fdrop(fp, td);
	*bufp = buf;
	return (0);
}

static int
dmabuf_close(struct file *fp, struct thread *td)
{

	dmabuf_put(fp->f_data);
	return (0);
}

static int
dmabuf_stat(struct file *fp, struct stat *sb, struct ucred *active_cred)
{
	struct dmabuf *buf = fp->f_data;

	bzero(sb, sizeof(*sb));
	sb->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
	sb->st_size = buf->size;
	sb->st_blksize = PAGE_SIZE;
	return (0);
}

/* lseek(fd, 0, SEEK_END) is how userland asks how big the buffer is. */
static int
dmabuf_seek(struct file *fp, off_t offset, int whence, struct thread *td)
{
	struct dmabuf *buf = fp->f_data;
	off_t base;

	if (offset != 0)
		return (EINVAL);
	if (whence == SEEK_END)
		base = buf->size;
	else if (whence == SEEK_SET)
		base = 0;
	else
		return (EINVAL);
	td->td_uretoff.tdu_off = fp->f_offset = base;
	return (0);
}

static int
dmabuf_fill_kinfo(struct file *fp, struct kinfo_file *kif, struct filedesc *fdp)
{

	kif->kf_type = KF_TYPE_UNKNOWN;
	return (0);
}

static int
dmabuf_mmap_f(struct file *fp, vm_map_t map, vm_offset_t *addr, vm_size_t size,
    vm_prot_t prot, vm_prot_t cap_maxprot, int flags, vm_ooffset_t foff,
    struct thread *td)
{
	struct dmabuf *buf = fp->f_data;
	vm_prot_t maxprot = 0;
	vm_object_t obj;
	int error;

	if (buf->ops->mmap == NULL)
		return (ENODEV);
	if ((flags & MAP_SHARED) == 0)
		return (EINVAL);		/* the memory is shared by nature */
	if (size == 0 || foff > round_page(buf->size) ||
	    size > round_page(buf->size) - foff)
		return (EINVAL);
	if ((fp->f_flag & FREAD) != 0)
		maxprot |= VM_PROT_READ;
	if ((fp->f_flag & FWRITE) != 0)
		maxprot |= VM_PROT_WRITE;
	maxprot &= cap_maxprot;
	if ((prot & ~maxprot & (VM_PROT_READ | VM_PROT_WRITE)) != 0)
		return (EACCES);
	error = buf->ops->mmap(buf, &foff, size, &obj, prot);
	if (error != 0)
		return (error);
	error = vm_mmap_object(map, addr, size, prot, maxprot, flags, obj, foff,
	    FALSE, td);
	if (error != 0)
		vm_object_deallocate(obj);
	return (error);
}

/* ------------------------------------------------------------------ module */

static int
dmabuf_modevent(module_t mod, int what, void *arg)
{

	switch (what) {
	case MOD_LOAD:
		return (0);
	case MOD_UNLOAD:
		/* A live buffer calls back into this module (release, mmap). */
		return (atomic_load_int(&dmabuf_live) != 0 ? EBUSY : 0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t dmabuf_mod = {
	"dmabuf_base",
	dmabuf_modevent,
	NULL
};
DECLARE_MODULE(dmabuf_base, dmabuf_mod, SI_SUB_DRIVERS, SI_ORDER_FIRST);
MODULE_VERSION(dmabuf_base, 1);
