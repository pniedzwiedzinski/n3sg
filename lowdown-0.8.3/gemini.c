/*	$Id: gemini.c,v 1.11 2021/02/18 22:37:50 kristaps Exp $ */
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

/*
 * A standalone link is one that lives in its own paragraph.
 */
#define	IS_STANDALONE_LINK(_n, _prev) \
	((_n)->parent != NULL && \
	 (_n)->parent->type == LOWDOWN_PARAGRAPH && \
	 (_n)->parent->parent != NULL && \
	 (_n)->parent->parent->type == LOWDOWN_ROOT && \
	 (_prev) == NULL && \
	 TAILQ_NEXT((_n), entries) == NULL)

/*
 * A link queued for display.
 * This only happens when using footnote or endnote links.
 */
struct link {
	const struct lowdown_node	*n; /* node needing link */
	size_t				 id; /* link-%zu */
	TAILQ_ENTRY(link)		 entries;
};

TAILQ_HEAD(linkq, link);

struct gemini {
	unsigned int		 flags; /* output flags */
	ssize_t			 last_blank; /* line breaks or -1 (start) */
	struct lowdown_buf	*tmp; /* for temporary allocations */
	struct linkq		 linkq; /* link queue */
	size_t			 linkqsz; /* position in link queue */
	wchar_t			*buf; /* buffer for counting wchar */
	size_t			 bufsz; /* size of buf */
};

/*
 * Forward declaration.
 */
static int
rndr(struct lowdown_buf *, struct lowdown_metaq *, 
	struct gemini *, const struct lowdown_node *);

static void
link_freeq(struct linkq *q)
{
	struct link	*l;

	while ((l = TAILQ_FIRST(q)) != NULL) {
		TAILQ_REMOVE(q, l, entries);
		free(l);
	}
}

static int
rndr_link_ref(const struct gemini *st,
	struct lowdown_buf *out, const struct link *l, int nl)
{
	char		 buf[32], c;
	size_t		 ref = l->id, sz = 0, i;

	assert(ref);

	if (st->flags & LOWDOWN_GEMINI_LINK_NOREF)
		return hbuf_printf(out, "%s", nl ? "\n" : "");

	buf[0] = '\0';
	if (st->flags & LOWDOWN_GEMINI_LINK_ROMAN) {
		while(ref)
			if (ref >= 1000) {
				strlcat(buf, "m", sizeof(buf));
				ref -= 1000;
			} else if (ref >= 900) {
				strlcat(buf, "cm", sizeof(buf));
				ref -= 900;
			} else if (ref >= 500) {
				strlcat(buf, "d", sizeof(buf));
				ref -= 500;
			} else if (ref >= 400) {
				strlcat(buf, "cd", sizeof(buf));
				ref -= 400;
			} else if (ref >= 100) {
				strlcat(buf, "c", sizeof(buf));
				ref -= 100;
			} else if (ref >= 90) {
				strlcat(buf, "xc", sizeof(buf));
				ref -= 90;
			} else if (ref >= 50) {
				strlcat(buf, "l", sizeof(buf));
				ref -= 50;
			} else if (ref >= 40) {
				strlcat(buf, "xl", sizeof(buf));
				ref -= 40;
			} else if (ref >= 10) {
				strlcat(buf, "x", sizeof(buf));
				ref -= 10;
			} else if (ref >= 9) {
				strlcat(buf, "ix", sizeof(buf));
				ref -= 9;
			} else if (ref >= 5) {
				strlcat(buf, "v", sizeof(buf));
				ref -= 5;
			} else if (ref >= 4) {
				strlcat(buf, "iv", sizeof(buf));
				ref -= 4;
			} else if (ref >= 1) {
				strlcat(buf, "i", sizeof(buf));
				ref -= 1;
			}
	} else {
		while (ref && sz < sizeof(buf) - 1) {
			buf[sz++] = 'a' + (ref - 1) % 26;
			ref = (ref - 1) / 26;
		}
		buf[sz] = '\0';
		for (i = 0; i < sz; i++, sz--) {
			c = buf[i];
			buf[i] = buf[sz - 1];
			buf[sz - 1] = c;
		}
	}

