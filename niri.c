/* niri.c -- a niri-like layout for vxwm.
 *
 * The windows are lined up in columns on the infinite canvas: the strip can be
 * scrolled along the X axis and a column's stack along the Y axis, just like
 * niri does it.
 *
 * A single window takes the whole screen, a second one splits the screen in
 * half and every further column pushes the older ones off the screen, the
 * focused window is always fully visible.
 *
 * New windows open as their own column directly left of the focused stack, so
 * opening a window pushes the focused stack and everything right of it to the
 * right. A column can hold more than one window though, they stack vertically
 * and the stack scrolls so the focused window stays visible.
 *
 * The layout knobs:
 *   niri_incnmaster (MODKEY+i / MODKEY+d)  give the focused column one window
 *                                          more / less, like niri's consume
 *                                          and expel
 *   niri_focus      (MODKEY+h/j/k/l)       move left/down/up/right between the
 *                                          columns and the windows stacked in
 *                                          them, the view scrolls along
 *   niri_zoom       (MODKEY+Shift+h/l)     how many columns fit on the screen,
 *                                          every press changes it by one
 *   niri_vzoom      (MODKEY+Shift+k/j)     how many stacked windows are visible,
 *                                          every press changes it by one
 *   niri_scroll     (MODKEY+Shift+arrows)  left/right scrolls the columns,
 *                                          up/down scrolls the focused column
 *                                          one window, the other columns stay
 * The column layout, the scroll offsets and the vertical zoom are stored per
 * tag in a table in this file, like pertag does it and only when PERTAG is
 * enabled; the horizontal zoom stays in mfact so setmfact() persists it. The
 * table is also mirrored onto the root window, one property per tag, so a
 * restarted vxwm rebuilds the columns the way they were. That only uses Xlib
 * and the structures vxwm already has, so this file still works with an
 * unpatched vxwm (there without the extra tag/monitor restore).
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
 *     { MODKEY, XK_h, niri_focus,      {.i = 0 } },
 *     { MODKEY, XK_j, niri_focus,      {.i = 3 } },
 *     { MODKEY, XK_k, niri_focus,      {.i = 2 } },
 *     { MODKEY, XK_l, niri_focus,      {.i = 1 } },
 *     { MODKEY|ShiftMask, XK_h, niri_zoom,  {.i = +1 } },
 *     { MODKEY|ShiftMask, XK_l, niri_zoom,  {.i = -1 } },
 *     { MODKEY|ShiftMask, XK_k, niri_vzoom, {.i = +1 } },
 *     { MODKEY|ShiftMask, XK_j, niri_vzoom, {.i = -1 } },
 *     { MODKEY|ShiftMask, XK_Left,  niri_scroll, {.i = 0} },
 *     { MODKEY|ShiftMask, XK_Right, niri_scroll, {.i = 1} },
 *     { MODKEY|ShiftMask, XK_Up,    niri_scroll, {.i = 2} },
 *     { MODKEY|ShiftMask, XK_Down,  niri_scroll, {.i = 3} },
 */

#define NIRI_SCROLL_STEP 120 /* pixels the view travels per scroll step */
#define NIRI_MAX 64          /* windows the strip can hold */
#if PERTAG
#define NIRI_STATE_VERSION 1
#endif

/* the windows of the strip in display order, left to right then top to bottom */
static Client *niri_list[NIRI_MAX];
static int niri_n;

