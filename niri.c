/* niri.c -- a niri-like layout for vxwm.
 *
 * The windows are lined up in columns on the infinite canvas: the strip can be
 * scrolled along the X axis and a column's stack along the Y axis, just like
 * niri does it.
 *
 * Every column has its own width and every window its own height: one of
 * one third, one half, two thirds or the full monitor size. Columns scroll
 * off the edges of the screen instead of shrinking to fit it, and so do the
 * windows stacked in a column: the focused window is always kept fully
 * visible, the rest goes off the top or the bottom.
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
 *   niri_zoom       (MODKEY+Shift+h/l)     cycle the focused column's width
 *                                          one step smaller / bigger
 *   niri_vzoom      (MODKEY+Shift+k/j)     cycle the focused window's height
 *                                          one step smaller / bigger
 *   niri_scroll     (MODKEY+Shift+arrows)  left/right scrolls the columns,
 *                                          up/down scrolls the focused column
 *                                          one window, the other columns stay
 *
 * Define NIRI_ALWAYS_CENTER_SINGLE_COLUMN to 1 in config.h to center a
 * single column horizontally, like niri's always-center-single-column.
 * The column order, the width/height presets and the scroll offsets are stored
 * per tag in a table in this file, like pertag does it and only when PERTAG is
 * enabled. The table is also mirrored onto the root window, one property per
 * tag, so a restarted vxwm rebuilds the columns the way they were. That only
 * uses Xlib and the structures vxwm already has, so this file still works
 * with an unpatched vxwm (there without the extra tag/monitor restore).
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
#define NIRI_STATE_VERSION 2
#define NIRI_STATE_LEGACY_VERSION 1
#endif

/* the windows of the strip in display order, left to right then top to bottom */
static Client *niri_list[NIRI_MAX];
static int niri_n;

/* the width/height presets a column or window can be set to */
#define NIRI_SIZE_THIRD 0
#define NIRI_SIZE_HALF  1
#define NIRI_SIZE_TWOHIRD 2
#define NIRI_SIZE_FULL  3
#define NIRI_NSIZES     4
#define NIRI_SIZE_DEFAULT NIRI_SIZE_HALF
#define NIRI_HEIGHT_DEFAULT NIRI_SIZE_FULL

static const double niri_sizes[NIRI_NSIZES] = {
	1.0 / 3.0,
	0.5,
	2.0 / 3.0,
	1.0,
};

/* fit the cells of one axis: a contiguous group of cells whose fractions add
 * up to one screen shares the inner span minus the gaps, so opposite edges
 * leave the same gap; cells that do not make a full screen keep their
 * absolute fractions, allowing the strip to run off the screen */
static void
niri_fit_one(const double *f, int i, int j, int inner, int gap, int min,
             int *span)
{
	int count, k, total, used = 0;

	count = j - i + 1;
	total = inner - (count - 1) * gap;
	if (total < min)
		total = min;
	for (k = i; k < j; k++) {
		span[k] = (int)(f[k] * total + 0.5);
		span[k] = span[k] < min ? min : span[k];
		used += span[k];
	}
	span[j] = total - used;
	for (k = i; k <= j; k++)
		if (span[k] < min)
			span[k] = min;
}

/* fit the cells of one axis: any contiguous run whose fractions add up to one
 * screen shares the inner span minus the gaps, so opposite edges leave the
 * same gap. Cells that are not part of a full screen keep their absolute
 * fractions, allowing the strip to run off the screen. This finds the largest
 * set of non-overlapping full-screen runs anywhere in the strip. */
