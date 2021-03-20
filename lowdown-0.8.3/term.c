/*	$Id: term.c,v 1.48 2021/03/13 09:45:51 kristaps Exp $ */
/*
 * Copyright (c) 2020--2021 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "config.h"

#if HAVE_SYS_QUEUE
# include <sys/queue.h>
#endif

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "lowdown.h"
#include "extern.h"

struct tstack {
	const struct lowdown_node 	*n; /* node in question */
	size_t				 lines; /* times emitted */
};

struct term {
	unsigned int		 opts; /* oflags from lowdown_cfg */
	size_t			 col; /* output column from zero */
	ssize_t			 last_blank; /* line breaks or -1 (start) */
	struct tstack		*stack; /* stack of nodes */
	size_t			 stackmax; /* size of stack */
	size_t			 stackpos; /* position in stack */
	size_t			 maxcol; /* soft limit */
	size_t			 hmargin; /* left of content */
	size_t			 vmargin; /* before/after content */
	struct lowdown_buf	*tmp; /* for temporary allocations */
	wchar_t			*buf; /* buffer for counting wchar */
	size_t			 bufsz; /* size of buf */
};

/*
 * How to style the output on the screen.
 */
struct sty {
	int	 italic;
	int	 strike;
	int	 bold;
	int	 under;
	size_t	 bcolour; /* not inherited */
	size_t	 colour; /* not inherited */
	int	 override; /* don't inherit */
#define	OSTY_ITALIC	0x01
#define	OSTY_BOLD	0x02
};

/* Per-node styles. */

static const struct sty sty_image =	{ 0, 0, 1, 0,   0, 92, 1 };
static const struct sty sty_foot_ref =	{ 0, 0, 1, 0,   0, 92, 1 };
static const struct sty sty_codespan = 	{ 0, 0, 0, 0,  47, 31, 0 };
static const struct sty sty_hrule = 	{ 0, 0, 0, 0,   0, 37, 0 };
static const struct sty sty_blockhtml =	{ 0, 0, 0, 0,   0, 37, 0 };
static const struct sty sty_rawhtml = 	{ 0, 0, 0, 0,   0, 37, 0 };
static const struct sty sty_strike = 	{ 0, 1, 0, 0,   0,  0, 0 };
static const struct sty sty_emph = 	{ 1, 0, 0, 0,   0,  0, 0 };
static const struct sty sty_highlight =	{ 0, 0, 1, 0,   0,  0, 0 };
static const struct sty sty_d_emph = 	{ 0, 0, 1, 0,   0,  0, 0 };
static const struct sty sty_t_emph = 	{ 1, 0, 1, 0,   0,  0, 0 };
static const struct sty sty_link = 	{ 0, 0, 0, 1,   0, 32, 0 };
static const struct sty sty_autolink =	{ 0, 0, 0, 1,   0, 32, 0 };
static const struct sty sty_header =	{ 0, 0, 1, 0,   0,  0, 0 };

static const struct sty *stys[LOWDOWN__MAX] = {
	NULL, /* LOWDOWN_ROOT */
	NULL, /* LOWDOWN_BLOCKCODE */
	NULL, /* LOWDOWN_BLOCKQUOTE */
	NULL, /* LOWDOWN_DEFINITION */
	NULL, /* LOWDOWN_DEFINITION_TITLE */
	NULL, /* LOWDOWN_DEFINITION_DATA */
	&sty_header, /* LOWDOWN_HEADER */
	&sty_hrule, /* LOWDOWN_HRULE */
	NULL, /* LOWDOWN_LIST */
	NULL, /* LOWDOWN_LISTITEM */
	NULL, /* LOWDOWN_PARAGRAPH */
	NULL, /* LOWDOWN_TABLE_BLOCK */
	NULL, /* LOWDOWN_TABLE_HEADER */
	NULL, /* LOWDOWN_TABLE_BODY */
	NULL, /* LOWDOWN_TABLE_ROW */
	NULL, /* LOWDOWN_TABLE_CELL */
	NULL, /* LOWDOWN_FOOTNOTES_BLOCK */
	NULL, /* LOWDOWN_FOOTNOTE_DEF */
	&sty_blockhtml, /* LOWDOWN_BLOCKHTML */
	&sty_autolink, /* LOWDOWN_LINK_AUTO */
	&sty_codespan, /* LOWDOWN_CODESPAN */
	&sty_d_emph, /* LOWDOWN_DOUBLE_EMPHASIS */
	&sty_emph, /* LOWDOWN_EMPHASIS */
	&sty_highlight, /* LOWDOWN_HIGHLIGHT */
	&sty_image, /* LOWDOWN_IMAGE */
	NULL, /* LOWDOWN_LINEBREAK */
	&sty_link, /* LOWDOWN_LINK */
	&sty_t_emph, /* LOWDOWN_TRIPLE_EMPHASIS */
	&sty_strike, /* LOWDOWN_STRIKETHROUGH */
	NULL, /* LOWDOWN_SUPERSCRIPT */
	&sty_foot_ref, /* LOWDOWN_FOOTNOTE_REF */
	NULL, /* LOWDOWN_MATH_BLOCK */
	&sty_rawhtml, /* LOWDOWN_RAW_HTML */
	NULL, /* LOWDOWN_ENTITY */
	NULL, /* LOWDOWN_NORMAL_TEXT */
	NULL, /* LOWDOWN_DOC_HEADER */
	NULL, /* LOWDOWN_META */
	NULL /* LOWDOWN_DOC_FOOTER */
};

/* 
 * Special styles.
 * These are invoked in key places, below.
 */

