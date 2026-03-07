#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <new>
#include <vector>
#include <limits>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>

#include "7411d.h"
#include "bc_dts_glob_lnx.h"
#include "bc_dts_defs.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"

struct crystalhd_config {
    VAConfigID id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    uint32_t rt_format;
};

struct crystalhd_context {
    VAContextID id;
    VAConfigID config_id;
    uint32_t width;
    uint32_t height;
    HANDLE device;
    bool decoder_open = false;
    bool decoder_started = false;
    bool capture_started = false;
    uint32_t video_algo = BC_VID_ALGO_H264;
    BC_MEDIA_SUBTYPE media_subtype = BC_MSUBTYPE_INVALID;

    struct crystalhd_va_buffer {
        VABufferID id = VA_INVALID_ID;
        VABufferType type = (VABufferType)0;
        size_t num_elements = 0;
        size_t element_size = 0;
        bool mapped = false;
        void *map_ptr = nullptr;
        // Re-use the storage vector to avoid malloc/free churn on low-end CPUs.
        std::vector<uint8_t> storage;
    };

    struct crystalhd_pending_picture {
        bool in_progress = false;
        VASurfaceID target = VA_INVALID_SURFACE;
        std::vector<uint8_t> pic_params;
        std::vector<uint8_t> slice_params;
        std::vector<uint8_t> slice_data;
        std::vector<uint8_t> sei_payload;

        void reset()
        {
            in_progress = false;
            target = VA_INVALID_SURFACE;
            pic_params.clear();
            slice_params.clear();
            slice_data.clear();
            sei_payload.clear();
        }
    };

    std::vector<crystalhd_va_buffer> buffers;
    crystalhd_pending_picture pending_picture;
};

static crystalhd_context::crystalhd_va_buffer * __attribute__((unused))
crystalhd_find_buffer(crystalhd_context &ctx,
                      VABufferID id)
{
    for (auto &buffer : ctx.buffers) {
        if (buffer.id == id)
            return &buffer;
    }
    return nullptr;
}

static crystalhd_context::crystalhd_va_buffer *crystalhd_alloc_buffer(crystalhd_driver_state *drv,
                                                                      crystalhd_context &ctx)
{
    crystalhd_context::crystalhd_va_buffer buffer;
    buffer.id = drv->next_buffer_id++;
    ctx.buffers.push_back(std::move(buffer));
    return &ctx.buffers.back();
}

struct crystalhd_surface {
    VASurfaceID id;
    uint32_t width;
    uint32_t height;
    uint32_t rt_format;
    uint32_t fourcc;
    uint32_t pitch;
    bool dmabuf_backed = false;
    int dmabuf_fd = -1;
    uint32_t dmabuf_index = 0;
    uint32_t uv_stride = 0;
    uint32_t uv_offset = 0;
    std::vector<uint8_t> buffer;
};

struct crystalhd_driver_state {
    std::vector<crystalhd_config> configs;
    std::vector<crystalhd_context> contexts;
    std::vector<crystalhd_surface> surfaces;
    VAConfigID next_config_id = 1;
    VAContextID next_context_id = 1;
    VASurfaceID next_surface_id = 1;
    VABufferID next_buffer_id = 1;
    HANDLE surface_device = nullptr;
};

static inline crystalhd_driver_state *crystalhd_drv(VADriverContextP ctx)
{
    return ctx ? reinterpret_cast<crystalhd_driver_state *>(ctx->pDriverData) : nullptr;
}

static HANDLE crystalhd_get_surface_device(crystalhd_driver_state *drv)
{
    if (!drv)
        return nullptr;

    if (!drv->surface_device) {
        HANDLE device = nullptr;
        uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB;
        if (DtsDeviceOpen(&device, mode) != BC_STS_SUCCESS)
            return nullptr;
        drv->surface_device = device;
    }

    return drv->surface_device;
}

static VAStatus crystalhd_status_to_va(BC_STATUS status)
{
    switch (status) {
    case BC_STS_SUCCESS:
        return VA_STATUS_SUCCESS;
    case BC_STS_INV_ARG:
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    case BC_STS_INSUFF_RES:
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    default:
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
}

static bool crystalhd_profile_supported(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileH264ConstrainedBaseline:
        return true;
    default:
        return false;
    }
}

