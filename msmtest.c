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

static struct {
	int fd;
	drmModeModeInfo *mode;
	uint32_t crtc_id;
	uint32_t connector_id;
} drm;

struct drm_fb {
	struct fd_bo *bo;
	uint32_t fb_id;
	uint32_t width, height, stride;
};

static struct drm_fb *drm_fb_new(struct fd_device *dev,
		uint32_t width, uint32_t height)
{
	struct drm_fb *fb = calloc(1, sizeof(*fb));
	int ret;

	fb->width  = width;
	fb->height = height;
	fb->stride = width * 4;

	fb->bo = fd_bo_new(dev, fb->height * fb->stride, 0);

	ret = drmModeAddFB(drm.fd, width, height, 24, 32, fb->stride,
			fd_bo_handle(fb->bo), &fb->fb_id);
	if (ret) {
		printf("failed to create fb: %s\n", strerror(errno));
		fd_bo_del(fb->bo);
		free(fb);
		return NULL;
	}

	return fb;
}

static void drm_fb_del(struct drm_fb *fb)
{
	if (fb->fb_id)
		drmModeRmFB(drm.fd, fb->fb_id);
	if (fb->bo)
		fd_bo_del(fb->bo);
	free(fb);
}

static int init_drm(void)
{
	static const char *modules[] = {
			"msm", "i915", "radeon", "nouveau", "vmwgfx", "omapdrm", "exynos"
	};
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	int i, area;

	for (i = 0; i < ARRAY_SIZE(modules); i++) {
		printf("trying to load module %s...", modules[i]);
		drm.fd = drmOpen(modules[i], NULL);
		if (drm.fd < 0) {
			printf("failed.\n");
		} else {
			printf("success.\n");
			break;
		}
	}

	if (drm.fd < 0) {
		printf("could not open drm device\n");
		return -1;
	}

	resources = drmModeGetResources(drm.fd);
	if (!resources) {
		printf("drmModeGetResources failed: %s\n", strerror(errno));
		return -1;
	}

	/* find a connected connector: */
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			/* it's connected, let's use this! */
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		printf("no connected connector!\n");
		return -1;
	}

	/* find highest resolution mode: */
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];
		int current_area = current_mode->hdisplay * current_mode->vdisplay;
		if (current_area > area) {
			drm.mode = current_mode;
			area = current_area;
		}
	}

	if (!drm.mode) {
		printf("could not find mode!\n");
		return -1;
	}

	/* find encoder: */
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (!encoder) {
		printf("no encoder!\n");
		return -1;
	}

	drm.crtc_id = encoder->crtc_id;
	drm.connector_id = connector->connector_id;

	return 0;
}

static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

int main(int argc, char *argv[])
{
	fd_set fds;
	drmEventContext evctx = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
	};
	struct fd_device *dev;
	struct fd_pipe *pipe;
	struct fd_ringbuffer *ring;
	struct drm_fb *fb;
	uint64_t val;
	uint32_t gpu_id, i = 0;
	int ret;

	ret = init_drm();
	if (ret) {
		printf("failed to initialize DRM\n");
		return ret;
	}

	FD_ZERO(&fds);
	FD_SET(0, &fds);
	FD_SET(drm.fd, &fds);

	dev = fd_device_new(drm.fd);
	if (!dev) {
		printf("failed to initialize freedreno device\n");
		return -1;
	}

	pipe = fd_pipe_new(dev, FD_PIPE_3D);
	if (!pipe) {
		printf("failed to initialize freedreno pipe\n");
		return -1;
	}

	if (fd_pipe_get_param(pipe, FD_GPU_ID, &val)) {
		printf("could not get gpu-id\n");
		return -1;
	}
	gpu_id = val;

	ring = fd_ringbuffer_new(pipe, 0x10000);
	if (!ring) {
		printf("failed to initialize freedreno ring\n");
		return -1;
	}

	fb = drm_fb_new(dev, drm.mode->hdisplay, drm.mode->vdisplay);
	if (!fb) {
		printf("failed to create scanout buffer\n");
		return -1;
	}

	/* set mode: */
	ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
			&drm.connector_id, 1, drm.mode);
	if (ret) {
		printf("failed to set mode: %s\n", strerror(errno));
		return ret;
	}

	/* something simple.. try to write some data into the buffer: */
	for (i = 0; i < 32 /*fb->height*/; i++) {
		uint32_t sizedwords = 256;
		if (gpu_id >= 500) {
			OUT_PKT7(ring, CP_MEM_WRITE, sizedwords+2);
			OUT_RELOC64W(ring, fb->bo, i * fb->stride, 0, 0);
		} else {
			OUT_PKT3(ring, CP_MEM_WRITE, sizedwords+1);
			OUT_RELOCW(ring, fb->bo, i * fb->stride, 0, 0);
		}
		while (sizedwords--) {
			OUT_RING(ring,
				(sizedwords << 24) |
				(sizedwords << 16) |
				(sizedwords <<  8) |
				(sizedwords <<  0));
		}
	}

	fd_ringbuffer_flush(ring);

	sleep(20);

	drm_fb_del(fb);

	return 0;
}
