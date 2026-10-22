/* niri.c -- a niri-like layout for vxwm.
 *
 * The windows are lined up in columns on the infinite canvas, so the view can
 * be scrolled along the X axis only, just like niri does it.
 *
 * A single window takes the whole screen, a second one splits the screen in
 * half and every further window pushes the older ones off the screen, the
 * focused window is always fully visible.
 *
 * The strip follows the client list: the newest window is the leftmost one,
 * so opening a window pushes the older ones to the right.
 *
 * The two layout knobs are reused:
 *   incnmaster  (MODKEY+i / MODKEY+d)  how many windows share a column
 *   niri_zoom   (MODKEY+h / MODKEY+l)  how many columns fit on the screen,
 *                                      every press changes it by one
 *
 * The gaps are laid out like tile() does it: gappx between two windows and at
 * the edges, so everything is evenly spaced.
 *
 * The scroll offset is kept in the canvas offset of the current tag, so the
 * coordinates shown in the bar keep working and so does homecanvas.
 *
 * Use it from config.h:
 *
 *     #include "niri.c"
 *
 *     static const Layout layouts[] = {
 *             { "><>",      NULL },    // no layout function means floating behavior
 *             { "[]=",      tile },
 *             { "[>]",      niri },
 *     };
 *
 * and bind the scrolling:
 *
 *     { MODKEY|ShiftMask, XK_Left,  niri_scroll, {.i = 0} },
 *     { MODKEY|ShiftMask, XK_Right, niri_scroll, {.i = 1} },
 */

#define NIRI_SCROLL_STEP 120 /* pixels the view travels per scroll step */
#define NIRI_MAX 64          /* windows the strip can hold */

/* the windows of the strip, the newest one first */
static Client *niri_list[NIRI_MAX];

/* index of the tag the monitor is currently showing */
static int
niri_tag(Monitor *m)
{
	unsigned int i;

	for (i = 0; i < LENGTH(tags) && !(m->tagset[m->seltags] & (1 << i)); i++)
		;
	return i < LENGTH(tags) ? i : 0;
}

/* how far the view is scrolled along the X axis */
static int
niri_offset(Monitor *m)
{
#if INFINITE_TAGS
	return m->canvas[niri_tag(m)].cx;
#else
	return 0;
#endif
}

static void
niri_set_offset(Monitor *m, int cx)
{
#if INFINITE_TAGS
	m->canvas[niri_tag(m)].cx = cx;
#endif
}

static int
niri_gap(Monitor *m)
{
#if GAPS
	return m->gappx;
#else
	return 0;
#endif
}

/* fill the list with the windows of the strip, the newest one first */
static int
niri_collect(Monitor *m)
{
	Client *c;
	int n = 0;

	for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
		if (n < NIRI_MAX)
			niri_list[n++] = c;

	return n;
}

/* how many windows share the height of a column */
static int
niri_percol(Monitor *m)
{
	return m->nmaster > 0 ? m->nmaster : 1;
}

/* how many columns fit on the screen */
static int
niri_visible(Monitor *m)
{
	int v;

	if (m->mfact <= 0)
		return 1;
	v = (int)(1.0 / m->mfact + 0.5);
	if (v < 1)
		v = 1;
	if (v > NIRI_MAX)
		v = NIRI_MAX;
	return v;
}

/* how many columns the strip has */
static int
niri_cols(int n, int percol)
{
	return (n + percol - 1) / percol;
}

/* the position of a window in the strip, -1 if it is not part of it */
static int
niri_index(Monitor *m, Client *target, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (niri_list[i] == target)
			return i;
	return -1;
}

struct NiriGeom {
	int n;       /* windows in the strip */
	int percol;  /* windows that share a column */
	int ncols;   /* columns in the strip */
	int cols;    /* columns that share the screen */
	int cw;      /* client width of a window */
	int bw;      /* border width */
	int gap;     /* gap between two windows and at the edges */
	int stride;  /* distance between the left edges of two columns */
};

static void
niri_geom(Monitor *m, struct NiriGeom *g)
{
	g->n = niri_collect(m);
	g->percol = niri_percol(m);
	g->ncols = niri_cols(g->n, g->percol);
	g->bw = g->n > 0 ? niri_list[0]->bw : 0;
	g->gap = niri_gap(m);

	/* fewer columns than the zoom level: let them share the screen */
	g->cols = g->ncols < niri_visible(m) ? g->ncols : niri_visible(m);
	if (g->cols < 1)
		g->cols = 1;

	/* one gap between neighbouring windows and at both edges */
	g->cw = (m->ww - (g->cols + 1) * g->gap) / g->cols - 2 * g->bw;
	if (g->cw < 1)
		g->cw = 1;
	g->stride = g->cw + 2 * g->bw + g->gap;
}

/* the view cannot leave the strip */
static int
niri_clamp(Monitor *m, struct NiriGeom *g, int cx)
{
	int min;

	if (g->ncols < 2)
		return 0;

	min = m->ww - g->cw - 2 * g->bw - 2 * g->gap - (g->ncols - 1) * g->stride;
	if (min > 0)
		min = 0;
	if (cx > 0)
		return 0;
	if (cx < min)
		return min;
	return cx;
}