static void
niri_fit_spans(const double *f, int n, int inner, int gap, int min, int *span)
{
	int best[NIRI_MAX + 1], end[NIRI_MAX + 1];
	int i, j;
	double sum;

	best[n] = 0;
	for (i = n - 1; i >= 0; i--) {
		best[i] = best[i + 1];
		end[i] = i + 1;
		sum = 0.0;
		for (j = i; j < n; j++) {
			sum += f[j];
			if (sum > 1.01)
				break;
			if (sum > 0.99 && j - i + 1 + best[j + 1] > best[i]) {
				best[i] = j - i + 1 + best[j + 1];
				end[i] = j + 1;
			}
		}
	}

	i = 0;
	while (i < n) {
		if (end[i] > i + 1) {
			niri_fit_one(f, i, end[i] - 1, inner, gap, min, span);
			i = end[i];
		} else {
			span[i] = (int)(f[i] * inner + 0.5);
			if (span[i] < min)
				span[i] = min;
			i++;
		}
	}
}

/* one column of the strip */
struct NiriColumn {
	Client *c[NIRI_MAX]; /* the windows sharing the column, top to bottom */
	int h[NIRI_MAX];     /* one NIRI_SIZE_* per window, same order */
	int n;               /* how many windows share the column */
	int width;           /* one NIRI_SIZE_*, applies to every window in it */
	int voff;            /* how far the stack is scrolled, in pixels */
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

/* a column decoded from the saved record */
struct NiriSavedColumn {
	int width;
	int h[NIRI_MAX];
	Window win[NIRI_MAX];
	int n;
};

static int
niri_size_index(unsigned long v, int def)
{
	return v < NIRI_NSIZES ? (int)v : def;
}

static int
niri_state_parse(const unsigned long *data, int ndata, int *ncols,
                 struct NiriSavedColumn *cols)
{
	int i, j, wincnt, top, version, col;

	*ncols = 0;
	if (!data || ndata < 2)
		return 0;

	version = (int)data[0];
	if (version == NIRI_STATE_VERSION) {
		wincnt = 1;
		i = 2;
		*ncols = (int)data[1];
		for (j = 0; j < *ncols && i + 1 < ndata; j++) {
			cols[j].width = niri_size_index(data[i], NIRI_SIZE_DEFAULT);
			wincnt = (int)data[i + 1];
			i += 2;
			if (wincnt < 0 || wincnt > NIRI_MAX || i + wincnt * 2 > ndata)
				return 0;
			cols[j].n = wincnt;
			for (col = 0; col < wincnt; col++) {
				cols[j].h[col] = niri_size_index(data[i], NIRI_HEIGHT_DEFAULT);
				cols[j].win[col] = (Window)data[i + 1];
				i += 2;
			}
		}
		return j == *ncols || j == NIRI_MAX;
	}

	if (version == NIRI_STATE_LEGACY_VERSION) {
		if (ndata < 3)
			return 0;
		i = 3;
		*ncols = (int)data[2];
		for (j = 0; j < *ncols && i + 1 < ndata; j++) {
			top = (int)data[i];
			(void)top;
			wincnt = (int)data[i + 1];
			i += 2;
			if (wincnt < 0 || wincnt > NIRI_MAX || i + wincnt > ndata)
				return 0;
			cols[j].width = NIRI_SIZE_DEFAULT;
			cols[j].n = wincnt;
			for (col = 0; col < wincnt; col++) {
				cols[j].h[col] = NIRI_HEIGHT_DEFAULT;
				cols[j].win[col] = (Window)data[i++];
			}
		}
		return j == *ncols || j == NIRI_MAX;
	}