	return hbuf_printf(out, "%s[%s]%s", 
		nl ? " " : "", buf, nl ? "\n" : "");
}

/*
 * Convert newlines to spaces, elide control characters.
 * If a newline follows a period, it's converted to two spaces.
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_escape(struct lowdown_buf *out, const char *buf, size_t sz)
{
	size_t	 i, start = 0;

	for (i = 0; i < sz; i++) {
		if (buf[i] == '\n') {
			if (!hbuf_put(out, buf + start, i - start))
				return 0;
			if (out->size && 
			    out->data[out->size - 1] == '.' &&
			    !hbuf_putc(out, ' '))
				return 0;
			if (!hbuf_putc(out, ' '))
				return 0;
			start = i + 1;
		} else if (iscntrl((unsigned char)buf[i])) {
			if (!hbuf_put(out, buf + start, i - start))
				return 0;
			start = i + 1;
		}
	}

	if (start < sz &&
	    !hbuf_put(out, buf + start, sz - start))
		return 0;

	return 1;
}

/*
 * Output optional number of newlines before or after content.
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_buf_vspace(struct gemini *st, struct lowdown_buf *out, size_t sz)
{

	if (st->last_blank >= 0)
		while ((size_t)st->last_blank < sz) {
			if (!HBUF_PUTSL(out, "\n"))
				return 0;
			st->last_blank++;
		}

	return 1;
}

/*
 * Emit text in "in" the current line with output "out".
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_buf(struct gemini *st, struct lowdown_buf *out, 
	const struct lowdown_node *n, const struct lowdown_buf *in)
{
	const struct lowdown_node	*nn;
	size_t				 i = 0;

	for (nn = n; nn != NULL; nn = nn->parent)
		if (nn->type == LOWDOWN_BLOCKCODE ||
	  	    nn->type == LOWDOWN_BLOCKHTML) {
			st->last_blank = 1;
			return hbuf_putb(out, in);
		}

	/* 
	 * If we last printed some space and we're not in literal mode,
	 * suppress any leading blanks.
	 * This is only likely to happen around links.
	 */

	assert(in != NULL);
	if (st->last_blank != 0)
		for ( ; i < in->size; i++)
			if (!isspace((unsigned char)in->data[i]))
				break;

	if (!rndr_escape(out, in->data + i, in->size - i))
		return 0;
	if (in->size && st->last_blank != 0)
		st->last_blank = 0;
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

static int
rndr_doc_header(struct gemini *st, struct lowdown_buf *out,
	const struct lowdown_metaq *mq)
{
	const struct lowdown_meta	*m;

	if (!(st->flags & LOWDOWN_GEMINI_METADATA))
		return 1;
	TAILQ_FOREACH(m, mq, entries) {
		if (!rndr_escape(out, m->key, strlen(m->key)))
			return 0;
		if (!HBUF_PUTSL(out, ": "))
			return 0;
		if (!rndr_escape(out, m->value, strlen(m->value)))
			return 0;
		st->last_blank = 0;
		if (!rndr_buf_vspace(st, out, 1))
			return 0;
	}
	return 1;
}