static crystalhd_config *crystalhd_find_config(crystalhd_driver_state *drv, VAConfigID id)
{
    if (!drv)
        return nullptr;
    for (auto &cfg : drv->configs) {
        if (cfg.id == id)
            return &cfg;
    }
    return nullptr;
}

static crystalhd_context *crystalhd_find_context(crystalhd_driver_state *drv,
                                                 VAContextID id)
{
    if (!drv)
        return nullptr;
    for (auto &context : drv->contexts) {
        if (context.id == id)
            return &context;
    }
    return nullptr;
}

static crystalhd_surface *crystalhd_find_surface(crystalhd_driver_state *drv,
                                                VASurfaceID id)
{
    if (!drv)
        return nullptr;
    for (auto &surface : drv->surfaces) {
        if (surface.id == id)
            return &surface;
    }
    return nullptr;
}

static bool crystalhd_profile_to_algo(VAProfile profile, uint32_t *algo,
                                      BC_MEDIA_SUBTYPE *subtype)
{
    if (!algo || !subtype)
        return false;

    switch (profile) {
    case VAProfileH264Main:
    case VAProfileH264High:
    case VAProfileH264ConstrainedBaseline:
        *algo = BC_VID_ALGO_H264;
        *subtype = BC_MSUBTYPE_H264;
        return true;
    default:
        return false;
    }
}

static void crystalhd_context_stop_decoder(crystalhd_context &ctx)
{
    if (!ctx.device)
        return;

    if (ctx.capture_started)
        DtsFlushRxCapture(ctx.device, true);

    if (ctx.decoder_started)
        DtsStopDecoder(ctx.device);

    if (ctx.decoder_open)
        DtsCloseDecoder(ctx.device);

    ctx.capture_started = false;
    ctx.decoder_started = false;
    ctx.decoder_open = false;
}

static BC_STATUS crystalhd_context_start_decoder(crystalhd_context &ctx,
                                                 VAProfile profile)
{
    uint32_t algo = 0;
    BC_MEDIA_SUBTYPE subtype = BC_MSUBTYPE_INVALID;
    if (!crystalhd_profile_to_algo(profile, &algo, &subtype))
        return BC_STS_NOT_IMPL;

    BC_STATUS sts = DtsOpenDecoder(ctx.device, BC_STREAM_TYPE_ES);
    if (sts != BC_STS_SUCCESS)
        return sts;
    ctx.decoder_open = true;

    sts = DtsSetVideoParams(ctx.device, algo, FALSE, FALSE, TRUE, 0);
    if (sts != BC_STS_SUCCESS)
        goto fail;

    BC_INPUT_FORMAT input;
    memset(&input, 0, sizeof(input));
    input.mSubtype = subtype;
    input.width = ctx.width;
    input.height = ctx.height;
    input.startCodeSz = 4;
    input.Progressive = 1;
    sts = DtsSetInputFormat(ctx.device, &input);
    if (sts != BC_STS_SUCCESS)
        goto fail;

    sts = DtsSetColorSpace(ctx.device, OUTPUT_MODE420);
    if (sts != BC_STS_SUCCESS)
        goto fail;

    sts = DtsStartDecoder(ctx.device);
    if (sts != BC_STS_SUCCESS)
        goto fail;
    ctx.decoder_started = true;

    sts = DtsStartCapture(ctx.device);
    if (sts != BC_STS_SUCCESS)
        goto fail;
    ctx.capture_started = true;
    ctx.video_algo = algo;
    ctx.media_subtype = subtype;
    return BC_STS_SUCCESS;

fail:
    crystalhd_context_stop_decoder(ctx);
    return sts;
}

