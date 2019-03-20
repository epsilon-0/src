/* $OpenBSD: format-draw.c,v 1.4 2019/03/20 07:24:03 nicm Exp $ */

/*
 * Copyright (c) 2019 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/* Format range. */
struct format_range {
	u_int				 index;
	struct screen			*s;

	u_int				 start;
	u_int				 end;

	enum style_range_type		 type;
	u_int				 argument;

	TAILQ_ENTRY(format_range)	 entry;
};
TAILQ_HEAD(format_ranges, format_range);

/* Does this range match this style? */
static int
format_is_type(struct format_range *fr, struct style *sy)
{
	if (fr->type != sy->range_type)
		return (0);
	if (fr->type == STYLE_RANGE_WINDOW &&
	    fr->argument != sy->range_argument)
		return (0);
	return (1);
}

/* Free a range. */
static void
format_free_range(struct format_ranges *frs, struct format_range *fr)
{
	TAILQ_REMOVE(frs, fr, entry);
	free(fr);
}

/* Fix range positions. */
static void
format_update_ranges(struct format_ranges *frs, struct screen *s, u_int offset,
    u_int start, u_int width)
{
	struct format_range	*fr, *fr1;

	if (frs == NULL)
		return;

	TAILQ_FOREACH_SAFE(fr, frs, entry, fr1) {
		if (fr->s != s)
			continue;

		if (fr->end <= start || fr->start >= start + width) {
			format_free_range(frs, fr);
			continue;
		}

		if (fr->start < start)
			fr->start = start;
		if (fr->end > start + width)
			fr->end = start + width;
		if (fr->start == fr->end) {
			format_free_range(frs, fr);
			continue;
		}

		fr->start += offset;
		fr->end += offset;
	}
}

/* Draw a part of the format. */
static void
format_draw_put(struct screen_write_ctx *octx, u_int ocx, u_int ocy,
    struct screen *s, struct format_ranges *frs, u_int offset, u_int start,
    u_int width)
{
	/*
	 * The offset is how far from the cursor on the target screen; start
	 * and width how much to copy from the source screen.
	 */
	screen_write_cursormove(octx, ocx + offset, ocy, 0);
	screen_write_fast_copy(octx, s, start, 0, width, 1);
	format_update_ranges(frs, s, offset, start, width);
}

/* Draw list part of format. */
static void
format_draw_put_list(struct screen_write_ctx *octx,
    u_int ocx, u_int ocy, u_int offset, u_int width, struct screen *list,
    struct screen *list_left, struct screen *list_right, int focus_start,
    int focus_end, struct format_ranges *frs)
{
	u_int	start, focus_centre;

	/* If there is enough space for the list, draw it entirely. */
	if (width >= list->cx) {
		format_draw_put(octx, ocx, ocy, list, frs, offset, 0, width);
		return;
	}

	/* The list needs to be trimmed. Try to keep the focus visible. */
	focus_centre = focus_start + (focus_end - focus_start) / 2;
	if (focus_centre < width / 2)
		start = 0;
	else
		start = focus_centre - width / 2;
	if (start + width > list->cx)
		start = list->cx - width;

	/* Draw <> markers at either side if needed. */
	if (start != 0 && width > list_left->cx) {
		screen_write_cursormove(octx, ocx + offset, ocy, 0);
		screen_write_fast_copy(octx, list_left, 0, 0, list_left->cx, 1);
		offset += list_left->cx;
		start += list_left->cx;
		width -= list_left->cx;
	}
	if (start + width < list->cx && width > list_right->cx) {
		screen_write_cursormove(octx, ocx + offset + width - 1, ocy, 0);
		screen_write_fast_copy(octx, list_right, 0, 0, list_right->cx,
		    1);
		width -= list_right->cx;
	}

	/* Draw the list screen itself. */
	format_draw_put(octx, ocx, ocy, list, frs, offset, start, width);
}

/* Draw format with no list. */
static void
format_draw_none(struct screen_write_ctx *octx, u_int available, u_int ocx,
    u_int ocy, struct screen *left, struct screen *centre, struct screen *right,
    struct format_ranges *frs)
{
	u_int	width_left, width_centre, width_right;

	width_left = left->cx;
	width_centre = centre->cx;
	width_right = right->cx;

	/*
	 * Try to keep as much of the left and right as possible at the expense
	 * of the centre.
	 */
	while (width_left + width_centre + width_right > available) {
		if (width_centre > 0)
			width_centre--;
		else if (width_right > 0)
			width_right--;
		else
			width_left--;
	}

