/* Dhara - NAND flash management layer
 * Copyright (C) 2013 Daniel Beer <dlbeer@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sim.h"
#include "seq.h"

#define LOG2_PAGE_SIZE		9
#define LOG2_PAGES_PER_BLOCK	3
#define LOG2_BLOCK_SIZE		(LOG2_PAGE_SIZE + LOG2_PAGES_PER_BLOCK)
#define NUM_BLOCKS		113

#define PAGE_SIZE		(1 << LOG2_PAGE_SIZE)
#define PAGES_PER_BLOCK		(1 << LOG2_PAGES_PER_BLOCK)
#define BLOCK_SIZE		(1 << LOG2_BLOCK_SIZE)
#define MEM_SIZE		(NUM_BLOCKS * BLOCK_SIZE)

const struct Dhara_NAND sim_nand = {
	.log2_page_size		= LOG2_PAGE_SIZE,
	.log2_ppb		= LOG2_PAGES_PER_BLOCK,
	.num_blocks		= NUM_BLOCKS
};

#define BLOCK_BAD_MARK		0x01
#define BLOCK_FAILED		0x02

/* Call counts */
struct sim_stats {
	int		is_bad;
	int		mark_bad;

	int		erase;
	int		erase_fail;

	int		is_erased;
	int		prog;
	int		prog_fail;

	int		read;
	int		read_bytes;
};

struct block_status {
	int		flags;

	/* Index of the next unprogrammed page. 0 means a fully erased
	 * block, and PAGES_PER_BLOCK is a fully programmed block.
	 */
	int		next_page;

	/* Timebomb counter: if non-zero, this is the number of
	 * operations until permanent failure.
	 */
	int		timebomb;
};

static struct sim_stats stats;
static struct block_status blocks[NUM_BLOCKS];
static uint8_t pages[MEM_SIZE];

static void timebomb_tick(Dhara_block_t blk)
{
	struct block_status *b = &blocks[blk];

	if (b->timebomb) {
		b->timebomb--;
		if (!b->timebomb)
			b->flags |= BLOCK_FAILED;
	}
}

int Dhara_NAND_is_bad(const struct Dhara_NAND *n, Dhara_block_t bno)
{
	if (bno >= NUM_BLOCKS) {
		fprintf(stderr, "sim: NAND_is_bad called on "
			"invalid block: %d\n", bno);
		abort();
	}

	stats.is_bad++;
	return blocks[bno].flags & BLOCK_BAD_MARK;
}

void Dhara_NAND_mark_bad(const struct Dhara_NAND *n, Dhara_block_t bno)
{
	if (bno >= NUM_BLOCKS) {
		fprintf(stderr, "sim: NAND_mark_bad called on "
			"invalid block: %d\n", bno);
		abort();
	}

	stats.mark_bad++;
	blocks[bno].flags |= BLOCK_BAD_MARK;
}

int Dhara_NAND_erase(const struct Dhara_NAND *n, Dhara_block_t bno,
		     Dhara_error_t *err)
{
	uint8_t *blk = pages + (bno << LOG2_BLOCK_SIZE);

	if (bno >= NUM_BLOCKS) {
		fprintf(stderr, "sim: NAND_erase called on "
			"invalid block: %d\n", bno);
		abort();
	}

	if (blocks[bno].flags & BLOCK_BAD_MARK) {
		fprintf(stderr, "sim: NAND_erase called on "
			"block which is marked bad: %d\n", bno);
		abort();
	}

	stats.erase++;
	blocks[bno].next_page = 0;

	timebomb_tick(bno);

	if (blocks[bno].flags & BLOCK_FAILED) {
		stats.erase_fail++;
		seq_gen(bno * 57 + 29, blk, BLOCK_SIZE);
		Dhara_set_error(err, DHARA_E_BAD_BLOCK);
		return -1;
	}

	memset(blk, 0xff, BLOCK_SIZE);
	return 0;
}

int Dhara_NAND_prog(const struct Dhara_NAND *n, Dhara_page_t p,
		    const uint8_t *data, Dhara_error_t *err)
{
	const int bno = p >> LOG2_PAGES_PER_BLOCK;
	const int pno = p & ((1 << LOG2_PAGES_PER_BLOCK) - 1);
	uint8_t *page = pages + (p << LOG2_PAGE_SIZE);

	if ((bno < 0) || (bno >= NUM_BLOCKS)) {
		fprintf(stderr, "sim: NAND_prog called on "
			"invalid block: %d\n", bno);
		abort();
	}

	if (blocks[bno].flags & BLOCK_BAD_MARK) {
		fprintf(stderr, "sim: NAND_prog called on "
			"block which is marked bad: %d\n", bno);
		abort();
	}

	if (pno < blocks[bno].next_page) {
		fprintf(stderr, "sim: NAND_prog: out-of-order "
			"page programming. Block %d, page %d "
			"(expected %d)\n",
			bno, pno, blocks[bno].next_page);
		abort();
	}

	stats.prog++;
	blocks[bno].next_page = pno + 1;

	timebomb_tick(bno);

	if (blocks[bno].flags & BLOCK_FAILED) {
		stats.prog_fail++;
		seq_gen(p * 57 + 29, page, PAGE_SIZE);
		Dhara_set_error(err, DHARA_E_BAD_BLOCK);
		return -1;
	}

	memcpy(page, data, PAGE_SIZE);
	return 0;
}

