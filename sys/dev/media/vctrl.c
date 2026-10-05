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
 * Compound controls (stateless codec parameters) arrive with the request API.
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

void
vctrl_handler_free(struct vctrl_handler *h)
{
	struct vctrl *c;

	while ((c = TAILQ_FIRST(&h->ctrls)) != NULL) {
		TAILQ_REMOVE(&h->ctrls, c, link);
		free(c, M_VCTRL);
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
		free(c, M_VCTRL);
		return (NULL);
	}
	c->min = min; c->max = max; c->step = step; c->def = def;
	c->val = def;
	return (c);
}

struct vctrl *
vctrl_new_std_menu(struct vctrl_handler *h, uint32_t id, int64_t max,
    int64_t def, const char * const *menu)
{
	struct vctrl *c;

	c = vctrl_create(h, id, V4L2_CTRL_TYPE_MENU);
	if (c == NULL)
		return (NULL);
	if (max < 0 || def < 0 || def > max || menu == NULL) {
		h->error = EINVAL;
		TAILQ_REMOVE(&h->ctrls, c, link);
		free(c, M_VCTRL);
		return (NULL);
	}
	c->min = 0; c->max = max; c->step = 1; c->def = def;
	c->menu = menu;
	c->val = def;
	return (c);
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
	q->minimum = (int32_t)min;
	q->maximum = (int32_t)max;
	q->step = (int32_t)step;
	q->default_value = (int32_t)def;
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
	q->elem_size = sizeof(int32_t);
	q->elems = 1;
	q->nr_of_dims = 0;
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

/* Round to the step and clamp (integers), normalise (booleans). */
static int
vctrl_validate(const struct vctrl *c, int64_t *val)
{
	int64_t v = *val;

	switch (c->type) {
	case V4L2_CTRL_TYPE_INTEGER:
		if (v < c->min)
			v = c->min;
		else if (v > c->max)
			v = c->max;
		else if (c->step > 1)
			v = c->min + (((v - c->min) + (int64_t)(c->step / 2)) /
			    (int64_t)c->step) * (int64_t)c->step;
		if (v > c->max)
			v = c->max;
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
	if (c == NULL)
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
	error = VDEV_S_CTRL(v, c);
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
	struct vctrl_handler *h = v->ctrl_handler;
	struct vctrl *c;
	int64_t val;
	int error;

	if (h == NULL)
		return (EINVAL);
	sx_xlock(&h->lock);
	c = vctrl_find(h, ctl->id);
	if (c == NULL) {
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

/* ------------------------------------------------------- extended controls */

static int
vctrl_which_ok(struct vctrl_handler *h, uint32_t which)
{

	if (which == V4L2_CTRL_WHICH_CUR_VAL || which == V4L2_CTRL_WHICH_DEF_VAL)
		return (0);
	if (which == V4L2_CTRL_WHICH_REQUEST_VAL)
		return (EINVAL);		/* needs the request API */
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
 * DEF_VAL can be read but not written or tried.
 */
int
vctrl_ioctl_ext_ctrls(struct vdev *v, struct vfh *fh, u_long cmd,
    struct v4l2_ext_controls *ec)
{
	struct vctrl_handler *h = v->ctrl_handler;
	struct v4l2_ext_control *ctrls = NULL;
	struct vctrl **cl = NULL;
	int64_t *newv = NULL;
	uint32_t i, which = ec->which;
	bool set = (cmd == VIDIOC_S_EXT_CTRLS), get = (cmd == VIDIOC_G_EXT_CTRLS);
	int error;

	ec->error_idx = ec->count;
	error = vctrl_which_ok(h, which);
	if (error != 0)
		return (error);
	if (!get && which == V4L2_CTRL_WHICH_DEF_VAL)
		return (EINVAL);
	if (ec->count == 0)
		return (0);
	if (h == NULL || ec->count > VCTRL_MAX_EXT || ec->controls == NULL)
		return (EINVAL);

	ctrls = malloc(ec->count * sizeof(*ctrls), M_VCTRL, M_WAITOK);
	cl = malloc(ec->count * sizeof(*cl), M_VCTRL, M_WAITOK | M_ZERO);
	newv = malloc(ec->count * sizeof(*newv), M_VCTRL, M_WAITOK | M_ZERO);
	error = copyin(ec->controls, ctrls, ec->count * sizeof(*ctrls));
	if (error != 0)
		goto out;

	if (set)
		sx_xlock(&h->lock);
	else
		sx_slock(&h->lock);

	/* Validate every control first: nothing is set unless all are fine. */
	for (i = 0; i < ec->count; i++) {
		struct vctrl *c = vctrl_find(h, ctrls[i].id);

		/* Where to report a failure of control i, see the comment above. */
		ec->error_idx = (cmd == VIDIOC_TRY_EXT_CTRLS) ? i : ec->count;
		if (c == NULL ||
		    (which != V4L2_CTRL_WHICH_CUR_VAL &&
		    which != V4L2_CTRL_WHICH_DEF_VAL &&
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
			continue;
		}
		if (c->flags & V4L2_CTRL_FLAG_READ_ONLY) {
			error = EACCES;
			goto unlock;
		}
		newv[i] = ctrls[i].value;
		error = vctrl_validate(c, &newv[i]);
		if (error != 0)
			goto unlock;
	}
	ec->error_idx = ec->count;

	if (get) {
		for (i = 0; i < ec->count; i++)
			ctrls[i].value = (int32_t)(which == V4L2_CTRL_WHICH_DEF_VAL ?
			    cl[i]->def : cl[i]->val);
	} else {
		for (i = 0; i < ec->count; i++) {
			/* TRY returns the value the control would take. */
			ctrls[i].value = (int32_t)newv[i];
			if (!set)
				continue;
			error = vctrl_apply(v, fh, cl[i], newv[i]);
			if (error != 0) {
				/* Earlier controls are set; this one and later are not. */
				ec->error_idx = i;
				goto unlock;
			}
		}
	}
unlock:
	sx_unlock(&h->lock);
	/* The values are reported even on error, as for a partial S_EXT_CTRLS. */
	if (copyout(ctrls, ec->controls, ec->count * sizeof(*ctrls)) != 0 &&
	    error == 0)
		error = EFAULT;
out:
	free(newv, M_VCTRL);
	free(cl, M_VCTRL);
	free(ctrls, M_VCTRL);
	return (error);
}
