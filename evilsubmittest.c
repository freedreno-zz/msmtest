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

#define __user
#define U642VOID(x) ((void *)(unsigned long)(x))
#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

#include "msm_drm.h"

#include "util.h"
#include "ring.h"
#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"

/* test for args->bos which would fault under spinlock:
 */

static int test_invalid_submit(int fd, struct fd_device *dev);

int main(int argc, char *argv[])
{
	struct fd_device *dev;
	struct fd_pipe *pipe;
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

	printf("Test 1: invalid submit:\n");
	test_invalid_submit(fd, dev);
	sleep(1);

	return 0;
}

static int test_invalid_submit(int fd, struct fd_device *dev)
{
	struct fd_bo *bo = fd_bo_new(dev, 0x1000, 0);
	struct fd_bo *cmd = fd_bo_new(dev, 0x1000, 0);
	/* use an unmapped but un-faulted-in bo for usersace vaddr for bos
	 * table.. this should trigger a pagefault in kernel in copy_from_user()
	 */
	struct fd_bo *bogus_bos = fd_bo_new(dev, 0x1000, 0);
	struct drm_msm_gem_submit_reloc relocs[2] = {
		[0] = {
			.submit_offset = 4 * 1,/* cmdbuf[1] */
			.reloc_idx  = 1,       /* bos[1] */
		},
		[1] = {
			.submit_offset = 4 * 2,/* cmdbuf[2] */
			.reloc_idx  = 1,       /* bos[2] */
		},
	};
	struct drm_msm_gem_submit_cmd cmds[1] = {
		[0] = {
			.type       = MSM_SUBMIT_CMD_BUF,
			.submit_idx = 0,      /* bos[0] */
			.size       = 4 * 4,  /* 4 dwords in cmdbuf */
			.nr_relocs  = ARRAY_SIZE(relocs),
			.relocs     = VOID2U64(relocs),
		},
	};
	struct drm_msm_gem_submit req = {
			.pipe       = MSM_PIPE_3D0,
			.nr_bos     = 3,
			.bos        = VOID2U64(fd_bo_map(bogus_bos)),
			.nr_cmds    = ARRAY_SIZE(cmds),
			.cmds       = VOID2U64(cmds),
	};
	uint32_t *cmdbuf = fd_bo_map(cmd);

	uint32_t name;
	fd_bo_get_name(bo, &name);

	/* CP_NOP packet, payload length 3:
	 * (use a no-op packet so gpu will ignore)
	 */
	cmdbuf[0] = CP_TYPE3_PKT | ((3-1) << 16) | ((CP_NOP & 0xff) << 8);
	cmdbuf[1] = 0;      /* reloc[0] */
	cmdbuf[2] = 0;      /* reloc[1] */
	cmdbuf[3] = 0;      /* unused */

	return drmCommandWriteRead(fd, DRM_MSM_GEM_SUBMIT, &req, sizeof(req));
}
