/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * V4L2 controls: integer, boolean and menu controls, grouped into classes.
 *
 * The behaviour follows the V4L2 documentation and what v4l2-compliance
 * checks: out-of-range integers are clamped and rounded to the step, booleans
 * are normalised, an out-of-range menu index is ERANGE, a class control can
 * be enumerated but neither read nor written (EACCES), and a failing
 * VIDIOC_*_EXT_CTRLS reports the index of the offending control in error_idx,
 * or `count` when the failure is not specific to one control.
 *
 * Array and compound controls (arrays of integers, structures such as the
 * stateless codec parameters) carry a payload: userland passes a pointer and a
 * size, a dynamic array may hold fewer elements than its maximum, and a size
 * that does not fit is ENOSPC with the right size filled in.
 *
 * With which = V4L2_CTRL_WHICH_REQUEST_VAL the values are kept in a media
 * request (vreq.c) instead of being applied: they take effect when a driver
 * calls vctrl_request_setup() for the request's job, and once the request is
 * complete they can be read back as they were then.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/sx.h>

#include <dev/media/media_priv.h>

static MALLOC_DEFINE(M_VCTRL, "vctrl", "V4L2 controls");

#define	VCTRL_MAX_EXT	1024

struct vctrl_std {
	uint32_t	id;
	const char	*name;
	uint32_t	type;
	uint32_t	flags;
};

#define	CLASS_FLAGS	(V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_WRITE_ONLY)

static const struct vctrl_std vctrl_std_table[] = {
	{ V4L2_CID_USER_CLASS, "User Controls", V4L2_CTRL_TYPE_CTRL_CLASS, CLASS_FLAGS },
	{ V4L2_CID_BRIGHTNESS, "Brightness", V4L2_CTRL_TYPE_INTEGER, 0 },
	{ V4L2_CID_CONTRAST, "Contrast", V4L2_CTRL_TYPE_INTEGER, 0 },
	{ V4L2_CID_SATURATION, "Saturation", V4L2_CTRL_TYPE_INTEGER, 0 },
	{ V4L2_CID_HUE, "Hue", V4L2_CTRL_TYPE_INTEGER, 0 },
	{ V4L2_CID_AUTO_WHITE_BALANCE, "White Balance, Automatic", V4L2_CTRL_TYPE_BOOLEAN, 0 },
	{ V4L2_CID_GAIN, "Gain", V4L2_CTRL_TYPE_INTEGER, 0 },
	{ V4L2_CID_HFLIP, "Horizontal Flip", V4L2_CTRL_TYPE_BOOLEAN, 0 },
	{ V4L2_CID_VFLIP, "Vertical Flip", V4L2_CTRL_TYPE_BOOLEAN, 0 },
	{ V4L2_CID_IMAGE_PROC_CLASS, "Image Processing Controls", V4L2_CTRL_TYPE_CTRL_CLASS, CLASS_FLAGS },
	{ V4L2_CID_TEST_PATTERN, "Test Pattern", V4L2_CTRL_TYPE_MENU, 0 },
	{ V4L2_CID_CODEC_STATELESS_CLASS, "Stateless Codec Controls", V4L2_CTRL_TYPE_CTRL_CLASS, CLASS_FLAGS },
	{ V4L2_CID_STATELESS_H264_DECODE_MODE, "H264 Decode Mode", V4L2_CTRL_TYPE_MENU, 0 },
	{ V4L2_CID_STATELESS_H264_START_CODE, "H264 Start Code", V4L2_CTRL_TYPE_MENU, 0 },
};

static const struct vctrl_std *
vctrl_std_lookup(uint32_t id)
{
	size_t i;

	for (i = 0; i < nitems(vctrl_std_table); i++)
		if (vctrl_std_table[i].id == id)
			return (&vctrl_std_table[i]);
	return (NULL);
}

/* -------------------------------------------------------------- handler */

void
vctrl_handler_init(struct vctrl_handler *h)
{

	sx_init(&h->lock, "vctrl");
	TAILQ_INIT(&h->ctrls);
	h->error = 0;
}

static void
vctrl_free(struct vctrl *c)
{

	free(c->p_cur, M_VCTRL);
	free(c->p_def, M_VCTRL);
	free(c, M_VCTRL);
}

void
vctrl_handler_free(struct vctrl_handler *h)
{
	struct vctrl *c;

	vreq_handler_gone(h);
	while ((c = TAILQ_FIRST(&h->ctrls)) != NULL) {
		TAILQ_REMOVE(&h->ctrls, c, link);
		vctrl_free(c);
	}
	sx_destroy(&h->lock);
}

