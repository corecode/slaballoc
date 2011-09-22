/*
 * This code is derived from software contributed to The DragonFly Project
 * by Simon Schubert <corecode@fs.ei.tum.de>.
 *
 * Copyright (c) 2004 The DragonFly Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $TheBOFH: slaballoc/alloc.c,v 1.8 2004/12/24 09:09:45 corecode Exp $
 */

#include <sys/param.h>
#include <sys/queue.h>

#define SLIST_REMOVE_AFTER(head, elm, field) do {			\
	if ((elm) == NULL) {						\
		SLIST_REMOVE_HEAD((head), field);			\
	}								\
	else {								\
		(elm)->field.sle_next =					\
		    (elm)->field.sle_next->field.sle_next;		\
	}								\
} while (0)


#ifndef _KERNEL
#include <sys/mman.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>

#define	M_WAITOK	0
#define	PAGESIZ		4096
#define	NCPU		1

#define	kmem_get_pages(count, flags)		\
	mmap(NULL, (count) * PAGESIZ, PROT_READ | PROT_WRITE, MAP_ANON, -1, 0)
#define	kmem_return_pages(addr, count)		\
	munmap((addr), (count) * PAGESIZ)

#define	KKASSERT(cond) do {			\
	if (!(cond)) {				\
		fprintf(stderr, "panic: assertion `%s' failed", #cond); \
		abort();			\
	}					\
} while (0)

#endif


#include "alloc.h"

#define	KH_NUM		64
#define	KM_MAXROUNDS	64
#define	KM_MINROUNDS	16

struct kmem_bufctl;
struct kmem_magazine;
typedef SLIST_HEAD(, kmem_bufctl) kmem_hashentry;
typedef kmem_hashentry	kmem_hashtab[KH_NUM];

struct kmem_cpu_cache {
	int		kcc_rounds;		/* Rounds in loaded magazine */
	struct kmem_magazine *kcc_loaded;	/* Loaded magazine */
	int		kcc_prevrounds;		/* Rounds in previous magazine */
	struct kmem_magazine *kcc_previous;	/* Previous magazine */
	int		kcc_magsize;		/* Rounds per magazine */
	struct kmem_cache_stats kcc_stats;	/* Statistics */
	char		kcc_pad;		/* XXX Pad to cache line */
};

struct kmem_cache {
	TAILQ_HEAD(, kmem_slab) kc_slabs;	/* Slabs: empty to full */
	struct kmem_slab *kc_freeslab;		/* First slab w/ bufs */
	SLIST_HEAD(, kmem_magazine) kc_fulldepot;	/* Full magazines depot */
	SLIST_HEAD(, kmem_magazine) kc_emptydepot;	/* Empty magazines depot */
	const char	*kc_name;		/* Informational name */
	size_t		kc_size;		/* Size of objects */
	size_t		kc_realsize;		/* Size incl. alignment */
	unsigned int	kc_align;		/* Needed alignment */
	kmem_cache_cdtor *kc_ctor;		/* Constructor of objects */
	kmem_cache_cdtor *kc_dtor;		/* Destructor of objects */
	unsigned int	kc_color;		/* Coloring of next slab */
	unsigned int	kc_maxcolor;		/* Maximum color allowed */
	unsigned int	kc_pages;		/* Pages per slab */
	unsigned int	kc_bufs;		/* Buffers per slab */
	kmem_hashtab	*kc_hashtab;		/* Bufctl hash table */
	char		kc_pad;			/* XXX Pad to cache line */
	struct kmem_cpu_cache kc_cpu[NCPU];	/* Per-CPU data */
};

struct kmem_slab {
	TAILQ_ENTRY(kmem_slab) ks_entry;	/* Slab linkage */
	SLIST_HEAD(, kmem_bufctl) ks_freebufs;	/* List of free bufs */
	unsigned int	ks_refcnt;		/* Used buf count */
	void		*ks_page;		/* Base of the page(s) used */
};

