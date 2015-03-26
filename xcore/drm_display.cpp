/*
 * drm_display.cpp - drm display
 *
 *  Copyright (c) 2015 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: John Ye <john.ye@intel.com>
 */


#include "drm_display.h"
#include "drm_v4l2_buffer.h"
#include "drm_bo_buffer.h"

#define DEFAULT_DRM_DEVICE "i915"
#define DEFAULT_DRM_BATCH_SIZE 0x80000

namespace XCam {

SmartPtr<DrmDisplay> DrmDisplay::_instance(NULL);
Mutex DrmDisplay::_mutex;

static std::atomic<uint32_t> global_signal_index(0);

SmartPtr<DrmDisplay>
DrmDisplay::instance()
{
    SmartLock lock(_mutex);
    if (_instance.ptr())
        return _instance;
    _instance = new DrmDisplay;
    return _instance;
}

DrmDisplay::DrmDisplay(const char* module)
    : _module(NULL)
    , _fd (-1)
    , _buf_manager (NULL)
    , _crtc_index (-1)
    , _crtc_id (0)
    , _con_id (0)
    , _plane_id (0)
    , _connector (NULL)
    , _is_render_inited (false)
    , _format (0)
    , _width (0)
    , _height (0)
{
    xcam_mem_clear(&_compose);

    if (module)
        _module = strdup (module);
    else
        _module = strdup (DEFAULT_DRM_DEVICE);

    //_fd = drmOpenRender (128);
    _fd = drmOpen (_module, NULL);
    if (_fd < 0)
        XCAM_LOG_ERROR("failed to open drm device %s", _module);

    _buf_manager = drm_intel_bufmgr_gem_init (_fd, DEFAULT_DRM_BATCH_SIZE);
    drm_intel_bufmgr_gem_enable_reuse (_buf_manager);
}

DrmDisplay::~DrmDisplay()
{
    if (_buf_manager)
        drm_intel_bufmgr_destroy (_buf_manager);
    if (_fd > 0)
        drmClose(_fd);
    if (_module)
        xcam_free (_module);
};

XCamReturn
DrmDisplay::get_crtc(drmModeRes *res)
{
    _crtc_index = -1;

    for (int i = 0; i < res->count_crtcs; i++) {
        if (_crtc_id == res->crtcs[i]) {
            _crtc_index = i;
            break;
        }
    }
    XCAM_FAIL_RETURN(ERROR, _crtc_index != -1, XCAM_RETURN_ERROR_PARAM,
                     "CRTC %d not found", _crtc_id);

    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
DrmDisplay::get_connector(drmModeRes *res)
{
    XCAM_FAIL_RETURN(ERROR, res->count_connectors > 0, XCAM_RETURN_ERROR_PARAM,
                     "No connector found");
    _connector = drmModeGetConnector(_fd, _con_id);
    XCAM_FAIL_RETURN(ERROR, _connector, XCAM_RETURN_ERROR_PARAM,
                     "drmModeGetConnector failed: %s", strerror(errno));

    return XCAM_RETURN_NO_ERROR;
}


XCamReturn
DrmDisplay::get_plane()
{
    drmModePlaneResPtr planes = drmModeGetPlaneResources(_fd);
    XCAM_FAIL_RETURN(ERROR, planes, XCAM_RETURN_ERROR_PARAM,
                     "failed to query planes: %s", strerror(errno));

    drmModePlanePtr plane = NULL;
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        if (plane) {
            drmModeFreePlane(plane);
            plane = NULL;
        }
        plane = drmModeGetPlane(_fd, planes->planes[i]);
        XCAM_FAIL_RETURN(ERROR, plane, XCAM_RETURN_ERROR_PARAM,
                         "failed to query plane %d: %s", i, strerror(errno));

        if (plane->crtc_id || !(plane->possible_crtcs & (1 << _crtc_index))) {
            continue;
        }

        for (uint32_t j = 0; j < plane->count_formats; j++) {
            // found a plane matching the requested format
            if (plane->formats[j] == _format) {
                _plane_id = plane->plane_id;
                drmModeFreePlane(plane);
                drmModeFreePlaneResources(planes);
                return XCAM_RETURN_NO_ERROR;
            }
        }
    }

    if (plane)
        drmModeFreePlane(plane);

    drmModeFreePlaneResources(planes);

    return XCAM_RETURN_ERROR_PARAM;
}

XCamReturn
DrmDisplay::render_init (
    uint32_t con_id,
    uint32_t crtc_id,
    uint32_t width,
    uint32_t height,
    uint32_t format,
    const struct v4l2_rect* compose)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;

    if (is_render_inited ())
        return ret;

    _con_id = con_id;
    _crtc_id = crtc_id;
    _width = width;
    _height = height;
    _format = format;
    _compose = *compose;
    _crtc_index = -1;
    _plane_id = 0;
    _connector = NULL;

