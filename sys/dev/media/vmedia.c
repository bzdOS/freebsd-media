/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The media controller: /dev/mediaN, the graph of a device's entities, pads
 * and links, and the entry point of the request API.
 *
 * Objects carry an id whose top byte tells the kind (entity 0, pad 1, link 2,
 * interface 3) and whose low 24 bits count up from one, which is how
 * MEDIA_IOC_G_TOPOLOGY users tell what an id refers to.
 *
 * The graph is protected by m->lock. Every ioctl only reads it, but a driver
 * may add and remove objects while the device is open; each change bumps
 * topology_version.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/sx.h>
#include <sys/types.h>
#include <sys/limits.h>

#include <dev/media/media_priv.h>

static MALLOC_DEFINE(M_VMEDIA, "vmedia", "media controller");

#define	VMEDIA_MAX_UNITS	64
#define	VMEDIA_VERSION		((6U << 16) | (12U << 8) | 0U)	/* what userland sees as the API level */

#define	ID_ENTITY	(0U << 24)
#define	ID_PAD		(1U << 24)
#define	ID_LINK		(2U << 24)
#define	ID_INTF		(3U << 24)

static struct unrhdr *vmedia_units;
static u_int vmedia_serial;

static d_open_t		vmedia_open;
static d_close_t	vmedia_close;
static d_ioctl_t	vmedia_ioctl;

static struct cdevsw vmedia_cdevsw = {
	.d_version = D_VERSION,
	.d_name = "media",
	.d_open = vmedia_open,
	.d_close = vmedia_close,
	.d_ioctl = vmedia_ioctl,
};

/* ------------------------------------------------------------------- graph */

void
vmedia_init(struct vmedia *m, const char *driver, const char *model,
    const char *bus_info, uint32_t hw_revision)
{

	memset(m, 0, sizeof(*m));
	strlcpy(m->driver, driver, sizeof(m->driver));
	strlcpy(m->model, model, sizeof(m->model));
	if (bus_info != NULL)
		strlcpy(m->bus_info, bus_info, sizeof(m->bus_info));
	m->hw_revision = hw_revision;
	m->driver_version = VMEDIA_VERSION;
	sx_init(&m->lock, "vmedia");
	TAILQ_INIT(&m->entities);
	TAILQ_INIT(&m->intfs);
	TAILQ_INIT(&m->links);
	m->serial_no = atomic_fetchadd_int(&vmedia_serial, 1) + 1;
}

int
vmedia_entity_add(struct vmedia *m, struct vmedia_entity *e, const char *name,
    uint32_t function, uint32_t npads, const uint32_t *pad_flags)
{
	uint32_t i;

	memset(e, 0, sizeof(*e));
	strlcpy(e->name, name, sizeof(e->name));
	e->function = function;
	e->npads = npads;
	if (npads != 0)
		e->pads = malloc(npads * sizeof(*e->pads), M_VMEDIA, M_WAITOK | M_ZERO);
	sx_xlock(&m->lock);
	e->id = ID_ENTITY | ++m->next_ent;
	for (i = 0; i < npads; i++) {
		e->pads[i].ent = e;
		e->pads[i].index = i;
		e->pads[i].flags = pad_flags[i];
		e->pads[i].id = ID_PAD | ++m->next_pad;
	}
	TAILQ_INSERT_TAIL(&m->entities, e, link);
	m->topology_version++;
	sx_xunlock(&m->lock);
	return (0);
}

/* Drop every link that touches `e`'s pads or `intf`; m->lock held. */
static void
vmedia_drop_links(struct vmedia *m, struct vmedia_entity *e, struct vmedia_intf *intf)
{
	struct vmedia_link *l, *tmp;

	TAILQ_FOREACH_SAFE(l, &m->links, link, tmp) {
		if ((e != NULL && ((l->src != NULL && l->src->ent == e) ||
		    (l->sink != NULL && l->sink->ent == e) || l->ent == e)) ||
		    (intf != NULL && l->intf == intf)) {
			TAILQ_REMOVE(&m->links, l, link);
			free(l, M_VMEDIA);
		}
	}
}

void
vmedia_entity_remove(struct vmedia *m, struct vmedia_entity *e)
{

	sx_xlock(&m->lock);
	vmedia_drop_links(m, e, NULL);
	TAILQ_REMOVE(&m->entities, e, link);
	m->topology_version++;
	sx_xunlock(&m->lock);
	free(e->pads, M_VMEDIA);
	e->pads = NULL;
}