struct kmem_bufctl {
	SLIST_ENTRY(kmem_bufctl) kb_entry;	/* Next free buf */
	void		*kb_buf;		/* Data */
	struct kmem_slab *kb_slab;		/* Back link to slab */
};

struct kmem_bufctl_inline {
	SLIST_ENTRY(kmem_bufctl) kb_entry;	/* Linkage */
};

struct kmem_magazine {
	SLIST_ENTRY(kmem_magazine) km_entry;	/* Next magazine */
	unsigned int	km_rounds;		/* Bufs available */
	struct kmem_bufctl *km_round[KM_MAXROUNDS];	/* Array of bufs */
};


static void kmem_cache_init(struct kmem_cache *, const char *, size_t,
		unsigned int, kmem_cache_cdtor *, kmem_cache_cdtor *);
static unsigned int kmem_bufaddr_makehash(void *);
static struct kmem_slab *kmem_alloc_slab(struct kmem_cache *, int);
static void kmem_empty_magazine(struct kmem_cache *, struct kmem_magazine *);
static void kmem_returnto_slab(struct kmem_cache *, void *);


static struct kmem_cache cache_cch;
static struct kmem_cache *slab_cch;
static struct kmem_cache *bufctl_cch;
static struct kmem_cache *hashtab_cch;
static struct kmem_cache *mag_cch;

void
kmem_init(void)
{
	/* Bootstrap cache cache */
	kmem_cache_init(&cache_cch, "kmem_cache", sizeof(struct kmem_cache), 0, NULL, NULL);

	/* Bootstrap remaining caches */
	slab_cch = kmem_cache_create("kmem_slab", sizeof(struct kmem_slab), 0, NULL, NULL);
	bufctl_cch = kmem_cache_create("kmem_bufctl", sizeof(struct kmem_slab), 0, NULL, NULL);
	hashtab_cch = kmem_cache_create("kmem_hashtab", sizeof(kmem_hashtab), 0, NULL, NULL);
	mag_cch = kmem_cache_create("kmem_magazine", sizeof(struct kmem_magazine), 0, NULL, NULL);
}

struct kmem_cache *
kmem_cache_create(const char *name, size_t size, unsigned int align,
		kmem_cache_cdtor *ctor, kmem_cache_cdtor *dtor)
{
	struct kmem_cache *cp;

	cp = kmem_cache_alloc(&cache_cch, M_WAITOK);
	kmem_cache_init(cp, name, size, align, ctor, dtor);

	return cp;
}

static void
kmem_cache_init(struct kmem_cache *cp, const char *name, size_t size,
		unsigned int align, kmem_cache_cdtor *ctor, kmem_cache_cdtor *dtor)
{
	int i;

	TAILQ_INIT(&cp->kc_slabs);
	cp->kc_freeslab = NULL;
	SLIST_INIT(&cp->kc_fulldepot);
	SLIST_INIT(&cp->kc_emptydepot);
	cp->kc_name = name;
	cp->kc_size = size;
	cp->kc_align = align;
	cp->kc_ctor = ctor;
	cp->kc_dtor = dtor;
	cp->kc_color = 0;	/* randomize? */

	if (cp->kc_align < ALIGN(1))
		cp->kc_align = ALIGN(1);

	cp->kc_realsize = (cp->kc_size + cp->kc_align - 1) /
		cp->kc_align * cp->kc_align;

	/* At the moment a no-op */
	if (cp->kc_realsize < sizeof(struct kmem_bufctl_inline))
		cp->kc_realsize = sizeof(struct kmem_bufctl_inline);

	/*
	 * Only accept up to 1/5 waste. If it is more,
	 * use external administrative information.
	 */
	if ((PAGESIZ - sizeof(struct kmem_slab)) / cp->kc_realsize * cp->kc_realsize
	    < PAGESIZ * 4 / 5) {
		int i;

		cp->kc_pages = 2;
		while (cp->kc_pages * PAGESIZ / cp->kc_realsize * cp->kc_realsize
		    < (PAGESIZ + sizeof(struct kmem_slab)) * cp->kc_pages * 4 / 5)
			cp->kc_pages++;

		cp->kc_hashtab = kmem_cache_alloc(hashtab_cch, M_WAITOK);
		for (i = 0; i < KH_NUM; i++)
			SLIST_INIT(&(*cp->kc_hashtab)[i]);

		cp->kc_bufs = cp->kc_pages * PAGESIZ / cp->kc_realsize;
		cp->kc_maxcolor = cp->kc_pages * PAGESIZ - cp->kc_bufs * cp->kc_realsize;
	} else {
		cp->kc_pages = 1;
		cp->kc_bufs = (PAGESIZ - sizeof(struct kmem_slab)) / cp->kc_realsize;
		cp->kc_maxcolor = PAGESIZ - sizeof(struct kmem_slab) - cp->kc_bufs * cp->kc_realsize;
	}

	for (i = 0; i < NCPU; ++i) {
		struct kmem_cpu_cache *cpu;

		cpu = &cp->kc_cpu[i];
		cpu->kcc_rounds = cpu->kcc_prevrounds = -1;
		cpu->kcc_loaded = cpu->kcc_previous = NULL;
		cpu->kcc_magsize = KM_MINROUNDS;
		cpu->kcc_stats.kcs_allocs = 0;
		cpu->kcc_stats.kcs_misses = 0;
	}
}