	return 0;
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
	if (!data || format != 32 || *n < 2
	        || (data[0] != NIRI_STATE_VERSION
	            && data[0] != NIRI_STATE_LEGACY_VERSION)) {
		if (data)
			XFree(data);
		*n = 0;
		return NULL;
	}
	return (unsigned long *)data;
}

/* Rebuild the strip of the current tag from its saved record.
 *
 * Version 2:
 *     version, column count
 *     column: width preset, window count, then per window:
 *             height preset, window id
 *
 * Version 1 (legacy):
 *     version, unused, column count
 *     column: unused, window count, window ids...
 *
 * During the startup scan the record is applied as far as possible: a saved
 * window that still exists on the server but is not managed yet means the scan
 * is still running, so the strip is rebuilt again on the next arrange. */
static void
niri_state_restore(Monitor *m, Client **tiles, int n)
{
	struct NiriTagState *st = niri_state_of(m);
	struct NiriStrip *s = &st->strip;
	struct NiriSavedColumn saved[NIRI_MAX];
	XErrorHandler oldhandler;
	unsigned long *data, ndata, id;
	XWindowAttributes wa;
	int tag = niri_tag(m);
	int i, j, ncol, pending = 0;

	if (st->restored)
		return;

	data = niri_record_get(tag, &ndata);
	if (!data) {
		/* no saved layout, or one from another version */
		st->restored = 1;
		return;
	}
	if (!niri_state_parse(data, (int)ndata, &ncol, saved)) {
		XFree(data);
		st->restored = 1;
		return;
	}

	/* The error handler is swapped out because a window can disappear
	 * between saving the layout and reading it back. */
	oldhandler = XSetErrorHandler(niri_ignore_error);
	for (i = 0; i < ncol; i++)
		for (j = 0; j < saved[i].n; j++) {
			id = saved[i].win[j];
			if (wintoclient((Window)id))
				continue;
			if (XGetWindowAttributes(dpy, (Window)id, &wa))
				pending = 1;
		}
	XSync(dpy, False);
	XSetErrorHandler(oldhandler);

	/* put the known windows back into their columns, in their saved order */
	s->ncol = 0;
	for (i = 0; i < ncol && s->ncol < NIRI_MAX; i++) {
		struct NiriColumn col;
		Client *c;

		col.width = saved[i].width;
		col.voff = 0;
		col.n = 0;
		for (j = 0; j < saved[i].n && col.n < NIRI_MAX; j++) {
			c = niri_tile_by_win(tiles, n, saved[i].win[j]);
			if (c) {
				col.h[col.n] = saved[i].h[j];
				col.c[col.n] = c;
				col.n++;
			}
		}
		if (col.n > 0)
			s->col[s->ncol++] = col;
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
	long data[2 + 2 * NIRI_MAX + 2 * NIRI_MAX];
	int tag = niri_tag(m);
	int i, j, k = 0;

	/* while the startup scan is still running the old record is better */
	if (!st->restored)
		return;

	data[k++] = NIRI_STATE_VERSION;
	data[k++] = s->ncol;
	for (i = 0; i < s->ncol; i++) {
		data[k++] = s->col[i].width;
		data[k++] = s->col[i].n;
		for (j = 0; j < s->col[i].n; j++) {
			data[k++] = s->col[i].h[j];
			data[k++] = (long)s->col[i].c[j]->win;
		}
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
				for (k = j; k + 1 < s->col[i].n; k++) {
					s->col[i].c[k] = s->col[i].c[k + 1];
					s->col[i].h[k] = s->col[i].h[k + 1];
				}
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
		s->col[pos].width = NIRI_SIZE_DEFAULT;
		s->col[pos].voff = 0;
		s->col[pos].c[0] = tiles[k];
		s->col[pos].h[0] = NIRI_HEIGHT_DEFAULT;
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
	int n;                /* windows in the strip */
	int ncols;            /* columns in the strip */
	int bw;               /* border width */
	int gap;              /* gap between two windows and at the edges */
	int cw[NIRI_MAX];     /* client width of a window, per column */
	int relx[NIRI_MAX];   /* left edge of the column, relative to the strip */
	int stripw;           /* width of the strip, leading gap to trailing edge */
	int centered;         /* centering a single column? */
};

static void
niri_geom(Monitor *m, struct NiriGeom *g)
{
	struct NiriStrip *s = &niri_state_of(m)->strip;
	int inner, x, i, foot;

	g->n = niri_n;
	g->ncols = s->ncol;
	g->bw = g->n > 0 ? niri_list[0]->bw : 0;
	g->gap = niri_gap(m);

	/* full sets of widths that add up to one screen share the width and the
	 * gaps between columns exactly; other sizes keep their absolute width */
	double f[NIRI_MAX];
	int span[NIRI_MAX];

	inner = m->ww - 2 * g->gap;
	if (inner < 1)
		inner = 1;
	for (i = 0; i < g->ncols; i++) {
		double v = niri_sizes[s->col[i].width];

		f[i] = v > 0.0 ? v : niri_sizes[NIRI_SIZE_DEFAULT];
	}
	niri_fit_spans(f, g->ncols, inner, g->gap, 2 * g->bw + 1, span);
	x = g->gap;
	for (i = 0; i < g->ncols; i++) {
		foot = span[i];
		g->cw[i] = foot - 2 * g->bw;
		g->relx[i] = x;
		x += foot + g->gap;
	}
	g->stripw = x - g->gap;
	g->centered = 0;
#if NIRI_ALWAYS_CENTER_SINGLE_COLUMN
	g->centered = g->ncols == 1;
#endif
}

/* how far the view is scrolled along the X axis */
static int
niri_clamp(Monitor *m, struct NiriGeom *g, int cx)
{
	int min;

	if (g->centered)
		return cx;

	if (g->stripw <= m->ww - g->gap)
		return 0;

	min = m->ww - g->gap - g->stripw;
	if (min > 0)
		min = 0;
	if (cx > 0)
		return 0;
	if (cx < min)
		return min;
	return cx;
}

/* lay out the windows of a column: each one keeps its own height preset, and
 * the stack as a whole can be taller than the screen and scroll instead; the
 * heights describe the outer cell, borders included */
static void
niri_col_layout(Monitor *m, struct NiriColumn *col, int *off, int *span,
                int *stack)
{
	double frac[NIRI_MAX];
	double f;
	int bw, innerh, i;

	innerh = m->wh - 2 * niri_gap(m);
	if (innerh < 1)
		innerh = 1;
	bw = col->n > 0 ? col->c[0]->bw : 0;

	for (i = 0; i < col->n; i++) {
		f = niri_sizes[col->h[i]];
		frac[i] = f > 0.0 ? f : niri_sizes[NIRI_HEIGHT_DEFAULT];
	}
	niri_fit_spans(frac, col->n, innerh, niri_gap(m), 2 * bw + 1, span);
	*stack = 0;
	for (i = 0; i < col->n; i++) {
		off[i] = *stack;
		*stack += span[i] + niri_gap(m);
	}
	if (col->n > 0)
		*stack -= niri_gap(m);
}

/* keep the stack inside its own bounds */
static void
niri_clamp_voff(Monitor *m, struct NiriColumn *col)
{
	int off[NIRI_MAX], span[NIRI_MAX], stack;
	int innerh, maxv;

	niri_col_layout(m, col, off, span, &stack);
	innerh = m->wh - 2 * niri_gap(m);
	if (innerh < 1)
		innerh = 1;
	maxv = stack - innerh;
	if (maxv < 0)
		maxv = 0;
	if (col->voff < 0)
		col->voff = 0;
	if (col->voff > maxv)
		col->voff = maxv;
}

/* focus the window of a column that is visible closest to its top */
static void
niri_focus_first_visible(Monitor *m, int col)
{
	struct NiriStrip *s = &niri_state_of(m)->strip;
	struct NiriColumn *c = &s->col[col];
	int off[NIRI_MAX], span[NIRI_MAX], stack, k;

	niri_clamp_voff(m, c);
	niri_col_layout(m, c, off, span, &stack);
	(void)stack;
	k = 0;
	while (k + 1 < c->n && off[k] + span[k] <= c->voff)
		k++;
	focus(c->c[k]);
}

void
niri(Monitor *m)
{
	struct NiriGeom g;
	struct NiriStrip *s;
	int cx, i, j, x, fcol, left, right, innerh;

	if (!m)
		return;

	niri_sync(m);
	niri_geom(m, &g);
	s = &niri_state_of(m)->strip;
	if (g.centered) {
		int foot = g.cw[0] + 2 * g.bw;

		cx = (m->ww - foot) / 2 - g.relx[0];
	} else
		cx = niri_offset(m);
	innerh = m->wh - 2 * g.gap;
	if (innerh < 1)
		innerh = 1;

	/* the focused column has to be fully visible */
	fcol = niri_column_of(m, m->sel);
	if (fcol >= 0) {
		left = m->wx + cx + g.relx[fcol];     /* outer left edge */
		right = left + g.cw[fcol] + 2 * g.bw;  /* outer right edge */

		if (left < m->wx + g.gap)
			cx = -g.relx[fcol];
		else if (right > m->wx + m->ww - g.gap)
			cx = m->ww - g.gap - (g.relx[fcol] + g.cw[fcol] + 2 * g.bw);
	}

	cx = niri_clamp(m, &g, cx);
	niri_set_offset(m, cx);

	/* the focused window has to be inside the visible part of its stack */
	if (fcol >= 0) {
		int off[NIRI_MAX], span[NIRI_MAX], stack;
		struct NiriColumn *col = &s->col[fcol];

		niri_clamp_voff(m, col);
		niri_col_layout(m, col, off, span, &stack);
		for (j = 0; j < col->n; j++)
			if (col->c[j] == m->sel)
				break;
		if (j < col->n) {
			if (off[j] < col->voff)
				col->voff = off[j];
			else if (off[j] + span[j] > col->voff + innerh)
				col->voff = off[j] + span[j] - innerh;
		}
	}

	for (i = 0; i < g.ncols; i++) {
		int off[NIRI_MAX], span[NIRI_MAX], stack, k = s->col[i].n;
		struct NiriColumn *col = &s->col[i];

		if (k < 1)
			continue;

		niri_clamp_voff(m, col);
		niri_col_layout(m, col, off, span, &stack);

		x = m->wx + cx + g.relx[i];
		for (j = 0; j < k; j++) {
			/* resizeclient() instead of resize(): the windows of the strip
			 * have to be allowed to sit outside of the window area */
			resizeclient(col->c[j], x,
			             m->wy + g.gap + off[j] - col->voff,
			             g.cw[i], span[j] - 2 * g.bw);
		}
	}

	/* remember the columns for the next start */
	niri_state_put(m);
}

/* scroll the focused column up or down to the next window boundary, the
 * columns and their horizontal position stay where they are; the focus is
 * pulled along when it would leave the visible area */
static void
niri_scroll_column(Monitor *m, int dir)
{
	struct NiriStrip *s = &niri_state_of(m)->strip;
	struct NiriColumn *col;
	int fcol = niri_column_of(m, m->sel), j, i, innerh, maxv;
	int off[NIRI_MAX], span[NIRI_MAX], stack;

	if (fcol < 0)
		return;

	col = &s->col[fcol];
	niri_clamp_voff(m, col);
	niri_col_layout(m, col, off, span, &stack);
	innerh = m->wh - 2 * niri_gap(m);
	if (innerh < 1)
		innerh = 1;
	maxv = stack - innerh;
	if (maxv < 0)
		maxv = 0;

	if (dir > 0) {
		j = col->voff;
		/* the next boundary below the current top */
		for (i = 0; i < col->n; i++)
			if (off[i] > j + 1) {
				col->voff = off[i];
				break;
			}
		if (i == col->n)
			col->voff = maxv;
	} else if (dir < 0) {
		j = col->voff;
		/* the next boundary above the current top */
		for (i = col->n - 1; i >= 0; i--)
			if (off[i] + span[i] < j - 1) {
				col->voff = off[i];
				break;
			}
		if (i < 0)
			col->voff = 0;
	}

	for (j = 0; j < col->n; j++)
		if (col->c[j] == m->sel)
			break;
	if (j < col->n) {
		/* the focus has to stay inside what is visible */
		if (off[j] + span[j] <= col->voff || off[j] >= col->voff + innerh) {
			int k;

			for (k = 0; k < col->n; k++)
				if (off[k] + span[k] > col->voff && off[k] < col->voff + innerh) {
					focus(col->c[k]);
					break;
				}
		}
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
	int col, cx;

	if (fcol < 0)
		return;

	col = fcol + dir;
	if (col < 0 || col >= s->ncol || s->col[col].n < 1)
		return;

	niri_geom(m, &g);
	if (g.stripw > m->ww - g.gap) {
		/* only a strip that does not fit on the screen scrolls */
		cx = niri_clamp(m, &g, niri_offset(m) + g.relx[fcol] - g.relx[col]);
		niri_set_offset(m, cx);
	}

	/* focus the window of the new column that is visible closest to the top */
	niri_focus_first_visible(m, col);
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
	int j, nj;

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
	if (g.stripw <= m->ww - g.gap)
		return;

	cx = niri_clamp(m, &g, niri_offset(m) - dx);
	niri_set_offset(m, cx);

	/* the focus follows the view, it has to happen before the layout runs
	 * again, otherwise the new view would be scrolled back to the column
	 * that is still focused */
	{
		int center = m->ww / 2 - cx, i;

		col = 0;
		for (i = 1; i < g.ncols; i++)
			if (g.relx[i] <= center)
				col = i;
	}

	niri_focus_first_visible(m, col);
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
	s->col[fcol].h[k] = s->col[fcol + 1].h[0];
	s->col[fcol].n++;

	for (i = 0; i + 1 < s->col[fcol + 1].n; i++) {
		s->col[fcol + 1].c[i] = s->col[fcol + 1].c[i + 1];
		s->col[fcol + 1].h[i] = s->col[fcol + 1].h[i + 1];
	}
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
	{
		int h = s->col[fcol].h[j];

		for (i = j; i + 1 < s->col[fcol].n; i++) {
			s->col[fcol].c[i] = s->col[fcol].c[i + 1];
			s->col[fcol].h[i] = s->col[fcol].h[i + 1];
		}
		s->col[fcol].n--;

		for (i = s->ncol; i > fcol + 1; i--)
			s->col[i] = s->col[i - 1];
		s->col[fcol + 1].n = 1;
		s->col[fcol + 1].width = NIRI_SIZE_DEFAULT;
		s->col[fcol + 1].voff = 0;
		s->col[fcol + 1].c[0] = c;
		s->col[fcol + 1].h[0] = h;
		s->ncol++;
	}
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

/* change the width of the focused column, one step per keypress; it wraps
 * through third, half, two thirds and full */
void
niri_zoom(const Arg *arg)
{
	Monitor *m = selmon;

	if (!m)
		return;

	if (m->lt[m->sellt]->arrange != niri) {
		/* another layout is active, keep the usual master factor */
		Arg a;
		a.f = arg->i > 0 ? -0.05 : 0.05;
		setmfact(&a);
		return;
	}

	niri_sync(m);
	{
		struct NiriStrip *s = &niri_state_of(m)->strip;
		int c = niri_column_of(m, m->sel);

		if (c < 0)
			return;
		if (arg->i > 0)
			s->col[c].width = (s->col[c].width + NIRI_NSIZES - 1) % NIRI_NSIZES;
		else
			s->col[c].width = (s->col[c].width + 1) % NIRI_NSIZES;
	}
	arrange(m);
}

/* change the height of the focused window, one step per keypress; it wraps
 * through third, half, two thirds and full */
void
niri_vzoom(const Arg *arg)
{
	Monitor *m = selmon;

	if (!m)
		return;

	if (m->lt[m->sellt]->arrange != niri)
		return;

	niri_sync(m);
	{
		struct NiriStrip *s = &niri_state_of(m)->strip;
		int c = niri_column_of(m, m->sel), j;

		if (c < 0)
			return;
		for (j = 0; j < s->col[c].n; j++)
			if (s->col[c].c[j] == m->sel)
				break;
		if (j >= s->col[c].n)
			return;
		if (arg->i > 0)
			s->col[c].h[j] = (s->col[c].h[j] + NIRI_NSIZES - 1) % NIRI_NSIZES;
		else
			s->col[c].h[j] = (s->col[c].h[j] + 1) % NIRI_NSIZES;
	}
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