static VAStatus crystalhd_CreateBuffer(VADriverContextP ctx, VAContextID context,
                                       VABufferType type, unsigned int size,
                                       unsigned int num_elements, void *data,
                                       VABufferID *buf_id)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv || !buf_id || !size || !num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    crystalhd_context *va_ctx = crystalhd_find_context(drv, context);
    if (!va_ctx)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    size_t total_sz = static_cast<size_t>(size) * static_cast<size_t>(num_elements);
    auto *buffer = crystalhd_alloc_buffer(drv, *va_ctx);
    buffer->type = type;
    buffer->num_elements = num_elements;
    buffer->element_size = size;
    buffer->mapped = false;
    buffer->map_ptr = nullptr;
    buffer->storage.resize(total_sz);
    if (data && total_sz)
        memcpy(buffer->storage.data(), data, total_sz);

    *buf_id = buffer->id;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_DestroyBuffer(VADriverContextP ctx, VABufferID buffer_id)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    for (auto &context : drv->contexts) {
        auto it = std::remove_if(context.buffers.begin(), context.buffers.end(),
                                 [&](const crystalhd_context::crystalhd_va_buffer &buf) {
                                     return buf.id == buffer_id;
                                 });
        if (it != context.buffers.end()) {
            context.buffers.erase(it, context.buffers.end());
            return VA_STATUS_SUCCESS;
        }
    }

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus crystalhd_BufferSetNumElements(VADriverContextP ctx, VABufferID buffer_id,
                                               unsigned int num_elements)
{
    if (!num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    for (auto &context : drv->contexts) {
        auto *buffer = crystalhd_find_buffer(context, buffer_id);
        if (!buffer)
            continue;

        buffer->num_elements = num_elements;
        size_t total_sz = static_cast<size_t>(buffer->element_size) * num_elements;
        buffer->storage.resize(total_sz);
        return VA_STATUS_SUCCESS;
    }

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus crystalhd_MapBuffer(VADriverContextP ctx, VABufferID buffer_id, void **pbuf)
{
    if (!pbuf)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    for (auto &context : drv->contexts) {
        auto *buffer = crystalhd_find_buffer(context, buffer_id);
        if (!buffer)
            continue;

        buffer->mapped = true;
        buffer->map_ptr = buffer->storage.data();
        *pbuf = buffer->map_ptr;
        return VA_STATUS_SUCCESS;
    }

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus crystalhd_UnmapBuffer(VADriverContextP ctx, VABufferID buffer_id)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    for (auto &context : drv->contexts) {
        auto *buffer = crystalhd_find_buffer(context, buffer_id);
        if (!buffer)
            continue;

        buffer->mapped = false;
        buffer->map_ptr = nullptr;
        return VA_STATUS_SUCCESS;
    }

    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus crystalhd_BeginPicture(VADriverContextP ctx, VAContextID context,
                                       VASurfaceID render_target)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_context *va_ctx = crystalhd_find_context(drv, context);
    if (!va_ctx)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    va_ctx->pending_picture.reset();
    va_ctx->pending_picture.in_progress = true;
    va_ctx->pending_picture.target = render_target;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_RenderPicture(VADriverContextP ctx, VAContextID context,
                                        VABufferID *buffers, int num_buffers)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!buffers || num_buffers <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    crystalhd_context *va_ctx = crystalhd_find_context(drv, context);
    if (!va_ctx || !va_ctx->pending_picture.in_progress)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    for (int i = 0; i < num_buffers; ++i) {
        VABufferID id = buffers[i];
        auto *buffer = crystalhd_find_buffer(*va_ctx, id);
        if (!buffer)
            return VA_STATUS_ERROR_INVALID_BUFFER;

        switch (buffer->type) {
        case VAPictureParameterBufferType:
            va_ctx->pending_picture.pic_params.assign(buffer->storage.begin(),
                                                      buffer->storage.end());
            break;
        case VASliceParameterBufferType:
            va_ctx->pending_picture.slice_params.insert(
                va_ctx->pending_picture.slice_params.end(),
                buffer->storage.begin(), buffer->storage.end());
            break;
        case VASliceDataBufferType:
            va_ctx->pending_picture.slice_data.insert(
                va_ctx->pending_picture.slice_data.end(),
                buffer->storage.begin(), buffer->storage.end());
            break;
        default:
            return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_EndPicture(VADriverContextP ctx, VAContextID context)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_context *va_ctx = crystalhd_find_context(drv, context);
    if (!va_ctx || !va_ctx->pending_picture.in_progress)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    if (!va_ctx->pending_picture.slice_data.empty()) {
        if (va_ctx->pending_picture.slice_data.size() > std::numeric_limits<uint32_t>::max())
            return VA_STATUS_ERROR_OPERATION_FAILED;
        uint32_t payload_size = static_cast<uint32_t>(va_ctx->pending_picture.slice_data.size());
        BC_STATUS sts = DtsProcInput(va_ctx->device,
                                     va_ctx->pending_picture.slice_data.data(),
                                     payload_size, 0, 0);
        if (sts != BC_STS_SUCCESS)
            return crystalhd_status_to_va(sts);
    }

    va_ctx->pending_picture.reset();
    return VA_STATUS_SUCCESS;
}

static uint32_t crystalhd_align_up(uint32_t value, uint32_t align)
{
    return (value + align - 1) & ~(align - 1);
}

static bool crystalhd_surface_format_supported(uint32_t rt_format, uint32_t fourcc)
{
    return rt_format == VA_RT_FORMAT_YUV420 && fourcc == VA_FOURCC_NV12;
}

static VAStatus crystalhd_surface_allocation(uint32_t width, uint32_t height,
                                             uint32_t rt_format, uint32_t fourcc,
                                             uint32_t *pitch, size_t *alloc_size)
{
    if (!width || !height || !pitch || !alloc_size)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!crystalhd_surface_format_supported(rt_format, fourcc))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    // Hardware prefers 64-byte alignment for DMA transfers; follow that for now.
    uint32_t stride = crystalhd_align_up(width, 64);
    uint64_t y_plane = static_cast<uint64_t>(stride) * height;
    uint64_t uv_plane = static_cast<uint64_t>(stride) * ((height + 1) / 2);
    uint64_t total = y_plane + uv_plane;
    if (total > SIZE_MAX)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    *pitch = stride;
    *alloc_size = static_cast<size_t>(total);
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_create_surface(crystalhd_driver_state *drv, uint32_t width,
                                         uint32_t height, uint32_t rt_format,
                                         uint32_t fourcc, VASurfaceID *out_id)
{
    if (!drv || !out_id)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    uint32_t pitch = 0;
    size_t buffer_size = 0;
    VAStatus status = crystalhd_surface_allocation(width, height, rt_format, fourcc,
                                                   &pitch, &buffer_size);
    if (status != VA_STATUS_SUCCESS)
        return status;

    crystalhd_surface surface{};
    surface.id = drv->next_surface_id++;
    surface.width = width;
    surface.height = height;
    surface.rt_format = rt_format;
    surface.fourcc = fourcc;
    surface.pitch = pitch;

    try {
        surface.buffer.resize(buffer_size);
    } catch (const std::bad_alloc &) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    drv->surfaces.push_back(std::move(surface));
    if (out_id)
        *out_id = drv->surfaces.back().id;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_create_surface_from_dmabuf(crystalhd_driver_state *drv,
                                                     uint32_t width, uint32_t height,
                                                     uint32_t rt_format,
                                                     uint32_t fourcc,
                                                     const BC_RX_DMABUF_DESC &desc,
                                                     VASurfaceID *out_id)
{
    if (!drv || desc.dmabuf_fd < 0)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    crystalhd_surface surface{};
    surface.id = drv->next_surface_id++;
    surface.width = width;
    surface.height = height;
    surface.rt_format = rt_format;
    surface.fourcc = fourcc;
    surface.pitch = desc.y_stride;
    surface.uv_stride = desc.uv_stride;
    surface.uv_offset = desc.uv_offset;
    surface.dmabuf_backed = true;
    surface.dmabuf_fd = desc.dmabuf_fd;
    surface.dmabuf_index = desc.index;

    drv->surfaces.push_back(std::move(surface));
    if (out_id)
        *out_id = drv->surfaces.back().id;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_destroy_surface(crystalhd_driver_state *drv,
                                          VASurfaceID id)
{
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_surface *surface = crystalhd_find_surface(drv, id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (surface->dmabuf_fd >= 0)
        ::close(surface->dmabuf_fd);
    drv->surfaces.erase(std::remove_if(drv->surfaces.begin(), drv->surfaces.end(),
                                       [id](const crystalhd_surface &surf) {
                                           return surf.id == id;
                                       }),
                        drv->surfaces.end());
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_terminate(VADriverContextP ctx)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_SUCCESS;

    for (auto &context : drv->contexts) {
        if (context.device)
            DtsDeviceClose(context.device);
    }
    if (drv->surface_device)
        DtsDeviceClose(drv->surface_device);
    drv->contexts.clear();
    drv->configs.clear();
    delete drv;
    ctx->pDriverData = nullptr;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_QueryConfigProfiles(VADriverContextP, VAProfile *profile_list, int *num_profiles)
{
    static const VAProfile supported[] = {
        VAProfileH264ConstrainedBaseline,
        VAProfileH264Main,
        VAProfileH264High,
    };

    if (num_profiles)
        *num_profiles = sizeof(supported) / sizeof(supported[0]);

    if (profile_list) {
        for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i)
            profile_list[i] = supported[i];
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_QueryConfigEntrypoints(VADriverContextP, VAProfile profile,
                                               VAEntrypoint *entrypoint_list,
                                               int *num_entrypoints)
{
    if (!crystalhd_profile_supported(profile)) {
        if (num_entrypoints)
            *num_entrypoints = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (num_entrypoints)
        *num_entrypoints = 1;
    if (entrypoint_list)
        entrypoint_list[0] = VAEntrypointVLD;

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_stub_unimplemented(VADriverContextP ctx, ...)
{
    (void)ctx;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus crystalhd_stub_vpp_filters(VADriverContextP ctx, VAContextID context,
                                          VAProcFilterType *filters, unsigned int *num_filters)
{
    (void)ctx;
    (void)context;
    (void)filters;
    if (num_filters)
        *num_filters = 0;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus crystalhd_stub_vpp_filter_caps(VADriverContextP ctx, VAContextID context,
                                               VAProcFilterType type, void *caps,
                                               unsigned int *num_caps)
{
    (void)ctx;
    (void)context;
    (void)type;
    (void)caps;
    if (num_caps)
        *num_caps = 0;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus crystalhd_stub_vpp_pipeline_caps(VADriverContextP ctx, VAContextID context,
                                                 VABufferID *filters, unsigned int num_filters,
                                                 VAProcPipelineCaps *pipeline_caps)
{
    (void)ctx;
    (void)context;
    (void)filters;
    (void)num_filters;
    (void)pipeline_caps;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus crystalhd_CreateContext(VADriverContextP ctx, VAConfigID config_id,
                                        int picture_width, int picture_height, int flag,
                                        VASurfaceID *render_targets, int num_render_targets,
                                        VAContextID *context_id)
{
    (void)flag;
    (void)render_targets;
    (void)num_render_targets;

    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_config *cfg = crystalhd_find_config(drv, config_id);
    if (!cfg)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    HANDLE device = nullptr;
    uint32_t mode = DTS_PLAYBACK_MODE | DTS_LOAD_FILE_PLAY_FW | DTS_SKIP_TX_CHK_CPB;
    BC_STATUS status = DtsDeviceOpen(&device, mode);
    if (status != BC_STS_SUCCESS)
        return crystalhd_status_to_va(status);

    crystalhd_context ctx_info{};
    ctx_info.id = drv->next_context_id++;
    ctx_info.config_id = cfg->id;
    ctx_info.width = static_cast<uint32_t>(picture_width);
    ctx_info.height = static_cast<uint32_t>(picture_height);
    ctx_info.device = device;

    BC_STATUS decode_status = crystalhd_context_start_decoder(ctx_info, cfg->profile);
    if (decode_status != BC_STS_SUCCESS) {
        DtsDeviceClose(device);
        return crystalhd_status_to_va(decode_status);
    }
    drv->contexts.push_back(ctx_info);

    if (context_id)
        *context_id = ctx_info.id;

    return VA_STATUS_SUCCESS;
}

struct crystalhd_surface_format {
    uint32_t rt_format;
    uint32_t fourcc;
};

static bool crystalhd_resolve_surface_format(uint32_t request, const VASurfaceAttrib *attrib_list,
                                             unsigned int num_attribs,
                                             crystalhd_surface_format *format)
{
    crystalhd_surface_format resolved{};

    if (request & VA_RT_FORMAT_YUV420)
        resolved.rt_format = VA_RT_FORMAT_YUV420;
    else if (request == VA_FOURCC_NV12)
        resolved.fourcc = VA_FOURCC_NV12;

    for (unsigned int i = 0; i < num_attribs && attrib_list; ++i) {
        if (attrib_list[i].type == VASurfaceAttribPixelFormat) {
            resolved.fourcc = attrib_list[i].value.value.i;
        }
    }

    if (!resolved.rt_format && resolved.fourcc == VA_FOURCC_NV12)
        resolved.rt_format = VA_RT_FORMAT_YUV420;
    if (!resolved.fourcc && resolved.rt_format == VA_RT_FORMAT_YUV420)
        resolved.fourcc = VA_FOURCC_NV12;

    if (!crystalhd_surface_format_supported(resolved.rt_format, resolved.fourcc))
        return false;

    if (format)
        *format = resolved;
    return true;
}

static VAStatus crystalhd_allocate_surfaces(VADriverContextP ctx, uint32_t width, uint32_t height,
                                            uint32_t request_format,
                                            const VASurfaceAttrib *attrib_list,
                                            unsigned int num_attribs,
                                            unsigned int num_surfaces, VASurfaceID *surfaces)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!surfaces || !num_surfaces)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    crystalhd_surface_format resolved{};
    if (!crystalhd_resolve_surface_format(request_format, attrib_list, num_attribs, &resolved))
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    std::vector<VASurfaceID> allocated;
    allocated.reserve(num_surfaces);

    bool exported_dma = false;
    HANDLE surf_dev = crystalhd_get_surface_device(drv);
    if (surf_dev) {
        BC_RX_DMABUF_EXPORT dmabuf{};
        dmabuf.requested = num_surfaces;
        dmabuf.width = width;
        dmabuf.height = height;
        dmabuf.format = resolved.fourcc;
        BC_STATUS sts = DtsExportRxDmabufs(surf_dev, &dmabuf);
        if (sts == BC_STS_SUCCESS && dmabuf.allocated == num_surfaces) {
            for (unsigned int i = 0; i < num_surfaces; ++i) {
                VAStatus status = crystalhd_create_surface_from_dmabuf(drv, width, height,
                                                                      resolved.rt_format,
                                                                      resolved.fourcc,
                                                                      dmabuf.desc[i],
                                                                      &surfaces[i]);
                if (status != VA_STATUS_SUCCESS) {
                    for (VASurfaceID id : allocated)
                        crystalhd_destroy_surface(drv, id);
                    return status;
                }
                allocated.push_back(surfaces[i]);
            }
            exported_dma = true;
        } else if (dmabuf.allocated) {
            for (unsigned int i = 0; i < dmabuf.allocated && i < BC_MAX_DMABUF_EXPORT; ++i) {
                if (dmabuf.desc[i].dmabuf_fd >= 0)
                    ::close(dmabuf.desc[i].dmabuf_fd);
            }
        }
    }

    if (exported_dma)
        return VA_STATUS_SUCCESS;

    for (unsigned int i = 0; i < num_surfaces; ++i) {
        VAStatus status = crystalhd_create_surface(drv, width, height, resolved.rt_format,
                                                   resolved.fourcc, &surfaces[i]);
        if (status != VA_STATUS_SUCCESS) {
            for (VASurfaceID id : allocated)
                crystalhd_destroy_surface(drv, id);
            return status;
        }
        allocated.push_back(surfaces[i]);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_CreateSurfaces(VADriverContextP ctx, int width, int height,
                                         int format, int num_surfaces, VASurfaceID *surfaces)
{
    if (width <= 0 || height <= 0 || num_surfaces <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    return crystalhd_allocate_surfaces(ctx, static_cast<uint32_t>(width),
                                       static_cast<uint32_t>(height),
                                       static_cast<uint32_t>(format), nullptr, 0,
                                       static_cast<unsigned int>(num_surfaces), surfaces);
}

static VAStatus crystalhd_CreateSurfaces2(VADriverContextP ctx, unsigned int format,
                                          unsigned int width, unsigned int height,
                                          VASurfaceID *surfaces, unsigned int num_surfaces,
                                          VASurfaceAttrib *attrib_list,
                                          unsigned int num_attribs)
{
    if (!width || !height || !num_surfaces)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    return crystalhd_allocate_surfaces(ctx, width, height, format, attrib_list, num_attribs,
                                       num_surfaces, surfaces);
}

static VAStatus crystalhd_DestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list,
                                          int num_surfaces)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!surface_list || num_surfaces <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (int i = 0; i < num_surfaces; ++i) {
        VAStatus status = crystalhd_destroy_surface(drv, surface_list[i]);
        if (status != VA_STATUS_SUCCESS)
            return status;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_DestroyContext(VADriverContextP ctx, VAContextID context_id)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    for (auto it = drv->contexts.begin(); it != drv->contexts.end(); ++it) {
        if (it->id == context_id) {
            crystalhd_context_stop_decoder(*it);
            if (it->device)
                DtsDeviceClose(it->device);
            drv->contexts.erase(it);
            return VA_STATUS_SUCCESS;
        }
    }

    return VA_STATUS_ERROR_INVALID_CONTEXT;
}

static VAStatus crystalhd_GetConfigAttributes(VADriverContextP, VAProfile profile,
                                             VAEntrypoint entrypoint,
                                             VAConfigAttrib *attrib_list,
                                             int num_attribs)
{
    if (!crystalhd_profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entrypoint != VAEntrypointVLD)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

    for (int i = 0; i < num_attribs; ++i) {
        if (!attrib_list)
            break;
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            break;
        default:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_CreateConfig(VADriverContextP ctx, VAProfile profile,
                                       VAEntrypoint entrypoint,
                                       VAConfigAttrib *attrib_list, int num_attribs,
                                       VAConfigID *config_id)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    if (!crystalhd_profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (entrypoint != VAEntrypointVLD)
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

    uint32_t rt_format = VA_RT_FORMAT_YUV420;
    for (int i = 0; i < num_attribs; ++i) {
        if (!attrib_list)
            break;
        if (attrib_list[i].type == VAConfigAttribRTFormat) {
            if (!(attrib_list[i].value & VA_RT_FORMAT_YUV420))
                return VA_STATUS_ERROR_INVALID_VALUE;
            rt_format = VA_RT_FORMAT_YUV420;
        }
    }

    crystalhd_config cfg{};
    cfg.id = drv->next_config_id++;
    cfg.profile = profile;
    cfg.entrypoint = entrypoint;
    cfg.rt_format = rt_format;
    drv->configs.push_back(cfg);

    if (config_id)
        *config_id = cfg.id;

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_DestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    auto it = std::remove_if(drv->configs.begin(), drv->configs.end(),
                             [config_id](const crystalhd_config &cfg) {
                                 return cfg.id == config_id;
                             });
    if (it == drv->configs.end())
        return VA_STATUS_ERROR_INVALID_CONFIG;

    drv->configs.erase(it, drv->configs.end());
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_QueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
                                                VAProfile *profile,
                                                VAEntrypoint *entrypoint,
                                                VAConfigAttrib *attrib_list,
                                                int *num_attribs)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_config *cfg = crystalhd_find_config(drv, config_id);
    if (!cfg)
        return VA_STATUS_ERROR_INVALID_CONFIG;

    if (profile)
        *profile = cfg->profile;
    if (entrypoint)
        *entrypoint = cfg->entrypoint;

    if (attrib_list && num_attribs) {
        for (int i = 0; i < *num_attribs; ++i) {
            switch (attrib_list[i].type) {
            case VAConfigAttribRTFormat:
                attrib_list[i].value = cfg->rt_format;
                break;
            default:
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
                break;
            }
        }
    }

    return VA_STATUS_SUCCESS;
}

#define ASSIGN_VTABLE_STUB(vtbl, field) \
    do { (vtbl)->field = reinterpret_cast<decltype((vtbl)->field)>(crystalhd_stub_unimplemented); } while (0)

static VAStatus crystalhd_driver_init(VADriverContextP ctx)
{
    if (!ctx || !ctx->vtable)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    memset(ctx->vtable, 0, sizeof(*ctx->vtable));
    if (ctx->vtable_vpp)
        memset(ctx->vtable_vpp, 0, sizeof(*ctx->vtable_vpp));

    if (!ctx->pDriverData) {
        auto *drv = new (std::nothrow) crystalhd_driver_state();
        if (!drv)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        ctx->pDriverData = drv;
    }

    ctx->str_vendor = "Broadcom CrystalHD";
    ctx->max_profiles = 3;
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 1;
    ctx->max_subpic_formats = 0;
    ctx->max_display_attributes = 0;
    ctx->max_image_formats = 0;

    ctx->vtable->vaTerminate = crystalhd_terminate;
    ctx->vtable->vaQueryConfigProfiles = crystalhd_QueryConfigProfiles;
    ctx->vtable->vaQueryConfigEntrypoints = crystalhd_QueryConfigEntrypoints;

    ctx->vtable->vaGetConfigAttributes = crystalhd_GetConfigAttributes;
    ctx->vtable->vaCreateConfig = crystalhd_CreateConfig;
    ctx->vtable->vaDestroyConfig = crystalhd_DestroyConfig;
    ctx->vtable->vaQueryConfigAttributes = crystalhd_QueryConfigAttributes;
    ctx->vtable->vaCreateSurfaces = crystalhd_CreateSurfaces;
    ctx->vtable->vaDestroySurfaces = crystalhd_DestroySurfaces;
    ctx->vtable->vaCreateContext = crystalhd_CreateContext;
    ctx->vtable->vaDestroyContext = crystalhd_DestroyContext;
    ctx->vtable->vaCreateBuffer = crystalhd_CreateBuffer;
    ctx->vtable->vaBufferSetNumElements = crystalhd_BufferSetNumElements;
    ctx->vtable->vaMapBuffer = crystalhd_MapBuffer;
    ctx->vtable->vaUnmapBuffer = crystalhd_UnmapBuffer;
    ctx->vtable->vaDestroyBuffer = crystalhd_DestroyBuffer;
    ctx->vtable->vaBeginPicture = crystalhd_BeginPicture;
    ctx->vtable->vaRenderPicture = crystalhd_RenderPicture;
    ctx->vtable->vaEndPicture = crystalhd_EndPicture;
    ASSIGN_VTABLE_STUB(ctx->vtable, vaSyncSurface);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaQuerySurfaceStatus);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaQuerySurfaceError);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaPutSurface);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaQueryImageFormats);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateImage);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaDestroyImage);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaSetImagePalette);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaGetImage);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaPutImage);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaQuerySubpictureFormats);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateSubpicture);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaDestroySubpicture);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaSetSubpictureImage);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaSetSubpictureChromakey);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaSetSubpictureGlobalAlpha);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaAssociateSubpicture);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaDeassociateSubpicture);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaQueryDisplayAttributes);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaGetDisplayAttributes);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaSetDisplayAttributes);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaBufferInfo);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaLockSurface);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaUnlockSurface);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaGetSurfaceAttributes);
    ctx->vtable->vaCreateSurfaces2 = crystalhd_CreateSurfaces2;
    ASSIGN_VTABLE_STUB(ctx->vtable, vaExportSurfaceHandle);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateMFContext);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaMFAddContext);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaMFReleaseContext);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaMFSubmit);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateBuffer2);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaQueryProcessingRate);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaSyncSurface2);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaSyncBuffer);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCopy);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaMapBuffer2);

    if (ctx->vtable_vpp) {
        ctx->vtable_vpp->version = VA_DRIVER_VTABLE_VPP_VERSION;
        ctx->vtable_vpp->vaQueryVideoProcFilters = crystalhd_stub_vpp_filters;
        ctx->vtable_vpp->vaQueryVideoProcFilterCaps = crystalhd_stub_vpp_filter_caps;
        ctx->vtable_vpp->vaQueryVideoProcPipelineCaps = crystalhd_stub_vpp_pipeline_caps;
    }

    return VA_STATUS_SUCCESS;
}

extern "C" VAStatus __attribute__((visibility("default")))
__vaDriverInit_1_22(VADriverContextP ctx)
{
    return crystalhd_driver_init(ctx);
}

extern "C" VAStatus __attribute__((visibility("default")))
__vaDriverInit_1_0(VADriverContextP ctx)
{
    return crystalhd_driver_init(ctx);
}