void
kmem_cache_destroy(struct kmem_cache *cp)
{
	struct kmem_slab *slab;
	struct kmem_magazine *mag;
	int i;

	while ((mag = SLIST_FIRST(&cp->kc_fulldepot)) != NULL) {
		SLIST_REMOVE_HEAD(&cp->kc_fulldepot, km_entry);
		kmem_empty_magazine(cp, mag);
		kmem_cache_free(mag_cch, mag);
	}

	while ((mag = SLIST_FIRST(&cp->kc_emptydepot)) != NULL) {
		SLIST_REMOVE_HEAD(&cp->kc_emptydepot, km_entry);
		kmem_cache_free(mag_cch, mag);
	}

	for (i = 0; i < NCPU; ++i) {
		struct kmem_cpu_cache *cpu;

		cpu = &cp->kc_cpu[i];

		if (cpu->kcc_loaded != NULL) {
			cpu->kcc_loaded->km_rounds = cpu->kcc_rounds;
			kmem_empty_magazine(cp, cpu->kcc_loaded);
			kmem_cache_free(mag_cch, cpu->kcc_loaded);
		}
		if (cpu->kcc_previous != NULL) {
			cpu->kcc_previous->km_rounds = cpu->kcc_prevrounds;
			kmem_empty_magazine(cp, cpu->kcc_previous);
			kmem_cache_free(mag_cch, cpu->kcc_previous);
		}
	}

	while ((slab = TAILQ_FIRST(&cp->kc_slabs)) != NULL) {
		void *page;

		KKASSERT((slab->ks_refcnt == 0));

		TAILQ_REMOVE(&cp->kc_slabs, slab, ks_entry);
		page = slab->ks_page;

		if (cp->kc_pages > 1) {
			struct kmem_bufctl *bufctl;

			while ((bufctl = SLIST_FIRST(&slab->ks_freebufs)) != NULL) {
				SLIST_REMOVE_HEAD(&slab->ks_freebufs, kb_entry);
				kmem_cache_free(bufctl_cch, bufctl);
			}

			kmem_cache_free(slab_cch, slab);
		}

		kmem_return_pages(page, cp->kc_pages);
	}

	if (cp->kc_pages > 1)
		kmem_cache_free(hashtab_cch, cp->kc_hashtab);

	kmem_cache_free(&cache_cch, cp);
}

