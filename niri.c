/* niri.c -- a niri-like layout for vxwm.
 *
 * The windows are lined up in columns on the infinite canvas, so the view can
 * be scrolled along the X axis only, just like niri does it.
 *
 * A single window takes the whole screen, a second one splits the screen in
 * half and every further column pushes the older ones off the screen, the
 * focused window is always fully visible.
 *
 * New windows open as their own column on the left, so opening a window pushes
 * the older ones to the right. A column can hold more than one window though,
 * they stack vertically, like niri's columns.
 *
 * The three layout knobs:
 *   niri_incnmaster (MODKEY+i / MODKEY+d)  give the focused column one window
 *                                          more / less, like niri's consume
 *                                          and expel
 *   niri_zoom       (MODKEY+h / MODKEY+l)  how many columns fit on the screen,
 *                                          every press changes it by one
 * Both are stored per tag, the column layout in a table in this file and the
 * zoom in mfact so setmfact() persists it.
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
 * and bind the knobs:
 *
 *     { MODKEY, XK_i, niri_incnmaster, {.i = +1 } },
 *     { MODKEY, XK_d, niri_incnmaster, {.i = -1 } },
 *     { MODKEY, XK_h, niri_zoom,       {.i = +1 } },
 *     { MODKEY, XK_l, niri_zoom,       {.i = -1 } },
 *     { MODKEY|ShiftMask, XK_Left,  niri_scroll, {.i = 0} },
 *     { MODKEY|ShiftMask, XK_Right, niri_scroll, {.i = 1} },
 */

#define NIRI_SCROLL_STEP 120 /* pixels the view travels per scroll step */
#define NIRI_MAX 64          /* windows the strip can hold */

/* the windows of the strip in display order, left to right then top to bottom */
static Client *niri_list[NIRI_MAX];
static int niri_n;

/* one column of the strip */
struct NiriColumn {
	Client *c[NIRI_MAX]; /* the windows sharing the column, top to bottom */
	int n;               /* how many windows share the column */
};

/* the strip of one tag */
struct NiriStrip {
	int ncol;                  /* how many columns the strip has */
	struct NiriColumn col[NIRI_MAX];
};

/*
 * The column a window ended up in is not derived from the window list, so the
 * layout remembers it per tag. Windows that are not in the table yet (new
 * ones) open as their own column, windows that are gone are dropped.
 */
static struct NiriStrip niri_strip[LENGTH(tags)];

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

/* throw the columns after i one position to the front */
static void
niri_remove_column(Monitor *m, int i)
{
	struct NiriStrip *s = &niri_strip[niri_tag(m)];
	int k;

	for (k = i; k + 1 < s->ncol; k++)
		s->col[k] = s->col[k + 1];
	if (s->ncol > 0)
		s->ncol--;
}

/* the column a window is in, -1 if it is not in the strip */
static int
niri_column_of(Monitor *m, Client *c)
{
	struct NiriStrip *s = &niri_strip[niri_tag(m)];
	int i, j;

	if (!c)
		return -1;
	for (i = 0; i < s->ncol; i++)
		for (j = 0; j < s->col[i].n; j++)
			if (s->col[i].c[j] == c)
				return i;
	return -1;
}

/* is the window already part of the strip? */
static int
niri_known(struct NiriStrip *s, Client *c)
{
	int i, j;

	for (i = 0; i < s->ncol; i++)
		for (j = 0; j < s->col[i].n; j++)
			if (s->col[i].c[j] == c)
				return 1;
	return 0;
}

/* make the columns match the windows that are actually there: forget windows
 * that are gone and columns that became empty, new windows open as their own
 * column with the newest one leftmost */
static void
niri_sync(Monitor *m)
{
	struct NiriStrip *s = &niri_strip[niri_tag(m)];
	Client *c, *tiles[NIRI_MAX];
	int n = 0, i, j, k, seen;

	for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
		if (n < NIRI_MAX)
			tiles[n++] = c;

	/* drop windows that are gone, then columns that became empty */
	for (i = 0; i < s->ncol; ) {
		for (j = 0; j < s->col[i].n; ) {
			seen = 0;
			for (k = 0; k < n; k++)
				if (tiles[k] == s->col[i].c[j]) {
					seen = 1;
					break;
				}
			if (seen) {
				j++;
			} else {
				for (k = j; k + 1 < s->col[i].n; k++)
					s->col[i].c[k] = s->col[i].c[k + 1];
				s->col[i].n--;
			}
		}
		if (s->col[i].n > 0)
			i++;
		else
			niri_remove_column(m, i);
	}

	/* new windows open as their own column, the newest one leftmost */
	for (k = n - 1; k >= 0 && s->ncol < NIRI_MAX; k--) {
		if (niri_known(s, tiles[k]))
			continue;
		for (i = s->ncol; i > 0; i--)
			s->col[i] = s->col[i - 1];
		s->col[0].n = 1;
		s->col[0].c[0] = tiles[k];
		s->ncol++;
	}

	/* flatten for the helpers that work on the window list */
	j = 0;
	for (i = 0; i < s->ncol; i++)
		for (k = 0; k < s->col[i].n && j < NIRI_MAX; k++)
			niri_list[j++] = s->col[i].c[k];
	niri_n = j;
}