static const struct sty sty_h1 = 	{ 0, 0, 0, 0, 104, 37, 0 };
static const struct sty sty_hn = 	{ 0, 0, 0, 0,   0, 36, 0 };
static const struct sty sty_linkalt =	{ 0, 0, 1, 0,   0, 92, 1|2 };
static const struct sty sty_imgurl = 	{ 0, 0, 0, 1,   0, 32, 2 };
static const struct sty sty_imgurlbox =	{ 0, 0, 0, 0,   0, 37, 2 };
static const struct sty sty_foots_div =	{ 0, 0, 0, 0,   0, 37, 0 };
static const struct sty sty_meta_key =	{ 0, 0, 0, 0,   0, 37, 0 };
static const struct sty sty_bad_ent = 	{ 0, 0, 0, 0,   0, 37, 0 };
static const struct sty sty_chng_ins =	{ 0, 0, 0, 0,  47, 30, 0 };
static const struct sty sty_chng_del =	{ 0, 0, 0, 0, 100,  0, 0 };

/*
 * Prefix styles.
 * These are applied to block-level prefix material.
 */

static const struct sty sty_ddata_pfx =	{ 0, 0, 0, 0,   0, 93, 0 };
static const struct sty sty_fdef_pfx =	{ 0, 0, 0, 0,   0, 92, 1 };
static const struct sty sty_bkqt_pfx =	{ 0, 0, 0, 0,   0, 37, 0 };
static const struct sty sty_oli_pfx =	{ 0, 0, 0, 0,   0, 93, 0 };
static const struct sty sty_uli_pfx =	{ 0, 0, 0, 0,   0, 93, 0 };

/*
 * Whether the style is not empty (i.e., has style attributes).
 */
#define	STY_NONEMPTY(_s) \
	((_s)->colour || (_s)->bold || (_s)->italic || \
	 (_s)->under || (_s)->strike || (_s)->bcolour || \
	 (_s)->override)

/* Forward declaration. */

static int
rndr(struct lowdown_buf *, struct lowdown_metaq *, 
	struct term *, const struct lowdown_node *);

/*
 * Get the column width of a multi-byte sequence.
 * If the sequence is bad, return the number of raw bytes to print.
 * Return <0 on failure (memory), >=0 otherwise.
 */
static ssize_t
rndr_mbswidth(struct term *term, const char *buf, size_t sz)
{
	size_t	 	 wsz, csz;
	const char	*cp;
	void		*pp;

	cp = buf;
	wsz = mbsnrtowcs(NULL, &cp, sz, 0, NULL);
	if (wsz == (size_t)-1)
		return sz;

	if (term->bufsz < wsz) {
		term->bufsz = wsz;
		pp = reallocarray(term->buf, wsz, sizeof(wchar_t));
		if (pp == NULL)
			return -1;
		term->buf = pp;
	}

	cp = buf;
	mbsnrtowcs(term->buf, &cp, sz, wsz, NULL);
	csz = wcswidth(term->buf, wsz);
	return csz == (size_t)-1 ? sz : csz;
}

/*
 * Copy the buffer into "out", escaping along the width.
 * Returns the number of actual printed columns, which in the case of
 * multi-byte glyphs, may be less than the given bytes.
 * Return <0 on failure (memory), >= 0 otherwise.
 */
static ssize_t
rndr_escape(struct term *term, struct lowdown_buf *out,
	const char *buf, size_t sz)
{
	size_t	 i, start = 0, cols = 0;
	ssize_t	 ret;

	/* Don't allow control characters through. */

	for (i = 0; i < sz; i++)
		if (iscntrl((unsigned char)buf[i])) {
			ret = rndr_mbswidth
				(term, buf + start, i - start);
			if (ret < 0)
				return -1;
			cols += ret;
			if (!hbuf_put(out, buf + start, i - start))
				return -1;
			start = i + 1;
		}

	/* Remaining bytes. */

	if (start < sz) {
		ret = rndr_mbswidth(term, buf + start, sz - start);
		if (ret < 0)
			return -1;
		cols += ret;
		if (!hbuf_put(out, buf + start, sz - start))
			return -1;
	}

	return cols;
}

/*
 * Output style "s" into "out" as an ANSI escape.
 * If "s" does not have any style information, output nothing.
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_buf_style(struct lowdown_buf *out, const struct sty *s)
{
	int	has = 0;

	if (!STY_NONEMPTY(s))
		return 1;

	if (!HBUF_PUTSL(out, "\033["))
		return 0;
	if (s->bold) {
		if (!HBUF_PUTSL(out, "1"))
			return 0;
		has++;
	}
	if (s->under) {
		if (has++ && !HBUF_PUTSL(out, ";"))
			return 0;
		if (!HBUF_PUTSL(out, "4"))
			return 0;
	}
	if (s->italic) {
		if (has++ && !HBUF_PUTSL(out, ";"))
			return 0;
		if (!HBUF_PUTSL(out, "3"))
			return 0;
	}
	if (s->strike) {
		if (has++ && !HBUF_PUTSL(out, ";"))
			return 0;
		if (!HBUF_PUTSL(out, "9"))
			return 0;
	}
	if (s->bcolour) {
		if (has++ && !HBUF_PUTSL(out, ";"))
			return 0;
		if (!hbuf_printf(out, "%zu", s->bcolour))
			return 0;
	}
	if (s->colour) {
		if (has++ && !HBUF_PUTSL(out, ";"))
			return 0;
		if (!hbuf_printf(out, "%zu", s->colour))
			return 0;
	}
	return HBUF_PUTSL(out, "m");
}

/*
 * Take the given style "from" and apply it to "to".
 * This accumulates styles: unless an override has been set, it adds to
 * the existing style in "to" instead of overriding it.
 * The one exception is TODO colours, which override each other.
 */
static void
rndr_node_style_apply(struct sty *to, const struct sty *from)
{

	if (from->italic)
		to->italic = 1;
	if (from->strike)
		to->strike = 1;
	if (from->bold)
		to->bold = 1;
	else if ((from->override & OSTY_BOLD))
		to->bold = 0;
	if (from->under)
		to->under = 1;
	else if ((from->override & OSTY_ITALIC))
		to->under = 0;
	if (from->bcolour)
		to->bcolour = from->bcolour;
	if (from->colour)
		to->colour = from->colour;
}

