/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2013 Rob Clark <robclark@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <freedreno_drmif.h>
#include <freedreno_ringbuffer.h>

#include "util.h"
#include "ring.h"
#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"

/* simple test for submit ioctl handling, in particular range checking
 * on cmdstream buffer
 */

int main(int argc, char *argv[])
{
	struct fd_device *dev;
	struct fd_pipe *pipe;
	struct fd_ringbuffer *ring;
	struct fd_ringmarker *start, *end;
	uint32_t i = 0;
	int fd, ret;

	fd = drmOpen("msm", NULL);
	if (fd < 0) {
		printf("failed to initialize DRM\n");
		return fd;
	}

	dev = fd_device_new(fd);
	if (!dev) {
		printf("failed to initialize freedreno device\n");
		return -1;
	}

	pipe = fd_pipe_new(dev, FD_PIPE_3D);
	if (!pipe) {
		printf("failed to initialize freedreno pipe\n");
		return -1;
	}

	ring = fd_ringbuffer_new(pipe, 4096);
	if (!ring) {
		printf("failed to initialize freedreno ring\n");
		return -1;
	}

	start = fd_ringmarker_new(ring);
	end = fd_ringmarker_new(ring);

	/* setup end of ring with a CP_NOP packet extending beyond
	 * the end of the ringbuffer..  CP ignores payload, so this
	 * should be a safe way to test the bounds checking.  We
	 * have to frob the rb a bit, since we are intentionally
	 * misusing the libdrm_freedreno API to do this:
	 */
	ring->cur = ring->end - 4;
	fd_ringmarker_mark(start);

	OUT_PKT3(ring, CP_NOP, 10);
	ring->cur += 10;
	fd_ringmarker_mark(end);

	/* Test 1: first level IB check: */
	fd_ringmarker_flush(start);
	sleep(1);

	/* Test 2: second level IB check: */
	ring->cur = ring->last_start = ring->start;
	OUT_IB(ring, start, end);
	fd_ringbuffer_flush(ring);
	sleep(1);

	return 0;
}
