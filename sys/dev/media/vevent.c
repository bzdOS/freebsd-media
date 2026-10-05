/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * V4L2 events: a small queue per open file, filled for the events that file
 * subscribed to.
 *
 * Today the only event the core generates itself is V4L2_EVENT_CTRL (a control
 * changed). Drivers can queue others with vdev_event_queue(); a file can only
 * subscribe to a type the core knows how to validate, which for now means
 * controls.
 *
 * A file's queue holds VEVENT_QUEUE_LEN events; when it is full the oldest is
 * dropped. `pending` in a dequeued event tells how many are still waiting.
 *
 * Locking: fh->evmtx protects the queue and the subscriptions. Delivery runs
 * under the core's global lock (it walks the device's file list), so the order
 * is global lock, then fh->evmtx.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/poll.h>
#include <sys/proc.h>
#include <sys/selinfo.h>
#include <sys/sx.h>
#include <sys/time.h>

#include <dev/media/media_priv.h>

static MALLOC_DEFINE(M_VEVENT, "vevent", "V4L2 events");

void
vevent_fh_init(struct vfh *fh)
{

	mtx_init(&fh->evmtx, "vevent", NULL, MTX_DEF);
	TAILQ_INIT(&fh->subs);
}

void
vevent_fh_destroy(struct vfh *fh)
{
	struct vevent_sub *s;

	while ((s = TAILQ_FIRST(&fh->subs)) != NULL) {
		TAILQ_REMOVE(&fh->subs, s, link);
		free(s, M_VEVENT);
	}
	seldrain(&fh->esel);
	mtx_destroy(&fh->evmtx);
}

static struct vevent_sub *
vevent_find(struct vfh *fh, uint32_t type, uint32_t id)
{
	struct vevent_sub *s;

	mtx_assert(&fh->evmtx, MA_OWNED);
	TAILQ_FOREACH(s, &fh->subs, link)
		if (s->type == type && s->id == id)
			return (s);
	return (NULL);
}

/* Append to the file's ring, dropping the oldest event if it is full. */
static void
vevent_push(struct vfh *fh, const struct v4l2_event *ev)
{
	struct v4l2_event *e;
	struct timespec ts;

	mtx_assert(&fh->evmtx, MA_OWNED);
	if (fh->ev_count == VEVENT_QUEUE_LEN) {
		fh->ev_head = (fh->ev_head + 1) % VEVENT_QUEUE_LEN;
		fh->ev_count--;
	}
	e = &fh->events[(fh->ev_head + fh->ev_count) % VEVENT_QUEUE_LEN];
	*e = *ev;
	e->sequence = fh->ev_seq++;
	nanouptime(&ts);
	e->timestamp = ts;
	fh->ev_count++;
	wakeup(&fh->events);
	selwakeuppri(&fh->esel, PRIBIO);
}

void
vevent_deliver(struct vfh *fh, const struct v4l2_event *ev, struct vfh *src)
{
	struct vevent_sub *s;

	mtx_lock(&fh->evmtx);
	s = vevent_find(fh, ev->type, ev->id);
	if (s != NULL && (fh != src || (s->flags & V4L2_EVENT_SUB_FL_ALLOW_FEEDBACK)))
		vevent_push(fh, ev);
	mtx_unlock(&fh->evmtx);
}

static void
vevent_fill_ctrl(struct v4l2_event *ev, const struct vctrl *c, uint32_t changes)
{

	memset(ev, 0, sizeof(*ev));
	ev->type = V4L2_EVENT_CTRL;
	ev->id = c->id;
	ev->u.ctrl.changes = changes;
	ev->u.ctrl.type = c->type;
	ev->u.ctrl.value = (int32_t)c->val;
	ev->u.ctrl.flags = c->flags;
	ev->u.ctrl.minimum = (int32_t)c->min;
	ev->u.ctrl.maximum = (int32_t)c->max;
	ev->u.ctrl.step = (int32_t)c->step;
	ev->u.ctrl.default_value = (int32_t)c->def;
}