/*
 * Apply the style for only the given node to the current style.
 * This *augments* the current style: see rndr_node_style_apply().
 * (This does not ascend to the parent node.)
 */
static void
rndr_node_style(struct sty *s, const struct lowdown_node *n)
{

	/* The basic node itself. */

	if (stys[n->type] != NULL)
		rndr_node_style_apply(s, stys[n->type]);

	/* Any special node situation that overrides. */

	switch (n->type) {
	case LOWDOWN_HEADER:
		if (n->rndr_header.level > 0)
			rndr_node_style_apply(s, &sty_hn);
		else
			rndr_node_style_apply(s, &sty_h1);
		break;
	default:
		/* FIXME: crawl up nested? */
		if (n->parent != NULL && 
		    n->parent->type == LOWDOWN_LINK)
			rndr_node_style_apply(s, &sty_linkalt);
		break;
	}

	if (n->chng == LOWDOWN_CHNG_INSERT) 
		rndr_node_style_apply(s, &sty_chng_ins);
	if (n->chng == LOWDOWN_CHNG_DELETE) 
		rndr_node_style_apply(s, &sty_chng_del);
}

/*
 * Bookkeep that we've put "len" characters into the current line.
 */
static void
rndr_buf_advance(struct term *term, size_t len)
{

	term->col += len;
	if (term->col && term->last_blank != 0)
		term->last_blank = 0;
}

/*
 * Return non-zero if "n" or any of its ancestors require resetting the
 * output line mode, otherwise return zero.
 * This applies to both block and inline styles.
 */
static int
rndr_buf_endstyle(const struct lowdown_node *n)
{
	struct sty	s;

	if (n->parent != NULL)
		if (rndr_buf_endstyle(n->parent))
			return 1;

	memset(&s, 0, sizeof(struct sty));
	rndr_node_style(&s, n);
	return STY_NONEMPTY(&s);
}

/*
 * Unsets the current style context given "n" and an optional terminal
 * style "osty", if applies.
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_buf_endwords(struct term *term, struct lowdown_buf *out,
	const struct lowdown_node *n, const struct sty *osty)
{

	if (rndr_buf_endstyle(n) ||
	    (osty != NULL && STY_NONEMPTY(osty)))
		return HBUF_PUTSL(out, "\033[0m");

	return 1;
}

/*
 * Like rndr_buf_endwords(), but also terminating the line itself.
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_buf_endline(struct term *term, struct lowdown_buf *out,
	const struct lowdown_node *n, const struct sty *osty)
{

	if (!rndr_buf_endwords(term, out, n, osty))
		return 0;

	/* 
	 * We can legit be at col == 0 if, for example, we're in a
	 * literal context with a blank line.
	 * assert(term->col > 0);
	 * assert(term->last_blank == 0);
	 */

	term->col = 0;
	term->last_blank = 1;
	return HBUF_PUTSL(out, "\n");
}

/*
 * Output optional number of newlines before or after content.
 * Return zero on failure, non-zero on success.
 */
static int
rndr_buf_vspace(struct term *term, struct lowdown_buf *out,
	const struct lowdown_node *n, size_t sz)
{

	if (term->last_blank == -1)
		return 1;
	while ((size_t)term->last_blank < sz) {
		if (!HBUF_PUTSL(out, "\n"))
			return 0;
		term->last_blank++;
	}
	term->col = 0;
	return 1;
}