int
vmedia_link_pads(struct vmedia *m, struct vmedia_entity *src, uint32_t src_pad,
    struct vmedia_entity *sink, uint32_t sink_pad, uint32_t flags)
{
	struct vmedia_link *l;

	if (src_pad >= src->npads || sink_pad >= sink->npads ||
	    (src->pads[src_pad].flags & MEDIA_PAD_FL_SOURCE) == 0 ||
	    (sink->pads[sink_pad].flags & MEDIA_PAD_FL_SINK) == 0)
		return (EINVAL);
	l = malloc(sizeof(*l), M_VMEDIA, M_WAITOK | M_ZERO);
	l->src = &src->pads[src_pad];
	l->sink = &sink->pads[sink_pad];
	l->flags = (flags & ~MEDIA_LNK_FL_LINK_TYPE) | MEDIA_LNK_FL_DATA_LINK;
	sx_xlock(&m->lock);
	l->id = ID_LINK | ++m->next_link;
	TAILQ_INSERT_TAIL(&m->links, l, link);
	m->topology_version++;
	sx_xunlock(&m->lock);
	return (0);
}

int
vmedia_intf_add(struct vmedia *m, struct vmedia_intf *intf, uint32_t type,
    uint32_t major, uint32_t minor)
{

	memset(intf, 0, sizeof(*intf));
	intf->type = type;
	intf->major = major;
	intf->minor = minor;
	sx_xlock(&m->lock);
	intf->id = ID_INTF | ++m->next_intf;
	TAILQ_INSERT_TAIL(&m->intfs, intf, link);
	m->topology_version++;
	sx_xunlock(&m->lock);
	return (0);
}

void
vmedia_intf_remove(struct vmedia *m, struct vmedia_intf *intf)
{

	sx_xlock(&m->lock);
	vmedia_drop_links(m, NULL, intf);
	TAILQ_REMOVE(&m->intfs, intf, link);
	m->topology_version++;
	sx_xunlock(&m->lock);
}

int
vmedia_link_intf(struct vmedia *m, struct vmedia_intf *intf,
    struct vmedia_entity *e, uint32_t flags)
{
	struct vmedia_link *l;

	l = malloc(sizeof(*l), M_VMEDIA, M_WAITOK | M_ZERO);
	l->intf = intf;
	l->ent = e;
	l->flags = (flags & ~MEDIA_LNK_FL_LINK_TYPE) | MEDIA_LNK_FL_INTERFACE_LINK;
	sx_xlock(&m->lock);
	l->id = ID_LINK | ++m->next_link;
	TAILQ_INSERT_TAIL(&m->links, l, link);
	if (e->intf == NULL)
		e->intf = intf;
	m->topology_version++;
	sx_xunlock(&m->lock);
	return (0);
}

/* ------------------------------------------------------------------ ioctls */

static struct vmedia_entity *
vmedia_ent_by_id(struct vmedia *m, uint32_t id)
{
	struct vmedia_entity *e;

	TAILQ_FOREACH(e, &m->entities, link)
		if (e->id == id)
			return (e);
	return (NULL);
}

static int
vmedia_enum_entities(struct vmedia *m, struct media_entity_desc *d)
{
	struct vmedia_entity *e, *best = NULL;
	struct vmedia_link *l;
	uint32_t id = d->id, links = 0;

	sx_slock(&m->lock);
	if ((id & MEDIA_ENT_ID_FLAG_NEXT) != 0) {
		id &= ~MEDIA_ENT_ID_FLAG_NEXT;
		TAILQ_FOREACH(e, &m->entities, link)
			if (e->id > id && (best == NULL || e->id < best->id))
				best = e;
	} else {
		best = vmedia_ent_by_id(m, id);
	}
	if (best == NULL) {
		sx_sunlock(&m->lock);
		return (EINVAL);
	}
	e = best;
	TAILQ_FOREACH(l, &m->links, link)
		if (l->src != NULL && l->src->ent == e)
			links++;
	memset(d, 0, sizeof(*d));
	d->id = e->id;
	strlcpy(d->name, e->name, sizeof(d->name));
	d->type = e->function;
	d->flags = e->flags;
	d->pads = e->npads;
	d->links = links;
	if (e->intf != NULL) {
		d->dev.major = e->intf->major;
		d->dev.minor = e->intf->minor;
	}
	sx_sunlock(&m->lock);
	return (0);
}