struct vctrl *
vctrl_find(struct vctrl_handler *h, uint32_t id)
{
	struct vctrl *c;

	TAILQ_FOREACH(c, &h->ctrls, link) {
		if (c->id == id)
			return (c);
		if (c->id > id)
			break;
	}
	return (NULL);
}

static struct vctrl *
vctrl_add(struct vctrl_handler *h, const struct vctrl_std *s)
{
	struct vctrl *c, *pos;

	c = malloc(sizeof(*c), M_VCTRL, M_WAITOK | M_ZERO);
	c->hdl = h;
	c->id = s->id;
	c->type = s->type;
	c->flags = s->flags;
	c->name = s->name;
	c->elem_size = sizeof(int32_t);
	c->elems = c->max_elems = 1;
	/* Keep the list sorted by id, which is the order NEXT_CTRL walks. */
	TAILQ_FOREACH(pos, &h->ctrls, link)
		if (pos->id > c->id)
			break;
	if (pos != NULL)
		TAILQ_INSERT_BEFORE(pos, c, link);
	else
		TAILQ_INSERT_TAIL(&h->ctrls, c, link);
	return (c);
}

/* Every class that has a control also has its class control. */
static int
vctrl_ensure_class(struct vctrl_handler *h, uint32_t id)
{
	const struct vctrl_std *s;
	uint32_t class_id = V4L2_CTRL_ID2WHICH(id) | 1;

	if (vctrl_find(h, class_id) != NULL)
		return (0);
	s = vctrl_std_lookup(class_id);
	if (s == NULL || s->type != V4L2_CTRL_TYPE_CTRL_CLASS)
		return (EINVAL);
	vctrl_add(h, s);
	return (0);
}

static struct vctrl *
vctrl_create(struct vctrl_handler *h, uint32_t id, uint32_t want_type)
{
	const struct vctrl_std *s;
	struct vctrl *c;

	if (h->error != 0)
		return (NULL);
	s = vctrl_std_lookup(id);
	if (s == NULL || vctrl_find(h, id) != NULL ||
	    (want_type == V4L2_CTRL_TYPE_MENU) != (s->type == V4L2_CTRL_TYPE_MENU) ||
	    s->type == V4L2_CTRL_TYPE_CTRL_CLASS) {
		h->error = EINVAL;
		return (NULL);
	}
	if (vctrl_ensure_class(h, id) != 0) {
		h->error = EINVAL;
		return (NULL);
	}
	c = vctrl_add(h, s);
	return (c);
}

struct vctrl *
vctrl_new_std(struct vctrl_handler *h, uint32_t id, int64_t min, int64_t max,
    uint64_t step, int64_t def)
{
	struct vctrl *c;

	c = vctrl_create(h, id, V4L2_CTRL_TYPE_INTEGER);
	if (c == NULL)
		return (NULL);
	if (c->type == V4L2_CTRL_TYPE_BOOLEAN) {
		min = 0; max = 1; step = 1;
	}
	if (min > max || step == 0 || def < min || def > max ||
	    (uint64_t)(max - min) % step != 0 ||
	    (uint64_t)(def - min) % step != 0) {
		h->error = EINVAL;
		TAILQ_REMOVE(&h->ctrls, c, link);
		vctrl_free(c);
		return (NULL);
	}
	c->min = min; c->max = max; c->step = step; c->def = def;
	c->val = def;
	return (c);
}

struct vctrl *
vctrl_new_std_menu_range(struct vctrl_handler *h, uint32_t id, int64_t min,
    int64_t max, int64_t def, const char * const *menu)
{
	struct vctrl *c;

	c = vctrl_create(h, id, V4L2_CTRL_TYPE_MENU);
	if (c == NULL)
		return (NULL);
	if (min < 0 || max < min || def < min || def > max || menu == NULL) {
		h->error = EINVAL;
		TAILQ_REMOVE(&h->ctrls, c, link);
		vctrl_free(c);
		return (NULL);
	}
	c->min = min; c->max = max; c->step = 1; c->def = def;
	c->menu = menu;
	c->val = def;
	return (c);
}

struct vctrl *
vctrl_new_std_menu(struct vctrl_handler *h, uint32_t id, int64_t max,
    int64_t def, const char * const *menu)
{

	return (vctrl_new_std_menu_range(h, id, 0, max, def, menu));
}

#define	VCTRL_MAX_PAYLOAD	(256 * 1024)

static uint32_t
vctrl_type_size(uint32_t type)
{

	switch (type) {
	case V4L2_CTRL_TYPE_U8:
		return (1);
	case V4L2_CTRL_TYPE_U16:
		return (2);
	case V4L2_CTRL_TYPE_AREA:
		return (sizeof(struct v4l2_area));
	default:
		return (sizeof(int32_t));
	}
}