	/* Write left. */
	format_draw_put(octx, ocx, ocy, left, frs, 0, 0, width_left);

	/* Write right at available - width_right. */
	format_draw_put(octx, ocx, ocy, right, frs,
	    available - width_right,
	    right->cx - width_right,
	    width_right);

	/*
	 * Write centre halfway between
	 *     width_left
	 * and
	 *     available - width_right.
	 */
	format_draw_put(octx, ocx, ocy, centre, frs,
	    width_left
	    + ((available - width_right) - width_left) / 2
	    - width_centre / 2,
	    centre->cx / 2 - width_centre / 2,
	    width_centre);
}

/* Draw format with list on the left. */
static void
format_draw_left(struct screen_write_ctx *octx, u_int available, u_int ocx,
    u_int ocy, struct screen *left, struct screen *centre, struct screen *right,
    struct screen *list, struct screen *list_left, struct screen *list_right,
    struct screen *after, int focus_start, int focus_end,
    struct format_ranges *frs)
{
	u_int			width_left, width_centre, width_right;
	u_int			width_list, width_after;
	struct screen_write_ctx	ctx;

	width_left = left->cx;
	width_centre = centre->cx;
	width_right = right->cx;
	width_list = list->cx;
	width_after = after->cx;

	/*
	 * Trim first the centre, then the list, then the right, then after the
	 * list, then the left.
	 */
	while (width_left +
	    width_centre +
	    width_right +
	    width_list +
	    width_after > available) {
		if (width_centre > 0)
			width_centre--;
		else if (width_list > 0)
			width_list--;
		else if (width_right > 0)
			width_right--;
		else if (width_after > 0)
			width_after--;
		else
			width_left--;
	}

	/* If there is no list left, pass off to the no list function. */
	if (width_list == 0) {
		screen_write_start(&ctx, NULL, left);
		screen_write_fast_copy(&ctx, after, 0, 0, width_after, 1);
		screen_write_stop(&ctx);

		format_draw_none(octx, available, ocx, ocy, left, centre,
		    right, frs);
		return;
	}

	/* Write left at 0. */
	format_draw_put(octx, ocx, ocy, left, frs, 0, 0, width_left);

	/* Write right at available - width_right. */
	format_draw_put(octx, ocx, ocy, right, frs,
	    available - width_right,
	    right->cx - width_right,
	    width_right);

	/* Write after at width_left + width_list. */
	format_draw_put(octx, ocx, ocy, after, frs,
	    width_left + width_list,
	    0,
	    width_after);

	/*
	 * Write centre halfway between
	 *     width_left + width_list + width_after
	 * and
	 *     available - width_right.
	 */
	format_draw_put(octx, ocx, ocy, centre, frs,
	    (width_left + width_list + width_after)
	    + ((available - width_right)
		- (width_left + width_list + width_after)) / 2
	    - width_centre / 2,
	    centre->cx / 2 - width_centre / 2,
	    width_centre);

	/*
	 * The list now goes from
	 *     width_left
	 * to
	 *     width_left + width_list.
	 * If there is no focus given, keep the left in focus.
	 */
	if (focus_start == -1 || focus_end == -1)
		focus_start = focus_end = 0;
	format_draw_put_list(octx, ocx, ocy, width_left, width_list, list,
	    list_left, list_right, focus_start, focus_end, frs);
}

/* Draw format with list in the centre. */
static void
format_draw_centre(struct screen_write_ctx *octx, u_int available, u_int ocx,
    u_int ocy, struct screen *left, struct screen *centre, struct screen *right,
    struct screen *list, struct screen *list_left, struct screen *list_right,
    struct screen *after, int focus_start, int focus_end,
    struct format_ranges *frs)
{
	u_int			width_left, width_centre, width_right;
	u_int			width_list, width_after, middle;
	struct screen_write_ctx	ctx;

	width_left = left->cx;
	width_centre = centre->cx;
	width_right = right->cx;
	width_list = list->cx;
	width_after = after->cx;

	/*
	 * Trim first the list, then after the list, then the centre, then the
	 * right, then the left.
	 */
	while (width_left +
	    width_centre +
	    width_right +
	    width_list +
	    width_after > available) {
		if (width_list > 0)
			width_list--;
		else if (width_after > 0)
			width_after--;
		else if (width_centre > 0)
			width_centre--;
		else if (width_right > 0)
			width_right--;
		else
			width_left--;
	}