static int
vmedia_enum_links(struct vmedia *m, struct media_links_enum *le)
{
	struct vmedia_entity *e;
	struct vmedia_link *l;
	struct media_pad_desc pd;
	struct media_link_desc ld;
	uint32_t i, n = 0;
	int error = 0;

	memset(le->reserved, 0, sizeof(le->reserved));
	sx_slock(&m->lock);
	e = vmedia_ent_by_id(m, le->entity);
	if (e == NULL) {
		sx_sunlock(&m->lock);
		return (EINVAL);
	}
	if (le->pads != NULL) {
		for (i = 0; i < e->npads && error == 0; i++) {
			memset(&pd, 0, sizeof(pd));
			pd.entity = e->id;
			pd.index = e->pads[i].index;
			pd.flags = e->pads[i].flags;
			error = copyout(&pd, &le->pads[i], sizeof(pd));
		}
	}
	if (error == 0 && le->links != NULL) {
		TAILQ_FOREACH(l, &m->links, link) {
			if (l->src == NULL || l->src->ent != e)
				continue;
			memset(&ld, 0, sizeof(ld));
			ld.source.entity = l->src->ent->id;
			ld.source.index = l->src->index;
			ld.source.flags = l->src->flags;
			ld.sink.entity = l->sink->ent->id;
			ld.sink.index = l->sink->index;
			ld.sink.flags = l->sink->flags;
			ld.flags = l->flags;
			error = copyout(&ld, &le->links[n++], sizeof(ld));
			if (error != 0)
				break;
		}
	}
	sx_sunlock(&m->lock);
	return (error);
}

static int
vmedia_setup_link(struct vmedia *m, struct media_link_desc *ld)
{
	struct vmedia_link *l;
	int error = EINVAL;

	sx_xlock(&m->lock);
	TAILQ_FOREACH(l, &m->links, link) {
		if (l->src == NULL || l->src->ent->id != ld->source.entity ||
		    l->src->index != ld->source.index ||
		    l->sink->ent->id != ld->sink.entity ||
		    l->sink->index != ld->sink.index)
			continue;
		/* The link type is not up to the caller, and an immutable link cannot change. */
		if (((ld->flags ^ l->flags) & MEDIA_LNK_FL_LINK_TYPE) != 0) {
			error = EINVAL;
		} else if ((l->flags & MEDIA_LNK_FL_IMMUTABLE) != 0) {
			error = ld->flags != l->flags ? EINVAL : 0;
		} else {
			l->flags = (l->flags & ~MEDIA_LNK_FL_ENABLED) |
			    (ld->flags & MEDIA_LNK_FL_ENABLED);
			m->topology_version++;
			error = 0;
		}
		if (error != 0)
			break;
		ld->flags = l->flags;
		memset(ld->reserved, 0, sizeof(ld->reserved));
		break;
	}
	sx_xunlock(&m->lock);
	return (error);
}

static int
vmedia_topology(struct vmedia *m, struct media_v2_topology *t)
{
	struct vmedia_entity *e;
	struct vmedia_intf *in;
	struct vmedia_link *l;
	struct media_v2_entity me;
	struct media_v2_interface mi;
	struct media_v2_pad mp;
	struct media_v2_link ml;
	uint32_t i, n_ent = 0, n_if = 0, n_pad = 0, n_lnk = 0;
	int error = 0;

	sx_slock(&m->lock);
	TAILQ_FOREACH(e, &m->entities, link) {
		if (t->ptr_entities != 0 && n_ent < t->num_entities) {
			memset(&me, 0, sizeof(me));
			me.id = e->id;
			strlcpy(me.name, e->name, sizeof(me.name));
			me.function = e->function;
			me.flags = e->flags;
			error = copyout(&me, (char *)(uintptr_t)t->ptr_entities +
			    n_ent * sizeof(me), sizeof(me));
			if (error != 0)
				goto out;
		}
		n_ent++;
		for (i = 0; i < e->npads; i++) {
			if (t->ptr_pads != 0 && n_pad < t->num_pads) {
				memset(&mp, 0, sizeof(mp));
				mp.id = e->pads[i].id;
				mp.entity_id = e->id;
				mp.flags = e->pads[i].flags;
				mp.index = e->pads[i].index;
				error = copyout(&mp, (char *)(uintptr_t)t->ptr_pads +
				    n_pad * sizeof(mp), sizeof(mp));
				if (error != 0)
					goto out;
			}
			n_pad++;
		}
	}
	TAILQ_FOREACH(in, &m->intfs, link) {
		if (t->ptr_interfaces != 0 && n_if < t->num_interfaces) {
			memset(&mi, 0, sizeof(mi));
			mi.id = in->id;
			mi.intf_type = in->type;
			mi.devnode.major = in->major;
			mi.devnode.minor = in->minor;
			error = copyout(&mi, (char *)(uintptr_t)t->ptr_interfaces +
			    n_if * sizeof(mi), sizeof(mi));
			if (error != 0)
				goto out;
		}
		n_if++;
	}
	TAILQ_FOREACH(l, &m->links, link) {
		if (t->ptr_links != 0 && n_lnk < t->num_links) {
			memset(&ml, 0, sizeof(ml));
			ml.id = l->id;
			if (l->intf != NULL) {
				ml.source_id = l->intf->id;
				ml.sink_id = l->ent->id;
			} else {
				ml.source_id = l->src->id;
				ml.sink_id = l->sink->id;
			}
			ml.flags = l->flags;
			error = copyout(&ml, (char *)(uintptr_t)t->ptr_links +
			    n_lnk * sizeof(ml), sizeof(ml));
			if (error != 0)
				goto out;
		}
		n_lnk++;
	}
	t->topology_version = m->topology_version;
	t->reserved1 = t->reserved2 = t->reserved3 = t->reserved4 = 0;
	t->num_entities = n_ent;
	t->num_interfaces = n_if;
	t->num_pads = n_pad;
	t->num_links = n_lnk;
out:
	sx_sunlock(&m->lock);
	return (error);
}