struct vctrl *
vctrl_new_custom(struct vctrl_handler *h, const struct vctrl_config *cfg)
{
	struct vctrl *c, *pos;
	uint32_t i, elems = 1;
	bool payload, dyn;

	if (h->error != 0)
		return (NULL);
	if (cfg->id == 0 || cfg->name == NULL || vctrl_find(h, cfg->id) != NULL ||
	    cfg->nr_dims > V4L2_CTRL_MAX_DIMS ||
	    cfg->type == V4L2_CTRL_TYPE_CTRL_CLASS ||
	    vctrl_ensure_class(h, cfg->id) != 0) {
		h->error = EINVAL;
		return (NULL);
	}
	dyn = (cfg->flags & V4L2_CTRL_FLAG_DYNAMIC_ARRAY) != 0;
	for (i = 0; i < cfg->nr_dims; i++) {
		if (cfg->dims[i] == 0 || elems > UINT32_MAX / cfg->dims[i]) {
			h->error = EINVAL;
			return (NULL);
		}
		elems *= cfg->dims[i];
	}
	payload = cfg->nr_dims != 0 || cfg->type >= V4L2_CTRL_COMPOUND_TYPES;
	if (dyn && cfg->nr_dims == 0) {
		h->error = EINVAL;
		return (NULL);
	}
	if (!payload && cfg->type == V4L2_CTRL_TYPE_MENU) {
		if (cfg->menu == NULL || cfg->min != 0 || cfg->max < 0 ||
		    cfg->def < 0 || cfg->def > cfg->max) {
			h->error = EINVAL;
			return (NULL);
		}
	} else if (!payload && cfg->type != V4L2_CTRL_TYPE_INTEGER &&
	    cfg->type != V4L2_CTRL_TYPE_BOOLEAN) {
		h->error = EINVAL;
		return (NULL);
	}

	c = malloc(sizeof(*c), M_VCTRL, M_WAITOK | M_ZERO);
	c->hdl = h;
	c->id = cfg->id;
	c->name = cfg->name;
	c->type = cfg->type;
	c->flags = cfg->flags;
	c->menu = cfg->menu;
	c->check = cfg->check;
	c->min = cfg->min; c->max = cfg->max; c->def = cfg->def;
	c->step = cfg->step != 0 ? cfg->step : 1;
	if (c->type == V4L2_CTRL_TYPE_BOOLEAN) {
		c->min = 0; c->max = 1; c->step = 1;
	}
	c->elem_size = cfg->elem_size != 0 ? cfg->elem_size : vctrl_type_size(cfg->type);
	c->max_elems = elems;
	c->elems = dyn ? 1 : elems;
	c->nr_dims = cfg->nr_dims;
	memcpy(c->dims, cfg->dims, sizeof(c->dims));
	if (c->min > c->max || c->def < c->min || c->def > c->max ||
	    (uint64_t)c->elem_size * elems > VCTRL_MAX_PAYLOAD) {
		h->error = EINVAL;
		free(c, M_VCTRL);
		return (NULL);
	}
	if (payload) {
		c->flags |= V4L2_CTRL_FLAG_HAS_PAYLOAD;
		c->p_cur = malloc((size_t)c->elem_size * elems, M_VCTRL, M_WAITOK | M_ZERO);
		c->p_def = malloc((size_t)c->elem_size * elems, M_VCTRL, M_WAITOK | M_ZERO);
		/* Integer arrays start out at the default value. */
		for (i = 0; i < elems && c->def != 0; i++) {
			switch (c->elem_size) {
			case 1:
				c->p_def[i] = (uint8_t)c->def;
				break;
			case 2:
				((uint16_t *)c->p_def)[i] = (uint16_t)c->def;
				break;
			case 4:
				if (c->type == V4L2_CTRL_TYPE_INTEGER ||
				    c->type == V4L2_CTRL_TYPE_U32)
					((uint32_t *)c->p_def)[i] = (uint32_t)c->def;
				break;
			}
		}
		memcpy(c->p_cur, c->p_def, (size_t)c->elem_size * elems);
	} else {
		c->val = c->def;
	}
	TAILQ_FOREACH(pos, &h->ctrls, link)
		if (pos->id > c->id)
			break;
	if (pos != NULL)
		TAILQ_INSERT_BEFORE(pos, c, link);
	else
		TAILQ_INSERT_TAIL(&h->ctrls, c, link);
	return (c);
}