void
kmem_cache_getstats(struct kmem_cache *cp, struct kmem_cache_stats *stats)
{
	int i;

	KKASSERT((stats != NULL));

	stats->kcs_allocs = stats->kcs_magmiss = stats->kcs_misses = 0;
	for (i = 0; i < NCPU; ++i) {
		struct kmem_cache_stats *cpustat;

		cpustat = &cp->kc_cpu[i].kcc_stats;
		stats->kcs_misses += cpustat->kcs_misses;
		stats->kcs_magmiss += cpustat->kcs_magmiss;
		stats->kcs_allocs += cpustat->kcs_allocs;
	}
}

void
kmem_cache_debug(struct kmem_cache *cp)
{
	struct kmem_slab *slab;
	struct kmem_magazine *mag;
	unsigned empty, partial, full;
	unsigned used;
	int i;

	printf("kmem cache statistics for: %s\n", cp->kc_name);

	for (i = 0; i < NCPU; ++i) {
		struct kmem_cpu_cache *cpu;

		cpu = &cp->kc_cpu[i];
		printf("cpu%i:\n", i);
		printf("\tallocs: %u\tmisses: %u\thit ratio: %3u%%\n", cpu->kcc_stats.kcs_allocs,
		    cpu->kcc_stats.kcs_misses, (cpu->kcc_stats.kcs_allocs -
			    cpu->kcc_stats.kcs_misses) * 100 / cpu->kcc_stats.kcs_allocs);
		printf("\tmagazine misses: %u\n", cpu->kcc_stats.kcs_magmiss);

		printf("\tloaded: %i\tprevious: %i\n", cpu->kcc_rounds, cpu->kcc_prevrounds);
	}

	used = full = 0;
	SLIST_FOREACH(mag, &cp->kc_fulldepot, km_entry) {
		used += mag->km_rounds;
		full++;
	}
	printf("full depot: %u\ttotal rounds: %u\n", full, used);

	empty = 0;
	SLIST_FOREACH(mag, &cp->kc_emptydepot, km_entry) {
		empty++;
	}
	printf("empty depot: %u\n", empty);

	empty = partial = full = used = 0;
	TAILQ_FOREACH(slab, &cp->kc_slabs, ks_entry) {
		if (slab->ks_refcnt == 0) {
			full++;
		} else if (SLIST_EMPTY(&slab->ks_freebufs)) {
			empty++;
		} else {
			partial++;
			used += slab->ks_refcnt;
		}
	}

	printf("empty: %u\tpartial: %u\tfull: %u\n", empty, partial, full);
	printf("fragmentation: %3u%%\n", used / cp->kc_bufs * 100 / (empty + partial + full));

	if (cp->kc_pages > 1) {
		unsigned i;

		printf("hash map distribution:\n");
		for (i = 0; i < KH_NUM; i++) {
			unsigned bufs;
			struct kmem_bufctl *bufctl;

			bufs = 0;
			SLIST_FOREACH(bufctl, &(*cp->kc_hashtab)[i], kb_entry)
				bufs++;

			printf("%2u: %u%c", i, bufs, (i + 1) % 8 ? '\t' : '\n');
		}
	}
}

static unsigned int
kmem_bufaddr_makehash(void *bufaddr)
{
	unsigned long hash;

	/* Alignment is the same, so strip it */
	hash = (unsigned long)bufaddr;
	while (hash >= KH_NUM)
		hash = (hash % KH_NUM) ^ (hash / KH_NUM);

	return hash;
}