	/* If there is no list left, pass off to the no list function. */
	if (width_list == 0) {
		screen_write_start(&ctx, NULL, centre);
		screen_write_fast_copy(&ctx, after, 0, 0, width_after, 1);
		screen_write_stop(&ctx);

		format_draw_none(octx, available, ocx, ocy, left, centre,
		    right, frs);
		return;
	}

	/* Write left at 0. */
	format_draw_put(octx, ocx, ocy, left, frs, 0, 0, width_left);

	/* Write after at available - width_after. */
	format_draw_put(octx, ocx, ocy, after, frs,
	    available - width_after,
	    after->cx - width_after,
	    width_after);

	/* Write right at available - width_right. */
	format_draw_put(octx, ocx, ocy, right, frs,
	    available - width_right,
	    right->cx - width_right,
	    width_right);

	/*
	 * All three centre sections are offset from the middle of the
	 * available space.
	 */
	middle = (width_left + ((available - width_right) - width_left) / 2);

	/*
	 * Write centre at
	 *     middle - width_list / 2 - width_centre.
	 */
	format_draw_put(octx, ocx, ocy, centre, frs,
	    middle - width_list / 2 - width_centre,
	    0,
	    width_centre);

	/*
	 * Write after at
	 *     middle + width_list / 2 - width_centre.
	 */
	format_draw_put(octx, ocx, ocy, after, frs,
	    middle + width_list / 2,
	    0,
	    width_after);

	/*
	 * The list now goes from
	 *     middle - width_list / 2
	 * to
	 *     middle + width_list / 2
	 * If there is no focus given, keep the centre in focus.
	 */
	if (focus_start == -1 || focus_end == -1)
		focus_start = focus_end = list->cx / 2;
	format_draw_put_list(octx, ocx, ocy, middle - width_list / 2,
	    width_list, list, list_left, list_right, focus_start, focus_end,
	    frs);
}

/* Draw format with list on the right. */
static void
format_draw_right(struct screen_write_ctx *octx, u_int available, u_int ocx,
    u_int ocy, struct screen *left, struct screen *centre, struct screen *right,
    struct screen *list, struct screen *list_left, struct screen *list_right,
    struct screen *after, int focus_start, int focus_end,
    struct format_ranges *frs)
{
	u_int			width_left, width_centre, width_right;
	u_int			width_list, width_after;
	struct screen_write_ctx	ctx;

	width_left = left->cx;
	width_centre = centre->cx;
	width_right = right->cx;
	width_list = list->cx;
	width_after = after->cx;

	/*
	 * Trim first the centre, then the list, then the right, then
	 * after the list, then the left.
	 */
	while (width_left +
	    width_centre +
	    width_right +
	    width_list +
	    width_after > available) {
		if (width_centre > 0)
			width_centre--;
		else if (width_list > 0)
			width_list--;
		else if (width_right > 0)
			width_right--;
		else if (width_after > 0)
			width_after--;
		else
			width_left--;
	}

	/* If there is no list left, pass off to the no list function. */
	if (width_list == 0) {
		screen_write_start(&ctx, NULL, right);
		screen_write_fast_copy(&ctx, after, 0, 0, width_after, 1);
		screen_write_stop(&ctx);

		format_draw_none(octx, available, ocx, ocy, left, centre,
		    right, frs);
		return;
	}

	/* Write left at 0. */
	format_draw_put(octx, ocx, ocy, left, frs, 0, 0, width_left);

	/* Write after at available - width_after. */
	format_draw_put(octx, ocx, ocy, after, frs,
	    available - width_after,
	    after->cx - width_after,
	    width_after);

	/*
	 * Write right at
	 *     available - width_right - width_list - width_after.
	 */
	format_draw_put(octx, ocx, ocy, right, frs,
	    available - width_right - width_list - width_after,
	    0,
	    width_right);

	/*
	 * Write centre halfway between
	 *     width_left
	 * and
	 *     available - width_right - width_list - width_after.
	 */
	format_draw_put(octx, ocx, ocy, centre, frs,
	    width_left
	    + ((available - width_right - width_list - width_after)
		- width_left) / 2
	    - width_centre / 2,
	    centre->cx / 2 - width_centre / 2,
	    width_centre);