int
vctrl_get_payload(struct vctrl *c, void *out, size_t size)
{
	size_t n;
	int error = 0;

	sx_slock(&c->hdl->lock);
	n = (size_t)c->elems * c->elem_size;
	if (c->p_cur == NULL)
		error = EINVAL;
	else if (size < n)
		error = ENOSPC;
	else
		memcpy(out, c->p_cur, n);
	sx_sunlock(&c->hdl->lock);
	return (error);
}

int64_t
vctrl_get(struct vctrl *c)
{
	int64_t v;

	sx_slock(&c->hdl->lock);
	v = c->val;
	sx_sunlock(&c->hdl->lock);
	return (v);
}

/* -------------------------------------------------------------- queries */

/*
 * Resolve the id of a query. With NEXT_CTRL the next regular control is
 * returned, with NEXT_COMPOUND the next compound one, with both the next of
 * either kind; without them the id is looked up exactly.
 */
static struct vctrl *
vctrl_query_find(struct vctrl_handler *h, uint32_t id)
{
	struct vctrl *c;
	uint32_t real = id & V4L2_CTRL_ID_MASK;
	uint32_t next = id & (V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND);

	if (h == NULL)
		return (NULL);
	if (next == 0)
		return (vctrl_find(h, real));
	TAILQ_FOREACH(c, &h->ctrls, link) {
		bool compound = c->type >= V4L2_CTRL_COMPOUND_TYPES;

		if (c->id <= real)
			continue;
		if (compound ? (next & V4L2_CTRL_FLAG_NEXT_COMPOUND) != 0 :
		    (next & V4L2_CTRL_FLAG_NEXT_CTRL) != 0)
			return (c);
	}
	return (NULL);
}

static void
vctrl_range(const struct vctrl *c, int64_t *min, int64_t *max, uint64_t *step,
    int64_t *def)
{

	switch (c->type) {
	case V4L2_CTRL_TYPE_CTRL_CLASS:
		*min = *max = *def = 0;
		*step = 0;
		break;
	default:
		*min = c->min; *max = c->max; *step = c->step; *def = c->def;
		break;
	}
}

int
vctrl_ioctl_queryctrl(struct vctrl_handler *h, struct v4l2_queryctrl *q)
{
	struct vctrl *c;
	int64_t min, max, def;
	uint64_t step;

	if (h == NULL)
		return (EINVAL);
	sx_slock(&h->lock);
	c = vctrl_query_find(h, q->id);
	if (c == NULL) {
		sx_sunlock(&h->lock);
		return (EINVAL);
	}
	vctrl_range(c, &min, &max, &step, &def);
	memset(q, 0, sizeof(*q));
	q->id = c->id;
	q->type = c->type;
	strlcpy((char *)q->name, c->name, sizeof(q->name));
	/* The legacy query has no room for a compound control's range. */
	if (c->type < V4L2_CTRL_COMPOUND_TYPES) {
		q->minimum = (int32_t)min;
		q->maximum = (int32_t)max;
		q->step = (int32_t)step;
		q->default_value = (int32_t)def;
	}
	q->flags = c->flags;
	sx_sunlock(&h->lock);
	return (0);
}

int
vctrl_ioctl_query_ext_ctrl(struct vctrl_handler *h, struct v4l2_query_ext_ctrl *q)
{
	struct vctrl *c;
	int64_t min, max, def;
	uint64_t step;

	if (h == NULL)
		return (EINVAL);
	sx_slock(&h->lock);
	c = vctrl_query_find(h, q->id);
	if (c == NULL) {
		sx_sunlock(&h->lock);
		return (EINVAL);
	}
	vctrl_range(c, &min, &max, &step, &def);
	memset(q, 0, sizeof(*q));
	q->id = c->id;
	q->type = c->type;
	strlcpy((char *)q->name, c->name, sizeof(q->name));
	q->minimum = min;
	q->maximum = max;
	q->step = step;
	q->default_value = def;
	q->flags = c->flags;
	q->elem_size = c->elem_size;
	q->elems = c->elems;
	q->nr_of_dims = c->nr_dims;
	memcpy(q->dims, c->dims, sizeof(q->dims));
	sx_sunlock(&h->lock);
	return (0);
}

int
vctrl_ioctl_querymenu(struct vctrl_handler *h, struct v4l2_querymenu *m)
{
	struct vctrl *c;
	uint32_t id = m->id, index = m->index;
	int error = EINVAL;

	if (h == NULL)
		return (EINVAL);
	sx_slock(&h->lock);
	c = vctrl_find(h, id);
	if (c != NULL && c->type == V4L2_CTRL_TYPE_MENU &&
	    (int64_t)index >= c->min && (int64_t)index <= c->max &&
	    c->menu[index] != NULL) {
		memset(m, 0, sizeof(*m));
		m->id = id;
		m->index = index;
		strlcpy((char *)m->name, c->menu[index], sizeof(m->name));
		error = 0;
	}
	sx_sunlock(&h->lock);
	return (error);
}