static struct kmem_slab *
kmem_alloc_slab(struct kmem_cache *cp, int flags)
{
	void *pages;
	char *bufpos;
	unsigned int i;
	struct kmem_slab *slab;

	cp->kc_cpu[0].kcc_stats.kcs_misses++;		/* XXX curcpu */

	/* Get the memory */
	pages = kmem_get_pages(cp->kc_pages, flags);
	if (pages == NULL)
		return NULL;

	bufpos = pages + cp->kc_color;

	/* Change coloring for next slab */
	cp->kc_color += cp->kc_align;
	if (cp->kc_color > cp->kc_maxcolor);
		cp->kc_color = 0;

	/*
	 * If one slab spans multiple pages, we can't inline
	 * the administrative data and need to allocate it
	 * separately.
	 */
	if (cp->kc_pages > 1) {
		/* XXX recursion? */
		slab = kmem_cache_alloc(slab_cch, flags);
		if (slab == NULL) {
			kmem_return_pages(pages, cp->kc_pages);
			return NULL;
		}

		SLIST_INIT(&slab->ks_freebufs);
		for (i = cp->kc_bufs; i; --i) {
			struct kmem_bufctl *newbufctl;

			newbufctl = kmem_cache_alloc(bufctl_cch, flags);
			if (newbufctl == NULL) {
				while ((newbufctl = SLIST_FIRST(&slab->ks_freebufs)) != NULL) {
					kmem_cache_free(bufctl_cch, newbufctl);
					SLIST_REMOVE_HEAD(&slab->ks_freebufs, kb_entry);
				}
				kmem_return_pages(pages, cp->kc_pages);
				return NULL;
			}

			newbufctl->kb_buf = bufpos;
			bufpos += cp->kc_realsize;
			newbufctl->kb_slab = slab;
			SLIST_INSERT_HEAD(&slab->ks_freebufs, newbufctl, kb_entry);
		}
	}
	/*
	 * This cache uses slabs with inlined administative
	 * information. Probably the hot path.
	 */
	else {
		/*
		 * Struct kmem_slab resides at the very end of the page
		 * when administrative data is stored inline.
		 */
		slab = pages + PAGESIZ - sizeof(struct kmem_slab);

		/*
		 * Pre-calc location of the linkage, which is located
		 * at the far end of each buf (to minimize the possibility
		 * of being overwritten by use-after-free code).
		 */
		bufpos += cp->kc_realsize - sizeof(struct kmem_bufctl_inline);

		SLIST_INIT(&slab->ks_freebufs);
		for (i = cp->kc_bufs; i; --i) {
			struct kmem_bufctl_inline *newbufctl;

			newbufctl = (struct kmem_bufctl_inline *)bufpos;
			bufpos += cp->kc_realsize;
			SLIST_INSERT_HEAD(&slab->ks_freebufs, (struct kmem_bufctl *)newbufctl, kb_entry);
			/*printf("%p->%p(%p)\n", SLIST_FIRST(&slab->ks_freebufs), SLIST_NEXT(newbufctl, kb_entry), newbufctl);*/
		}
	}

	slab->ks_refcnt = 0;
	slab->ks_page = pages;

	return slab;
}