/*
 * Output prefixes of the given node in the style further accumulated
 * from the parent nodes.
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_buf_startline_prefixes(struct term *term,
	struct sty *s, const struct lowdown_node *n,
	struct lowdown_buf *out)
{
	size_t	 			 i, emit;
	int	 		 	 pstyle = 0;
	struct sty			 sinner;
	const struct lowdown_node	*np;

	if (n->parent != NULL &&
	    !rndr_buf_startline_prefixes(term, s, n->parent, out))
		return 0;

	/*
	 * The "sinner" value is temporary for only this function.
	 * This allows us to set a temporary style mask that only
	 * applies to the prefix data.
	 * Otherwise "s" propogates to the subsequent line.
	 */

	rndr_node_style(s, n);
	sinner = *s;

	/*
	 * Look up the current node in the list of node's we're
	 * servicing so we can get how many times we've output the
	 * prefix.
	 * This is used for (e.g.) lists, where we only output the list
	 * prefix once.
	 * FIXME: read backwards for faster perf.
	 */

	for (i = 0; i <= term->stackpos; i++)
		if (term->stack[i].n == n)
			break;
	assert(i <= term->stackpos);
	emit = term->stack[i].lines++;

	/*
	 * Output any prefixes.
	 * Any output must have rndr_buf_style() and set pstyle so that
	 * we close out the style afterward.
	 */

	switch (n->type) {
	case LOWDOWN_TABLE_BLOCK:
	case LOWDOWN_PARAGRAPH:
		/*
		 * Collapse leading white-space if we're already within
		 * a margin-bearing block statement.
		 */

		for (np = n->parent; np != NULL; np = np->parent)
			if (np->type == LOWDOWN_LISTITEM ||
			    np->type == LOWDOWN_BLOCKQUOTE ||
			    np->type == LOWDOWN_FOOTNOTE_DEF)
				break;
		if (np == NULL) {
			if (!HBUF_PUTSL(out, "    "))
				return 0;
			rndr_buf_advance(term, 4);
		}
		break;
	case LOWDOWN_BLOCKCODE:
		if (!rndr_buf_style(out, &sinner))
			return 0;
		pstyle = 1;
		if (!HBUF_PUTSL(out, "      "))
			return 0;
		rndr_buf_advance(term, 6);
		break;
	case LOWDOWN_ROOT:
		if (!rndr_buf_style(out, &sinner))
			return 0;
		pstyle = 1;
		for (i = 0; i < term->hmargin; i++)
			if (!HBUF_PUTSL(out, " "))
				return 0;
		break;
	case LOWDOWN_BLOCKQUOTE:
		rndr_node_style_apply(&sinner, &sty_bkqt_pfx);
		if (!rndr_buf_style(out, &sinner))
			return 0;
		pstyle = 1;
		if (!HBUF_PUTSL(out, "  | "))
			return 0;
		rndr_buf_advance(term, 4);
		break;
	case LOWDOWN_DEFINITION_DATA:
		rndr_node_style_apply(&sinner, &sty_ddata_pfx);
		if (!rndr_buf_style(out, &sinner))
			return 0;
		pstyle = 1;
		if (emit == 0 && !HBUF_PUTSL(out, "  : "))
			return 0;
		else if (emit != 0 && !HBUF_PUTSL(out, "    "))
			return 0;
		rndr_buf_advance(term, 4);
		break;
	case LOWDOWN_FOOTNOTE_DEF:
		rndr_node_style_apply(&sinner, &sty_fdef_pfx);
		if (!rndr_buf_style(out, &sinner))
			return 0;
		pstyle = 1;
		if (emit == 0 && !hbuf_printf
		    (out, "%2zu. ", n->rndr_footnote_def.num))
			return 0;
		else if (emit != 0 && !HBUF_PUTSL(out, "    "))
			return 0;
		rndr_buf_advance(term, 4);
		break;
	case LOWDOWN_HEADER:
		/* Use the same colour as the text following. */

		if (n->rndr_header.level == 0)
			break;
		if (!rndr_buf_style(out, &sinner))
			return 0;
		pstyle = 1;
		for (i = 0; i < n->rndr_header.level + 1; i++)
			if (!HBUF_PUTSL(out, "#"))
				return 0;
		if (!HBUF_PUTSL(out, " "))
			return 0;
		rndr_buf_advance(term, i + 1);
		break;
	case LOWDOWN_LISTITEM:
		if (n->parent == NULL ||
		    n->parent->type == LOWDOWN_DEFINITION_DATA)
			break;
		if (n->parent->type == LOWDOWN_LIST &&
		    (n->parent->rndr_list.flags & HLIST_FL_ORDERED))
			rndr_node_style_apply(&sinner, &sty_oli_pfx);
		else
			rndr_node_style_apply(&sinner, &sty_uli_pfx);
		if (!rndr_buf_style(out, &sinner))
			return 0;
		pstyle = 1;
		if (n->parent->rndr_list.flags & HLIST_FL_UNORDERED) {
			if (!hbuf_puts
			    (out, emit == 0 ?  "  - " : "    "))
				return 0;
			rndr_buf_advance(term, 4);
			break;
		}
		if (emit == 0 && !hbuf_printf
		    (out, "%2zu. ", n->rndr_listitem.num))
			return 0;
		else if (emit != 0 && !HBUF_PUTSL(out, "    "))
			return 0;
		rndr_buf_advance(term, 4);
		break;
	default:
		break;
	}

	if (pstyle && STY_NONEMPTY(&sinner))
		if (!HBUF_PUTSL(out, "\033[0m"))
			return 0;

	return 1;
}

/*
 * Like rndr_buf_startwords(), but at the start of a line.
 * This also outputs all line prefixes of the block context.
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_buf_startline(struct term *term, struct lowdown_buf *out, 
	const struct lowdown_node *n, const struct sty *osty)
{
	struct sty	 s;

	assert(term->last_blank);
	assert(term->col == 0);

	memset(&s, 0, sizeof(struct sty));
	if (!rndr_buf_startline_prefixes(term, &s, n, out))
		return 0;
	if (osty != NULL)
		rndr_node_style_apply(&s, osty);
	return rndr_buf_style(out, &s);
}

/*
 * Ascend to the root of the parse tree from rndr_buf_startwords(),
 * accumulating styles as we do so.
 */
static void
rndr_buf_startwords_style(const struct lowdown_node *n, struct sty *s)
{

	if (n->parent != NULL)
		rndr_buf_startwords_style(n->parent, s);
	rndr_node_style(s, n);
}

/*
 * Accumulate and output the style at the start of one or more words.
 * Should *not* be called on the start of a new line, which calls for
 * rndr_buf_startline().
 * Return zero on failure, non-zero on success.
 */
static int
rndr_buf_startwords(struct term *term, struct lowdown_buf *out,
	const struct lowdown_node *n, const struct sty *osty)
{
	struct sty	 s;

	assert(!term->last_blank);
	assert(term->col > 0);

	memset(&s, 0, sizeof(struct sty));
	rndr_buf_startwords_style(n, &s);
	if (osty != NULL)
		rndr_node_style_apply(&s, osty);
	return rndr_buf_style(out, &s);
}

/*
 * Return zero on failure, non-zero on success.
 */
static int
rndr_buf_literal(struct term *term, struct lowdown_buf *out, 
	const struct lowdown_node *n, const struct lowdown_buf *in,
	const struct sty *osty)
{
	size_t		 i = 0, len;
	const char	*start;

	while (i < in->size) {
		start = &in->data[i];
		while (i < in->size && in->data[i] != '\n')
			i++;
		len = &in->data[i] - start;
		i++;
		if (!rndr_buf_startline(term, out, n, osty))
			return 0;

		/* 
		 * No need to record the column width here because we're
		 * going to reset to zero anyway.
		 */

		if (rndr_escape(term, out, start, len) < 0)
			return 0;
		rndr_buf_advance(term, len);
		if (!rndr_buf_endline(term, out, n, osty))
			return 0;
	}

	return 1;
}

/*
 * Emit text in "in" the current line with output "out".
 * Use "n" and its ancestry to determine our context.
 * Return zero on failure, non-zero on success.
 */
