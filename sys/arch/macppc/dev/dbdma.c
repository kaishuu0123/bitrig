/*	$OpenBSD: dbdma.c,v 1.3 2001/09/15 01:51:11 mickey Exp $	*/
/*	$NetBSD: dbdma.c,v 1.2 1998/08/21 16:13:28 tsubai Exp $	*/

/*
 * Copyright 1991-1998 by Open Software Foundation, Inc.
 *              All Rights Reserved
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby granted,
 * provided that the above copyright notice appears in all copies and
 * that both the copyright notice and this permission notice appear in
 * supporting documentation.
 *
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE.
 *
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT,
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/systm.h>

#include <vm/vm.h>

#include <machine/bus.h>
#include <macppc/dev/dbdma.h>

dbdma_command_t	*dbdma_alloc_commands = NULL;

void
dbdma_start(dmap, dt)
	dbdma_regmap_t *dmap;
	dbdma_t dt;
{
	u_int32_t addr = dt->d_paddr;

	DBDMA_ST4_ENDIAN(&dmap->d_intselect, DBDMA_CLEAR_CNTRL((0xffff)));
	DBDMA_ST4_ENDIAN(&dmap->d_control, DBDMA_CLEAR_CNTRL((
	    DBDMA_CNTRL_ACTIVE |
	    DBDMA_CNTRL_DEAD |
	    DBDMA_CNTRL_WAKE |
	    DBDMA_CNTRL_FLUSH |
	    DBDMA_CNTRL_PAUSE |
	    DBDMA_CNTRL_RUN)));

	/* XXX time-bind it? */
	do {
		delay(10);
	} while (DBDMA_LD4_ENDIAN(&dmap->d_status) & DBDMA_CNTRL_ACTIVE);


	DBDMA_ST4_ENDIAN(&dmap->d_cmdptrhi, 0); /* 64-bit not yet */
	DBDMA_ST4_ENDIAN(&dmap->d_cmdptrlo, addr);

	DBDMA_ST4_ENDIAN(&dmap->d_control,
		DBDMA_SET_CNTRL(DBDMA_CNTRL_RUN|DBDMA_CNTRL_WAKE)|
		DBDMA_CLEAR_CNTRL(DBDMA_CNTRL_PAUSE|DBDMA_CNTRL_DEAD) );
}

void
dbdma_stop(dmap)
	dbdma_regmap_t *dmap;
{
	DBDMA_ST4_ENDIAN(&dmap->d_control, DBDMA_CLEAR_CNTRL(DBDMA_CNTRL_RUN) |
			  DBDMA_SET_CNTRL(DBDMA_CNTRL_FLUSH));

	while (DBDMA_LD4_ENDIAN(&dmap->d_status) &
		(DBDMA_CNTRL_ACTIVE|DBDMA_CNTRL_FLUSH));
}

void
dbdma_flush(dmap)
	dbdma_regmap_t *dmap;
{
	DBDMA_ST4_ENDIAN(&dmap->d_control, DBDMA_SET_CNTRL(DBDMA_CNTRL_FLUSH));

	/* XXX time-bind it? */
	while (DBDMA_LD4_ENDIAN(&dmap->d_status) & (DBDMA_CNTRL_FLUSH));
}

void
dbdma_reset(dmap)
	dbdma_regmap_t *dmap;
{
	DBDMA_ST4_ENDIAN(&dmap->d_control,
			 DBDMA_CLEAR_CNTRL( (DBDMA_CNTRL_ACTIVE	|
					     DBDMA_CNTRL_DEAD	|
					     DBDMA_CNTRL_WAKE	|
					     DBDMA_CNTRL_FLUSH	|
					     DBDMA_CNTRL_PAUSE	|
					     DBDMA_CNTRL_RUN      )));

	/* XXX time-bind it? */
	while (DBDMA_LD4_ENDIAN(&dmap->d_status) & DBDMA_CNTRL_RUN);
}

void
dbdma_continue(dmap)
	dbdma_regmap_t *dmap;
{
	DBDMA_ST4_ENDIAN(&dmap->d_control,
		DBDMA_SET_CNTRL(DBDMA_CNTRL_RUN | DBDMA_CNTRL_WAKE) |
		DBDMA_CLEAR_CNTRL(DBDMA_CNTRL_PAUSE | DBDMA_CNTRL_DEAD));
}

void
dbdma_pause(dmap)
	dbdma_regmap_t *dmap;
{
	DBDMA_ST4_ENDIAN(&dmap->d_control,DBDMA_SET_CNTRL(DBDMA_CNTRL_PAUSE));

	/* XXX time-bind it? */
	while (DBDMA_LD4_ENDIAN(&dmap->d_status) & DBDMA_CNTRL_ACTIVE);
}

dbdma_t
dbdma_alloc(dmat, size)
	bus_dma_tag_t dmat;
	int size;
{
	dbdma_t dt;
	int error, nsegs = 0;

	dt = malloc(sizeof *dt, M_DEVBUF, M_NOWAIT);
	if (!dt)
		return (dt);
	bzero(dt, sizeof *dt);

	dt->d_size = size *= sizeof(dbdma_command_t);
	if ((error = bus_dmamem_alloc(dmat, size, NBPG, 0, dt->d_segs,
	    1, &nsegs, BUS_DMA_NOWAIT)) != 0) {
		printf("dbdma: unable to allocate dma, error = %d\n", error);
	} else if ((error = bus_dmamem_map(dmat, dt->d_segs, nsegs, size,
	    (caddr_t *)&dt->d_addr, BUS_DMA_NOWAIT | BUS_DMA_COHERENT)) != 0) {
		printf("dbdma: unable to map dma, error = %d\n", error);
	} else if ((error = bus_dmamap_create(dmat, dt->d_size, 1,
	    dt->d_size, 0, BUS_DMA_NOWAIT, &dt->d_map)) != 0) {
		printf("dbdma: unable to create dma map, error = %d\n", error);
	} else if ((error = bus_dmamap_load_raw(dmat, dt->d_map,
	    dt->d_segs, nsegs, size, BUS_DMA_NOWAIT)) != 0) {
		printf("dbdma: unable to load dma map, error = %d\n", error);
	} else
		return dt;

	if (dt->d_map)
		bus_dmamap_destroy(dmat, dt->d_map);
	if (dt->d_addr)
		bus_dmamem_unmap(dmat, (caddr_t)dt->d_addr, size);
	if (nsegs)
		bus_dmamem_free(dmat, dt->d_segs, nsegs);
	free(dt, M_DEVBUF);

	return (NULL);
}