void
niri(Monitor *m)
{
	struct NiriGeom g;
	int cx, ch, i, j, k, x, fidx, fcol, left, right;

	if (!m)
		return;

	niri_geom(m, &g);
	cx = niri_offset(m);

	/* the focused column has to be fully visible */
	fidx = niri_index(m, m->sel, g.n);
	if (fidx >= 0 && g.ncols > 1) {
		fcol = fidx / g.percol;
		left = m->wx + cx + fcol * g.stride + g.gap; /* outer left edge */
		right = left + g.cw + 2 * g.bw;              /* outer right edge */

		if (left < m->wx + g.gap)
			cx = -fcol * g.stride;
		else if (right > m->wx + m->ww - g.gap)
			cx = m->ww - g.cw - 2 * g.bw - 2 * g.gap - fcol * g.stride;
	}

	cx = niri_clamp(m, &g, cx);
	niri_set_offset(m, cx);

	for (i = 0; i < g.n; i += g.percol) {
		int col = i / g.percol;

		/* the last column may hold fewer windows, they fill its height */
		k = g.percol < g.n - i ? g.percol : g.n - i;
		ch = (m->wh - (k + 1) * g.gap) / k - 2 * g.bw;
		if (ch < 1)
			ch = 1;

		x = m->wx + cx + col * g.stride + g.gap;
		for (j = 0; j < k; j++) {
			/* resizeclient() instead of resize(): the windows of the strip
			 * have to be allowed to sit outside of the window area */
			resizeclient(niri_list[i + j], x,
			             m->wy + g.gap + j * (ch + 2 * g.bw + g.gap),
			             g.cw, ch);
		}
	}
}

/* scroll the strip along the X axis, the Y axis is not scrollable */
void
niri_scroll(const Arg *arg)
{
	struct NiriGeom g;
	Monitor *m = selmon;
	Client *c;
	int dx = 0, cx, col, fidx;

	if (!m)
		return;

	if (m->lt[m->sellt]->arrange != niri) {
		/* another layout is active, let the canvas deal with it */
#if INFINITE_TAGS
		movecanvas(arg);
#endif
		return;
	}

	switch (arg->i) {
	case 0: dx = -NIRI_SCROLL_STEP; break; /* left */
	case 1: dx =  NIRI_SCROLL_STEP; break; /* right */
	default: return;                        /* this layout scrolls on one axis only */
	}

	niri_geom(m, &g);
	if (g.ncols < 2)
		return;

	cx = niri_clamp(m, &g, niri_offset(m) - dx);
	niri_set_offset(m, cx);

	/* the focus follows the view, it has to happen before the layout runs
	 * again, otherwise the new view would be scrolled back to the column
	 * that is still focused */
	col = (m->ww / 2 - cx) / g.stride;
	if (col < 0)
		col = 0;
	if (col >= g.ncols)
		col = g.ncols - 1;

	fidx = niri_index(m, m->sel, g.n);
	if (fidx < 0 || fidx / g.percol != col) {
		c = niri_list[col * g.percol];
		if (c && c != m->sel)
			focus(c);
	}
	restack(m);
	niri(m);
	drawbar(m);
}

/* scroll the strip back to its beginning */
void
niri_home(const Arg *arg)
{
	Monitor *m = selmon;
	Client *c;
	int n;

	if (!m)
		return;

	if (m->lt[m->sellt]->arrange != niri) {
#if INFINITE_TAGS
		homecanvas(arg);
#endif
		return;
	}

	n = niri_collect(m);
	niri_set_offset(m, 0);

	c = n > 0 ? niri_list[0] : NULL;
	if (c && c != m->sel)
		focus(c);
	restack(m);
	niri(m);
	drawbar(m);
}

/* change how many columns share the screen, one step per keypress, the zoom is
 * kept in mfact so setmfact() stores it per tag */
void
niri_zoom(const Arg *arg)
{
	Monitor *m = selmon;
	Arg a;
	float f;
	int cols;

	if (!m)
		return;

	if (m->lt[m->sellt]->arrange != niri) {
		/* another layout is active, keep the usual master factor */
		a.f = arg->i > 0 ? -0.05 : 0.05;
		setmfact(&a);
		return;
	}

	cols = niri_visible(m) + arg->i;
	if (cols < 1)
		cols = 1;
	if (cols > NIRI_MAX)
		cols = NIRI_MAX;

	/* a column is as wide as the screen allows, one column of them is full;
	 * stay clear of setmfact()'s 0.05..0.95 bounds so the float round trip
	 * through its absolute mode cannot be rejected */
	f = 1.0 / cols;
	if (f > 0.90)
		f = 0.90;
	if (f < 0.06)
		f = 0.06;

	/* absolute mode of setmfact() */
	a.f = 1.0 + f;
	setmfact(&a);
}

/* focusstack() does not arrange, so the view would not follow the focus */
void
niri_focusstack(const Arg *arg)
{
	focusstack(arg);
	if (selmon && selmon->lt[selmon->sellt]->arrange == niri)
		arrange(selmon);
}

/* a click focuses the window before the button binding runs, arrange here to
 * scroll it into view */
void
niri_arrange(const Arg *arg)
{
	if (selmon && selmon->lt[selmon->sellt]->arrange == niri)
		arrange(selmon);
}