static int
rndr_buf(struct term *term, struct lowdown_buf *out, 
	const struct lowdown_node *n, const struct lowdown_buf *in,
	const struct sty *osty)
{
	size_t				 i = 0, len, cols;
	ssize_t				 ret;
	int				 needspace, begin = 1, end = 0;
	const char			*start;
	const struct lowdown_node	*nn;

	for (nn = n; nn != NULL; nn = nn->parent)
		if (nn->type == LOWDOWN_BLOCKCODE ||
	  	    nn->type == LOWDOWN_BLOCKHTML)
			return rndr_buf_literal(term, out, n, in, osty);

	/* Start each word by seeing if it has leading space. */

	while (i < in->size) {
		needspace = isspace((unsigned char)in->data[i]);

		while (i < in->size && 
		       isspace((unsigned char)in->data[i]))
			i++;

		/* See how long it the coming word (may be 0). */

		start = &in->data[i];
		while (i < in->size &&
		       !isspace((unsigned char)in->data[i]))
			i++;
		len = &in->data[i] - start;

		/* 
		 * If we cross our maximum width and are preceded by a
		 * space, then break.
		 * (Leaving out the check for a space will cause
		 * adjacent text or punctuation to have a preceding
		 * newline.)
		 * This will also unset the current style.
		 */

		if ((needspace || 
	 	     (out->size && isspace
		      ((unsigned char)out->data[out->size - 1]))) &&
		    term->col && term->col + len > term->maxcol) {
			if (!rndr_buf_endline(term, out, n, osty))
				return 0;
			end = 0;
		}

		/*
		 * Either emit our new line prefix (only if we have a
		 * word that will follow!) or, if we need space, emit
		 * the spacing.  In the first case, or if we have
		 * following text and are starting this node, emit our
		 * current style.
		 */

		if (term->last_blank && len) {
			if (!rndr_buf_startline(term, out, n, osty))
				return 0;
			begin = 0;
			end = 1;
		} else if (!term->last_blank) {
			if (begin && len) {
				if (!rndr_buf_startwords
				    (term, out, n, osty))
					return 0;
				begin = 0;
				end = 1;
			}
			if (needspace) {
				if (!HBUF_PUTSL(out, " "))
					return 0;
				rndr_buf_advance(term, 1);
			}
		}

		/* Emit the word itself. */

		if ((ret = rndr_escape(term, out, start, len)) < 0)
			return 0;
		cols = ret;
		rndr_buf_advance(term, cols);
	}

	if (end) {
		assert(begin == 0);
		if (!rndr_buf_endwords(term, out, n, osty))
			return 0;
	}

	return 1;
}

/*
 * Output the unicode entry "val", which must be strictly greater than
 * zero, as a UTF-8 sequence.
 * This does no error checking.
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_entity(struct lowdown_buf *buf, int32_t val)
{

	assert(val > 0);

	if (val < 0x80)
		return hbuf_putc(buf, val);

       	if (val < 0x800)
		return hbuf_putc(buf, 192 + val / 64) &&
			hbuf_putc(buf, 128 + val % 64);

       	if (val - 0xd800u < 0x800) 
		return 1;

       	if (val < 0x10000)
		return hbuf_putc(buf, 224 + val / 4096) &&
			hbuf_putc(buf, 128 + val / 64 % 64) &&
			hbuf_putc(buf, 128 + val % 64);

       	if (val < 0x110000)
		return hbuf_putc(buf, 240 + val / 262144) &&
			hbuf_putc(buf, 128 + val / 4096 % 64) &&
			hbuf_putc(buf, 128 + val / 64 % 64) &&
			hbuf_putc(buf, 128 + val % 64);

	return 1;
}

/*
 * Adjust the stack of current nodes we're looking at.
 */
static int
rndr_stackpos_init(struct term *p, const struct lowdown_node *n)
{
	void	*pp;

	if (p->stackpos >= p->stackmax) {
		p->stackmax += 256;
		pp = reallocarray(p->stack,
			p->stackmax, sizeof(struct tstack));
		if (pp == NULL)
			return 0;
		p->stack = pp;
	}

	memset(&p->stack[p->stackpos], 0, sizeof(struct tstack));
	p->stack[p->stackpos].n = n;
	return 1;
}