    drmModeRes *resource = drmModeGetResources(_fd);
    XCAM_FAIL_RETURN(ERROR, resource, XCAM_RETURN_ERROR_PARAM,
                     "failed to query Drm Mode resources: %s", strerror(errno));

    ret = get_crtc(resource);
    XCAM_FAIL_RETURN(ERROR, ret == XCAM_RETURN_NO_ERROR,
                     XCAM_RETURN_ERROR_PARAM,
                     "failed to get CRTC %s", strerror(errno));

    ret = get_connector(resource);
    XCAM_FAIL_RETURN(ERROR, ret == XCAM_RETURN_NO_ERROR,
                     XCAM_RETURN_ERROR_PARAM,
                     "failed to get connector %s", strerror(errno));

    ret = get_plane();
    XCAM_FAIL_RETURN(ERROR, ret == XCAM_RETURN_NO_ERROR,
                     XCAM_RETURN_ERROR_PARAM,
                     "failed to get plane with required format %s", strerror(errno));

    drmModeFreeResources(resource);

    _is_render_inited = true;
    return XCAM_RETURN_NO_ERROR;
}


SmartPtr<V4l2Buffer>
DrmDisplay::create_drm_buf (
    const struct v4l2_format &format,
    const uint32_t index,
    const enum v4l2_buf_type buf_type)
{
    struct drm_mode_create_dumb gem;
    struct drm_prime_handle prime;
    struct v4l2_buffer v4l2_buf;
    int ret = 0;

    xcam_mem_clear (&gem);
    xcam_mem_clear (&prime);
    xcam_mem_clear (&v4l2_buf);

    gem.width = format.fmt.pix.bytesperline;
    gem.height = format.fmt.pix.height;
    gem.bpp = 8;
    ret = xcam_device_ioctl (_fd, DRM_IOCTL_MODE_CREATE_DUMB, &gem);
    XCAM_ASSERT (ret >= 0);

    prime.handle = gem.handle;
    ret = xcam_device_ioctl (_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
    if (ret < 0) {
        XCAM_LOG_WARNING ("create drm failed on DRM_IOCTL_PRIME_HANDLE_TO_FD");
        return NULL;
    }

    v4l2_buf.index = index;
    v4l2_buf.type = buf_type;
    v4l2_buf.memory = V4L2_MEMORY_DMABUF;
    v4l2_buf.m.fd = prime.fd;
    v4l2_buf.length = XCAM_MAX (format.fmt.pix.sizeimage, gem.size); // todo check gem.size and format.fmt.pix.length
    XCAM_LOG_DEBUG ("create drm buffer size:%lld", gem.size);
    return new DrmV4l2Buffer (gem.handle, v4l2_buf, format, _instance);
}

XCamReturn
DrmDisplay::render_setup_frame_buffer (SmartPtr<VideoBuffer> &buf)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    VideoBufferInfo video_info = buf->get_video_info ();
    uint32_t fourcc = video_info.format;
    uint32_t fb_handle = 0;
    uint32_t bo_handle = 0;
    uint32_t bo_handles[4] = { 0 };
    FB fb;
    SmartPtr<V4l2BufferProxy> v4l2_proxy;
    SmartPtr<DrmBoBuffer> bo_buf;

    if (has_frame_buffer (buf))
        return XCAM_RETURN_NO_ERROR;

    v4l2_proxy = buf.dynamic_cast_ptr<V4l2BufferProxy> ();
    bo_buf = buf.dynamic_cast_ptr<DrmBoBuffer> ();
    if (v4l2_proxy.ptr ()) {
        struct drm_prime_handle prime;
        memset(&prime, 0, sizeof (prime));
        prime.fd = v4l2_proxy->get_v4l2_dma_fd();

        ret = (XCamReturn) xcam_device_ioctl(_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime);
        if (ret) {
            XCAM_LOG_WARNING("FD_TO_PRIME_HANDLE failed: %s", strerror(errno));
            return XCAM_RETURN_ERROR_IOCTL;
        }
        bo_handle = prime.handle;
    } else if (bo_buf.ptr ()) {
        const drm_intel_bo* bo = bo_buf->get_bo ();
        bo_handle = bo->handle;
    } else {
        XCAM_ASSERT (false);
        XCAM_LOG_WARNING("drm setup framebuffer doesn't support this buffer");
        return XCAM_RETURN_ERROR_PARAM;
    }

    for (uint32_t i = 0; i < 4; ++i) {
        bo_handles [i] = bo_handle;
    }

    ret = (XCamReturn) drmModeAddFB2(_fd, video_info.width, video_info.height, fourcc, bo_handles,
                                     video_info.strides, video_info.offsets, &fb_handle, 0);

    fb.fb_handle = fb_handle;
    fb.index = global_signal_index++;
    _buf_fb_handles[buf.ptr ()] = fb;

    XCAM_FAIL_RETURN(ERROR, ret == XCAM_RETURN_NO_ERROR, XCAM_RETURN_ERROR_PARAM,
                     "drmModeAddFB2 failed: %s", strerror(errno));

    return ret;
}

