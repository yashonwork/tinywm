/* See LICENSE file for copyright and license details.
 * river-wm: a dwm-like window manager for the River Wayland compositor.
 *
 * Connects to river via the river-window-management-v1 protocol.
 * Manages windows with tags, master-stack tiling, monocle, and floating.
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "river-wm.h"

/* -------------------------------------------------------------------------
 * Global state
 * ------------------------------------------------------------------------- */
struct wl_display *display;
struct river_window_manager_v1 *wmgr;
struct river_xkb_bindings_v1 *xkb;
struct wl_list monitors;
struct wl_list globwindows;
struct wl_list seats;
Monitor *selmon;
Seat *selseat;

/* Pull in compile-time configuration */
#include "config.h"

/* -------------------------------------------------------------------------
 * dwm-style action functions
 * These are called from seat_action() during the manage sequence.
 * ------------------------------------------------------------------------- */

void
spawn(const Arg *arg)
{
	const char **cmd = (const char **)arg->v;
	fprintf(stderr, "spawn %s\n", cmd[0]);
	if (fork() == 0) {
		setsid();
		execvp(cmd[0], (char *const *)cmd);
		_exit(1);
	}
}

void
killclient(const Arg *arg)
{
	(void)arg;
	if (selmon && selmon->focus)
		river_window_v1_close(selmon->focus->obj);
}

void
quitsession(const Arg *arg)
{
	(void)arg;
	river_window_manager_v1_exit_session(wmgr);
}