/*
 * Render the key and value, then store the results in our "mq"
 * conditional to it existing.
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_meta(struct gemini *st, 
	const struct lowdown_node *n, struct lowdown_metaq *mq)
{
	ssize_t				 last_blank;
	struct lowdown_buf		*tmp = NULL;
	struct lowdown_meta		*m;
	const struct lowdown_node	*child;

	/*
	 * Manually render the children of the meta into a
	 * buffer and use that as our value.  Start by zeroing
	 * our terminal position and using another output buffer
	 * (st->tmp would be clobbered by children).
	 */

	last_blank = st->last_blank;
	st->last_blank = -1;

	if ((tmp = hbuf_new(128)) == NULL)
		goto err;
	if ((m = calloc(1, sizeof(struct lowdown_meta))) == NULL)
		goto err;
	TAILQ_INSERT_TAIL(mq, m, entries);

	m->key = strndup(n->rndr_meta.key.data,
		n->rndr_meta.key.size);
	if (m->key == NULL)
		goto err;

	TAILQ_FOREACH(child, &n->children, entries)
		if (!rndr(tmp, mq, st, child))
			goto err;

	m->value = strndup(tmp->data, tmp->size);
	if (m->value == NULL)
		goto err;

	hbuf_free(tmp);
	st->last_blank = last_blank;
	return 1;
err:
	hbuf_free(tmp);
	return 0;
}

/*
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_flush_linkq(struct gemini *st, struct lowdown_buf *out)
{
	struct link	*l;
	int		 rc;

	while ((l = TAILQ_FIRST(&st->linkq)) != NULL) {
		TAILQ_REMOVE(&st->linkq, l, entries);
		if (!HBUF_PUTSL(out, "=> "))
			return 0;
		if (l->n->type == LOWDOWN_LINK)
			rc = hbuf_putb(out, &l->n->rndr_link.link);
		else if (l->n->type == LOWDOWN_LINK_AUTO)
			rc = hbuf_putb(out, &l->n->rndr_autolink.link);
		else if (l->n->type == LOWDOWN_IMAGE)
			rc = hbuf_putb(out, &l->n->rndr_image.link);
		else
			rc = 1;
		if (!rc)
			return 0;
		if (!rndr_link_ref(st, out, l, 1))
			return 0;
		st->last_blank = 1;
		free(l);
	}
	return 1;
}

/*
 * Get the column width of a multi-byte sequence.
 * If the sequence is bad, return the number of raw bytes to print.
 * Return <0 on failure (memory), >=0 otherwise.
 */
static ssize_t
rndr_mbswidth(struct gemini *st, const struct lowdown_buf *in)
{
	size_t	 	 wsz, csz;
	const char	*cp;
	void		*pp;

	cp = in->data;
	wsz = mbsnrtowcs(NULL, &cp, in->size, 0, NULL);
	if (wsz == (size_t)-1)
		return in->size;

	if (st->bufsz < wsz) {
		st->bufsz = wsz;
		pp = reallocarray(st->buf, wsz, sizeof(wchar_t));
		if (pp == NULL)
			return -1;
		st->buf = pp;
	}

	cp = in->data;
	mbsnrtowcs(st->buf, &cp, in->size, wsz, NULL);
	csz = wcswidth(st->buf, wsz);
	return csz == (size_t)-1 ? in->size : csz;
}

