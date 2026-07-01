/* See LICENSE file for copyright and license details. */
#include "river-wm.h"

void
tile(Monitor *m)
{
	Window *w;
	int n = 0, i, mw, sy, she, sw, nh, ns;

	wl_list_for_each(w, &m->windows, mlink)
		if (w->tags & m->tagset[m->seltags] && !w->isfloating)
			n++;

	if (n == 0)
		return;

	mw  = (n > m->nmaster) ? m->w * m->mfact : m->w - 2 * m->gappx;
	sy  = m->y + m->gappx;
	she = m->h - 2 * m->gappx;
	sw  = m->w - mw - m->gappx;

	i = 0;
	wl_list_for_each(w, &m->windows, mlink) {
		if (!(w->tags & m->tagset[m->seltags]) || w->isfloating)
			continue;
		if (i < m->nmaster) {
			nh = (she - (m->nmaster - 1) * m->gappx) / MAX(m->nmaster, 1);
			w->x = m->x + m->gappx;
			w->y = sy;
			w->w = mw - 2 * m->gappx - w->bw * 2;
			w->h = nh - w->bw * 2;
			sy += nh + m->gappx;
		} else {
			ns = MAX(n - m->nmaster, 1);
			nh = (she - (ns - 1) * m->gappx) / ns;
			w->x = m->x + mw + m->gappx;
			w->y = sy;
			w->w = sw - 2 * m->gappx - w->bw * 2;
			w->h = nh - w->bw * 2;
			sy += nh + m->gappx;
		}
		if (w->w < 1) w->w = 1;
		if (w->h < 1) w->h = 1;
		i++;
	}
}

void
monocle(Monitor *m)
{
	Window *w;

	wl_list_for_each(w, &m->windows, mlink) {
		if (!(w->tags & m->tagset[m->seltags]))
			continue;
		w->x = m->x + m->gappx;
		w->y = m->y + m->gappx;
		w->w = m->w - 2 * m->gappx - w->bw * 2;
		w->h = m->h - 2 * m->gappx - w->bw * 2;
		if (w->w < 1) w->w = 1;
		if (w->h < 1) w->h = 1;
	}
}

void
float_(Monitor *m)
{
	Window *w;

	wl_list_for_each(w, &m->windows, mlink) {
		if (!(w->tags & m->tagset[m->seltags]))
			continue;
		if (!w->isfloating)
			monocle(m);
	}
}
