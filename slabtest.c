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
 * $TheBOFH: slaballoc/slabtest.c,v 1.9 2004/12/24 09:09:45 corecode Exp $
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/queue.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "alloc.h"


struct cache_info {
	struct kmem_cache *cache;
	size_t		size;
	unsigned long	count;
	unsigned long	num;
	char		*name;
};

struct testitem {
	TAILQ_ENTRY(testitem) entry;
	struct cache_info *cache;
	unsigned long	seq;
	char		fill;
	char		data[0];
};


int verbose;
unsigned long count, seq;
unsigned long iterations, cachecnt;
long randseed;
struct cache_info chs[15];
TAILQ_HEAD(, testitem) items;


struct test_set {
	const char	*name;
	void		(*init)(void);
	void		*(*cache_init)(const char *, size_t);
	void		(*free)(void *);
	void		*(*alloc)(struct cache_info *);
	void		(*stats)(struct cache_info *);
	void		(*cleanup)(struct cache_info *);
};

void
test_kmem_init(void)
{
	kmem_init();
}

void *
test_kmem_cache_init(const char *name, size_t size)
{
	return kmem_cache_create(name, size, 0, NULL, NULL);
}

void
test_kmem_free(void *obj)
{
	struct testitem *itm = obj;

	kmem_cache_free(itm->cache->cache, itm);
}

void *
test_kmem_alloc(struct cache_info *cache)
{
	return kmem_cache_alloc(cache->cache, 0);
}

void
test_kmem_stats(struct cache_info *chs)
{
	struct kmem_cache_stats complete, s;
	int i;

	complete.kcs_allocs = complete.kcs_misses = 0;

	for (i = 0; i < 15; ++i) {
		kmem_cache_debug(chs[i].cache);

		kmem_cache_getstats(chs[i].cache, &s);
		complete.kcs_allocs += s.kcs_allocs;
		complete.kcs_misses += s.kcs_misses;
	}
	printf("\ntotal %i/%i=%i%%\n", complete.kcs_misses, complete.kcs_allocs,
		complete.kcs_misses * 100 / complete.kcs_allocs);
}

void
test_kmem_cleanup(struct cache_info *cache)
{
	kmem_cache_destroy(cache->cache);
}

struct test_set kmem_set = {
	"kmem_cache",
	test_kmem_init,
	test_kmem_cache_init,
	test_kmem_free,
	test_kmem_alloc,
	test_kmem_stats,
	test_kmem_cleanup
};

void
test_null(void)
{
}

void
test_malloc_free(void *obj)
{
	free(obj);
}

void *
test_malloc_alloc(struct cache_info *cache)
{
	return malloc(cache->size);
}

struct test_set malloc_set = {
	"malloc",
	NULL,
	NULL,
	test_malloc_free,
	test_malloc_alloc,
	NULL,
	NULL
};


void
do_test_free(struct testitem *itm, struct test_set *set)
{
	size_t i;
	char *t;

	if (verbose >= 2) {
		printf("%4lu %lu dealloc %lu %lu\n", itm->seq, count - 1, itm->cache->num, itm->cache->count - 1);

		for (i = itm->cache->size - sizeof(struct testitem), t = itm->data; i; --i, ++t) {
			if (*t != (char)itm->seq) {
				fprintf(stderr, "corrupted block %lu!", itm->seq);
				abort();
			}
		}
	}

	TAILQ_REMOVE(&items, itm, entry);
	count--;
	itm->cache->count--;
	set->free(itm);
}

void
do_test_alloc(struct cache_info *cache, struct test_set *set)
{
	struct testitem *itm;

	if (verbose >= 2)
		printf("%4lu %lu alloc %lu %lu\n", seq, count + 1, cache->num, cache->count + 1);

	itm = set->alloc(cache);
	itm->cache = cache;
	itm->seq = seq++;
	itm->cache->count++;
	count++;

	if (verbose >= 2)
		memset(itm->data, (char)itm->seq, itm->cache->size - sizeof(struct testitem));
	TAILQ_INSERT_HEAD(&items, itm, entry);
}

void do_test(struct test_set *set)
{
	struct timeval t_start, t_end;
	struct testitem *itm;
	double timediff;
	long i;

	printf("testing %s\n", set->name);

	if (set->init)
		set->init();

	srandom(randseed);

	for (i = 0; i < cachecnt; ++i) {
		char *name;
		size_t size;

		if (asprintf(&name, "testcache%li", i) < 0)
			err(1, "asprintf");

		size = sizeof(struct testitem) + random() % 400 + (random() % 4 > 2) * random() % 8000;
		chs[i].cache = set->cache_init ? set->cache_init(name, size) : NULL;
		chs[i].size = size;
		chs[i].count = 0;
		chs[i].num = i;
		chs[i].name = name;
		if (verbose >= 2)
			printf("%s %lu\n", name, size);
	}

	TAILQ_INIT(&items);
	count = seq = 0;
	gettimeofday(&t_start, NULL);
	for (; seq < iterations;) {
		if (random() % 2 && count > 20) {
			struct testitem *nextitm;

			for(itm = TAILQ_FIRST(&items);
			    (random() % 4 != 0) && (nextitm = TAILQ_NEXT(itm, entry)) != NULL;
			    itm = nextitm)
				/* NOTHING */;

			do_test_free(itm, set);
		} else {
			i = random() % cachecnt;

			do_test_alloc(&chs[i], set);
		}
	}

	if (verbose && set->stats)
		set->stats(chs);

	while ((itm = TAILQ_FIRST(&items)) != NULL)
		do_test_free(itm, set);

	gettimeofday(&t_end, NULL);
	timersub(&t_end, &t_start, &t_end);
	timediff = (double)t_end.tv_sec + (double)t_end.tv_usec / 1000000;
	printf("%s took %g sec, that is %g us average per alloc/dealloc\n",
	    set->name, timediff, timediff / iterations * 1000000);

	for (i = 0; i < cachecnt; ++i) {
		if (set->cleanup)
			set->cleanup(&chs[i]);

		free(chs[i].name);
	}
}


int
main(int argc, char **argv)
{
	int ch;
	int runmalloc, runplain, runslab;

	cachecnt = 15;
	iterations = 10000;
	verbose = 0;
	runmalloc = 1;
	runplain = 0;
	runslab = 1;
	randseed = 1;

	while ((ch = getopt(argc, argv, "c:Mn:pr:Sv")) != -1) {
		switch (ch) {
		case 'c':
			cachecnt = strtol(optarg, &optarg, 10);
			if (*optarg != '\0')
				errx(1, "invalid parameter to -c");
			break;
		case 'M':
			runmalloc = 0;
			break;
		case 'n':
			iterations = strtol(optarg, &optarg, 10);
			if (*optarg != '\0')
				errx(1, "invalid parameter to -n");
			break;
		case 'p':
			runplain = 1;
			break;
		case 'r':
			randseed = strtol(optarg, &optarg, 10);
			if (*optarg != '\0')
				errx(1, "invalid parameter to -r");
			break;
		case 'S':
			runslab = 0;
			break;
		case 'v':
			verbose++;
			break;
		default:
			errx(1, "unknown parameter `%s'", optarg);
			break;
		}
	}

	if (runslab)
		do_test(&kmem_set);

	if (runmalloc)
		do_test(&malloc_set);

	return 0;
}