/*
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr_table(struct lowdown_buf *ob, struct gemini *st, 
	const struct lowdown_node *n)
{
	size_t				*widths = NULL;
	const struct lowdown_node	*row, *top, *cell;
	struct lowdown_buf		*celltmp = NULL, 
					*rowtmp = NULL;
	size_t				 i, j, sz;
	ssize_t			 	 last_blank, ssz;
	unsigned int			 flags, oflags;
	int				 rc = 0;

	assert(n->type == LOWDOWN_TABLE_BLOCK);

	/*
	 * Temporarily make us not use in-line links.
	 * This is obviously because tables and inline links don't work
	 * well together.
	 */

	oflags = st->flags;
	if (st->flags & LOWDOWN_GEMINI_LINK_IN)
		st->flags &= ~LOWDOWN_GEMINI_LINK_IN;

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

	assert(st->linkqsz == 0);
	TAILQ_FOREACH(top, &n->children, entries) {
		assert(top->type == LOWDOWN_TABLE_HEADER ||
			top->type == LOWDOWN_TABLE_BODY);
		TAILQ_FOREACH(row, &top->children, entries)
			TAILQ_FOREACH(cell, &row->children, entries) {
				i = cell->rndr_table_cell.col;
				assert(i < n->rndr_table.columns);
				hbuf_truncate(celltmp);
				last_blank = st->last_blank;
				st->last_blank = 0;
				if (!rndr(celltmp, NULL, st, cell))
					goto out;
				ssz = rndr_mbswidth(st, celltmp);
				if (ssz < 0)
					goto out;
				if (widths[i] < (size_t)ssz)
					widths[i] = (size_t)ssz;
				st->last_blank = last_blank;
			}
	}

	/* 
	 * We may have accumulated links when temporarily rendering.
	 * Remove these now, as we're going to have them printed for
	 * real in the subsequent block.
	 */

	link_freeq(&st->linkq);
	st->linkqsz = 0;

	/* Now actually print, row-by-row into the output. */

	TAILQ_FOREACH(top, &n->children, entries) {
		assert(top->type == LOWDOWN_TABLE_HEADER ||
			top->type == LOWDOWN_TABLE_BODY);
		TAILQ_FOREACH(row, &top->children, entries) {
			hbuf_truncate(rowtmp);
			TAILQ_FOREACH(cell, &row->children, entries) {
				i = cell->rndr_table_cell.col;
				hbuf_truncate(celltmp);
				last_blank = st->last_blank;
				st->last_blank = 0;
				if (!rndr(celltmp, NULL, st, cell))
					goto out;
				ssz = rndr_mbswidth(st, celltmp);
				if (ssz < 0)
					goto out;
				assert(widths[i] >= (size_t)ssz);
				sz = widths[i] - (size_t)ssz;

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

				st->last_blank = last_blank;
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

			if (!hbuf_putb(ob, rowtmp))
				goto out;
			st->last_blank = 0;
			if (!rndr_buf_vspace(st, ob, 1))
				goto out;
		}

		if (top->type == LOWDOWN_TABLE_HEADER) {
			for (i = 0; i < n->rndr_table.columns; i++) {
				for (j = 0; j <= widths[i]; j++)
					if (!HBUF_PUTSL(ob, "-"))
						goto out;
				if (i < n->rndr_table.columns - 1 &&
				    !HBUF_PUTSL(ob, "|-"))
					goto out;
			}
			st->last_blank = 0;
			if (!rndr_buf_vspace(st, ob, 1))
				goto out;
		}
	}

	rc = 1;
out:
	hbuf_free(celltmp);
	hbuf_free(rowtmp);
	free(widths);
	st->flags = oflags;
	return rc;
}

/*
 * Return zero on failure (memory), non-zero on success.
 */
static int
rndr(struct lowdown_buf *ob, struct lowdown_metaq *mq,
	struct gemini *st, const struct lowdown_node *n)
{
	const struct lowdown_node	*child, *prev;
	int32_t				 entity;
	size_t				 i;
	struct link			*l;
	int				 rc = 1;
	
	prev = n->parent == NULL ? NULL :
		TAILQ_PREV(n, lowdown_nodeq, entries);
	
	/* Vertical space before content. */

	switch (n->type) {
	case LOWDOWN_ROOT:
		st->last_blank = -1;
		break;
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
			rc = rndr_buf_vspace(st, ob, 1);
		else
			rc = rndr_buf_vspace(st, ob, 2);
		break;
	case LOWDOWN_MATH_BLOCK:
		if (n->rndr_math.blockmode)
			rc = rndr_buf_vspace(st, ob, 1);
		break;
	case LOWDOWN_DEFINITION_DATA:
		/* 
		 * Vertical space if previous block-mode data. 
		 */

		if (n->parent != NULL &&
		    n->parent->type == LOWDOWN_DEFINITION &&
		    (n->parent->rndr_definition.flags &
		     HLIST_FL_BLOCK) &&
		    prev != NULL &&
		    prev->type == LOWDOWN_DEFINITION_DATA)
			rc = rndr_buf_vspace(st, ob, 2);
		else
			rc = rndr_buf_vspace(st, ob, 1);
		break;
	case LOWDOWN_DEFINITION_TITLE:
	case LOWDOWN_HRULE:
	case LOWDOWN_LINEBREAK:
	case LOWDOWN_LISTITEM:
	case LOWDOWN_META:
	case LOWDOWN_TABLE_ROW:
		rc = rndr_buf_vspace(st, ob, 1);
		break;
	case LOWDOWN_IMAGE:
	case LOWDOWN_LINK:
	case LOWDOWN_LINK_AUTO:
		if (st->flags & LOWDOWN_GEMINI_LINK_IN)
			rc = rndr_buf_vspace(st, ob, 1);
		break;
	default:
		break;
	}

