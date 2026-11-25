// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include "himax_release.h"

int main(void)
{
	struct t70_hx_release_state s = {0};

	assert(t70_hx_bad(&s) == T70_HX_NONE);
	t70_hx_valid(&s, 1);
	assert(t70_hx_timeout(&s) == T70_HX_NONE); /* stationary good touch */
	assert(t70_hx_bad(&s) == T70_HX_ARM_TIMEOUT);
	assert(t70_hx_timeout(&s) == T70_HX_RELEASE); /* isolated bad frame */
	assert(t70_hx_timeout(&s) == T70_HX_NONE); /* no duplicate release */

	t70_hx_valid(&s, 1);
	assert(t70_hx_bad(&s) == T70_HX_ARM_TIMEOUT);
	assert(t70_hx_bad(&s) == T70_HX_NONE);
	assert(t70_hx_bad(&s) == T70_HX_RELEASE);
	assert(t70_hx_timeout(&s) == T70_HX_NONE);

	t70_hx_valid(&s, 2);
	assert(t70_hx_bad(&s) == T70_HX_ARM_TIMEOUT);
	t70_hx_valid(&s, 1); /* subsequent valid report cancels old release */
	assert(t70_hx_timeout(&s) == T70_HX_NONE);
	assert(t70_hx_bad(&s) == T70_HX_ARM_TIMEOUT);
	t70_hx_valid(&s, 0);
	assert(t70_hx_timeout(&s) == T70_HX_NONE);
	return 0;
}
