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

#include <freedreno_drmif.h>
#include <freedreno_ringbuffer.h>

#include "util.h"
#include "ring.h"
#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"

int main(int argc, char *argv[])
{
	struct fd_device *dev;
	struct fd_pipe *pipe;
	struct fd_ringbuffer *ring;
	struct fd_bo *bo;
	uint32_t i = 0;
	uint32_t *ptr;
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

	bo = fd_bo_new(dev, 4096, 0);

#define BASE REG_A3XX_GRAS_CL_VPORT_XOFFSET
#define SIZE 6

	OUT_PKT0(ring, REG_AXXX_CP_SCRATCH_REG4, 1);
	OUT_RING(ring, 0x123);

	OUT_PKT0(ring, BASE, SIZE);
	for (i = 0; i < SIZE; i++)
		OUT_RING(ring, i);

	/* this adds the value of CP_SCRATCH_REG4 to 0x111 and writes
	 * to REG_BASE+2
	 */
	OUT_PKT3(ring, CP_SET_CONSTANT, 3);
	OUT_RING(ring, 0x80000000 | CP_REG(BASE + 2));
	OUT_RING(ring, REG_AXXX_CP_SCRATCH_REG4);
	OUT_RING(ring, 0x111);

	/* read back all the regs: */
	for (i = 0; i < SIZE; i++) {
		OUT_PKT3(ring, CP_REG_TO_MEM, 2);
		OUT_RING(ring, BASE + i);
		OUT_RELOCW(ring, bo, i * 4, 0, 0);
	}

	fd_ringbuffer_flush(ring);

	/* and read back the values: */
	fd_bo_cpu_prep(bo, pipe, DRM_FREEDRENO_PREP_READ);
	ptr = fd_bo_map(bo);
	for (i = 0; i < SIZE; i++) {
		printf("%02x: %08x\n", i, ptr[i]);
	}
	fd_bo_cpu_fini(bo);

	return 0;
}