	/*
	 * The list now goes from
	 *     available - width_list - width_after
	 * to
	 *     available - width_after
	 * If there is no focus given, keep the right in focus.
	 */
	if (focus_start == -1 || focus_end == -1)
		focus_start = focus_end = 0;
	format_draw_put_list(octx, ocx, ocy, available - width_list -
	    width_after, width_list, list, list_left, list_right, focus_start,
	    focus_end, frs);
}

/* Draw a format to a screen. */
void
format_draw(struct screen_write_ctx *octx, const struct grid_cell *base,
    u_int available, const char *expanded, struct style_ranges *srs)
{
	enum { LEFT,
	       CENTRE,
	       RIGHT,
	       LIST,
	       LIST_LEFT,
	       LIST_RIGHT,
	       AFTER,
	       TOTAL } current = LEFT, last = LEFT;
	const char	        *names[] = { "LEFT",
					     "CENTRE",
					     "RIGHT",
					     "LIST",
					     "LIST_LEFT",
					     "LIST_RIGHT",
					     "AFTER" };
	size_t			 size = strlen(expanded);
	struct screen		*os = octx->s, s[TOTAL];
	struct screen_write_ctx	 ctx[TOTAL];
	u_int			 ocx = os->cx, ocy = os->cy, i, width[TOTAL];
	u_int			 map[] = { LEFT, LEFT, CENTRE, RIGHT };
	int			 focus_start = -1, focus_end = -1;
	int			 list_state = -1;
	enum style_align	 list_align = STYLE_ALIGN_DEFAULT;
	struct style		 sy;
	struct utf8_data	*ud = &sy.gc.data;
	const char		*cp, *end;
	enum utf8_state		 more;
	char			*tmp;
	struct format_range	*fr = NULL, *fr1;
	struct format_ranges	 frs;
	struct style_range	*sr;

	style_set(&sy, base);
	TAILQ_INIT(&frs);
	log_debug("%s: %s", __func__, expanded);

	/*
	 * We build three screens for left, right, centre alignment, one for
	 * the list, one for anything after the list and two for the list left
	 * and right markers.
	 */
	for (i = 0; i < TOTAL; i++) {
		screen_init(&s[i], size, 1, 0);
		screen_write_start(&ctx[i], NULL, &s[i]);
		screen_write_clearendofline(&ctx[i], base->bg);
		width[i] = 0;
	}

	/*
	 * Walk the string and add to the corresponding screens,
	 * parsing styles as we go.
	 */
	cp = expanded;
	while (*cp != '\0') {
		if (cp[0] != '#' || cp[1] != '[') {
			/* See if this is a UTF-8 character. */
			if ((more = utf8_open(ud, *cp)) == UTF8_MORE) {
				while (*++cp != '\0' && more == UTF8_MORE)
					more = utf8_append(ud, *cp);
				if (more != UTF8_DONE)
					cp -= ud->have;
			}

			/* Not a UTF-8 character - ASCII or not valid. */
			if (more != UTF8_DONE) {
				if (*cp < 0x20 || *cp > 0x7e) {
					/* Ignore nonprintable characters. */
					cp++;
					continue;
				}
				utf8_set(ud, *cp);
				cp++;
			}

			/* Draw the cell to th current screen. */
			screen_write_cell(&ctx[current], &sy.gc);
			width[current] += ud->width;
			continue;
		}

		/* This is a style. Work out where the end is and parse it. */
		end = format_skip(cp + 2, "]");
		if (end == NULL) {
			log_debug("no terminating ] at '%s'", cp + 2);
			return;
		}
		tmp = xstrndup(cp + 2, end - (cp + 2));
		if (style_parse(&sy, base, tmp) != 0) {
			log_debug("invalid style '%s'", tmp);
			free(tmp);
			cp = end + 1;
			continue;
		}
		log_debug("style '%s' -> '%s'", tmp, style_tostring(&sy));
		free(tmp);

		/* Check the list state. */
		switch (sy.list) {
		case STYLE_LIST_ON:
			/*
			 * Entering the list, exiting a marker, or exiting the
			 * focus.
			 */
			if (list_state != 0) {
				if (fr != NULL) { /* abort any region */
					free(fr);
					fr = NULL;
				}
				list_state = 0;
				list_align = sy.align;
			}

			/* End the focus if started. */
			if (focus_start != -1 && focus_end == -1)
				focus_end = s[LIST].cx;

			current = LIST;
			break;
		case STYLE_LIST_FOCUS:
			/* Entering the focus. */
			if (list_state != 0) /* not inside the list */
				break;
			if (focus_start == -1) /* focus already started */
				focus_start = s[LIST].cx;
			break;
		case STYLE_LIST_OFF:
			/* Exiting or outside the list. */
			if (list_state == 0) {
				if (fr != NULL) { /* abort any region */
					free(fr);
					fr = NULL;
				}
				if (focus_start != -1 && focus_end == -1)
					focus_end = s[LIST].cx;

				map[list_align] = AFTER;
				if (list_align == STYLE_ALIGN_LEFT)
					map[STYLE_ALIGN_DEFAULT] = AFTER;
				list_state = 1;
			}
			current = map[sy.align];
			break;
		case STYLE_LIST_LEFT_MARKER:
			/* Entering left marker. */
			if (list_state != 0) /* not inside the list */
				break;
			if (s[LIST_LEFT].cx != 0) /* already have marker */
				break;
			if (fr != NULL) { /* abort any region */
				free(fr);
				fr = NULL;
			}
			if (focus_start != -1 && focus_end == -1)
				focus_start = focus_end = -1;
			current = LIST_LEFT;
			break;
		case STYLE_LIST_RIGHT_MARKER:
			/* Entering right marker. */
			if (list_state != 0) /* not inside the list */
				break;
			if (s[LIST_RIGHT].cx != 0) /* already have marker */
				break;
			if (fr != NULL) { /* abort any region */
				free(fr);
				fr = NULL;
			}
			if (focus_start != -1 && focus_end == -1)
				focus_start = focus_end = -1;
			current = LIST_RIGHT;
			break;
		}
		if (current != last) {
			log_debug("%s: change %s -> %s", __func__,
			    names[last], names[current]);
			last = current;
		}

		/*
		 * Check if the range style has changed and if so end the
		 * current range and start a new one if needed.
		 */
		if (srs != NULL) {
			if (fr != NULL && !format_is_type(fr, &sy)) {
				if (s[current].cx != fr->start) {
					fr->end = s[current].cx + 1;
					TAILQ_INSERT_TAIL(&frs, fr, entry);
				} else
					free(fr);
				fr = NULL;
			}
			if (fr == NULL && sy.range_type != STYLE_RANGE_NONE) {
				fr = xcalloc(1, sizeof *fr);
				fr->index = current;

				fr->s = &s[current];
				fr->start = s[current].cx;

				fr->type = sy.range_type;
				fr->argument = sy.range_argument;
			}
		}

		cp = end + 1;
	}
	free(fr);

	for (i = 0; i < TOTAL; i++) {
		screen_write_stop(&ctx[i]);
		log_debug("%s: width %s is %u", __func__, names[i], width[i]);
	}
	if (focus_start != -1 && focus_end != -1)
		log_debug("focus is %d-%d", focus_start, focus_end);
	TAILQ_FOREACH(fr, &frs, entry) {
		log_debug("%s: range %d|%u is %s %u-%u", __func__, fr->type,
		    fr->argument, names[fr->index], fr->start, fr->end);
	}

	/*
	 * Draw the screens. How they are arranged depends on where the list
	 * appearsq.
	 */
	switch (list_align) {
	case STYLE_ALIGN_DEFAULT:
		/* No list. */
		format_draw_none(octx, available, ocx, ocy, &s[LEFT],
		    &s[CENTRE], &s[RIGHT], &frs);
		break;
	case STYLE_ALIGN_LEFT:
		/* List is part of the left. */
		format_draw_left(octx, available, ocx, ocy, &s[LEFT],
		    &s[CENTRE], &s[RIGHT], &s[LIST], &s[LIST_LEFT],
		    &s[LIST_RIGHT], &s[AFTER], focus_start, focus_end, &frs);
		break;
	case STYLE_ALIGN_CENTRE:
		/* List is part of the centre. */
		format_draw_centre(octx, available, ocx, ocy, &s[LEFT],
		    &s[CENTRE], &s[RIGHT], &s[LIST], &s[LIST_LEFT],
		    &s[LIST_RIGHT], &s[AFTER], focus_start, focus_end, &frs);
		break;
	case STYLE_ALIGN_RIGHT:
		/* List is part of the right. */
		format_draw_right(octx, available, ocx, ocy, &s[LEFT],
		    &s[CENTRE], &s[RIGHT], &s[LIST], &s[LIST_LEFT],
		    &s[LIST_RIGHT], &s[AFTER], focus_start, focus_end, &frs);
		break;
	}

	/* Create ranges to return. */
	TAILQ_FOREACH_SAFE(fr, &frs, entry, fr1) {
		sr = xcalloc(1, sizeof *sr);
		sr->type = fr->type;
		sr->argument = fr->argument;
		sr->start = fr->start;
		sr->end = fr->end;
		TAILQ_INSERT_TAIL(srs, sr, entry);

		log_debug("%s: range %d|%u at %u-%u", __func__, sr->type,
		    sr->argument, sr->start, sr->end);

		format_free_range(&frs, fr);
	}

	/* Free the screens. */
	for (i = 0; i < TOTAL; i++)
		screen_free(&s[i]);

	/* Restore the original cursor position. */
	screen_write_cursormove(octx, ocx, ocy, 0);
}