/*
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_table(struct lowdown_buf *ob, struct lowdown_metaq *mq,
	struct term *p, const struct lowdown_node *n)
{
	size_t				*widths = NULL;
	const struct lowdown_node	*row, *top, *cell;
	struct lowdown_buf		*celltmp = NULL, 
					*rowtmp = NULL;
	size_t				 col, i, j, maxcol, sz;
	ssize_t			 	 last_blank;
	unsigned int			 flags;
	int				 rc = 0;

	assert(n->type == LOWDOWN_TABLE_BLOCK);

	widths = calloc(n->rndr_table.columns, sizeof(size_t));
	if (widths == NULL)
		goto out;

	if ((rowtmp = hbuf_new(128)) == NULL ||
	    (celltmp = hbuf_new(128)) == NULL)
		goto out;

	/*
	 * Begin by counting the number of printable columns in each
	 * column in each row.
	 */

	TAILQ_FOREACH(top, &n->children, entries) {
		assert(top->type == LOWDOWN_TABLE_HEADER ||
			top->type == LOWDOWN_TABLE_BODY);
		TAILQ_FOREACH(row, &top->children, entries)
			TAILQ_FOREACH(cell, &row->children, entries) {
				i = cell->rndr_table_cell.col;
				assert(i < n->rndr_table.columns);
				hbuf_truncate(celltmp);

				/* 
				 * Simulate that we're starting within
				 * the line by unsetting last_blank,
				 * having a non-zero column, and an
				 * infinite maximum column to prevent
				 * line wrapping.
				 */

				maxcol = p->maxcol;
				last_blank = p->last_blank;
				col = p->col;
				p->last_blank = 0;
				p->maxcol = SIZE_MAX;
				p->col = 1;
				if (!rndr(celltmp, mq, p, cell))
					goto out;
				if (widths[i] < p->col)
					widths[i] = p->col;
				p->last_blank = last_blank;
				p->col = col;
				p->maxcol = maxcol;
			}
	}

	/* Now actually print, row-by-row into the output. */

	TAILQ_FOREACH(top, &n->children, entries) {
		assert(top->type == LOWDOWN_TABLE_HEADER ||
			top->type == LOWDOWN_TABLE_BODY);
		TAILQ_FOREACH(row, &top->children, entries) {
			hbuf_truncate(rowtmp);
			TAILQ_FOREACH(cell, &row->children, entries) {
				i = cell->rndr_table_cell.col;
				hbuf_truncate(celltmp);
				maxcol = p->maxcol;
				last_blank = p->last_blank;
				col = p->col;
				p->last_blank = 0;
				p->maxcol = SIZE_MAX;
				p->col = 1;
				if (!rndr(celltmp, mq, p, cell))
					goto out;
				assert(widths[i] >= p->col);
				sz = widths[i] - p->col;

				/* 
				 * Alignment is either beginning,
				 * ending, or splitting the remaining
				 * spaces around the word.
				 * Be careful about uneven splitting in
				 * the case of centre.
				 */

				flags = cell->rndr_table_cell.flags & 
					HTBL_FL_ALIGNMASK;
				if (flags == HTBL_FL_ALIGN_RIGHT)
					for (j = 0; j < sz; j++)
						if (!HBUF_PUTSL(rowtmp, " "))
							goto out;
				if (flags == HTBL_FL_ALIGN_CENTER)
					for (j = 0; j < sz / 2; j++)
						if (!HBUF_PUTSL(rowtmp, " "))
							goto out;
				if (!hbuf_putb(rowtmp, celltmp))
					goto out;
				if (flags == 0 ||
				    flags == HTBL_FL_ALIGN_LEFT)
					for (j = 0; j < sz; j++)
						if (!HBUF_PUTSL(rowtmp, " "))
							goto out;
				if (flags == HTBL_FL_ALIGN_CENTER) {
					sz = (sz % 2) ? 
						(sz / 2) + 1 : (sz / 2);
					for (j = 0; j < sz; j++)
						if (!HBUF_PUTSL(rowtmp, " "))
							goto out;
				}

				p->last_blank = last_blank;
				p->col = col;
				p->maxcol = maxcol;
				if (TAILQ_NEXT(cell, entries) != NULL &&
				    !HBUF_PUTSL(rowtmp, " | "))
					goto out;
			}

			/* 
			 * Some magic here.
			 * First, emulate rndr() by setting the
			 * stackpos to the table, which is required for
			 * checking the line start.
			 * Then directly print, as we've already escaped
			 * all characters, and have embedded escapes of
			 * our own.  Then end the line.
			 */

			p->stackpos++;
			if (!rndr_stackpos_init(p, n))
				goto out;
			if (!rndr_buf_startline(p, ob, n, NULL))
				goto out;
			if (!hbuf_putb(ob, rowtmp))
				goto out;
			rndr_buf_advance(p, 1);
			if (!rndr_buf_endline(p, ob, n, NULL))
				goto out;
			if (!rndr_buf_vspace(p, ob, n, 1))
				goto out;
			p->stackpos--;
		}

		if (top->type == LOWDOWN_TABLE_HEADER) {
			p->stackpos++;
			if (!rndr_stackpos_init(p, n))
				goto out;
			if (!rndr_buf_startline(p, ob, n, NULL))
				goto out;
			for (i = 0; i < n->rndr_table.columns; i++) {
				for (j = 0; j < widths[i]; j++)
					if (!HBUF_PUTSL(ob, "-"))
						goto out;
				if (i < n->rndr_table.columns - 1 &&
				    !HBUF_PUTSL(ob, "|-"))
					goto out;
			}
			rndr_buf_advance(p, 1);
			if (!rndr_buf_endline(p, ob, n, NULL))
				goto out;
			if (!rndr_buf_vspace(p, ob, n, 1))
				goto out;
			p->stackpos--;
		}
	}

	rc = 1;
out:
	hbuf_free(celltmp);
	hbuf_free(rowtmp);
	free(widths);
	return rc;
}

static int
rndr(struct lowdown_buf *ob, struct lowdown_metaq *mq,
	struct term *p, const struct lowdown_node *n)
{
	const struct lowdown_node	*child, *prev;
	struct lowdown_meta		*m;
	struct lowdown_buf		*metatmp;
	int32_t				 entity;
	size_t				 i, col;
	ssize_t			 	 last_blank;
	int				 rc;
	
	/* Current nodes we're servicing. */

	if (!rndr_stackpos_init(p, n))
		return 0;

	prev = n->parent == NULL ? NULL :
		TAILQ_PREV(n, lowdown_nodeq, entries);

	/* Vertical space before content. */