/* -------------------------------------------------------------- get/set */

/* Clamp to the range and round to the step. */
static int64_t
vctrl_clamp(const struct vctrl *c, int64_t v)
{

	if (v < c->min)
		v = c->min;
	else if (v > c->max)
		v = c->max;
	else if (c->step > 1)
		v = c->min + (((v - c->min) + (int64_t)(c->step / 2)) /
		    (int64_t)c->step) * (int64_t)c->step;
	if (v > c->max)
		v = c->max;
	return (v);
}

/* Round to the step and clamp (integers), normalise (booleans). */
static int
vctrl_validate(const struct vctrl *c, int64_t *val)
{
	int64_t v = *val;

	switch (c->type) {
	case V4L2_CTRL_TYPE_INTEGER:
		v = vctrl_clamp(c, v);
		break;
	case V4L2_CTRL_TYPE_BOOLEAN:
		v = (v != 0);
		break;
	case V4L2_CTRL_TYPE_MENU:
		if (v < c->min || v > c->max || c->menu[v] == NULL)
			return (ERANGE);
		break;
	default:
		return (EINVAL);
	}
	*val = v;
	return (0);
}

int
vctrl_ioctl_g_ctrl(struct vctrl_handler *h, struct v4l2_control *ctl)
{
	struct vctrl *c;
	int error = 0;

	if (h == NULL)
		return (EINVAL);
	sx_slock(&h->lock);
	c = vctrl_find(h, ctl->id);
	if (c == NULL || c->p_cur != NULL)
		error = EINVAL;
	else if (c->flags & V4L2_CTRL_FLAG_WRITE_ONLY)
		error = EACCES;
	else
		ctl->value = (int32_t)c->val;
	sx_sunlock(&h->lock);
	return (error);
}

/*
 * Apply `val` to c through the driver and tell subscribers. The handler lock
 * is held exclusively. `src` is the file that asked (it does not get the event
 * unless it subscribed with ALLOW_FEEDBACK).
 */
static int
vctrl_apply(struct vdev *v, struct vfh *src, struct vctrl *c, int64_t val)
{
	int64_t old = c->val;
	int error;

	if (val == old)
		return (0);
	c->val = val;
	error = VDEV_S_CTRL(v, src, c);
	if (error != 0) {
		c->val = old;
		return (error);
	}
	vevent_ctrl_changed(v, c, V4L2_EVENT_CTRL_CH_VALUE, src);
	return (0);
}

int
vctrl_ioctl_s_ctrl(struct vdev *v, struct vfh *fh, struct v4l2_control *ctl)
{
	struct vctrl_handler *h = fh->ctrl_handler;
	struct vctrl *c;
	int64_t val;
	int error;

	if (h == NULL)
		return (EINVAL);
	sx_xlock(&h->lock);
	c = vctrl_find(h, ctl->id);
	if (c == NULL || c->p_cur != NULL) {
		error = EINVAL;
	} else if (c->flags & (V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_GRABBED)) {
		error = EACCES;
	} else {
		val = ctl->value;
		error = vctrl_validate(c, &val);
		if (error == 0) {
			error = vctrl_apply(v, fh, c, val);
			if (error == 0)
				ctl->value = (int32_t)c->val;
		}
	}
	sx_xunlock(&h->lock);
	return (error);
}

/* ------------------------------------------------------- payload controls */

/* Clamp every element of an integer array into the control's range. */
static void
vctrl_clamp_payload(const struct vctrl *c, uint8_t *p, uint32_t elems)
{
	uint32_t i;

	for (i = 0; i < elems; i++) {
		switch (c->type) {
		case V4L2_CTRL_TYPE_U8:
			p[i] = (uint8_t)vctrl_clamp(c, p[i]);
			break;
		case V4L2_CTRL_TYPE_U16:
			((uint16_t *)p)[i] = (uint16_t)vctrl_clamp(c, ((uint16_t *)p)[i]);
			break;
		case V4L2_CTRL_TYPE_U32:
			((uint32_t *)p)[i] = (uint32_t)vctrl_clamp(c, ((uint32_t *)p)[i]);
			break;
		case V4L2_CTRL_TYPE_INTEGER:
			((int32_t *)p)[i] = (int32_t)vctrl_clamp(c, ((int32_t *)p)[i]);
			break;
		default:		/* structures: the driver knows what is valid */
			return;
		}
	}
}

