// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <string.h>
#include "t70-h4-rx.h"

struct log {
	unsigned char type[8], frames[8][T70_H4_FRAME_CAP];
	size_t size[8], count;
	int error;
};
static int capture(void *opaque, unsigned char type, const unsigned char *buf, size_t n)
{
	struct log *l = opaque;

	assert(l->count < 8 && n <= T70_H4_FRAME_CAP);
	l->type[l->count] = type;
	l->size[l->count] = n;
	memcpy(l->frames[l->count++], buf, n);
	return l->error;
}
int main(void)
{
	struct t70_h4_rx rx = {0};
	struct log l = {0};
	size_t used;
	const unsigned char frame[] = {4, 0x10, 1, 0, 3, 0, 0, 1, 0x55};
	const unsigned char oversize[] = {2, 0, 0, 1, 4, 0};
	const unsigned char invalid[] = {5, 4, 0x10, 1, 0};
	unsigned char maximum[T70_H4_FRAME_CAP + 1] = {2, 0, 0, 0, 4};

	for (size_t split = 0; split <= sizeof(frame); split++) {
		t70_h4_reset(&rx);
		l.count = 0;
		assert(!t70_h4_feed(&rx, frame, split, &used, capture, &l) && used == split);
		assert(!t70_h4_feed(&rx, frame + split, sizeof(frame) - split, &used, capture, &l));
		assert(used == sizeof(frame) - split && l.count == 2);
		assert(l.type[0] == 4 && l.size[0] == 3 && !memcmp(l.frames[0], frame + 1, 3));
		assert(l.type[1] == 3 && l.size[1] == 4 && !memcmp(l.frames[1], frame + 5, 4));
	}
	l.count = 0;
	assert(t70_h4_feed(&rx, oversize, sizeof(oversize), &used, capture, &l) == -EMSGSIZE);
	assert(used == 5 && !l.count && !rx.header);
	assert(t70_h4_feed(&rx, invalid, sizeof(invalid), &used, capture, &l) == -EPROTO);
	assert(used == 1 && !rx.header && !l.count);
	assert(!t70_h4_feed(&rx, maximum, sizeof(maximum), &used, capture, &l));
	assert(used == sizeof(maximum) && l.count == 1 && l.size[0] == T70_H4_FRAME_CAP);
	l.error = -EAGAIN;
	assert(t70_h4_feed(&rx, frame, sizeof(frame), &used, capture, &l) == -EAGAIN);
	assert(used == 4 && !rx.header);
	l.error = 0;
	assert(!t70_h4_feed(&rx, frame + used, sizeof(frame) - used, &used, capture, &l));
	assert(l.type[2] == 3 && l.count == 3);
	return 0;
}