void
view(const Arg *arg)
{
	if (!selmon || !arg->ui)
		return;
	if ((arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->tagset[selmon->seltags ^ 1] = selmon->tagset[selmon->seltags];
	selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
	focus(NULL, selseat);
	arrange(selmon);
}

void
tag(const Arg *arg)
{
	Window *w = selmon ? selmon->focus : NULL;
	if (!w || !arg->ui)
		return;
	w->tags = arg->ui & TAGMASK;
	focus(NULL, selseat);
	arrange(selmon);
}

void
toggletag(const Arg *arg)
{
	Window *w = selmon ? selmon->focus : NULL;
	if (!w || !arg->ui)
		return;
	w->tags ^= arg->ui & TAGMASK;
	focus(NULL, selseat);
	arrange(selmon);
}

void
viewprevtag(const Arg *arg)
{
	(void)arg;
	if (!selmon)
		return;
	Arg a = { .ui = selmon->tagset[selmon->seltags ^ 1] };
	view(&a);
}

void
focusmaster(const Arg *arg)
{
	Window *w;
	(void)arg;
	if (!selmon)
		return;
	wl_list_for_each(w, &selmon->stack, slink)
		if (w->tags & selmon->tagset[selmon->seltags]) {
			focus(w, selseat);
			return;
		}
}

void
focusstack(const Arg *arg)
{
	Window *w, *next;
	int dir;

	if (!selmon || wl_list_empty(&selmon->stack))
		return;

	w = selmon->focus;
	dir = arg->i;

	if (!w) {
		next = wl_container_of(selmon->stack.next, next, slink);
		focus(next, selseat);
		return;
	}

	next = w;
	do {
		if (dir > 0) {
			if (next->slink.next == &selmon->stack)
				next = wl_container_of(selmon->stack.next, next, slink);
			else
				next = wl_container_of(next->slink.next, next, slink);
		} else {
			if (next->slink.prev == &selmon->stack)
				next = wl_container_of(selmon->stack.prev, next, slink);
			else
				next = wl_container_of(next->slink.prev, next, slink);
		}
	} while (next != w && !(next->tags & selmon->tagset[selmon->seltags]));

	if (next != w)
		focus(next, selseat);
}

void
swapstack(const Arg *arg)
{
	if (!selmon || wl_list_empty(&selmon->stack))
		return;
	Window *w = selmon->focus;
	if (!w)
		return;
	Window *target;
	int dir = arg->i;

	if (dir > 0)
		target = wl_container_of(selmon->stack.prev, target, slink);
	else
		target = wl_container_of(selmon->stack.next, target, slink);

	/* swap positions in the stack list */
	wl_list_remove(&w->slink);
	wl_list_remove(&target->slink);

	if (dir > 0) {
		wl_list_insert(&target->slink, &w->slink);
		wl_list_insert(selmon->stack.prev, &target->slink);
	} else {
		wl_list_insert(&target->slink, &w->slink);
		wl_list_insert(selmon->stack.next, &target->slink);
	}
	arrange(selmon);
}

void
setlayout(const Arg *arg)
{
	if (!selmon || !arg->v)
		return;
	selmon->layout = (const Layout *)arg->v;
	arrange(selmon);
}

void
setmfact(const Arg *arg)
{
	if (!selmon)
		return;
	float f = arg->f + selmon->mfact;
	if (f < 0.1f || f > 0.9f)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

void
togglefloating(const Arg *arg)
{
	Window *w = selmon ? selmon->focus : NULL;
	(void)arg;
	if (!w)
		return;
	w->isfloating = !w->isfloating;
	if (w->isfloating) {
		w->oldx = w->x;
		w->oldy = w->y;
		w->oldw = w->w;
		w->oldh = w->h;
	}
	arrange(selmon);
}

void
zoom(const Arg *arg)
{
	(void)arg;
	if (!selmon)
		return;
	Window *w = selmon->focus;
	if (!w || wl_list_empty(&selmon->stack))
		return;

	/* move to top of stack */
	wl_list_remove(&w->slink);
	wl_list_insert(selmon->stack.prev, &w->slink);
	arrange(selmon);
}

/* -------------------------------------------------------------------------
 * Core WM logic
 * ------------------------------------------------------------------------- */

void
applyrules(Window *w)
{
	for (size_t i = 0; i < LENGTH(rules); i++) {
		const Rule *r = &rules[i];
		if (!r->app_id && !r->title)
			continue; /* skip default, handled below */
		if (r->app_id && !strstr(w->app_id, r->app_id))
			continue;
		if (r->title && !strstr(w->title, r->title))
			continue;
		w->isfloating = r->isfloating;
		if (r->tags)
			w->tags = r->tags;
		return;
	}
	/* last rule acts as default */
	if (LENGTH(rules) > 0) {
		const Rule *r = &rules[LENGTH(rules) - 1];
		if (!r->app_id && !r->title) {
			w->isfloating = r->isfloating;
			if (r->tags)
				w->tags = r->tags;
		}
	}
}

void
focus(Window *w, Seat *s)
{
	if (!s)
		s = selseat;
	if (!s)
		return;
	if (w && w == s->focus)
		return;

	if (w) {
		/* move to top of stack for its monitor */
		wl_list_remove(&w->slink);
		wl_list_insert(&w->mon->stack, &w->slink);

		river_seat_v1_focus_window(s->obj, w->obj);
		river_node_v1_place_top(w->node);
		w->mon->focus = w;
	} else {
		river_seat_v1_clear_focus(s->obj);
		if (s->focus)
			s->focus->mon->focus = NULL;
	}
	s->focus = w;
}

Window *
wintofocus(Monitor *m, Window *old)
{
	Window *w;

	if (old && (old->tags & m->tagset[m->seltags]) && !old->closed)
		return old;

	wl_list_for_each(w, &m->stack, slink)
		if (w->tags & m->tagset[m->seltags])
			return w;

	return NULL;
}

void
arrange(Monitor *m)
{
	Window *w;

	if (!m)
		return;

	m->focus = wintofocus(m, m->focus);

	/* apply layout function */
	if (m->layout && m->layout->arrange)
		m->layout->arrange(m);

	/* initial sizing for floating / new windows */
	wl_list_for_each(w, &m->windows, mlink) {
		if (!(w->tags & m->tagset[m->seltags]))
			continue;
		if (w->isnew) {
			w->isnew = 0;
			if (w->w < 1) w->w = 800;
			if (w->h < 1) w->h = 600;
			if (w->isfloating) {
				w->x = m->x + m->w / 4;
				w->y = m->y + m->h / 4;
			}
		}
		if (w->isfloating && (w->w < 1 || w->h < 1)) {
			w->w = 800;
			w->h = 600;
		}
	}
}

void
cleanup(Window *w)
{
	Seat *s;
	wl_list_for_each(s, &seats, link) {
		if (s->focus == w)     s->focus = NULL;
		if (s->hovered == w)   s->hovered = NULL;
		if (s->op_window == w) {
			river_seat_v1_op_end(s->obj);
			s->op = OP_NONE;
			s->op_window = NULL;
		}
	}
	if (w->mon) {
		if (w->mon->focus == w)
			w->mon->focus = NULL;
		wl_list_remove(&w->mlink);
		wl_list_remove(&w->slink);
	}
	river_window_v1_destroy(w->obj);
	free(w);
}

/* -------------------------------------------------------------------------
 * River protocol event listeners
 * ------------------------------------------------------------------------- */

/* --- Window listeners --- */

static void
win_closed(void *data, struct river_window_v1 *obj)
{
	(void)obj;
	((Window *)data)->closed = 1;
}

static void
win_dimensions(void *data, struct river_window_v1 *obj,
		int32_t w, int32_t h)
{
	(void)obj;
	Window *win = data;
	win->w = w;
	win->h = h;
}

static void
win_app_id(void *data, struct river_window_v1 *obj, const char *app_id)
{
	(void)obj;
	Window *win = data;
	strncpy(win->app_id, app_id, sizeof(win->app_id) - 1);
}

static void
win_title(void *data, struct river_window_v1 *obj, const char *title)
{
	(void)obj;
	Window *win = data;
	strncpy(win->title, title, sizeof(win->title) - 1);
}

static void
win_move_req(void *data, struct river_window_v1 *obj,
		struct river_seat_v1 *seat_obj)
{
	(void)obj;
	((Window *)data)->move_req = river_seat_v1_get_user_data(seat_obj);
}

static void
win_resize_req(void *data, struct river_window_v1 *obj,
		struct river_seat_v1 *seat_obj, uint32_t edges)
{
	(void)obj;
	Window *win = data;
	win->resize_req = river_seat_v1_get_user_data(seat_obj);
	win->resize_edges = edges;
}

static const struct river_window_v1_listener win_listener = {
	.closed                  = win_closed,
	.dimensions              = win_dimensions,
	.app_id                  = win_app_id,
	.title                   = win_title,
	.pointer_move_requested  = win_move_req,
	.pointer_resize_requested = win_resize_req,
};

/* --- Output listeners --- */

static void
out_position(void *data, struct river_output_v1 *obj,
		int32_t x, int32_t y)
{
	(void)obj;
	Monitor *m = data;
	m->x = x;
	m->y = y;
}

static void
out_dimensions(void *data, struct river_output_v1 *obj,
		int32_t w, int32_t h)
{
	(void)obj;
	Monitor *m = data;
	m->w = w;
	m->h = h;
}

static void
out_removed(void *data, struct river_output_v1 *obj)
{
	(void)obj;
	Monitor *m = data;
	river_output_v1_destroy(m->obj);
	wl_list_remove(&m->link);
	if (m == selmon)
		selmon = wl_list_empty(&monitors) ? NULL
		       : wl_container_of(monitors.next, selmon, link);
	free(m);
}

static const struct river_output_v1_listener out_listener = {
	.position   = out_position,
	.dimensions = out_dimensions,
	.removed    = out_removed,
};

/* --- Seat listeners --- */

static void
seat_pointer_enter(void *data, struct river_seat_v1 *obj,
		struct river_window_v1 *win_obj)
{
	(void)obj;
	Seat *s = data;
	s->hovered = win_obj ? river_window_v1_get_user_data(win_obj) : NULL;
}

static void
seat_pointer_leave(void *data, struct river_seat_v1 *obj)
{
	(void)obj;
	((Seat *)data)->hovered = NULL;
}

static void
seat_window_interaction(void *data, struct river_seat_v1 *obj,
		struct river_window_v1 *win_obj)
{
	(void)obj;
	Seat *s = data;
	s->interacted = win_obj ? river_window_v1_get_user_data(win_obj) : NULL;
}

static void
seat_op_delta(void *data, struct river_seat_v1 *obj,
		int32_t dx, int32_t dy)
{
	(void)obj;
	Seat *s = data;
	s->op_dx = dx;
	s->op_dy = dy;
}

static void
seat_op_release(void *data, struct river_seat_v1 *obj)
{
	(void)obj;
	((Seat *)data)->op_release = 1;
}

static void
seat_removed(void *data, struct river_seat_v1 *obj)
{
	(void)obj;
	((Seat *)data)->removed = 1;
}

static const struct river_seat_v1_listener seat_listener = {
	.removed            = seat_removed,
	.pointer_enter      = seat_pointer_enter,
	.pointer_leave      = seat_pointer_leave,
	.window_interaction = seat_window_interaction,
	.op_delta           = seat_op_delta,
	.op_release         = seat_op_release,
};

/* --- XKB binding listeners --- */

static void
xkb_pressed(void *data, struct river_xkb_binding_v1 *obj)
{
	(void)obj;
	KeyBinding *b = data;
	fprintf(stderr, "xkb_pressed\n");
	b->seat->pending = b;
}

static const struct river_xkb_binding_v1_listener xkb_listener = {
	.pressed  = xkb_pressed,
	.released = NULL,
};

static void
create_xkb_binding(Seat *s, uint32_t mods, xkb_keysym_t sym,
		void (*func)(const Arg *), Arg arg)
{
	KeyBinding *b = calloc(1, sizeof(*b));
	b->obj = river_xkb_bindings_v1_get_xkb_binding(xkb, s->obj, sym, mods);
	b->seat = s;
	b->func = func;
	b->arg  = arg;
	river_xkb_binding_v1_add_listener(b->obj, &xkb_listener, b);
	river_xkb_binding_v1_enable(b->obj);
	wl_list_insert(&s->xkb_bindings, &b->link);
}

/* --- Pointer binding listeners --- */

static void
ptr_pressed(void *data, struct river_pointer_binding_v1 *obj)
{
	(void)obj;
	PointerBinding *b = data;
	b->seat->pending = NULL; /* clear key pending */
	/* handle move/resize immediately by starting op */
	if (b->is_move) {
		if (b->seat->op == OP_NONE && b->seat->hovered) {
			Seat *s = b->seat;
			Window *w = s->hovered;
			selseat = s;
			selmon = w->mon;
			focus(w, s);
			river_seat_v1_op_start_pointer(s->obj);
			s->op = OP_MOVE;
			s->op_window = w;
			s->op_sx = w->x;
			s->op_sy = w->y;
			s->op_dx = 0;
			s->op_dy = 0;
		}
	} else {
		if (b->seat->op == OP_NONE && b->seat->hovered) {
			Seat *s = b->seat;
			Window *w = s->hovered;
			selseat = s;
			selmon = w->mon;
			focus(w, s);
			river_window_v1_inform_resize_start(w->obj);
			river_seat_v1_op_start_pointer(s->obj);
			s->op = OP_RESIZE;
			s->op_window = w;
			s->op_edges = RIVER_WINDOW_V1_EDGES_BOTTOM
			              | RIVER_WINDOW_V1_EDGES_RIGHT;
			s->op_sx = w->x;
			s->op_sy = w->y;
			s->op_sw = w->w;
			s->op_sh = w->h;
			s->op_dx = 0;
			s->op_dy = 0;
		}
	}
}

static const struct river_pointer_binding_v1_listener ptr_listener = {
	.pressed  = ptr_pressed,
	.released = NULL,
};

static void
create_ptr_binding(Seat *s, uint32_t mods, uint32_t button, int is_move)
{
	PointerBinding *b = calloc(1, sizeof(*b));
	b->obj = river_seat_v1_get_pointer_binding(s->obj, button, mods);
	b->seat = s;
	b->is_move = is_move;
	river_pointer_binding_v1_add_listener(b->obj, &ptr_listener, b);
	river_pointer_binding_v1_enable(b->obj);
	wl_list_insert(&s->pointer_bindings, &b->link);
}

/* -------------------------------------------------------------------------
 * Seat management (called during manage sequence)
 * ------------------------------------------------------------------------- */

static void
setup_seat(Seat *s)
{
	if (!s->isnew)
		return;
	s->isnew = 0;

	/* create xkb bindings for all keys in config */
	for (size_t i = 0; i < LENGTH(keys); i++)
		create_xkb_binding(s, keys[i].mod, keys[i].keysym,
		                   keys[i].func, keys[i].arg);

	/* pointer bindings for move/resize */
	create_ptr_binding(s, RIVER_SEAT_V1_MODIFIERS_MOD4,
	                   BTN_LEFT, 1);
	create_ptr_binding(s, RIVER_SEAT_V1_MODIFIERS_MOD4,
	                   BTN_RIGHT, 0);
}

static void
handle_seat_interaction(Seat *s)
{
	Window *w = s->interacted;
	if (!w)
		return;

	s->interacted = NULL;
	selseat = s;
	selmon = w->mon;
	focus(w, s);
}

static void
handle_seat_pending(Seat *s)
{
	KeyBinding *b = s->pending;
	if (!b)
		return;
	s->pending = NULL;
	b->func(&b->arg);
}

static void
handle_seat_op(Seat *s)
{
	if (s->op == OP_MOVE && s->op_release) {
		/* save final position */
		s->op_window->x = s->op_sx + s->op_dx;
		s->op_window->y = s->op_sy + s->op_dy;
		river_seat_v1_op_end(s->obj);
		s->op = OP_NONE;
		s->op_window = NULL;
	}
	if (s->op == OP_RESIZE && s->op_release) {
		/* save final dimensions */
		int nw = s->op_sw, nh = s->op_sh;
		int nx = s->op_sx, ny = s->op_sy;
		if (s->op_edges & RIVER_WINDOW_V1_EDGES_LEFT) {
			nw -= s->op_dx;
			nx += s->op_dx;
		}
		if (s->op_edges & RIVER_WINDOW_V1_EDGES_RIGHT)
			nw += s->op_dx;
		if (s->op_edges & RIVER_WINDOW_V1_EDGES_TOP) {
			nh -= s->op_dy;
			ny += s->op_dy;
		}
		if (s->op_edges & RIVER_WINDOW_V1_EDGES_BOTTOM)
			nh += s->op_dy;
		s->op_window->x = nx;
		s->op_window->y = ny;
		s->op_window->w = nw > 1 ? nw : 1;
		s->op_window->h = nh > 1 ? nh : 1;
		river_window_v1_inform_resize_end(s->op_window->obj);
		river_seat_v1_op_end(s->obj);
		s->op = OP_NONE;
		s->op_window = NULL;
	}
	s->op_release = 0;
}

/* -------------------------------------------------------------------------
 * River WM protocol callbacks
 * ------------------------------------------------------------------------- */

static void
wm_unavailable(void *data, struct river_window_manager_v1 *obj)
{
	(void)data;
	(void)obj;
	fprintf(stderr, "river-wm: another WM is already running\n");
	exit(1);
}

static void
wm_finished(void *data, struct river_window_manager_v1 *obj)
{
	(void)data;
	(void)obj;
	exit(0);
}

/* --- manage_start --- */

static void
wm_manage_start(void *data, struct river_window_manager_v1 *obj)
{
	(void)data;
	Window *w, *wtmp;
	Seat *s, *stmp;
	KeyBinding *kb, *kbtmp;
	PointerBinding *pb, *pbtmp;

	fprintf(stderr, "manage_start\n");

	/* 1. clean up dead objects */
	wl_list_for_each_safe(w, wtmp, &globwindows, mlink)
		if (w->closed) cleanup(w);

	wl_list_for_each_safe(s, stmp, &seats, link) {
		if (!s->removed) continue;
		wl_list_for_each_safe(kb, kbtmp, &s->xkb_bindings, link) {
			river_xkb_binding_v1_destroy(kb->obj);
			wl_list_remove(&kb->link);
			free(kb);
		}
		wl_list_for_each_safe(pb, pbtmp, &s->pointer_bindings, link) {
			river_pointer_binding_v1_destroy(pb->obj);
			wl_list_remove(&pb->link);
			free(pb);
		}
		river_seat_v1_destroy(s->obj);
		wl_list_remove(&s->link);
		if (s == selseat) selseat = NULL;
		free(s);
	}

	/* 2. handle new/manage windows */
	wl_list_for_each(w, &globwindows, mlink) {
		if (!w->isnew || w->closed) continue;

		/* assign first output if none */
		if (!w->mon) {
			if (!selmon && !wl_list_empty(&monitors))
				selmon = wl_container_of(monitors.next, selmon, link);
			w->mon = selmon;
		}

		if (w->mon) {
			applyrules(w);
			wl_list_insert(&w->mon->windows, &w->mlink);
			wl_list_insert(&w->mon->stack, &w->slink);
		}

		/* handle move/resize requests from client */
		if (w->move_req) {
			Seat *ms = w->move_req;
			w->move_req = NULL;
			selseat = ms;
			selmon = w->mon;
			focus(w, ms);
			river_seat_v1_op_start_pointer(ms->obj);
			ms->op = OP_MOVE;
			ms->op_window = w;
			ms->op_sx = w->x; ms->op_sy = w->y;
			ms->op_dx = 0; ms->op_dy = 0;
		}
		if (w->resize_req) {
			Seat *rs = w->resize_req;
			w->resize_req = NULL;
			selseat = rs;
			selmon = w->mon;
			focus(w, rs);
			river_window_v1_inform_resize_start(w->obj);
			river_seat_v1_op_start_pointer(rs->obj);
			rs->op = OP_RESIZE;
			rs->op_window = w;
			rs->op_edges = w->resize_edges;
			rs->op_sx = w->x; rs->op_sy = w->y;
			rs->op_sw = w->w; rs->op_sh = w->h;
			rs->op_dx = 0; rs->op_dy = 0;
		}
	}

	/* 3. arrange layout and propose dimensions */
	if (selmon) {
		fprintf(stderr, "  arrange(selmon %dx%d)\n", selmon->w, selmon->h);
		arrange(selmon);

		/* propose layout-calculated dimensions to all managed windows */
		wl_list_for_each(w, &globwindows, mlink) {
			if (w->closed) continue;
			if (w->w > 0 && w->h > 0) {
				fprintf(stderr, "  propose %p %dx%d+%d+%d\n",
					(void*)w, w->w, w->h, w->x, w->y);
				river_window_v1_propose_dimensions(w->obj, w->w, w->h);
			} else {
				fprintf(stderr, "  skip %p w=%d h=%d\n",
					(void*)w, w->w, w->h);
			}
		}
	} else {
		fprintf(stderr, "  no selmon!\n");
	}

	/* 4. handle seats */
	wl_list_for_each(s, &seats, link) {
		setup_seat(s);
		handle_seat_interaction(s);
		handle_seat_pending(s);
		handle_seat_op(s);
	}

	river_window_manager_v1_manage_finish(wmgr);
}

/* --- render_start --- */

static void
wm_render_start(void *data, struct river_window_manager_v1 *obj)
{
	(void)data;
	Window *w;
	Seat *s;

	/* commit window positions (dimensions were proposed in manage_start) */
	fprintf(stderr, "render_start\n");
	wl_list_for_each(w, &globwindows, mlink) {
		if (w->closed) continue;
		fprintf(stderr, "  set_position %d,%d\n", w->x, w->y);
		river_node_v1_set_position(w->node, w->x, w->y);
	}

	/* handle active move/resize operations (override above) */
	wl_list_for_each(s, &seats, link) {
		switch (s->op) {
		case OP_MOVE: {
			int mx = s->op_sx + s->op_dx;
			int my = s->op_sy + s->op_dy;
			river_node_v1_set_position(s->op_window->node, mx, my);
			s->op_window->x = mx;
			s->op_window->y = my;
			break;
		}
		case OP_RESIZE: {
			int nw = s->op_sw, nh = s->op_sh;
			int nx = s->op_sx, ny = s->op_sy;
			if (s->op_edges & RIVER_WINDOW_V1_EDGES_LEFT) {
				nw -= s->op_dx;
				nx += s->op_dx;
			}
			if (s->op_edges & RIVER_WINDOW_V1_EDGES_RIGHT)
				nw += s->op_dx;
			if (s->op_edges & RIVER_WINDOW_V1_EDGES_TOP) {
				nh -= s->op_dy;
				ny += s->op_dy;
			}
			if (s->op_edges & RIVER_WINDOW_V1_EDGES_BOTTOM)
				nh += s->op_dy;
			nw = nw > 1 ? nw : 1;
			nh = nh > 1 ? nh : 1;
			river_node_v1_set_position(s->op_window->node, nx, ny);
			river_window_v1_propose_dimensions(s->op_window->obj, nw, nh);
			s->op_window->x = nx;
			s->op_window->y = ny;
			s->op_window->w = nw;
			s->op_window->h = nh;
			break;
		}
		default:
			break;
		}
	}

	river_window_manager_v1_render_finish(wmgr);
}

/* --- object creation --- */

static void
wm_window(void *data, struct river_window_manager_v1 *obj,
		struct river_window_v1 *win_obj)
{
	(void)data;
	(void)obj;
	Window *w = calloc(1, sizeof(*w));
	w->obj = win_obj;
	w->node = river_window_v1_get_node(win_obj);
	w->isnew = 1;
	w->tags = TAGMASK;
	w->bw = borderpx;
	river_window_v1_add_listener(win_obj, &win_listener, w);
	river_window_v1_set_user_data(win_obj, w);
	wl_list_insert(&globwindows, &w->mlink);
}

static void
wm_output(void *data, struct river_window_manager_v1 *obj,
		struct river_output_v1 *out_obj)
{
	(void)data;
	(void)obj;
	Monitor *m = calloc(1, sizeof(*m));
	m->obj = out_obj;
	m->tagset[0] = 1;  /* tag 1 by default */
	m->tagset[1] = TAGMASK;
	m->seltags = 0;
	m->layout = &layouts[0];
	m->nmaster = nmaster;
	m->mfact = mfact;
	m->gappx = gappx;
	wl_list_init(&m->windows);
	wl_list_init(&m->stack);
	river_output_v1_add_listener(out_obj, &out_listener, m);
	river_output_v1_set_user_data(out_obj, m);
	wl_list_insert(&monitors, &m->link);
	if (!selmon)
		selmon = m;
}

static void
wm_seat(void *data, struct river_window_manager_v1 *obj,
		struct river_seat_v1 *seat_obj)
{
	(void)data;
	(void)obj;
	Seat *s = calloc(1, sizeof(*s));
	s->obj = seat_obj;
	s->isnew = 1;
	wl_list_init(&s->xkb_bindings);
	wl_list_init(&s->pointer_bindings);
	river_seat_v1_add_listener(seat_obj, &seat_listener, s);
	river_seat_v1_set_user_data(seat_obj, s);
	wl_list_insert(&seats, &s->link);
	if (!selseat)
		selseat = s;
}

static const struct river_window_manager_v1_listener wm_listener = {
	.unavailable   = wm_unavailable,
	.finished      = wm_finished,
	.manage_start  = wm_manage_start,
	.render_start  = wm_render_start,
	.window        = wm_window,
	.output        = wm_output,
	.seat          = wm_seat,
};

/* -------------------------------------------------------------------------
 * Wayland registry
 * ------------------------------------------------------------------------- */

static void
registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version)
{
	(void)data;
	if (strcmp(interface, "river_window_manager_v1") == 0) {
		if (version >= 4)
			wmgr = wl_registry_bind(registry, name,
			       &river_window_manager_v1_interface, 4);
	} else if (strcmp(interface, "river_xkb_bindings_v1") == 0) {
		xkb = wl_registry_bind(registry, name,
		      &river_xkb_bindings_v1_interface, 1);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global        = registry_global,
	.global_remove = NULL,
};

/* -------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

int
main(void)
{
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "river-wm: failed to connect to Wayland display\n");
		return 1;
	}

	unsetenv("WAYLAND_DEBUG");
	signal(SIGCHLD, SIG_IGN);

	wl_list_init(&monitors);
	wl_list_init(&globwindows);
	wl_list_init(&seats);

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);

	if (wl_display_roundtrip(display) < 0) {
		fprintf(stderr, "river-wm: roundtrip failed\n");
		return 1;
	}

	if (!wmgr || !xkb) {
		fprintf(stderr, "river-wm: compositor does not support required protocols\n");
		fprintf(stderr, "  need river_window_manager_v1 >= 4 and river_xkb_bindings_v1 >= 1\n");
		return 1;
	}

	river_window_manager_v1_add_listener(wmgr, &wm_listener, NULL);

	while (wl_display_dispatch(display) >= 0)
		;

	fprintf(stderr, "river-wm: display disconnected\n");
	return 1;
}