	if (!rc)
		return 0;

	/* Output leading content. */

	rc = 1;
	hbuf_truncate(st->tmp);

	switch (n->type) {
	case LOWDOWN_TABLE_BLOCK:
	case LOWDOWN_BLOCKCODE:
	case LOWDOWN_BLOCKHTML:
		rc = HBUF_PUTSL(st->tmp, "```") &&
			rndr_buf(st, ob, n, st->tmp) &&
			rndr_buf_vspace(st, ob, 1);
		break;
	case LOWDOWN_BLOCKQUOTE:
		rc = HBUF_PUTSL(st->tmp, "> ") &&
			rndr_buf(st, ob, n, st->tmp);
		st->last_blank = -1;
		break;
	case LOWDOWN_HEADER:
		for (i = 0; i <= n->rndr_header.level; i++)
			if (!HBUF_PUTSL(st->tmp, "#"))
				return 0;
		rc = HBUF_PUTSL(st->tmp, " ") &&
			rndr_buf(st, ob, n, st->tmp);
		st->last_blank = -1;
		break;
	case LOWDOWN_FOOTNOTES_BLOCK:
		rc = HBUF_PUTSL(st->tmp, "~~~~~~~~") &&
			rndr_buf(st, ob, n, st->tmp);
		break;
	case LOWDOWN_FOOTNOTE_DEF:
		rc = hbuf_printf(st->tmp, "[%zu] ", 
			n->rndr_footnote_def.num) &&
			rndr_buf(st, ob, n, st->tmp);
		st->last_blank = -1;
		break;
	case LOWDOWN_IMAGE:
	case LOWDOWN_LINK:
	case LOWDOWN_LINK_AUTO:
		if (!(IS_STANDALONE_LINK(n, prev) ||
		     (st->flags & LOWDOWN_GEMINI_LINK_IN)))
			break;
		if (!HBUF_PUTSL(st->tmp, "=> "))
			return 0;
		if (n->type == LOWDOWN_LINK_AUTO)
			rc = hbuf_putb(st->tmp, &n->rndr_autolink.link);
		else if (n->type == LOWDOWN_LINK)
			rc = hbuf_putb(st->tmp, &n->rndr_link.link);
		else if (n->type == LOWDOWN_IMAGE)
			rc = hbuf_putb(st->tmp, &n->rndr_image.link);
		if (!rc)
			return 0;
		rc = HBUF_PUTSL(st->tmp, " ") &&
			rndr_buf(st, ob, n, st->tmp);
		st->last_blank = -1;
		break;
	case LOWDOWN_LISTITEM:
		if (n->rndr_listitem.flags & HLIST_FL_ORDERED)
			rc = hbuf_printf(st->tmp, "%zu. ", 
				n->rndr_listitem.num);
		else
			rc = HBUF_PUTSL(st->tmp, "* ");
		if (!rc)
			return 0;
		rc = rndr_buf(st, ob, n, st->tmp);
		st->last_blank = -1;
		break;
	case LOWDOWN_SUPERSCRIPT:
		rc = HBUF_PUTSL(st->tmp, "^") &&
			rndr_buf(st, ob, n, st->tmp);
		break;
	default:
		break;
	}

	if (!rc)
		return 0;

	/* Descend into children. */