/* one column of the strip */
struct NiriColumn {
	Client *c[NIRI_MAX]; /* the windows sharing the column, top to bottom */
	int n;               /* how many windows share the column */
	int row;             /* topmost visible window of the stack */
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
 *
 * Like pertag the table is only indexed per tag when PERTAG is enabled;
 * without pertag niri keeps a single layout.
 */
struct NiriTagState {
	struct NiriStrip strip;
	int visy;     /* stacked windows sharing the screen height, 0 is one */
	int restored; /* has the saved layout of this tag been read back? */
};

#if PERTAG
static struct NiriTagState niri_tagstate[LENGTH(tags)];
#else
static struct NiriTagState niri_tagstate[1];
#endif

/* index of the tag the monitor is currently showing */
static int
niri_tag(Monitor *m)
{
	unsigned int i;

	for (i = 0; i < LENGTH(tags) && !(m->tagset[m->seltags] & (1 << i)); i++)
		;
	return i < LENGTH(tags) ? i : 0;
}

/* the layout state of the current tag */
static struct NiriTagState *
niri_state_of(Monitor *m)
{
#if PERTAG
	return &niri_tagstate[niri_tag(m)];
#else
	(void)m;
	return &niri_tagstate[0];
#endif
}

#if PERTAG

/* atom that holds the saved layout of one tag on the root window */
static Atom
niri_layout_atom(int tag)
{
	static Atom atoms[LENGTH(tags)];
	char name[32];

	if (!atoms[tag]) {
		snprintf(name, sizeof name, "_NIRI_LAYOUT_%d", tag);
		atoms[tag] = XInternAtom(dpy, name, False);
	}
	return atoms[tag];
}

static int
niri_ignore_error(Display *d, XErrorEvent *e)
{
	(void)d;
	(void)e;
	return 0;
}

static Client *
niri_tile_by_win(Client **tiles, int n, Window win)
{
	int i;

	for (i = 0; i < n; i++)
		if (tiles[i]->win == win)
			return tiles[i];
	return NULL;
}

/* read the saved layout of a tag, NULL when there is none */
static unsigned long *
niri_record_get(int tag, unsigned long *n)
{
	Atom actual;
	int format;
	unsigned long extra;
	unsigned char *data = NULL;

	*n = 0;
	if (XGetWindowProperty(dpy, root, niri_layout_atom(tag), 0L, 4096L, False,
	        XA_CARDINAL, &actual, &format, n, &extra, &data) != Success)
		return NULL;
	if (!data || format != 32 || *n < 3 || data[0] != NIRI_STATE_VERSION) {
		if (data)
			XFree(data);
		*n = 0;
		return NULL;
	}
	return (unsigned long *)data;
}

/* Rebuild the strip of the current tag from its saved record.
 *
 * The record is a list of CARDINALs:
 *     version, visible rows, column count
 *     column: top visible row, window count, window ids...
 *
 * During the startup scan the record is applied as far as possible: a saved
 * window that still exists on the server but is not managed yet means the scan
 * is still running, so the strip is rebuilt again on the next arrange. */
static void
niri_state_restore(Monitor *m, Client **tiles, int n)
{
	struct NiriTagState *st = niri_state_of(m);
	struct NiriStrip *s = &st->strip;
	XErrorHandler oldhandler;
	unsigned long *data, ndata, id;
	XWindowAttributes wa;
	int tag = niri_tag(m);
	int i, j, cnt, rows, pending = 0;

	if (st->restored)
		return;

	data = niri_record_get(tag, &ndata);
	if (!data) {
		/* no saved layout, or one from another version */
		st->restored = 1;
		return;
	}

	rows = (int)data[1];
	if (rows >= 1 && rows <= NIRI_MAX)
		st->visy = rows;

	/* The error handler is swapped out because a window can disappear
	 * between saving the layout and reading it back. */
	oldhandler = XSetErrorHandler(niri_ignore_error);
	i = 3;
	while (i + 1 < (int)ndata) {
		cnt = (int)data[i + 1];
		i += 2;
		if (cnt < 0 || i + cnt > (int)ndata)
			break;
		for (j = 0; j < cnt; j++) {
			id = data[i++];
			if (wintoclient((Window)id))
				continue;
			if (XGetWindowAttributes(dpy, (Window)id, &wa))
				pending = 1;
		}
	}
	XSync(dpy, False);
	XSetErrorHandler(oldhandler);

	/* put the known windows back into their columns, in their saved order */
	s->ncol = 0;
	i = 3;
	while (i + 1 < (int)ndata && s->ncol < NIRI_MAX) {
		struct NiriColumn col;
		Client *c;

		col.row = (int)data[i];
		cnt = (int)data[i + 1];
		i += 2;
		if (cnt < 0 || i + cnt > (int)ndata)
			break;

		col.n = 0;
		for (j = 0; j < cnt && col.n < NIRI_MAX; j++) {
			id = data[i++];
			c = niri_tile_by_win(tiles, n, (Window)id);
			if (c)
				col.c[col.n++] = c;
		}
		if (col.n > 0) {
			if (col.row < 0)
				col.row = 0;
			s->col[s->ncol++] = col;
		}
	}

	if (!pending)
		st->restored = 1;

	XFree(data);
}

/* mirror the layout of the current tag onto the root window */
static void
niri_state_put(Monitor *m)
{
	struct NiriTagState *st = niri_state_of(m);
	struct NiriStrip *s = &st->strip;
	long data[3 + 3 * NIRI_MAX];
	int tag = niri_tag(m);
	int i, j, k = 0;

	/* while the startup scan is still running the old record is better */
	if (!st->restored)
		return;

	data[k++] = NIRI_STATE_VERSION;
	data[k++] = st->visy;
	data[k++] = s->ncol;
	for (i = 0; i < s->ncol; i++) {
		data[k++] = s->col[i].row;
		data[k++] = s->col[i].n;
		for (j = 0; j < s->col[i].n; j++)
			data[k++] = (long)s->col[i].c[j]->win;
	}
	XChangeProperty(dpy, root, niri_layout_atom(tag), XA_CARDINAL, 32,
	                PropModeReplace, (unsigned char *)data, k);
}

#else

/* Without pertag niri keeps one layout and has nothing to store per tag. */
static void
niri_state_restore(Monitor *m, Client **tiles, int n)
{
	(void)m;
	(void)tiles;
	(void)n;
}

static void
niri_state_put(Monitor *m)
{
	(void)m;
}

#endif

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

/* how many windows of a stack fit on the screen */
static int
niri_rows(Monitor *m)
{
	int r = niri_state_of(m)->visy;

	if (r < 1)
		r = 1;
	if (r > NIRI_MAX)
		r = NIRI_MAX;
	return r;
}

/* throw the columns after i one position to the front */
static void
niri_remove_column(Monitor *m, int i)
{
	struct NiriStrip *s = &niri_state_of(m)->strip;
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
	struct NiriStrip *s = &niri_state_of(m)->strip;
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

/* where a new column goes: directly left of the focused stack; when the
 * focused window is not in the strip yet (it is the new window itself), use
 * the most recently focused window that is still there */
static int
niri_insert_index(Monitor *m)
{
	struct NiriStrip *s = &niri_state_of(m)->strip;
	Client *c;

	if (m->sel && niri_known(s, m->sel))
		return niri_column_of(m, m->sel);
	for (c = m->stack; c; c = c->snext)
		if (niri_known(s, c))
			return niri_column_of(m, c);
	return 0;
}

/* make the columns match the windows that are actually there: forget windows
 * that are gone and columns that became empty, new windows open as their own
 * column left of the focused one, newest first */
static void
niri_sync(Monitor *m)
{
	struct NiriStrip *s = &niri_state_of(m)->strip;
	Client *c, *tiles[NIRI_MAX];
	int n = 0, i, j, k, seen, pos;

	for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
		if (n < NIRI_MAX)
			tiles[n++] = c;

	/* after a restart the saved layout replaces the empty table */
	niri_state_restore(m, tiles, n);

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

	/* new windows open as their own column left of the focused stack; with
	 * several new windows the newest one ends up closest to the focus */
	pos = niri_insert_index(m);
	for (k = n - 1; k >= 0 && s->ncol < NIRI_MAX; k--) {
		if (niri_known(s, tiles[k]))
			continue;
		for (i = s->ncol; i > pos; i--)
			s->col[i] = s->col[i - 1];
		s->col[pos].n = 1;
		s->col[pos].c[0] = tiles[k];
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
	int rows;   /* windows that share the screen height in a stack */
	int cw;     /* client width of a window */
	int bw;     /* border width */
	int gap;    /* gap between two windows and at the edges */
	int stride; /* distance between the left edges of two columns */
};

static void
niri_geom(Monitor *m, struct NiriGeom *g)
{
	g->n = niri_n;
	g->ncols = niri_state_of(m)->strip.ncol;
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

	/* how many windows of a stack are visible */
	g->rows = niri_rows(m);
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
	int cx, i, j, k, rows, vh, vstride, x, fcol, left, right;

	if (!m)
		return;

	niri_sync(m);
	niri_geom(m, &g);
	s = &niri_state_of(m)->strip;
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

	/* the focused window has to be inside the visible part of its stack */
	if (fcol >= 0) {
		for (j = 0; j < s->col[fcol].n; j++)
			if (s->col[fcol].c[j] == m->sel)
				break;
		if (j < s->col[fcol].n) {
			rows = s->col[fcol].n < g.rows ? s->col[fcol].n : g.rows;
			if (s->col[fcol].row > j)
				s->col[fcol].row = j;
			else if (j >= s->col[fcol].row + rows)
				s->col[fcol].row = j - rows + 1;
		}
	}

	for (i = 0; i < g.ncols; i++) {
		k = s->col[i].n;
		if (k < 1)
			continue;

		rows = k < g.rows ? k : g.rows;

		/* keep the visible part of every stack inside its own windows */
		if (s->col[i].row < 0)
			s->col[i].row = 0;
		if (s->col[i].row > k - rows)
			s->col[i].row = k - rows;

		/* the rows share the screen height */
		vh = (m->wh - (rows + 1) * g.gap) / rows - 2 * g.bw;
		if (vh < 1)
			vh = 1;
		vstride = vh + 2 * g.bw + g.gap;

		x = m->wx + cx + i * g.stride + g.gap;
		for (j = 0; j < k; j++) {
			/* resizeclient() instead of resize(): the windows of the strip
			 * have to be allowed to sit outside of the window area */
			resizeclient(s->col[i].c[j], x,
			             m->wy + g.gap + (j - s->col[i].row) * vstride,
			             g.cw, vh);
		}
	}

	/* remember the columns for the next start */
	niri_state_put(m);
}

/* scroll the focused column one window up or down, the columns and their
 * horizontal position stay where they are; the focus is pulled along when it
 * would leave the visible rows */
static void
niri_scroll_column(Monitor *m, int dir)
{
	struct NiriStrip *s = &niri_state_of(m)->strip;
	int fcol = niri_column_of(m, m->sel);
	int j, rows;

	if (fcol < 0)
		return;

	rows = s->col[fcol].n < niri_rows(m) ? s->col[fcol].n : niri_rows(m);
	s->col[fcol].row += dir;
	if (s->col[fcol].row < 0)
		s->col[fcol].row = 0;
	if (s->col[fcol].row > s->col[fcol].n - rows)
		s->col[fcol].row = s->col[fcol].n - rows;

	for (j = 0; j < s->col[fcol].n; j++)
		if (s->col[fcol].c[j] == m->sel)
			break;
	if (j < s->col[fcol].n) {
		if (j < s->col[fcol].row)
			focus(s->col[fcol].c[s->col[fcol].row]);
		else if (j >= s->col[fcol].row + rows)
			focus(s->col[fcol].c[s->col[fcol].row + rows - 1]);
	}

	restack(m);
	niri(m);
	drawbar(m);
}

/* move the focus one column left or right and shift the strip along with it,
 * so the focused column stays on the same screen position */
static void
niri_focus_column(Monitor *m, int dir)
{
	struct NiriStrip *s = &niri_state_of(m)->strip;
	struct NiriGeom g;
	int fcol = niri_column_of(m, m->sel);
	int col, row, cx;

	if (fcol < 0)
		return;

	col = fcol + dir;
	if (col < 0 || col >= s->ncol || s->col[col].n < 1)
		return;

	niri_geom(m, &g);
	if (g.ncols > g.cols) {
		/* only a strip that does not fit on the screen scrolls */
		cx = niri_clamp(m, &g, niri_offset(m) - dir * g.stride);
		niri_set_offset(m, cx);
	}

	row = s->col[col].row;
	if (row >= s->col[col].n)
		row = s->col[col].n - 1;

	focus(s->col[col].c[row]);
	restack(m);
	niri(m);
	drawbar(m);
}

/* move the focus one window up or down inside the focused column and scroll
 * the stack along with it, so the focused window keeps its screen row */
static void
niri_focus_row(Monitor *m, int dir)
{
	struct NiriStrip *s = &niri_state_of(m)->strip;
	int fcol = niri_column_of(m, m->sel);
	int j, nj, rows;

	if (fcol < 0)
		return;

	for (j = 0; j < s->col[fcol].n; j++)
		if (s->col[fcol].c[j] == m->sel)
			break;
	if (j >= s->col[fcol].n)
		return;

	nj = j + dir;
	if (nj < 0 || nj >= s->col[fcol].n)
		return;

	rows = s->col[fcol].n < niri_rows(m) ? s->col[fcol].n : niri_rows(m);
	s->col[fcol].row += dir;
	if (s->col[fcol].row < 0)
		s->col[fcol].row = 0;
	if (s->col[fcol].row > s->col[fcol].n - rows)
		s->col[fcol].row = s->col[fcol].n - rows;

	focus(s->col[fcol].c[nj]);
	restack(m);
	niri(m);
	drawbar(m);
}

/* scroll the strip along the X axis and the focused column along the Y axis */
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

	if (arg->i == 2 || arg->i == 3) {
		/* up / down: scroll the focused column, the other columns stay */
		niri_sync(m);
		niri_scroll_column(m, arg->i == 2 ? -1 : +1);
		return;
	}

	switch (arg->i) {
	case 0: dx = -NIRI_SCROLL_STEP; break; /* left */
	case 1: dx =  NIRI_SCROLL_STEP; break; /* right */
	default: return;
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

	s = &niri_state_of(m)->strip;
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

	s = &niri_state_of(m)->strip;
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
	struct NiriStrip *s = &niri_state_of(m)->strip;
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
	struct NiriStrip *s = &niri_state_of(m)->strip;
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

/* change how many stacked windows share the screen height, one step per
 * keypress, kept per tag in this file */
void
niri_vzoom(const Arg *arg)
{
	Monitor *m = selmon;
	int rows;

	if (!m)
		return;

	if (m->lt[m->sellt]->arrange != niri)
		return;

	rows = niri_rows(m) + arg->i;
	if (rows < 1)
		rows = 1;
	if (rows > NIRI_MAX)
		rows = NIRI_MAX;

	niri_state_of(m)->visy = rows;
	arrange(m);
}

/* move the focus with h/j/k/l inside the strip; outside niri the usual dwm
 * bindings stay in effect: h/l change the master factor, k/j walk the stack */
void
niri_focus(const Arg *arg)
{
	Monitor *m = selmon;
	Arg a;

	if (!m)
		return;

	if (m->lt[m->sellt]->arrange != niri) {
		switch (arg->i) {
		case 0: a.f = -0.05; setmfact(&a); break;
		case 1: a.f =  0.05; setmfact(&a); break;
		case 2: a.i = -1; focusstack(&a); break;
		case 3: a.i = +1; focusstack(&a); break;
		}
		return;
	}

	niri_sync(m);
	switch (arg->i) {
	case 0: niri_focus_column(m, -1); break; /* left */
	case 1: niri_focus_column(m, +1); break; /* right */
	case 2: niri_focus_row(m, -1); break;    /* up */
	case 3: niri_focus_row(m, +1); break;    /* down */
	}
}
