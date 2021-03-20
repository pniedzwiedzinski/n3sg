/*	$Id: extern.h,v 1.77 2021/02/03 18:07:24 kristaps Exp $ */
/*
 * Copyright (c) 2016--2020 Kristaps Dzonsons <kristaps@bsd.lv>
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
#ifndef EXTERN_H
#define EXTERN_H

int	 	 smarty(struct lowdown_node *, size_t, enum lowdown_type);

int32_t	 	 entity_find_iso(const struct lowdown_buf *);
const char	*entity_find_tex(const struct lowdown_buf *, unsigned char *);
#define		 TEX_ENT_MATH	 0x01
#define		 TEX_ENT_ASCII	 0x02

int		 hbuf_eq(const struct lowdown_buf *, const struct lowdown_buf *);
int		 hbuf_streq(const struct lowdown_buf *, const char *);
int		 hbuf_strprefix(const struct lowdown_buf *, const char *);
void		 hbuf_free(struct lowdown_buf *);
int		 hbuf_grow(struct lowdown_buf *, size_t);
int		 hbuf_clone(const struct lowdown_buf *, struct lowdown_buf *);
struct lowdown_buf *hbuf_new(size_t) __attribute__((malloc));
int		 hbuf_printf(struct lowdown_buf *, const char *, ...) 
			__attribute__((format (printf, 2, 3)));
int		 hbuf_put(struct lowdown_buf *, const char *, size_t);
int		 hbuf_putb(struct lowdown_buf *, const struct lowdown_buf *);
int		 hbuf_putc(struct lowdown_buf *, char);
int		 hbuf_putf(struct lowdown_buf *, FILE *);
int		 hbuf_puts(struct lowdown_buf *, const char *);
void		 hbuf_truncate(struct lowdown_buf *);
int		 hbuf_shortlink(struct lowdown_buf *, const struct lowdown_buf *);

#define 	 HBUF_PUTSL(output, literal) \
		 hbuf_put(output, literal, sizeof(literal) - 1)

ssize_t		 halink_email(size_t *, struct lowdown_buf *, char *, size_t, size_t);
ssize_t		 halink_url(size_t *, struct lowdown_buf *, char *, size_t, size_t);
ssize_t		 halink_www(size_t *, struct lowdown_buf *, char *, size_t, size_t);

int		 hesc_attr(struct lowdown_buf *, const char *, size_t);
int		 hesc_href(struct lowdown_buf *, const char *, size_t);
int		 hesc_html(struct lowdown_buf *, const char *, size_t, int, int, int);

char		*rcsdate2str(const char *);
char		*date2str(const char *);
char		*rcsauthor2str(const char *);

#endif /* !EXTERN_H */