static bool
vctrl_dynamic(const struct vctrl *c)
{

	return ((c->flags & V4L2_CTRL_FLAG_DYNAMIC_ARRAY) != 0);
}

/*
 * The size a payload of c has when read (the current one), and what size a
 * written payload may have. A fixed array takes exactly its size, a dynamic
 * one any whole number of elements up to its maximum. Returns 0, or ENOSPC /
 * EINVAL with *size set to the largest acceptable size.
 */
static int
vctrl_payload_size_ok(const struct vctrl *c, uint32_t *size)
{
	uint32_t max = c->max_elems * c->elem_size;

	if (!vctrl_dynamic(c)) {
		if (*size < max) {
			*size = max;
			return (ENOSPC);
		}
		*size = max;
		return (0);
	}
	if (*size > max) {
		*size = max;
		return (ENOSPC);
	}
	if (*size == 0 || *size % c->elem_size != 0)
		return (EINVAL);
	return (0);
}

/*
 * Make `data` (`size` bytes, already validated) c's current value through the
 * driver, undoing it if the driver refuses. Handler lock held exclusively.
 */
static int
vctrl_apply_payload(struct vdev *v, struct vfh *src, struct vctrl *c,
    const uint8_t *data, uint32_t size)
{
	uint8_t *old;
	uint32_t old_elems = c->elems, old_size = c->elems * c->elem_size;
	int error;

	if (old_size == size && memcmp(c->p_cur, data, size) == 0)
		return (0);
	old = malloc(old_size, M_VCTRL, M_WAITOK);
	memcpy(old, c->p_cur, old_size);
	memcpy(c->p_cur, data, size);
	c->elems = size / c->elem_size;
	error = VDEV_S_CTRL(v, src, c);
	if (error != 0) {
		memcpy(c->p_cur, old, old_size);
		c->elems = old_elems;
	} else {
		vevent_ctrl_changed(v, c, V4L2_EVENT_CTRL_CH_VALUE, src);
	}
	free(old, M_VCTRL);
	return (error);
}

/* ------------------------------------------------------------ requests */

/* Store (or replace) the value of c in the request. clk held exclusively. */
static void
vctrl_req_store(struct vreq *req, const struct vctrl *c, const uint8_t *data,
    uint32_t size, int64_t val, bool set)
{
	struct vreq_ctrl *e;

	e = vreq_ctrl_find(req, c->id);
	if (e == NULL)
		e = vreq_ctrl_add(req, c->id);
	if (set)
		e->set = true;
	e->val = val;
	if (c->p_cur != NULL) {
		if (e->size != size) {
			free(e->data, M_VREQ);
			e->data = malloc(size, M_VREQ, M_WAITOK);
			e->size = size;
		}
		memcpy(e->data, data, size);
	}
}

void
vctrl_request_setup(struct vbuf *b, struct vctrl_handler *h)
{
	struct vreq *req = b->req;
	struct vreq_ctrl *e;
	struct vctrl *c;

	if (req == NULL || h == NULL)
		return;
	sx_xlock(&h->lock);
	sx_xlock(&req->clk);
	if (req->hdl == NULL)
		req->hdl = h;
	if (req->hdl == h) {
		/* What userland set takes effect... */
		TAILQ_FOREACH(e, &req->ctrls, link) {
			if (!e->set || (c = vctrl_find(h, e->id)) == NULL)
				continue;
			if (c->p_cur != NULL)
				(void)vctrl_apply_payload(h->vdev, h->owner, c,
				    e->data, e->size);
			else
				(void)vctrl_apply(h->vdev, h->owner, c, e->val);
		}
		/* ... and the request remembers how everything stood. */
		TAILQ_FOREACH(c, &h->ctrls, link) {
			if (c->type == V4L2_CTRL_TYPE_CTRL_CLASS)
				continue;
			vctrl_req_store(req, c, c->p_cur,
			    c->elems * c->elem_size, c->val, false);
		}
	}
	sx_xunlock(&req->clk);
	sx_xunlock(&h->lock);
}

/* -------------------------------------------------------- extended controls */

static int
vctrl_which_ok(struct vdev *v, struct vctrl_handler *h, uint32_t which)
{

	if (which == V4L2_CTRL_WHICH_CUR_VAL || which == V4L2_CTRL_WHICH_DEF_VAL)
		return (0);
	if (which == V4L2_CTRL_WHICH_REQUEST_VAL)
		return (v->mdev != NULL && v->requests ? 0 : EINVAL);
	/* A class: valid only if the handler has that class. */
	return (h != NULL && vctrl_find(h, which | 1) != NULL ? 0 : EINVAL);
}