	rc = 1;
	switch (n->type) {
	case LOWDOWN_ROOT:

		/* Emit vmargin. */

		for (i = 0; i < p->vmargin; i++)
			if (!HBUF_PUTSL(ob, "\n"))
				return 0;
		p->last_blank = -1;
		break;
	case LOWDOWN_BLOCKCODE:
	case LOWDOWN_BLOCKHTML:
	case LOWDOWN_BLOCKQUOTE:
	case LOWDOWN_DEFINITION:
	case LOWDOWN_FOOTNOTES_BLOCK:
	case LOWDOWN_FOOTNOTE_DEF:
	case LOWDOWN_HEADER:
	case LOWDOWN_LIST:
	case LOWDOWN_TABLE_BLOCK:
	case LOWDOWN_PARAGRAPH:
		/*
		 * Blocks in a definition list get special treatment
		 * because we only put one newline between the title and
		 * the data regardless of its contents.
		 */

		if (n->parent != NULL && 
		    n->parent->type == LOWDOWN_LISTITEM &&
		    n->parent->parent != NULL &&
		    n->parent->parent->type == 
		      LOWDOWN_DEFINITION_DATA &&
		    prev == NULL)
			rc = rndr_buf_vspace(p, ob, n, 1);
		else
			rc = rndr_buf_vspace(p, ob, n, 2);
		break;
	case LOWDOWN_MATH_BLOCK:
		if (n->rndr_math.blockmode)
			rc = rndr_buf_vspace(p, ob, n, 1);
		break;
	case LOWDOWN_DEFINITION_DATA:
		/* Vertical space if previous block-mode data. */

		if (n->parent != NULL &&
		    n->parent->type == LOWDOWN_DEFINITION &&
		    (n->parent->rndr_definition.flags &
		     HLIST_FL_BLOCK) &&
		    prev != NULL &&
		    prev->type == LOWDOWN_DEFINITION_DATA)
			rc = rndr_buf_vspace(p, ob, n, 2);
		else
			rc = rndr_buf_vspace(p, ob, n, 1);
		break;
	case LOWDOWN_DEFINITION_TITLE:
	case LOWDOWN_HRULE:
	case LOWDOWN_LINEBREAK:
	case LOWDOWN_LISTITEM:
	case LOWDOWN_META:
		rc = rndr_buf_vspace(p, ob, n, 1);
		break;
	default:
		break;
	}
	if (!rc)
		return 0;

	/* Output leading content. */

	switch (n->type) {
	case LOWDOWN_FOOTNOTES_BLOCK:
		hbuf_truncate(p->tmp);
		if (!HBUF_PUTSL(p->tmp, "~~~~~~~~"))
			return 0;
		if (!rndr_buf(p, ob, n, p->tmp, &sty_foots_div))
			return 0;
		break;
	case LOWDOWN_SUPERSCRIPT:
		hbuf_truncate(p->tmp);
		if (!HBUF_PUTSL(p->tmp, "^"))
			return 0;
		if (!rndr_buf(p, ob, n, p->tmp, NULL))
			return 0;
		break;
	case LOWDOWN_META:
		if (!rndr_buf(p, ob, n, 
		    &n->rndr_meta.key, &sty_meta_key))
			return 0;
		hbuf_truncate(p->tmp);
		if (!HBUF_PUTSL(p->tmp, ": "))
			return 0;
		if (!rndr_buf(p, ob, n, p->tmp, &sty_meta_key))
			return 0;
		if (mq == NULL)
			break;

		/*
		 * Manually render the children of the meta into a
		 * buffer and use that as our value.  Start by zeroing
		 * our terminal position and using another output buffer
		 * (p->tmp would be clobbered by children).
		 */

		last_blank = p->last_blank;
		p->last_blank = -1;
		col = p->col;
		p->col = 0;
		m = calloc(1, sizeof(struct lowdown_meta));
		if (m == NULL)
			return 0;
		TAILQ_INSERT_TAIL(mq, m, entries);
		m->key = strndup(n->rndr_meta.key.data,
			n->rndr_meta.key.size);
		if (m->key == NULL)
			return 0;
		if ((metatmp = hbuf_new(128)) == NULL)
			return 0;
		TAILQ_FOREACH(child, &n->children, entries) {
			p->stackpos++;
			if (!rndr(metatmp, mq, p, child)) {
				hbuf_free(metatmp);
				return 0;
			}
			p->stackpos--;
		}
		m->value = strndup(metatmp->data, metatmp->size);
		hbuf_free(metatmp);
		if (m->value == NULL)
			return 0;
		p->last_blank = last_blank;
		p->col = col;
		break;
	default:
		break;
	}

	/* Descend into children. */

	if (n->type != LOWDOWN_TABLE_BLOCK) {
		TAILQ_FOREACH(child, &n->children, entries) {
			p->stackpos++;
			if (!rndr(ob, mq, p, child))
				return 0;
			p->stackpos--;
		}
	} else if (!rndr_table(ob, mq, p, n))
		return 0;

	/* Output content. */