	switch (n->type) {
	case LOWDOWN_TABLE_BLOCK:
		if (!rndr_table(ob, st, n))
			return 0;
		break;
	case LOWDOWN_META:
		if (n->chng != LOWDOWN_CHNG_DELETE &&
		    !rndr_meta(st, n, mq))
			return 0;
		break;
	default:
		TAILQ_FOREACH(child, &n->children, entries)
			if (!rndr(ob, mq, st, child))
				return 0;
		break;
	}

	/* Output non-child or trailing content. */

	rc = 1;
	hbuf_truncate(st->tmp);

	switch (n->type) {
	case LOWDOWN_HRULE:
		rc = HBUF_PUTSL(st->tmp, "~~~~~~~~") &&
			rndr_buf(st, ob, n, st->tmp);
		break;
	case LOWDOWN_FOOTNOTE_REF:
		rc = hbuf_printf(st->tmp, "[%zu]", 
			n->rndr_footnote_ref.num) &&
			rndr_buf(st, ob, n, st->tmp);
		break;
	case LOWDOWN_RAW_HTML:
		rc = rndr_buf(st, ob, n, &n->rndr_raw_html.text);
		break;
	case LOWDOWN_MATH_BLOCK:
		rc = rndr_buf(st, ob, n, &n->rndr_math.text);
		break;
	case LOWDOWN_ENTITY:
		entity = entity_find_iso(&n->rndr_entity.text);
		if (entity > 0)
			rc = rndr_entity(st->tmp, entity) &&
				rndr_buf(st, ob, n, st->tmp);
		else
			rc = rndr_buf(st, ob, n, &n->rndr_entity.text);
		break;
	case LOWDOWN_BLOCKCODE:
		rc = rndr_buf(st, ob, n, &n->rndr_blockcode.text);
		break;
	case LOWDOWN_BLOCKHTML:
		rc = rndr_buf(st, ob, n, &n->rndr_blockhtml.text);
		break;
	case LOWDOWN_CODESPAN:
		rc = rndr_buf(st, ob, n, &n->rndr_codespan.text);
		break;
	case LOWDOWN_IMAGE:
		rc = rndr_buf(st, ob, n, &n->rndr_image.alt);
		/* FALLTHROUGH */
	case LOWDOWN_LINK:
	case LOWDOWN_LINK_AUTO:
		if (IS_STANDALONE_LINK(n, prev) ||
		    (st->flags & LOWDOWN_GEMINI_LINK_IN))
			break;
		if ((l = calloc(1, sizeof(struct link))) == NULL)
			return 0;
		l->n = n;
		l->id = ++st->linkqsz;
		TAILQ_INSERT_TAIL(&st->linkq, l, entries);
		rc = rndr_link_ref(st, st->tmp, l, 0) &&
			rndr_buf(st, ob, n, st->tmp);
		break;
	case LOWDOWN_NORMAL_TEXT:
		rc = rndr_buf(st, ob, n, &n->rndr_normal_text.text);
		break;
	case LOWDOWN_ROOT:
		if (TAILQ_EMPTY(&st->linkq) || 
		    !(st->flags & LOWDOWN_GEMINI_LINK_END))
			break;
		rc = rndr_buf_vspace(st, ob, 2) &&
			rndr_flush_linkq(st, ob);
		break;
	case LOWDOWN_DOC_HEADER:
		rc = rndr_doc_header(st, ob, mq);
		break;
	default:
		break;
	}
	if (!rc)
		return 0;

	/* Trailing block spaces. */

	rc = 1;
	hbuf_truncate(st->tmp);