XCamReturn
DrmDisplay::set_plane (const FB &fb)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    uint32_t fb_handle = fb.fb_handle;
    uint32_t index = fb.index;

    ret = (XCamReturn) drmModeSetPlane(_fd, _plane_id, _crtc_id,
                                       fb_handle, 0,
                                       _compose.left, _compose.top,
                                       _compose.width, _compose.height,
                                       0, 0, _width << 16, _height << 16);
    XCAM_FAIL_RETURN(ERROR, ret == XCAM_RETURN_NO_ERROR, XCAM_RETURN_ERROR_IOCTL,
                     "failed to set plane via drm: %s", strerror(errno));

    drmVBlank vblank;
    vblank.request.type = (drmVBlankSeqType) (DRM_VBLANK_EVENT | DRM_VBLANK_RELATIVE);
    vblank.request.sequence = 1;
    vblank.request.signal = (unsigned long) index;
    ret = (XCamReturn) drmWaitVBlank(_fd, &vblank);
    XCAM_FAIL_RETURN(ERROR, ret == XCAM_RETURN_NO_ERROR, XCAM_RETURN_ERROR_IOCTL,
                     "failed to wait vblank: %s", strerror(errno));

    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
DrmDisplay::page_flip (const FB &fb)
{
    XCamReturn ret;
    uint32_t fb_handle = fb.fb_handle;
    uint32_t index = fb.index;

    ret = (XCamReturn) drmModePageFlip(_fd, _crtc_id, fb_handle,
                                       DRM_MODE_PAGE_FLIP_EVENT,
                                       (void*)(unsigned long) index);
    XCAM_FAIL_RETURN(ERROR, ret == XCAM_RETURN_NO_ERROR, XCAM_RETURN_ERROR_IOCTL,
                     "failed on page flip: %s", strerror(errno));

    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
DrmDisplay::render_buffer(SmartPtr<VideoBuffer> &buf)
{
    FBMap::iterator iter = _buf_fb_handles.find (buf.ptr ());
    XCAM_FAIL_RETURN(
        ERROR,
        iter != _buf_fb_handles.end (),
        XCAM_RETURN_ERROR_PARAM,
        "buffer not register on framebuf");

    return _plane_id ? set_plane(iter->second) : page_flip(iter->second);
}


SmartPtr<DrmBoBuffer>
DrmDisplay::convert_to_drm_bo_buf (SmartPtr<DrmDisplay> &self, SmartPtr<VideoBuffer> &buf_in)
{
    drm_intel_bo *bo = NULL;
    int dma_fd = 0;
    SmartPtr<V4l2BufferProxy> v4l2_proxy;
    SmartPtr<DrmBoBuffer> new_bo_buf;
    SmartPtr<DrmBoWrapper> bo_wrapper;

    XCAM_ASSERT (self.ptr () == this);

    new_bo_buf = buf_in.dynamic_cast_ptr<DrmBoBuffer> ();
    if (new_bo_buf.ptr ())
        return new_bo_buf;

    v4l2_proxy = buf_in.dynamic_cast_ptr<V4l2BufferProxy> ();
    if (!v4l2_proxy.ptr () || v4l2_proxy->get_v4l2_mem_type () != V4L2_MEMORY_DMABUF) {
        XCAM_LOG_DEBUG ("DrmDisplay only support dma buffer conversion to drm bo by now");
        return NULL;
    }

    dma_fd = v4l2_proxy->get_v4l2_dma_fd ();
    XCAM_ASSERT (dma_fd > 0);
    bo = drm_intel_bo_gem_create_from_prime (_buf_manager, dma_fd, v4l2_proxy->get_v4l2_buf_length ());
    if (bo == NULL) {
        XCAM_LOG_WARNING ("convert dma fd to drm bo failed");
        return NULL;
    }
    bo_wrapper = new DrmBoWrapper (self, bo);
    new_bo_buf = new DrmBoBuffer (self, buf_in->get_video_info (), bo_wrapper);
    new_bo_buf->set_parent (buf_in);
    return new_bo_buf;
}

SmartPtr<DrmBoWrapper>
DrmDisplay::create_drm_bo (SmartPtr<DrmDisplay> &self, const VideoBufferInfo &info)
{
    SmartPtr<DrmBoWrapper> new_bo;

    XCAM_ASSERT (_buf_manager);
    XCAM_ASSERT (self.ptr() == this);
    drm_intel_bo *bo = drm_intel_bo_alloc (
                           _buf_manager, "xcam drm bo buf", info.size, 0x1000);

    new_bo = new DrmBoWrapper (self, bo);
    return new_bo;
}

};