	switch (n->type) {
	case LOWDOWN_HRULE:
		hbuf_truncate(p->tmp);
		if (!HBUF_PUTSL(p->tmp, "~~~~~~~~"))
			return 0;
		rc = rndr_buf(p, ob, n, p->tmp, NULL);
		break;
	case LOWDOWN_FOOTNOTE_REF:
		hbuf_truncate(p->tmp);
		if (!hbuf_printf(p->tmp, "[%zu]", 
		    n->rndr_footnote_ref.num))
			return 0;
		rc = rndr_buf(p, ob, n, p->tmp, NULL);
		break;
	case LOWDOWN_RAW_HTML:
		rc = rndr_buf(p, ob, n, &n->rndr_raw_html.text, NULL);
		break;
	case LOWDOWN_MATH_BLOCK:
		rc = rndr_buf(p, ob, n, &n->rndr_math.text, NULL);
		break;
	case LOWDOWN_ENTITY:
		entity = entity_find_iso(&n->rndr_entity.text);
		if (entity > 0) {
			hbuf_truncate(p->tmp);
			if (!rndr_entity(p->tmp, entity))
				return 0;
			rc = rndr_buf(p, ob, n, p->tmp, NULL);
		} else
			rc = rndr_buf(p, ob, n, &n->rndr_entity.text,
				&sty_bad_ent);
		break;
	case LOWDOWN_BLOCKCODE:
		rc = rndr_buf(p, ob, n, &n->rndr_blockcode.text, NULL);
		break;
	case LOWDOWN_BLOCKHTML:
		rc = rndr_buf(p, ob, n, &n->rndr_blockhtml.text, NULL);
		break;
	case LOWDOWN_CODESPAN:
		rc = rndr_buf(p, ob, n, &n->rndr_codespan.text, NULL);
		break;
	case LOWDOWN_LINK_AUTO:
		if (p->opts & LOWDOWN_TERM_SHORTLINK) {
			hbuf_truncate(p->tmp);
			if (!hbuf_shortlink
			    (p->tmp, &n->rndr_autolink.link))
				return 0;
			rc = rndr_buf(p, ob, n, p->tmp, NULL);
		} else 
			rc = rndr_buf(p, ob, n, &n->rndr_autolink.link, NULL);
		break;
	case LOWDOWN_LINK:
		if (p->opts & LOWDOWN_TERM_NOLINK)
			break;
		hbuf_truncate(p->tmp);
		if (!HBUF_PUTSL(p->tmp, " "))
			return 0;
		if (!rndr_buf(p, ob, n, p->tmp, NULL))
			return 0;
		if (p->opts & LOWDOWN_TERM_SHORTLINK) {
			hbuf_truncate(p->tmp);
			if (!hbuf_shortlink
			    (p->tmp, &n->rndr_link.link))
				return 0;
			rc = rndr_buf(p, ob, n, p->tmp, NULL);
		} else 
			rc = rndr_buf(p, ob, n, &n->rndr_link.link, NULL);
		break;
	case LOWDOWN_IMAGE:
		if (!rndr_buf(p, ob, n, &n->rndr_image.alt, NULL))
			return 0;
		if (n->rndr_image.alt.size) {
			hbuf_truncate(p->tmp);
			if (!HBUF_PUTSL(p->tmp, " "))
				return 0;
			if (!rndr_buf(p, ob, n, p->tmp, NULL))
				return 0;
		}
		if (p->opts & LOWDOWN_TERM_NOLINK) {
			hbuf_truncate(p->tmp);
			if (!HBUF_PUTSL(p->tmp, "[Image]"))
				return 0;
			rc = rndr_buf(p, ob, n, p->tmp, &sty_imgurlbox);
			break;
		}
		hbuf_truncate(p->tmp);
		if (!HBUF_PUTSL(p->tmp, "[Image: "))
			return 0;
		if (!rndr_buf(p, ob, n, p->tmp, &sty_imgurlbox))
			return 0;
		if (p->opts & LOWDOWN_TERM_SHORTLINK) {
			hbuf_truncate(p->tmp);
			if (!hbuf_shortlink
			    (p->tmp, &n->rndr_image.link))
				return 0;
			if (!rndr_buf(p, ob, n, p->tmp, &sty_imgurl))
				return 0;
		} else
			if (!rndr_buf(p, ob, n, 
			    &n->rndr_image.link, &sty_imgurl))
				return 0;
		hbuf_truncate(p->tmp);
		if (!HBUF_PUTSL(p->tmp, "]"))
			return 0;
		rc = rndr_buf(p, ob, n, p->tmp, &sty_imgurlbox);
		break;
	case LOWDOWN_NORMAL_TEXT:
		rc = rndr_buf(p, ob, n, &n->rndr_normal_text.text, NULL);
		break;
	default:
		break;
	}
	if (!rc)
		return 0;

	/* Trailing block spaces. */

	rc = 1;
	switch (n->type) {
	case LOWDOWN_BLOCKCODE:
	case LOWDOWN_BLOCKHTML:
	case LOWDOWN_BLOCKQUOTE:
	case LOWDOWN_DEFINITION:
	case LOWDOWN_FOOTNOTES_BLOCK:
	case LOWDOWN_FOOTNOTE_DEF:
	case LOWDOWN_HEADER:
	case LOWDOWN_LIST:
	case LOWDOWN_PARAGRAPH:
	case LOWDOWN_TABLE_BLOCK:
		rc = rndr_buf_vspace(p, ob, n, 2);
		break;
	case LOWDOWN_MATH_BLOCK:
		if (n->rndr_math.blockmode)
			rc = rndr_buf_vspace(p, ob, n, 1);
		break;
	case LOWDOWN_DOC_HEADER:
		if (!TAILQ_EMPTY(&n->children))
			rc = rndr_buf_vspace(p, ob, n, 2);
		break;
	case LOWDOWN_DEFINITION_DATA:
	case LOWDOWN_DEFINITION_TITLE:
	case LOWDOWN_HRULE:
	case LOWDOWN_LISTITEM:
	case LOWDOWN_META:
		rc = rndr_buf_vspace(p, ob, n, 1);
		break;
	case LOWDOWN_ROOT:
		if (!rndr_buf_vspace(p, ob, n, 1))
			return 0;
		while (ob->size && ob->data[ob->size - 1] == '\n')
			ob->size--;
		if (!HBUF_PUTSL(ob, "\n"))
			return 0;

		/* Strip breaks but for the vmargin. */

		for (i = 0; i < p->vmargin; i++)
			if (!HBUF_PUTSL(ob, "\n"))
				return 0;
		break;
	default:
		break;
	}

	return rc;
}

int
lowdown_term_rndr(struct lowdown_buf *ob,
	struct lowdown_metaq *mq, void *arg, 
	const struct lowdown_node *n)
{
	struct term	*p = arg;

	/* Reset ourselves to a sane parse point. */

	p->stackpos = 0;

	return rndr(ob, mq, p, n);
}

void *
lowdown_term_new(const struct lowdown_opts *opts)
{
	struct term	*p;

	if ((p = calloc(1, sizeof(struct term))) == NULL)
		return NULL;

	/* Give us 80 columns by default. */

	if (opts != NULL) {
		p->maxcol = opts->cols == 0 ? 80 : opts->cols;
		p->hmargin = opts->hmargin;
		p->vmargin = opts->vmargin;
		p->opts = opts->oflags;
	} else
		p->maxcol = 80;

	if ((p->tmp = hbuf_new(32)) == NULL) {
		free(p);
		return NULL;
	}
	return p;
}

void
lowdown_term_free(void *arg)
{
	struct term	*p = arg;
	
	if (p == NULL)
		return;

	hbuf_free(p->tmp);
	free(p->buf);
	free(p->stack);
	free(p);
}