/*
 * G/S/TRY_EXT_CTRLS. error_idx follows what v4l2-compliance expects of Linux:
 *
 *   TRY   a control that fails validation: its index
 *   GET   any failure of the request itself (bad id, write-only): count
 *   SET   a failure found before anything is set: count; a control the driver
 *         refused while setting: its index (the ones before it are set)
 *
 * DEF_VAL can be read but not written or tried. A payload that does not fit is
 * ENOSPC (or EINVAL for a bad size of a dynamic array) with error_idx at that
 * control and its size set to what would.
 *
 * REQUEST_VAL: S stores the values in the request (which must be idle) without
 * applying them; G reads them back, once the request is complete.
 */
int
vctrl_ioctl_ext_ctrls(struct vdev *v, struct vfh *fh, u_long cmd,
    struct v4l2_ext_controls *ec)
{
	struct vctrl_handler *h = fh->ctrl_handler;
	struct v4l2_ext_control *ctrls = NULL;
	struct vctrl **cl = NULL;
	int64_t *newv = NULL;
	uint8_t **newp = NULL;		/* validated payloads of S/TRY */
	struct vreq *req = NULL;
	uint32_t i, which = ec->which;
	bool set = (cmd == VIDIOC_S_EXT_CTRLS), get = (cmd == VIDIOC_G_EXT_CTRLS);
	bool rq = (which == V4L2_CTRL_WHICH_REQUEST_VAL);
	int error;

	ec->error_idx = ec->count;
	error = vctrl_which_ok(v, h, which);
	if (error != 0)
		return (error);
	if (!get && which == V4L2_CTRL_WHICH_DEF_VAL)
		return (EINVAL);
	if (rq) {
		if (cmd == VIDIOC_TRY_EXT_CTRLS)
			return (EINVAL);
		error = vreq_from_fd(ec->request_fd, v->mdev->serial_no, curthread,
		    &req);
		if (error != 0)
			return (error);
		vreq_slock();
		/* Values can only be read from a complete request, set in an idle one. */
		if (get ? !vreq_is_state(req, VREQ_COMPLETE) :
		    !vreq_is_state(req, VREQ_IDLE)) {
			error = get ? EACCES : EBUSY;
			goto out;
		}
	}
	if (ec->count == 0)
		goto out;
	if (h == NULL || ec->count > VCTRL_MAX_EXT || ec->controls == NULL) {
		error = EINVAL;
		goto out;
	}

	ctrls = malloc(ec->count * sizeof(*ctrls), M_VCTRL, M_WAITOK);
	cl = malloc(ec->count * sizeof(*cl), M_VCTRL, M_WAITOK | M_ZERO);
	newv = malloc(ec->count * sizeof(*newv), M_VCTRL, M_WAITOK | M_ZERO);
	newp = malloc(ec->count * sizeof(*newp), M_VCTRL, M_WAITOK | M_ZERO);
	error = copyin(ec->controls, ctrls, ec->count * sizeof(*ctrls));
	if (error != 0)
		goto out;

	if (set)
		sx_xlock(&h->lock);
	else
		sx_slock(&h->lock);
	if (rq && req->hdl != NULL && req->hdl != h && set) {
		error = EINVAL;		/* the request's controls belong to another file */
		goto unlock;
	}

	/* Validate every control first: nothing is set unless all are fine. */
	for (i = 0; i < ec->count; i++) {
		struct vctrl *c = vctrl_find(h, ctrls[i].id);
		uint32_t size;

		/* Where to report a failure of control i, see the comment above. */
		ec->error_idx = (cmd == VIDIOC_TRY_EXT_CTRLS) ? i : ec->count;
		if (c == NULL ||
		    (which != V4L2_CTRL_WHICH_CUR_VAL &&
		    which != V4L2_CTRL_WHICH_DEF_VAL && !rq &&
		    V4L2_CTRL_ID2WHICH(c->id) != which)) {
			error = EINVAL;
			goto unlock;
		}
		cl[i] = c;
		if (get) {
			if (c->flags & V4L2_CTRL_FLAG_WRITE_ONLY) {
				error = EACCES;
				goto unlock;
			}
			if (c->p_cur != NULL) {
				struct vreq_ctrl *e = NULL;

				if (rq) {
					sx_slock(&req->clk);
					e = vreq_ctrl_find(req, c->id);
					sx_sunlock(&req->clk);
				}
				size = e != NULL && e->data != NULL ? e->size :
				    c->elems * c->elem_size;
				if (which == V4L2_CTRL_WHICH_DEF_VAL)
					size = (vctrl_dynamic(c) ? 1 : c->max_elems) *
					    c->elem_size;
				if (ctrls[i].size < size) {
					ctrls[i].size = size;
					ec->error_idx = i;
					error = ENOSPC;
					goto unlock;
				}
			}
			continue;
		}
		if (c->flags & V4L2_CTRL_FLAG_READ_ONLY) {
			error = EACCES;
			goto unlock;
		}
		if (c->p_cur != NULL) {
			size = ctrls[i].size;
			error = vctrl_payload_size_ok(c, &size);
			if (error != 0) {
				ctrls[i].size = size;
				ec->error_idx = i;
				goto unlock;
			}
			if (ctrls[i].ptr == NULL) {
				error = EINVAL;
				goto unlock;
			}
			if (!vctrl_dynamic(c))
				size = c->max_elems * c->elem_size;
			else
				size = ctrls[i].size;
			newp[i] = malloc(size, M_VCTRL, M_WAITOK);
			error = copyin(ctrls[i].ptr, newp[i], size);
			if (error != 0)
				goto unlock;
			ctrls[i].size = size;
			vctrl_clamp_payload(c, newp[i], size / c->elem_size);
			/* The driver's own limits: nothing is set if it refuses. */
			if (c->check != NULL) {
				error = c->check(c, newp[i], size / c->elem_size);
				if (error != 0)
					goto unlock;
			}
			continue;
		}
		newv[i] = ctrls[i].value;
		error = vctrl_validate(c, &newv[i]);
		if (error != 0)
			goto unlock;
	}
	ec->error_idx = ec->count;