void *
kmem_cache_alloc(struct kmem_cache *cp, int flags)
{
	struct kmem_slab *slab;
	struct kmem_cpu_cache *cpu;
	struct kmem_magazine *mag;
	void *obj;

	cpu = &cp->kc_cpu[0];	/* XXX use cpu number */

	cpu->kcc_stats.kcs_allocs++;

	/*
	 * If the loaded magazine still has rounds in it,
	 * take one and return.
	 */
	if (cpu->kcc_rounds > 0) {
		mag = cpu->kcc_loaded;

alloc_loaded:
		obj = mag->km_round[--cpu->kcc_rounds];
		return obj;
	}

	/*
	 * The loaded magazine had drained. If there are rounds in
	 * the previous magazine, swap them and try again.
	 */
	if (cpu->kcc_prevrounds > 0) {
		cpu->kcc_rounds = cpu->kcc_prevrounds;
		cpu->kcc_prevrounds = 0;

		mag = cpu->kcc_previous;
		cpu->kcc_previous = cpu->kcc_loaded;
		cpu->kcc_loaded = mag;

		goto alloc_loaded;
	}

	/*
	 * Both magazines are empty (or not allocated), so return an
	 * empty one and load a full one.
	 */
	if (!SLIST_EMPTY(&cp->kc_fulldepot)) {
		/*
		 * If the previous magazine is not allocated, the loaded
		 * could also not be allocated. In both cases just put loaded
		 * into the free previous slot.
		 * If the previous magazine is allocated, loaded is too.
		 */
		if (cpu->kcc_previous == NULL) {
			cpu->kcc_previous = cpu->kcc_loaded;
			cpu->kcc_prevrounds = cpu->kcc_rounds;
		} else {
			SLIST_INSERT_HEAD(&cp->kc_emptydepot, cpu->kcc_loaded, km_entry);
		}

		mag = cpu->kcc_loaded = SLIST_FIRST(&cp->kc_fulldepot);
		SLIST_REMOVE_HEAD(&cp->kc_fulldepot, km_entry);
		cpu->kcc_rounds = mag->km_rounds;

		goto alloc_loaded;
	}

	cpu->kcc_stats.kcs_magmiss++;

	slab = cp->kc_freeslab;

	/* There is no free slab. Allocate one */
	if (slab == NULL) {
		slab = kmem_alloc_slab(cp, flags);
		if (slab == NULL)
			return NULL;

		TAILQ_INSERT_TAIL(&cp->kc_slabs, slab, ks_entry);
		if (cp->kc_freeslab == NULL)
			cp->kc_freeslab = slab;
	}

	if (cp->kc_pages > 1) {
		struct kmem_bufctl *bufctl;

		bufctl = SLIST_FIRST(&slab->ks_freebufs);
		obj = bufctl->kb_buf;
		SLIST_REMOVE_HEAD(&slab->ks_freebufs, kb_entry);
		SLIST_INSERT_HEAD(&(*cp->kc_hashtab)[kmem_bufaddr_makehash(bufctl->kb_buf)], bufctl, kb_entry);
	} else {
		obj = (char *)SLIST_FIRST(&slab->ks_freebufs) - cp->kc_realsize +
			sizeof(struct kmem_bufctl_inline);
		SLIST_REMOVE_HEAD(&slab->ks_freebufs, kb_entry);
	}

	if (SLIST_EMPTY(&slab->ks_freebufs)) {
		/*
		 * We drained this slab, so move it to the right
		 * position.
		 */
		cp->kc_freeslab = TAILQ_NEXT(slab, ks_entry);
		TAILQ_REMOVE(&cp->kc_slabs, slab, ks_entry);
		TAILQ_INSERT_HEAD(&cp->kc_slabs, slab, ks_entry);
	}
	slab->ks_refcnt++;

	/* Construct the object, if needed. */
	if (cp->kc_ctor != NULL)
		cp->kc_ctor(obj, cp->kc_size);

	return obj;
}

static void
kmem_empty_magazine(struct kmem_cache *cp, struct kmem_magazine *mag)
{
	while (mag->km_rounds)
		kmem_returnto_slab(cp, mag->km_round[--mag->km_rounds]);
}

