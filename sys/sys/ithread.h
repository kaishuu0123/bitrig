/*
 * Copyright (c) 2013 Christiano F. Haesbaert <haesbaert@haesbaert.org>
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

#ifndef _SYS_ITHREAD_H_
#define _SYS_ITHREAD_H_

struct intrsource;

void	ithread(void *);
int	ithread_run(struct intrsource *);
void	ithread_register(struct intrsource *);
void	ithread_deregister(struct intrsource *);
void	ithread_forkall(void);
void	ithread_softmain(void *);
struct intrsource *
ithread_softregister(int, int (*)(void *), void *, int);
void	ithread_softderegister(struct intrsource *);
void	ithread_softsleep(struct intrsource *);
#define ithread_softsched ithread_run
#endif /* _SYS_ITHREAD_H_ */