int Dhara_NAND_is_free(const struct Dhara_NAND *n, Dhara_page_t p)
{
	const int bno = p >> LOG2_PAGES_PER_BLOCK;
	const int pno = p & ((1 << LOG2_PAGES_PER_BLOCK) - 1);

	if ((bno < 0) || (bno >= NUM_BLOCKS)) {
		fprintf(stderr, "sim: NAND_is_free called on "
			"invalid block: %d\n", bno);
		abort();
	}

	stats.is_erased++;
	return blocks[bno].next_page <= pno;
}

int Dhara_NAND_read(const struct Dhara_NAND *n, Dhara_page_t p,
		    size_t offset, size_t length,
		    uint8_t *data, Dhara_error_t *err)
{
	const int bno = p >> LOG2_PAGES_PER_BLOCK;
	uint8_t *page = pages + (p << LOG2_PAGE_SIZE);

	if ((bno < 0) || (bno >= NUM_BLOCKS)) {
		fprintf(stderr, "sim: NAND_prog called on "
			"invalid block: %d\n", bno);
		abort();
	}

	if ((offset > PAGE_SIZE) || (length > PAGE_SIZE) ||
	    (offset + length > PAGE_SIZE)) {
		fprintf(stderr, "sim: NAND_read called on "
			"invalid range: offset = %ld, length = %ld\n",
			offset, length);
		abort();
	}

	stats.read++;
	stats.read_bytes += length;

	memcpy(data, page + offset, length);
	return 0;
}

int Dhara_NAND_copy(const struct Dhara_NAND *n,
		    Dhara_page_t src, Dhara_page_t dst,
		    Dhara_error_t *err)
{
	uint8_t buf[PAGE_SIZE];

	if ((Dhara_NAND_read(n, src, 0, PAGE_SIZE, buf, err) < 0) ||
	    (Dhara_NAND_prog(n, dst, buf, err) < 0))
		return -1;

	return 0;
}

static char rep_status(const struct block_status *b)
{
	switch (b->flags & (BLOCK_FAILED | BLOCK_BAD_MARK)) {
	case BLOCK_FAILED:
		return 'b';

	case BLOCK_BAD_MARK:
		return '?';

	case BLOCK_BAD_MARK | BLOCK_FAILED:
		return 'B';
	}

	if (b->next_page)
		return ':';

	return '.';
}

void sim_set_failed(Dhara_block_t bno)
{
	blocks[bno].flags |= BLOCK_FAILED;
}

void sim_set_timebomb(Dhara_block_t bno, int ttl)
{
	blocks[bno].timebomb = ttl;
}

void sim_inject_bad(int count)
{
	int i;

	for (i = 0; i < count; i++) {
		const int bno = random() % NUM_BLOCKS;

		blocks[bno].flags |= BLOCK_BAD_MARK | BLOCK_FAILED;
	}
}

void sim_inject_failed(int count)
{
	int i;

	for (i = 0; i < count; i++)
		sim_set_failed(random() % NUM_BLOCKS);
}

void sim_inject_timebombs(int count, int max_ttl)
{
	int i;

	for (i = 0; i < count; i++)
		sim_set_timebomb(random() % NUM_BLOCKS,
				 random() % max_ttl + 1);
}

void sim_dump(void)
{
	int i;

	printf("NAND operation counts:\n");
	printf("    is_bad:         %d\n", stats.is_bad);
	printf("    mark_bad        %d\n", stats.mark_bad);
	printf("    erase:          %d\n", stats.erase);
	printf("    erase failures: %d\n", stats.erase_fail);
	printf("    is_erased:      %d\n", stats.is_erased);
	printf("    prog:           %d\n", stats.prog);
	printf("    prog failures:  %d\n", stats.prog_fail);
	printf("    read:           %d\n", stats.read);
	printf("    read (bytes):   %d\n", stats.read_bytes);
	printf("\n");

	printf("Block status:\n");
	i = 0;
	while (i < NUM_BLOCKS) {
		int j = NUM_BLOCKS - i;
		int k;

		if (j > 64)
			j = 64;

		printf("    ");
		for (k = 0; k < j; k++)
			fputc(rep_status(&blocks[i + k]), stdout);
		fputc('\n', stdout);

		i += j;
	}
}