/* Get width, taking #[] into account. */
u_int
format_width(const char *expanded)
{
	const char		*cp, *end;
	u_int			 width = 0;
	struct utf8_data	 ud;
	enum utf8_state		 more;

	cp = expanded;
	while (*cp != '\0') {
		if (cp[0] == '#' && cp[1] == '[') {
			end = format_skip(cp + 2, "]");
			if (end == NULL)
				return 0;
			cp = end + 1;
		} else if ((more = utf8_open(&ud, *cp)) == UTF8_MORE) {
			while (*++cp != '\0' && more == UTF8_MORE)
				more = utf8_append(&ud, *cp);
			if (more == UTF8_DONE)
				width += ud.width;
			else
				cp -= ud.have;
		} else if (*cp > 0x1f && *cp < 0x7f) {
			width++;
			cp++;
		}
	}
	return (width);
}

/* Trim on the left, taking #[] into account. */
char *
format_trim_left(const char *expanded, u_int limit)
{
	char			*copy, *out;
	const char		*cp = expanded, *end;
	u_int			 width = 0;
	struct utf8_data	 ud;
	enum utf8_state		 more;

	out = copy = xmalloc(strlen(expanded) + 1);
	while (*cp != '\0') {
		if (cp[0] == '#' && cp[1] == '[') {
			end = format_skip(cp + 2, "]");
			if (end == NULL)
				break;
			memcpy(out, cp, end + 1 - cp);
			out += (end + 1 - cp);
			cp = end + 1;
		} else if ((more = utf8_open(&ud, *cp)) == UTF8_MORE) {
			while (*++cp != '\0' && more == UTF8_MORE)
				more = utf8_append(&ud, *cp);
			if (more == UTF8_DONE) {
				if (width + ud.width <= limit) {
					memcpy(out, ud.data, ud.size);
					out += ud.size;
				}
				width += ud.width;
			} else
				cp -= ud.have;
		} else if (*cp > 0x1f && *cp < 0x7f) {
			if (width + 1 <= limit)
				*out++ = *cp;
			width++;
			cp++;
		}
	}
	*out = '\0';
	return (copy);
}