	switch (n->type) {
	case LOWDOWN_TABLE_BLOCK:
	case LOWDOWN_BLOCKCODE:
	case LOWDOWN_BLOCKHTML:
		if (!HBUF_PUTSL(st->tmp, "```"))
			return 0;
		if (!rndr_buf(st, ob, n, st->tmp))
			return 0;
		st->last_blank = 0;
		rc = rndr_buf_vspace(st, ob, 2);
		break;
	case LOWDOWN_DOC_HEADER:
		if (st->flags & LOWDOWN_STANDALONE)
			rc = rndr_buf_vspace(st, ob, 2);
		break;
	case LOWDOWN_BLOCKQUOTE:
	case LOWDOWN_DEFINITION:
	case LOWDOWN_FOOTNOTES_BLOCK:
	case LOWDOWN_FOOTNOTE_DEF:
	case LOWDOWN_HEADER:
	case LOWDOWN_LIST:
	case LOWDOWN_PARAGRAPH:
		rc = rndr_buf_vspace(st, ob, 2);
		break;
	case LOWDOWN_MATH_BLOCK:
		if (n->rndr_math.blockmode)
			rc = rndr_buf_vspace(st, ob, 1);
		break;
	case LOWDOWN_DEFINITION_DATA:
	case LOWDOWN_DEFINITION_TITLE:
	case LOWDOWN_HRULE:
	case LOWDOWN_LISTITEM:
	case LOWDOWN_META:
	case LOWDOWN_TABLE_ROW:
		rc = rndr_buf_vspace(st, ob, 1);
		break;
	case LOWDOWN_IMAGE:
	case LOWDOWN_LINK:
	case LOWDOWN_LINK_AUTO:
		if (IS_STANDALONE_LINK(n, prev) ||
		    (st->flags & LOWDOWN_GEMINI_LINK_IN))
			rc = rndr_buf_vspace(st, ob, 1);
		break;
	case LOWDOWN_ROOT:
		/*
		 * Special case: snip any trailing newlines that may
		 * have been printed as trailing vertical space.
		 * This tidies up the output.
		 */

		if (!rndr_buf_vspace(st, ob, 1))
			return 0;
		while (ob->size && ob->data[ob->size - 1] == '\n')
			ob->size--;
		rc = HBUF_PUTSL(ob, "\n");
		break;
	default:
		break;
	}
	if (!rc)
		return 0;

	if (st->last_blank > 1 && !TAILQ_EMPTY(&st->linkq) &&
	    !(st->flags & LOWDOWN_GEMINI_LINK_END)) {
		if (!rndr_flush_linkq(st, ob))
			return 0;
		if (!HBUF_PUTSL(ob, "\n"))
			return 0;
		st->last_blank = 2;
	}

	return 1;
}

int
lowdown_gemini_rndr(struct lowdown_buf *ob,
	struct lowdown_metaq *mq, void *arg, 
	const struct lowdown_node *n)
{
	struct gemini		*st = arg;
	int			 c;
	struct lowdown_metaq	 metaq;

	if (mq == NULL) {
		TAILQ_INIT(&metaq);
		mq = &metaq;
	}

	/* Set ourselves into a sane state. */

	st->last_blank = 0;

	c = rndr(ob, mq, st, n);

	link_freeq(&st->linkq);
	st->linkqsz = 0;

	if (mq == &metaq)
		lowdown_metaq_free(mq);
	return c;
}

void *
lowdown_gemini_new(const struct lowdown_opts *opts)
{
	struct gemini	*p;

	if ((p = calloc(1, sizeof(struct gemini))) == NULL)
		return NULL;

	TAILQ_INIT(&p->linkq);
	p->flags = opts != NULL ? opts->oflags : 0;

	/* Only use one kind of flag output. */

	if ((p->flags & LOWDOWN_GEMINI_LINK_IN) &&
	    (p->flags & LOWDOWN_GEMINI_LINK_END))
		p->flags &= ~LOWDOWN_GEMINI_LINK_IN;

	if ((p->tmp = hbuf_new(32)) == NULL) {
		free(p);
		return NULL;
	}

	return p;
}

void
lowdown_gemini_free(void *arg)
{
	struct gemini	*p = arg;
	
	if (p == NULL)
		return;

	hbuf_free(p->tmp);
	free(p->buf);
	free(p);
}