static void
kmem_returnto_slab(struct kmem_cache *cp, void *obj)
{
	struct kmem_slab *slab, *nextslab;
	struct kmem_bufctl *bufctl;

	if (cp->kc_pages > 1) {
		kmem_hashentry *hashhead;
		struct kmem_bufctl *obufctl;

		hashhead = &(*cp->kc_hashtab)[kmem_bufaddr_makehash(obj)];
		obufctl = NULL;
		bufctl = SLIST_FIRST(hashhead);
		while (bufctl != NULL && bufctl->kb_buf != obj) {
			obufctl = bufctl;
			bufctl = SLIST_NEXT(bufctl, kb_entry);
		}

		KKASSERT((bufctl != NULL));

		SLIST_REMOVE_AFTER(hashhead, obufctl, kb_entry);

		slab = bufctl->kb_slab;
	} else {
		slab = (struct kmem_slab *)(((unsigned long)obj & ~(PAGESIZ - 1))
			+ PAGESIZ - sizeof(struct kmem_slab));
		bufctl = obj + cp->kc_realsize - sizeof(struct kmem_bufctl_inline);
	}

	SLIST_INSERT_HEAD(&slab->ks_freebufs, bufctl, kb_entry);
	slab->ks_refcnt--;

	nextslab = TAILQ_NEXT(slab, ks_entry);
	if (slab->ks_refcnt == 0 && nextslab && nextslab->ks_refcnt > 0) {
		/*
		 * If the slab is full again and isn't already at the
		 * end of the queue, then move it there.
		 */
		TAILQ_REMOVE(&cp->kc_slabs, slab, ks_entry);
		TAILQ_INSERT_TAIL(&cp->kc_slabs, slab, ks_entry);
	} else if (slab->ks_refcnt + 1 == cp->kc_bufs) {
		/*
		 * If the slab used to be empty, we need to
		 * move it to the "partly full" area.
		 */
		TAILQ_REMOVE(&cp->kc_slabs, slab, ks_entry);
		if (cp->kc_freeslab != NULL)
			TAILQ_INSERT_AFTER(&cp->kc_slabs, cp->kc_freeslab, slab, ks_entry);
		else
			TAILQ_INSERT_TAIL(&cp->kc_slabs, slab, ks_entry);
	}

	if (cp->kc_freeslab == NULL)
		cp->kc_freeslab = slab;
}

void
kmem_cache_free(struct kmem_cache *cp, void *obj)
{
	struct kmem_cpu_cache *cpu;
	struct kmem_magazine *mag;

	cpu = &cp->kc_cpu[0];	/* XXX use cpu number */

	/*
	 * If there is still space in the loaded magazine,
	 * put the round in it.
	 * Do an unsigned comparison so that the sign for
	 * non-allocated magazine (rounds == -1) won't match.
	 */
	if ((unsigned)cpu->kcc_rounds < (unsigned)cpu->kcc_magsize) {
		mag = cpu->kcc_loaded;

free_loaded:
		mag->km_round[cpu->kcc_rounds++] = obj;
		return;
	}

	/*
	 * The loaded magazine is full, so exchange it with the
	 * previous one if this exists and is empty.
	 */
	if (cpu->kcc_prevrounds == 0) {
		cpu->kcc_prevrounds = cpu->kcc_rounds;
		cpu->kcc_rounds = 0;

		mag = cpu->kcc_previous;
		cpu->kcc_previous = cpu->kcc_loaded;
		cpu->kcc_loaded = mag;

		goto free_loaded;
	}

	/*
	 * Both magazines are either full or not allocated. Try to
	 * fetch an empty one from the depot.
	 */
	if (!SLIST_EMPTY(&cp->kc_emptydepot)) {
free_depot:
		/*
		 * If the previous magazine is not allocated, the loaded
		 * could also not be allocated. In both cases just put loaded
		 * into the free previous slot.
		 * If the previous magazine is allocated, loaded is too.
		 */
		if (cpu->kcc_previous == NULL) {
			cpu->kcc_previous = cpu->kcc_loaded;
			cpu->kcc_prevrounds = cpu->kcc_rounds;
		} else {
			cpu->kcc_loaded->km_rounds = cpu->kcc_rounds;
			SLIST_INSERT_HEAD(&cp->kc_fulldepot, cpu->kcc_loaded, km_entry);
		}

		mag = cpu->kcc_loaded = SLIST_FIRST(&cp->kc_emptydepot);
		SLIST_REMOVE_HEAD(&cp->kc_emptydepot, km_entry);
		cpu->kcc_rounds = 0;

		goto free_loaded;
	}

	cpu->kcc_stats.kcs_magmiss++;

	/*
	 * Try to allocate a new empty magazine. If possible, add it
	 * to the depot and start over.
	 */
	mag = kmem_cache_alloc(mag_cch, 0);	/* XXX flags */
	if (mag != NULL) {
		SLIST_INSERT_HEAD(&cp->kc_emptydepot, mag, km_entry);

		goto free_depot;
	}

	kmem_returnto_slab(cp, obj);
}