/* Trim on the right, taking #[] into account. */
char *
format_trim_right(const char *expanded, u_int limit)
{
	char			*copy, *out;
	const char		*cp = expanded, *end;
	u_int			 width = 0, total_width, skip;
	struct utf8_data	 ud;
	enum utf8_state		 more;

	total_width = format_width(expanded);
	if (total_width <= limit)
		return (xstrdup(expanded));
	skip = total_width - limit;

	out = copy = xmalloc(strlen(expanded) + 1);
	while (*cp != '\0') {
		if (cp[0] == '#' && cp[1] == '[') {
			end = format_skip(cp + 2, "]");
			if (end == NULL)
				break;
			memcpy(out, cp, end + 1 - cp);
			out += (end + 1 - cp);
			cp = end + 1;
		} else if ((more = utf8_open(&ud, *cp)) == UTF8_MORE) {
			while (*++cp != '\0' && more == UTF8_MORE)
				more = utf8_append(&ud, *cp);
			if (more == UTF8_DONE) {
				if (width >= skip) {
					memcpy(out, ud.data, ud.size);
					out += ud.size;
				}
				width += ud.width;
			} else
				cp -= ud.have;
		} else if (*cp > 0x1f && *cp < 0x7f) {
			if (width >= skip)
				*out++ = *cp;
			width++;
			cp++;
		}
	}
	*out = '\0';
	return (copy);
}