struct NiriGeom {
	int n;      /* windows in the strip */
	int ncols;  /* columns in the strip */
	int cols;   /* columns that share the screen */
	int cw;     /* client width of a window */
	int bw;     /* border width */
	int gap;    /* gap between two windows and at the edges */
	int stride; /* distance between the left edges of two columns */
};

static void
niri_geom(Monitor *m, struct NiriGeom *g)
{
	g->n = niri_n;
	g->ncols = niri_strip[niri_tag(m)].ncol;
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
	struct NiriStrip *s;
	int cx, ch, i, j, k, x, fcol, left, right;

	if (!m)
		return;

	niri_sync(m);
	niri_geom(m, &g);
	s = &niri_strip[niri_tag(m)];
	cx = niri_offset(m);

	/* the focused column has to be fully visible */
	fcol = niri_column_of(m, m->sel);
	if (fcol >= 0 && g.ncols > 1) {
		left = m->wx + cx + fcol * g.stride + g.gap; /* outer left edge */
		right = left + g.cw + 2 * g.bw;              /* outer right edge */

		if (left < m->wx + g.gap)
			cx = -fcol * g.stride;
		else if (right > m->wx + m->ww - g.gap)
			cx = m->ww - g.cw - 2 * g.bw - 2 * g.gap - fcol * g.stride;
	}

	cx = niri_clamp(m, &g, cx);
	niri_set_offset(m, cx);

	for (i = 0; i < g.ncols; i++) {
		k = s->col[i].n;
		if (k < 1)
			continue;

		/* every column shares its own height between its windows */
		ch = (m->wh - (k + 1) * g.gap) / k - 2 * g.bw;
		if (ch < 1)
			ch = 1;

		x = m->wx + cx + i * g.stride + g.gap;
		for (j = 0; j < k; j++) {
			/* resizeclient() instead of resize(): the windows of the strip
			 * have to be allowed to sit outside of the window area */
			resizeclient(s->col[i].c[j], x,
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
	struct NiriStrip *s;
	Monitor *m = selmon;
	int dx = 0, cx, col;

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

	niri_sync(m);
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

	s = &niri_strip[niri_tag(m)];
	if (s->col[col].n > 0 && s->col[col].c[0] != m->sel)
		focus(s->col[col].c[0]);
	restack(m);
	niri(m);
	drawbar(m);
}

/* scroll the strip back to its beginning */
void
niri_home(const Arg *arg)
{
	struct NiriStrip *s;
	Monitor *m = selmon;
	Client *c;

	if (!m)
		return;

	if (m->lt[m->sellt]->arrange != niri) {
#if INFINITE_TAGS
		homecanvas(arg);
#endif
		return;
	}

	niri_sync(m);
	niri_set_offset(m, 0);

	s = &niri_strip[niri_tag(m)];
	c = s->ncol > 0 ? s->col[0].c[0] : NULL;
	if (c && c != m->sel)
		focus(c);
	restack(m);
	niri(m);
	drawbar(m);
}

/* give the focused column one more window, taken from the next column */
static void
niri_consume(Monitor *m)
{
	struct NiriStrip *s = &niri_strip[niri_tag(m)];
	int fcol = niri_column_of(m, m->sel);
	int i, k;

	if (fcol < 0 || fcol + 1 >= s->ncol || s->col[fcol].n >= NIRI_MAX)
		return;

	/* the top window of the next column moves to the bottom of this one */
	k = s->col[fcol].n;
	s->col[fcol].c[k] = s->col[fcol + 1].c[0];
	s->col[fcol].n++;

	for (i = 0; i + 1 < s->col[fcol + 1].n; i++)
		s->col[fcol + 1].c[i] = s->col[fcol + 1].c[i + 1];
	s->col[fcol + 1].n--;

	if (s->col[fcol + 1].n == 0)
		niri_remove_column(m, fcol + 1);
}

/* give the focused window its own column, a column keeps at least one window */
static void
niri_expel(Monitor *m)
{
	struct NiriStrip *s = &niri_strip[niri_tag(m)];
	int fcol = niri_column_of(m, m->sel);
	Client *c = m->sel;
	int i, j;

	if (fcol < 0 || s->col[fcol].n < 2 || s->ncol >= NIRI_MAX)
		return;

	for (j = 0; j < s->col[fcol].n; j++)
		if (s->col[fcol].c[j] == c)
			break;
	for (i = j; i + 1 < s->col[fcol].n; i++)
		s->col[fcol].c[i] = s->col[fcol].c[i + 1];
	s->col[fcol].n--;

	for (i = s->ncol; i > fcol + 1; i--)
		s->col[i] = s->col[i - 1];
	s->col[fcol + 1].n = 1;
	s->col[fcol + 1].c[0] = c;
	s->ncol++;
}

/* grow or shrink the focused column, incnmaster (MODKEY+i / MODKEY+d) */
void
niri_incnmaster(const Arg *arg)
{
	Monitor *m = selmon;

	if (!m)
		return;

	if (m->lt[m->sellt]->arrange != niri) {
		/* another layout is active, keep the usual master area */
		incnmaster(arg);
		return;
	}

	niri_sync(m);
	if (arg->i > 0)
		niri_consume(m);
	else
		niri_expel(m);
	arrange(m);
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