	if (get) {
		for (i = 0; i < ec->count; i++) {
			struct vctrl *c = cl[i];
			struct vreq_ctrl *e = NULL;
			const uint8_t *src;
			uint32_t size;
			int64_t val;

			if (rq) {
				sx_slock(&req->clk);
				e = vreq_ctrl_find(req, c->id);
			}
			val = which == V4L2_CTRL_WHICH_DEF_VAL ? c->def :
			    e != NULL ? e->val : c->val;
			if (c->p_cur != NULL) {
				if (which == V4L2_CTRL_WHICH_DEF_VAL) {
					src = c->p_def;
					size = (vctrl_dynamic(c) ? 1 : c->max_elems) *
					    c->elem_size;
				} else if (e != NULL && e->data != NULL) {
					src = e->data;
					size = e->size;
				} else {
					src = c->p_cur;
					size = c->elems * c->elem_size;
				}
				error = copyout(src, ctrls[i].ptr, size);
				ctrls[i].size = size;
			} else {
				ctrls[i].value = (int32_t)val;
				if (c->type == V4L2_CTRL_TYPE_INTEGER64)
					ctrls[i].value64 = val;
			}
			if (rq)
				sx_sunlock(&req->clk);
			if (error != 0) {
				ec->error_idx = i;
				goto unlock;
			}
		}
	} else {
		if (rq && set) {
			sx_xlock(&req->clk);
			req->hdl = h;
		}
		for (i = 0; i < ec->count; i++) {
			struct vctrl *c = cl[i];

			/* TRY returns the value the control would take. */
			if (c->p_cur != NULL) {
				if (!vctrl_dynamic(c))
					ctrls[i].size = c->max_elems * c->elem_size;
				if (copyout(newp[i], ctrls[i].ptr, ctrls[i].size) != 0 &&
				    error == 0) {
					error = EFAULT;
					ec->error_idx = i;
				}
			} else {
				ctrls[i].value = (int32_t)newv[i];
			}
			if (error != 0 || !set)
				continue;
			if (rq) {
				vctrl_req_store(req, c, newp[i], ctrls[i].size,
				    newv[i], true);
				continue;
			}
			error = c->p_cur != NULL ?
			    vctrl_apply_payload(v, fh, c, newp[i], ctrls[i].size) :
			    vctrl_apply(v, fh, c, newv[i]);
			if (error != 0) {
				/* Earlier controls are set; this one and later are not. */
				ec->error_idx = i;
				break;
			}
		}
		if (rq && set)
			sx_xunlock(&req->clk);
	}
unlock:
	sx_unlock(&h->lock);
	/* The values are reported even on error, as for a partial S_EXT_CTRLS. */
	if (copyout(ctrls, ec->controls, ec->count * sizeof(*ctrls)) != 0 &&
	    error == 0)
		error = EFAULT;
out:
	if (ctrls != NULL) {
		for (i = 0; i < ec->count; i++)
			free(newp[i], M_VCTRL);
	}
	free(newp, M_VCTRL);
	free(newv, M_VCTRL);
	free(cl, M_VCTRL);
	free(ctrls, M_VCTRL);
	if (req != NULL) {
		vreq_sunlock();
		vreq_put(req);
	}
	return (error);
}