static int
vmedia_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	struct vmedia *m = dev->si_drv1;

	atomic_add_int(&m->nopen, 1);
	return (0);
}

static int
vmedia_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	struct vmedia *m = dev->si_drv1;

	atomic_subtract_int(&m->nopen, 1);
	return (0);
}

static int
vmedia_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	struct vmedia *m = dev->si_drv1;
	struct media_device_info *di;

	switch (cmd) {
	case MEDIA_IOC_DEVICE_INFO:
		di = (struct media_device_info *)data;
		memset(di, 0, sizeof(*di));
		strlcpy(di->driver, m->driver, sizeof(di->driver));
		strlcpy(di->model, m->model, sizeof(di->model));
		strlcpy(di->serial, m->serial, sizeof(di->serial));
		strlcpy(di->bus_info, m->bus_info, sizeof(di->bus_info));
		di->media_version = VMEDIA_VERSION;
		di->hw_revision = m->hw_revision;
		di->driver_version = m->driver_version;
		return (0);
	case MEDIA_IOC_ENUM_ENTITIES:
		return (vmedia_enum_entities(m, (struct media_entity_desc *)data));
	case MEDIA_IOC_ENUM_LINKS:
		return (vmedia_enum_links(m, (struct media_links_enum *)data));
	case MEDIA_IOC_SETUP_LINK:
		return (vmedia_setup_link(m, (struct media_link_desc *)data));
	case MEDIA_IOC_G_TOPOLOGY:
		return (vmedia_topology(m, (struct media_v2_topology *)data));
	case MEDIA_IOC_REQUEST_ALLOC:
		if (!m->request_capable)
			return (ENOTTY);
		return (vreq_alloc(m, td, (int *)data));
	default:
		return (ENOTTY);
	}
}

/* ---------------------------------------------------------------- register */

int
vmedia_register(struct vmedia *m)
{
	struct make_dev_args args;
	int error, unit;

	unit = alloc_unr(vmedia_units);
	if (unit < 0)
		return (ENOSPC);
	m->unit = unit;
	make_dev_args_init(&args);
	args.mda_devsw = &vmedia_cdevsw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_OPERATOR;
	args.mda_mode = 0660;
	args.mda_si_drv1 = m;
	error = make_dev_s(&args, &m->cdev, "media%d", unit);
	if (error != 0) {
		free_unr(vmedia_units, unit);
		return (error);
	}
	m->registered = true;
	return (0);
}

void
vmedia_unregister(struct vmedia *m)
{

	if (m->registered) {
		destroy_dev(m->cdev);
		m->cdev = NULL;
		free_unr(vmedia_units, m->unit);
		m->registered = false;
	}
	sx_destroy(&m->lock);
}

void
vmedia_core_init(void)
{

	vmedia_units = new_unrhdr(0, VMEDIA_MAX_UNITS - 1, NULL);
}

void
vmedia_core_fini(void)
{

	delete_unrhdr(vmedia_units);
}
