/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T70_HIMAX_RELEASE_H
#define T70_HIMAX_RELEASE_H

#define T70_HX_BAD_FRAME_LIMIT 3
#define T70_HX_BAD_FRAME_TIMEOUT_MS 120

/* Used with the input report lock held. No timer for valid stationary touch. */
struct t70_hx_release_state {
	unsigned int active;
	unsigned int bad_frames;
};

enum t70_hx_release_action {
	T70_HX_NONE,
	T70_HX_ARM_TIMEOUT,
	T70_HX_RELEASE
};

static inline void t70_hx_valid(struct t70_hx_release_state *s, unsigned int active)
{
	s->active = active != 0;
	s->bad_frames = 0;
}

static inline enum t70_hx_release_action
t70_hx_bad(struct t70_hx_release_state *s)
{
	if (!s->active)
		return T70_HX_NONE;
	if (++s->bad_frames >= T70_HX_BAD_FRAME_LIMIT) {
		t70_hx_valid(s, 0);
		return T70_HX_RELEASE;
	}
	return s->bad_frames == 1 ? T70_HX_ARM_TIMEOUT : T70_HX_NONE;
}

static inline enum t70_hx_release_action
t70_hx_timeout(struct t70_hx_release_state *s)
{
	if (!s->active || !s->bad_frames)
		return T70_HX_NONE;
	t70_hx_valid(s, 0);
	return T70_HX_RELEASE;
}
#endif