void
vevent_ctrl_changed(struct vdev *v, struct vctrl *c, uint32_t changes,
    struct vfh *src)
{
	struct v4l2_event ev;

	vevent_fill_ctrl(&ev, c, changes);
	vdev_event_queue(v, &ev, src);
}

int
vevent_subscribe(struct vfh *fh, struct vctrl_handler *h,
    struct v4l2_event_subscription *sub)
{
	struct vevent_sub *s, *fresh = NULL;
	struct vctrl *c = NULL;
	struct v4l2_event ev;
	bool initial = false;

	if (sub->type != V4L2_EVENT_CTRL || h == NULL)
		return (EINVAL);
	sx_slock(&h->lock);
	c = vctrl_find(h, sub->id);
	if (c == NULL) {
		sx_sunlock(&h->lock);
		return (EINVAL);
	}
	if ((sub->flags & V4L2_EVENT_SUB_FL_SEND_INITIAL) != 0 &&
	    c->type != V4L2_CTRL_TYPE_CTRL_CLASS) {
		vevent_fill_ctrl(&ev, c, V4L2_EVENT_CTRL_CH_VALUE |
		    V4L2_EVENT_CTRL_CH_FLAGS | V4L2_EVENT_CTRL_CH_RANGE);
		initial = true;
	}
	sx_sunlock(&h->lock);

	fresh = malloc(sizeof(*fresh), M_VEVENT, M_WAITOK | M_ZERO);
	mtx_lock(&fh->evmtx);
	s = vevent_find(fh, sub->type, sub->id);
	if (s == NULL) {
		s = fresh;
		fresh = NULL;
		s->type = sub->type;
		s->id = sub->id;
		TAILQ_INSERT_TAIL(&fh->subs, s, link);
	}
	s->flags = sub->flags;
	if (initial)
		vevent_push(fh, &ev);
	mtx_unlock(&fh->evmtx);
	free(fresh, M_VEVENT);
	return (0);
}

int
vevent_unsubscribe(struct vfh *fh, const struct v4l2_event_subscription *sub)
{
	struct vevent_sub *s, *tmp;

	mtx_lock(&fh->evmtx);
	TAILQ_FOREACH_SAFE(s, &fh->subs, link, tmp) {
		if (sub->type == V4L2_EVENT_ALL ||
		    (s->type == sub->type && s->id == sub->id)) {
			TAILQ_REMOVE(&fh->subs, s, link);
			free(s, M_VEVENT);
		}
	}
	mtx_unlock(&fh->evmtx);
	return (0);
}

int
vevent_dequeue(struct vfh *fh, struct v4l2_event *ev, bool nonblock)
{
	int error;

	mtx_lock(&fh->evmtx);
	while (fh->ev_count == 0) {
		if (nonblock) {
			mtx_unlock(&fh->evmtx);
			return (ENOENT);
		}
		error = msleep(&fh->events, &fh->evmtx, PCATCH | PRIBIO, "vevent", 0);
		if (error != 0) {
			mtx_unlock(&fh->evmtx);
			return (error);
		}
	}
	*ev = fh->events[fh->ev_head];
	fh->ev_head = (fh->ev_head + 1) % VEVENT_QUEUE_LEN;
	fh->ev_count--;
	ev->pending = fh->ev_count;
	mtx_unlock(&fh->evmtx);
	return (0);
}

int
vevent_poll(struct vfh *fh, int events, struct thread *td)
{
	int revents = 0;

	if ((events & (POLLPRI | POLLRDBAND)) == 0)
		return (0);
	mtx_lock(&fh->evmtx);
	if (fh->ev_count != 0)
		revents = events & (POLLPRI | POLLRDBAND);
	else
		selrecord(td, &fh->esel);
	mtx_unlock(&fh->evmtx);
	return (revents);
}
