#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <algorithm>
#include <new>
#include <deque>
#include <vector>
#include <limits>
#include <unordered_map>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>
#include <va/va_drmcommon.h>

#include <drm/drm_fourcc.h>

#include "7411d.h"
#include "bc_dts_glob_lnx.h"
#include "bc_dts_defs.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

static constexpr bool kCrystalhdEnableDmabufSurfaces = true;
static constexpr uint32_t kCrystalhdPrimeMemTypes =
    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME |
    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 |
    VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_3;

struct crystalhd_config {
    VAConfigID id;
    VAProfile profile;
    VAEntrypoint entrypoint;
    uint32_t rt_format;
};

struct crystalhd_surface;

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
        VABufferID last_pic_param = VA_INVALID_ID;
        std::deque<VABufferID> pending_slice_params;
        std::deque<VABufferID> pending_slice_data;
        std::vector<uint8_t> pending_bitstream;
        std::vector<VASliceParameterBufferH264> parsed_slice_params;
        VAIQMatrixBufferH264 last_iq_matrix{};
        bool have_iq_matrix = false;

        void reset()
        {
            in_progress = false;
            target = VA_INVALID_SURFACE;
            last_pic_param = VA_INVALID_ID;
            pending_slice_params.clear();
            pending_slice_data.clear();
            pending_bitstream.clear();
            parsed_slice_params.clear();
            have_iq_matrix = false;
        }
    };

    std::vector<crystalhd_va_buffer> buffers;
    crystalhd_pending_picture pending_picture;
    BC_PIB_EXT_H264 user_pib_ext{};
    VAPictureParameterBufferH264 last_pic_param{};
    bool have_pic_param = false;
    BC_PIC_INFO_BLOCK pending_pic_info{};
    bool have_pending_pic_info = false;
    crystalhd_surface *current_target_surface = nullptr;
    bool surface_waiting_output = false;
    BC_DTS_PROC_OUT last_proc_out{};

    struct surface_status {
        enum class state {
            idle,
            submitted,
            pending_output
        } current_state = state::idle;
        uint64_t timestamp = 0;
    };

    std::unordered_map<VASurfaceID, surface_status> surface_states;
};

struct crystalhd_driver_state;

static VAStatus crystalhd_status_to_va(BC_STATUS status);
static crystalhd_context *crystalhd_find_context(crystalhd_driver_state *drv,
                                                 VAContextID id);
static inline DTS_LIB_CONTEXT *crystalhd_dts_ctx(HANDLE handle)
{
    return reinterpret_cast<DTS_LIB_CONTEXT *>(handle);
}

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

static VAStatus crystalhd_handle_picture_parameters(crystalhd_context &ctx,
                                                    const uint8_t *data,
                                                    size_t stride,
                                                    size_t count);
static VAStatus crystalhd_handle_slice_parameters(crystalhd_context &ctx,
                                                  const uint8_t *data,
                                                  size_t stride,
                                                  size_t count);

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
    VAContextID last_context = VA_INVALID_ID;
    VAContextID dmabuf_context = VA_INVALID_ID;
    bool dmabuf_registered = false;
    bool dmabuf_in_use = false;
    void *dmabuf_map = nullptr;
    size_t dmabuf_map_len = 0;
    std::vector<uint8_t> buffer;
};

struct crystalhd_image_record {
    VAImage image;
    std::vector<uint8_t> storage;
    bool derived = false;
    VASurfaceID surface = VA_INVALID_SURFACE;
    bool acquired = false;
    bool acquired_is_fd = false;
    int acquired_fd = -1;
};

static bool crystalhd_surface_get_planes(crystalhd_surface &surface,
                                         uint8_t **y_ptr,
                                         size_t *y_size,
                                         uint8_t **uv_ptr,
                                         size_t *uv_size)
{
    if (surface.dmabuf_backed || surface.buffer.empty())
        return false;

    if (y_ptr)
        *y_ptr = surface.buffer.data();
    if (y_size)
        *y_size = surface.uv_offset;

    if (uv_ptr)
        *uv_ptr = surface.buffer.data() + surface.uv_offset;
    if (uv_size) {
        size_t total = surface.buffer.size();
        if (surface.uv_offset > total)
            return false;
        *uv_size = total - surface.uv_offset;
    }

    return true;
}

struct crystalhd_driver_state {
    std::vector<crystalhd_config> configs;
    std::vector<crystalhd_context> contexts;
    std::vector<crystalhd_surface> surfaces;
    std::vector<crystalhd_image_record> images;
    VAConfigID next_config_id = 1;
    VAContextID next_context_id = 1;
    VASurfaceID next_surface_id = 1;
    VABufferID next_buffer_id = 1;
    VAImageID next_image_id = 1;
    HANDLE surface_device = nullptr;
};

static inline DTS_LIB_CONTEXT *crystalhd_dts_ctx(HANDLE handle);

static bool crystalhd_surface_map_dmabuf(crystalhd_surface &surface,
                                         const BC_RX_DMABUF_DESC &desc)
{
    if (desc.dmabuf_fd < 0 || !desc.length)
        return false;

    void *addr = mmap(nullptr, desc.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                      desc.dmabuf_fd, 0);
    if (addr == MAP_FAILED)
        return false;

    surface.dmabuf_map = addr;
    surface.dmabuf_map_len = desc.length;
    surface.uv_offset = desc.uv_offset;
    surface.uv_stride = desc.uv_stride;
    return true;
}

static void crystalhd_surface_unmap_dmabuf(crystalhd_surface &surface)
{
    if (surface.dmabuf_map && surface.dmabuf_map_len)
        munmap(surface.dmabuf_map, surface.dmabuf_map_len);
    surface.dmabuf_map = nullptr;
    surface.dmabuf_map_len = 0;
}

static VAStatus crystalhd_wait_surface_dmabuf(crystalhd_driver_state *drv,
                                              crystalhd_context &ctx,
                                              crystalhd_surface &surface);

static VAStatus crystalhd_surface_register_dmabuf(crystalhd_context &ctx,
                                                  crystalhd_surface &surface)
{
    if (!surface.dmabuf_backed || !surface.dmabuf_map || !surface.dmabuf_map_len)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (surface.dmabuf_context == ctx.id && surface.dmabuf_registered)
        return VA_STATUS_SUCCESS;

    auto *dts_ctx = crystalhd_dts_ctx(ctx.device);
    if (!dts_ctx)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    BC_IOCTL_DATA *io = DtsAllocIoctlData(dts_ctx);
    if (!io)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    io->u.RxBuffs.YuvBuff = static_cast<uint8_t *>(surface.dmabuf_map);
    io->u.RxBuffs.YuvBuffSz = static_cast<uint32_t>(surface.dmabuf_map_len);
    io->u.RxBuffs.UVbuffOffset = surface.uv_offset;
    io->u.RxBuffs.b422Mode = FALSE;
    io->u.RxBuffs.RefCnt = 0;

    BC_STATUS sts = DtsDrvCmd(dts_ctx, BCM_IOC_ADD_RXBUFFS, 0, io, TRUE);
    if (sts != BC_STS_SUCCESS)
        return crystalhd_status_to_va(sts);

    surface.dmabuf_context = ctx.id;
    surface.dmabuf_registered = true;
    surface.last_context = ctx.id;
    return VA_STATUS_SUCCESS;
}

static void crystalhd_surface_release_dmabuf(crystalhd_driver_state *drv,
                                             crystalhd_surface &surface)
{
    if (!surface.dmabuf_in_use || surface.last_context == VA_INVALID_ID)
        return;
    crystalhd_context *ctx = crystalhd_find_context(drv, surface.last_context);
    if (!ctx)
        return;
    DtsReleaseOutputBuffs(ctx->device, nullptr, FALSE);
    surface.dmabuf_in_use = false;
}

static crystalhd_context::crystalhd_va_buffer *crystalhd_alloc_buffer(crystalhd_driver_state *drv,
                                                                      crystalhd_context &ctx)
{
    crystalhd_context::crystalhd_va_buffer buffer;
    buffer.id = drv->next_buffer_id++;
    ctx.buffers.push_back(std::move(buffer));
    return &ctx.buffers.back();
}

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

static crystalhd_context *crystalhd_surface_context(crystalhd_driver_state *drv,
                                                    const crystalhd_surface &surface)
{
    if (!drv || surface.last_context == VA_INVALID_ID)
        return nullptr;
    return crystalhd_find_context(drv, surface.last_context);
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

static crystalhd_image_record *crystalhd_find_image(crystalhd_driver_state *drv,
                                                    VAImageID id)
{
    if (!drv)
        return nullptr;
    for (auto &image : drv->images) {
        if (image.image.image_id == id)
            return &image;
    }
    return nullptr;
}

static crystalhd_image_record *crystalhd_find_image_by_buffer(crystalhd_driver_state *drv,
                                                              VABufferID id)
{
    if (!drv)
        return nullptr;
    for (auto &image : drv->images) {
        if (image.image.buf == id)
            return &image;
    }
    return nullptr;
}

static uint8_t *crystalhd_surface_data(crystalhd_surface &surface)
{
    if (surface.dmabuf_backed)
        return static_cast<uint8_t *>(surface.dmabuf_map);
    if (!surface.buffer.empty())
        return surface.buffer.data();
    return nullptr;
}

static uint8_t *crystalhd_image_data_ptr(crystalhd_driver_state *drv,
                                         crystalhd_image_record &image)
{
    if (image.derived) {
        crystalhd_surface *surface = crystalhd_find_surface(drv, image.surface);
        if (!surface)
            return nullptr;
        return crystalhd_surface_data(*surface);
    }
    if (image.storage.empty())
        return nullptr;
    return image.storage.data();
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

static void crystalhd_reset_submission_state(crystalhd_context &ctx)
{
    ctx.current_target_surface = nullptr;
    ctx.surface_waiting_output = false;
    ctx.have_pending_pic_info = false;
}

static VAStatus crystalhd_submit_bitstream(crystalhd_context &ctx,
                                           const uint8_t *data,
                                           size_t size)
{
    if (!data || !size)
        return VA_STATUS_SUCCESS;

    while (size) {
        uint32_t chunk = static_cast<uint32_t>(std::min<size_t>(size, std::numeric_limits<uint32_t>::max()));
        BC_STATUS sts = DtsProcInput(ctx.device, const_cast<uint8_t *>(data), chunk, 0, 0);
        if (sts != BC_STS_SUCCESS)
            return crystalhd_status_to_va(sts);
        data += chunk;
        size -= chunk;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_process_slice_pair(crystalhd_context &ctx,
                                             VABufferID param_id,
                                             VABufferID data_id)
{
    auto *param_buffer = crystalhd_find_buffer(ctx, param_id);
    auto *data_buffer = crystalhd_find_buffer(ctx, data_id);
    if (!param_buffer || !data_buffer)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!param_buffer->element_size ||
        param_buffer->element_size < sizeof(VASliceParameterBufferBase))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    const uint8_t *param_base = param_buffer->storage.data();
    const uint8_t *slice_data_base = data_buffer->storage.data();
    const size_t slice_data_size = data_buffer->storage.size();

    if (param_buffer->storage.size() >= param_buffer->element_size * param_buffer->num_elements) {
        VAStatus status = crystalhd_handle_slice_parameters(ctx,
                                                            param_base,
                                                            param_buffer->element_size,
                                                            param_buffer->num_elements);
        if (status != VA_STATUS_SUCCESS)
            return status;
    }

    for (size_t i = 0; i < param_buffer->num_elements; ++i) {
        size_t offset = i * param_buffer->element_size;
        if (offset + sizeof(VASliceParameterBufferBase) > param_buffer->storage.size())
            return VA_STATUS_ERROR_INVALID_PARAMETER;

        const auto *slice = reinterpret_cast<const VASliceParameterBufferBase *>(param_base + offset);

        if (slice->slice_data_offset > slice_data_size)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (slice->slice_data_size > slice_data_size - slice->slice_data_offset)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL)
            return VA_STATUS_ERROR_UNIMPLEMENTED;

        const uint8_t *slice_ptr = slice_data_base + slice->slice_data_offset;
        try {
            ctx.pending_picture.pending_bitstream.insert(
                ctx.pending_picture.pending_bitstream.end(),
                slice_ptr, slice_ptr + slice->slice_data_size);
        } catch (const std::bad_alloc &) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_process_pending_slices(crystalhd_context &ctx)
{
    while (!ctx.pending_picture.pending_slice_params.empty() &&
           !ctx.pending_picture.pending_slice_data.empty()) {
        VABufferID param_id = ctx.pending_picture.pending_slice_params.front();
        VABufferID data_id = ctx.pending_picture.pending_slice_data.front();
        ctx.pending_picture.pending_slice_params.pop_front();
        ctx.pending_picture.pending_slice_data.pop_front();

        VAStatus status = crystalhd_process_slice_pair(ctx, param_id, data_id);
        if (status != VA_STATUS_SUCCESS)
            return status;
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_submit_pending_picture(crystalhd_context &ctx)
{
    if (!ctx.have_pic_param || ctx.pending_picture.pending_bitstream.empty())
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!ctx.current_target_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    const auto &bitstream = ctx.pending_picture.pending_bitstream;
    VAStatus status = crystalhd_submit_bitstream(ctx, bitstream.data(), bitstream.size());
    if (status == VA_STATUS_SUCCESS)
        ctx.surface_waiting_output = true;
    return status;
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

    auto *image = crystalhd_find_image_by_buffer(drv, buffer_id);
    if (image) {
        image->image.buf = VA_INVALID_ID;
        return VA_STATUS_SUCCESS;
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

    if (auto *image = crystalhd_find_image_by_buffer(drv, buffer_id)) {
        *pbuf = crystalhd_image_data_ptr(drv, *image);
        if (!*pbuf)
            return VA_STATUS_ERROR_OPERATION_FAILED;
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

    if (crystalhd_find_image_by_buffer(drv, buffer_id))
        return VA_STATUS_SUCCESS;

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
    crystalhd_reset_submission_state(*va_ctx);
    if (render_target != VA_INVALID_SURFACE) {
        auto &state = va_ctx->surface_states[render_target];
        state.current_state = crystalhd_context::surface_status::state::submitted;
        state.timestamp = 0;
        crystalhd_surface *surface = crystalhd_find_surface(drv, render_target);
        if (surface) {
            surface->last_context = va_ctx->id;
            if (surface->dmabuf_backed) {
                crystalhd_surface_release_dmabuf(drv, *surface);
                VAStatus reg_status = crystalhd_surface_register_dmabuf(*va_ctx, *surface);
                if (reg_status != VA_STATUS_SUCCESS)
                    return reg_status;
            }
            va_ctx->current_target_surface = surface;
        }
    }
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

    VAStatus status = VA_STATUS_SUCCESS;
    for (int i = 0; i < num_buffers; ++i) {
        VABufferID id = buffers[i];
        auto *buffer = crystalhd_find_buffer(*va_ctx, id);
        if (!buffer)
            return VA_STATUS_ERROR_INVALID_BUFFER;

        switch (buffer->type) {
        case VAPictureParameterBufferType:
            status = crystalhd_handle_picture_parameters(*va_ctx,
                                                         buffer->storage.data(),
                                                         buffer->element_size,
                                                         buffer->num_elements);
            if (status != VA_STATUS_SUCCESS)
                return status;
            va_ctx->pending_picture.last_pic_param = id;
            break;
        case VASliceParameterBufferType:
            va_ctx->pending_picture.pending_slice_params.push_back(id);
            status = crystalhd_process_pending_slices(*va_ctx);
            if (status != VA_STATUS_SUCCESS)
                return status;
            break;
        case VASliceDataBufferType:
            va_ctx->pending_picture.pending_slice_data.push_back(id);
            status = crystalhd_process_pending_slices(*va_ctx);
            if (status != VA_STATUS_SUCCESS)
                return status;
            break;
        case VAIQMatrixBufferType:
            if (buffer->storage.size() < sizeof(VAIQMatrixBufferH264))
                return VA_STATUS_ERROR_INVALID_BUFFER;
            memcpy(&va_ctx->pending_picture.last_iq_matrix, buffer->storage.data(),
                   sizeof(VAIQMatrixBufferH264));
            va_ctx->pending_picture.have_iq_matrix = true;
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

    VASurfaceID target = va_ctx->pending_picture.target;

    if (!va_ctx->pending_picture.pending_slice_params.empty() ||
        !va_ctx->pending_picture.pending_slice_data.empty())
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (target != VA_INVALID_SURFACE)
        va_ctx->surface_states[target].current_state =
            crystalhd_context::surface_status::state::pending_output;

    VAStatus submit_status = crystalhd_submit_pending_picture(*va_ctx);
    if (submit_status != VA_STATUS_SUCCESS)
        return submit_status;

    va_ctx->pending_picture.reset();
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_SyncSurface(VADriverContextP ctx, VASurfaceID render_target)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_surface *surface = crystalhd_find_surface(drv, render_target);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    crystalhd_context *va_ctx = crystalhd_surface_context(drv, *surface);
    if (!va_ctx)
        return VA_STATUS_SUCCESS;

    if (surface->dmabuf_backed)
        return crystalhd_wait_surface_dmabuf(drv, *va_ctx, *surface);

    auto &state = va_ctx->surface_states[render_target];
    if (state.current_state != crystalhd_context::surface_status::state::pending_output)
        return VA_STATUS_SUCCESS;

    uint8_t *y_ptr = nullptr;
    uint8_t *uv_ptr = nullptr;
    size_t y_size = 0;
    size_t uv_size = 0;
    if (!crystalhd_surface_get_planes(*surface, &y_ptr, &y_size, &uv_ptr, &uv_size))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

    auto &proc_out = va_ctx->last_proc_out;
    memset(&proc_out, 0, sizeof(proc_out));
    proc_out.Ybuff = y_ptr;
    proc_out.YbuffSz = static_cast<uint32_t>(y_size);
    proc_out.UVbuff = uv_ptr;
    proc_out.UVbuffSz = static_cast<uint32_t>(uv_size);
    proc_out.StrideSz = surface->pitch;
    proc_out.StrideSzUV = surface->uv_stride ? surface->uv_stride : surface->pitch;
    proc_out.PoutFlags = BC_POUT_FLAGS_SIZE | BC_POUT_FLAGS_STRIDE | BC_POUT_FLAGS_STRIDE_UV;

    BC_STATUS sts = DtsProcOutput(va_ctx->device, 16, &proc_out);
    if (sts == BC_STS_TIMEOUT)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    if (sts == BC_STS_FMT_CHANGE)
        return VA_STATUS_ERROR_DECODING_ERROR;
    if (sts != BC_STS_SUCCESS)
        return crystalhd_status_to_va(sts);

    if (!(proc_out.PoutFlags & BC_POUT_FLAGS_PIB_VALID))
        return VA_STATUS_ERROR_DECODING_ERROR;

    state.current_state = crystalhd_context::surface_status::state::idle;
    state.timestamp = proc_out.PicInfo.timeStamp;
    va_ctx->surface_waiting_output = false;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_ExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id,
                                              uint32_t mem_type, uint32_t flags,
                                              void *descriptor)
{
    (void)flags;

    auto *drv = crystalhd_drv(ctx);
    if (!drv || !descriptor)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!(mem_type & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME))
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

    crystalhd_surface *surface = crystalhd_find_surface(drv, surface_id);
    if (!surface || !surface->dmabuf_backed)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    auto *drm_desc = reinterpret_cast<VADRMPRIMESurfaceDescriptor *>(descriptor);
    memset(drm_desc, 0, sizeof(*drm_desc));
    drm_desc->num_objects = 1;
    drm_desc->objects[0].fd = dup(surface->dmabuf_fd);
    if (drm_desc->objects[0].fd < 0)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    drm_desc->objects[0].size = surface->dmabuf_map_len;
    drm_desc->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;

    drm_desc->num_layers = 1;
    drm_desc->layers[0].drm_format = DRM_FORMAT_NV12;
    drm_desc->layers[0].num_planes = 2;
    drm_desc->layers[0].object_index[0] = 0;
    drm_desc->layers[0].object_index[1] = 0;
    drm_desc->layers[0].offset[0] = 0;
    drm_desc->layers[0].offset[1] = surface->uv_offset;
    drm_desc->layers[0].pitch[0] = surface->pitch;
    drm_desc->layers[0].pitch[1] = surface->uv_stride ? surface->uv_stride : surface->pitch;

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_acquire_dmabuf_frame(crystalhd_driver_state *drv,
                                               crystalhd_context &ctx)
{
    BC_DTS_PROC_OUT proc_out;
    memset(&proc_out, 0, sizeof(proc_out));
    proc_out.PoutFlags = BC_POUT_FLAGS_PIB_VALID;

    BC_STATUS sts = DtsProcOutputNoCopy(ctx.device, 16, &proc_out);
    if (sts == BC_STS_TIMEOUT)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    if (sts == BC_STS_FMT_CHANGE)
        return VA_STATUS_ERROR_DECODING_ERROR;
    if (sts != BC_STS_SUCCESS)
        return crystalhd_status_to_va(sts);

    crystalhd_surface *matched = nullptr;
    for (auto &surface : drv->surfaces) {
        if (!surface.dmabuf_backed || surface.dmabuf_map != proc_out.Ybuff)
            continue;
        if (surface.dmabuf_context != ctx.id && surface.last_context != ctx.id)
            continue;
        matched = &surface;
        break;
    }

    if (!matched) {
        DtsReleaseOutputBuffs(ctx.device, nullptr, FALSE);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    auto &state = ctx.surface_states[matched->id];
    state.current_state = crystalhd_context::surface_status::state::idle;
    state.timestamp = proc_out.PicInfo.timeStamp;
    matched->dmabuf_in_use = true;
    matched->last_context = ctx.id;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_wait_surface_dmabuf(crystalhd_driver_state *drv,
                                              crystalhd_context &ctx,
                                              crystalhd_surface &surface)
{
    while (true) {
        auto &state = ctx.surface_states[surface.id];
        if (state.current_state != crystalhd_context::surface_status::state::pending_output)
            return VA_STATUS_SUCCESS;

        VAStatus status = crystalhd_acquire_dmabuf_frame(drv, ctx);
        if (status != VA_STATUS_SUCCESS)
            return status;
    }
}

static VAStatus crystalhd_QuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target,
                                             VASurfaceStatus *out_status)
{
    if (!out_status)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_surface *surface = crystalhd_find_surface(drv, render_target);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    crystalhd_context *va_ctx = crystalhd_surface_context(drv, *surface);
    if (!va_ctx) {
        *out_status = VASurfaceReady;
        return VA_STATUS_SUCCESS;
    }

    auto it = va_ctx->surface_states.find(render_target);
    crystalhd_context::surface_status::state state =
        it == va_ctx->surface_states.end() ?
        crystalhd_context::surface_status::state::idle : it->second.current_state;

    switch (state) {
    case crystalhd_context::surface_status::state::idle:
        *out_status = VASurfaceReady;
        break;
    case crystalhd_context::surface_status::state::submitted:
    case crystalhd_context::surface_status::state::pending_output:
    default:
        *out_status = VASurfaceRendering;
        break;
    }

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

static void crystalhd_fill_nv12_format(VAImageFormat *format)
{
    if (!format)
        return;
    memset(format, 0, sizeof(*format));
    format->fourcc = VA_FOURCC_NV12;
    format->byte_order = VA_LSB_FIRST;
    format->bits_per_pixel = 12;
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

static size_t crystalhd_image_size(uint32_t pitch, uint32_t height)
{
    uint64_t y = static_cast<uint64_t>(pitch) * height;
    uint64_t uv = static_cast<uint64_t>(pitch) * ((height + 1) / 2);
    return static_cast<size_t>(y + uv);
}

static bool crystalhd_copy_plane(uint8_t *dst, uint32_t dst_pitch,
                                 const uint8_t *src, uint32_t src_pitch,
                                 uint32_t width, uint32_t height)
{
    if (!dst || !src)
        return false;
    for (uint32_t row = 0; row < height; ++row) {
        memcpy(dst + row * dst_pitch, src + row * src_pitch, width);
    }
    return true;
}

static bool crystalhd_copy_surface_to_image(crystalhd_surface &surface,
                                            uint8_t *dst, uint32_t dst_pitch)
{
    uint8_t *src = crystalhd_surface_data(surface);
    if (!src || !dst)
        return false;
    if (!crystalhd_copy_plane(dst, dst_pitch, src, surface.pitch,
                              surface.width, surface.height))
        return false;
    uint8_t *src_uv = src + surface.uv_offset;
    uint8_t *dst_uv = dst + static_cast<size_t>(dst_pitch) * surface.height;
    return crystalhd_copy_plane(dst_uv, dst_pitch, src_uv,
                                surface.uv_stride ? surface.uv_stride : surface.pitch,
                                surface.width, (surface.height + 1) / 2);
}

static bool crystalhd_copy_image_to_surface(const uint8_t *src, uint32_t src_pitch,
                                            crystalhd_surface &surface)
{
    uint8_t *dst = crystalhd_surface_data(surface);
    if (!dst || !src)
        return false;
    if (!crystalhd_copy_plane(dst, surface.pitch, src, src_pitch,
                              surface.width, surface.height))
        return false;
    const uint8_t *src_uv = src + static_cast<size_t>(src_pitch) * surface.height;
    uint8_t *dst_uv = dst + surface.uv_offset;
    return crystalhd_copy_plane(dst_uv,
                                surface.uv_stride ? surface.uv_stride : surface.pitch,
                                src_uv, src_pitch, surface.width,
                                (surface.height + 1) / 2);
}

static void crystalhd_update_pic_info_from_h264(crystalhd_context &ctx,
                                                const VAPictureParameterBufferH264 &pic)
{
    BC_PIB_EXT_H264 &ext = ctx.user_pib_ext;
    ext.valid = 0;
    if (pic.seq_fields.bits.chroma_format_idc == 1) {
        ext.valid |= H264_VALID_VUI;
        ext.chroma_top = pic.picture_height_in_mbs_minus1 + 1;
        ext.chroma_bottom = pic.picture_height_in_mbs_minus1 + 1;
    }

    BC_PIC_INFO_BLOCK &info = ctx.pending_pic_info;
    memset(&info, 0, sizeof(info));
    uint32_t mb_width = pic.picture_width_in_mbs_minus1 + 1;
    uint32_t mb_height = pic.picture_height_in_mbs_minus1 + 1;
    info.width = mb_width * 16;
    uint32_t frame_height = mb_height * 16;
    if (!pic.seq_fields.bits.frame_mbs_only_flag)
        frame_height *= 2;
    info.height = frame_height;
    info.chroma_format = 0x420;
    info.flags = pic.seq_fields.bits.frame_mbs_only_flag ?
        VDEC_FLAG_PROGRESSIVE_SRC : VDEC_FLAG_INTERLACED_SRC;
    ctx.have_pending_pic_info = true;
}

static VAStatus crystalhd_handle_picture_parameters(crystalhd_context &ctx,
                                                    const uint8_t *data,
                                                    size_t stride,
                                                    size_t count)
{
    if (!data || !stride)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (size_t i = 0; i < count; ++i) {
        if (stride < sizeof(VAPictureParameterBufferH264))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        const auto *pic = reinterpret_cast<const VAPictureParameterBufferH264 *>(data + i * stride);
        ctx.last_pic_param = *pic;
        ctx.have_pic_param = true;
        crystalhd_update_pic_info_from_h264(ctx, *pic);
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_handle_slice_parameters(crystalhd_context &ctx,
                                                  const uint8_t *data,
                                                  size_t stride,
                                                  size_t count)
{
    if (!data || !stride)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (stride < sizeof(VASliceParameterBufferH264))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    auto &parsed = ctx.pending_picture.parsed_slice_params;
    try {
        parsed.reserve(parsed.size() + count);
    } catch (const std::bad_alloc &) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    for (size_t i = 0; i < count; ++i) {
        const auto *slice = reinterpret_cast<const VASliceParameterBufferH264 *>(data + i * stride);
        parsed.push_back(*slice);
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_QueryImageFormats(VADriverContextP, VAImageFormat *format_list,
                                            int *num_formats)
{
    if (num_formats)
        *num_formats = 1;
    if (format_list)
        crystalhd_fill_nv12_format(&format_list[0]);
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_CreateImage(VADriverContextP ctx, VAImageFormat *format,
                                      int width, int height, VAImage *out_image)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv || !format || !out_image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (width <= 0 || height <= 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (format->fourcc != VA_FOURCC_NV12)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    crystalhd_image_record image{};
    image.image.image_id = drv->next_image_id++;
    image.image.buf = drv->next_buffer_id++;
    crystalhd_fill_nv12_format(&image.image.format);
    image.image.width = static_cast<uint32_t>(width);
    image.image.height = static_cast<uint32_t>(height);
    image.image.num_planes = 2;
    image.image.pitches[0] = crystalhd_align_up(static_cast<uint32_t>(width), 2);
    image.image.pitches[1] = image.image.pitches[0];
    image.image.offsets[0] = 0;
    image.image.offsets[1] = image.image.pitches[0] * image.image.height;
    image.image.data_size = crystalhd_image_size(image.image.pitches[0], image.image.height);

    try {
        image.storage.resize(image.image.data_size);
    } catch (const std::bad_alloc &) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    drv->images.push_back(std::move(image));
    *out_image = drv->images.back().image;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_DestroyImage(VADriverContextP ctx, VAImageID image_id)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    for (auto it = drv->images.begin(); it != drv->images.end(); ++it) {
        if (it->image.image_id != image_id)
            continue;
        if (it->acquired)
            return VA_STATUS_ERROR_SURFACE_BUSY;
        drv->images.erase(it);
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_INVALID_IMAGE;
}

static VAStatus crystalhd_DeriveImage(VADriverContextP ctx, VASurfaceID surface_id,
                                      VAImage *out_image)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv || !out_image)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    crystalhd_surface *surface = crystalhd_find_surface(drv, surface_id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (!crystalhd_surface_data(*surface))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    crystalhd_image_record image{};
    image.derived = true;
    image.surface = surface_id;
    image.image.image_id = drv->next_image_id++;
    image.image.buf = drv->next_buffer_id++;
    crystalhd_fill_nv12_format(&image.image.format);
    image.image.width = surface->width;
    image.image.height = surface->height;
    image.image.num_planes = 2;
    image.image.pitches[0] = surface->pitch;
    image.image.pitches[1] = surface->uv_stride ? surface->uv_stride : surface->pitch;
    image.image.offsets[0] = 0;
    image.image.offsets[1] = surface->uv_offset;
    image.image.data_size = crystalhd_image_size(surface->pitch, surface->height);

    drv->images.push_back(std::move(image));
    *out_image = drv->images.back().image;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_GetImage(VADriverContextP ctx, VASurfaceID surface_id,
                                   int x, int y, unsigned int width,
                                   unsigned int height, VAImageID image_id)
{
    if (x || y)
        return VA_STATUS_ERROR_UNIMPLEMENTED;

    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_surface *surface = crystalhd_find_surface(drv, surface_id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    crystalhd_image_record *image = crystalhd_find_image(drv, image_id);
    if (!image)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    if (image->derived)
        return VA_STATUS_ERROR_UNIMPLEMENTED;

    if (width != surface->width || height != surface->height)
        return VA_STATUS_ERROR_UNIMPLEMENTED;

    uint8_t *dst = crystalhd_image_data_ptr(drv, *image);
    if (!dst)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    if (!crystalhd_copy_surface_to_image(*surface, dst, image->image.pitches[0]))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_PutImage(VADriverContextP ctx, VASurfaceID surface_id,
                                   VAImageID image_id, int src_x, int src_y,
                                   unsigned int src_width, unsigned int src_height,
                                   int dest_x, int dest_y,
                                   unsigned int dest_width, unsigned int dest_height)
{
    if (src_x || src_y || dest_x || dest_y)
        return VA_STATUS_ERROR_UNIMPLEMENTED;

    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_surface *surface = crystalhd_find_surface(drv, surface_id);
    if (!surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (src_width != surface->width || src_height != surface->height ||
        dest_width != surface->width || dest_height != surface->height)
        return VA_STATUS_ERROR_UNIMPLEMENTED;

    crystalhd_image_record *image = crystalhd_find_image(drv, image_id);
    if (!image)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    if (image->derived)
        return VA_STATUS_ERROR_UNIMPLEMENTED;

    uint8_t *src = crystalhd_image_data_ptr(drv, *image);
    if (!src)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    if (!crystalhd_copy_image_to_surface(src, image->image.pitches[0], *surface))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_AcquireBufferHandle(VADriverContextP ctx, VABufferID buf_id,
                                              VABufferInfo *buf_info)
{
    if (!buf_info)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_image_record *image = crystalhd_find_image_by_buffer(drv, buf_id);
    if (!image)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (image->acquired)
        return VA_STATUS_ERROR_SURFACE_BUSY;

    if (image->derived && image->surface != VA_INVALID_SURFACE) {
        VAStatus status = crystalhd_SyncSurface(ctx, image->surface);
        if (status != VA_STATUS_SUCCESS)
            return status;
    }

    uint32_t requested = buf_info->mem_type;
    uint32_t prime_mask = requested & kCrystalhdPrimeMemTypes;
    bool want_prime = prime_mask;
    if (!requested) {
        want_prime = image->derived && image->surface != VA_INVALID_SURFACE;
    }

    if (want_prime) {
        crystalhd_surface *surface = image->derived ?
            crystalhd_find_surface(drv, image->surface) : nullptr;
        if (surface && surface->dmabuf_backed && surface->dmabuf_fd >= 0) {
            int fd = ::dup(surface->dmabuf_fd);
            if (fd < 0)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            memset(buf_info, 0, sizeof(*buf_info));
            uint32_t selected_mem = prime_mask ? (prime_mask & (~prime_mask + 1))
                                               : VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
            buf_info->mem_type = selected_mem;
            buf_info->type = VAImageBufferType;
            buf_info->handle = static_cast<uintptr_t>(fd);
            buf_info->mem_size = surface->dmabuf_map_len;
            image->acquired = true;
            image->acquired_is_fd = true;
            image->acquired_fd = fd;
            return VA_STATUS_SUCCESS;
        }
        // Fall back to USER_PTR if PRIME export is unavailable.
    }

    uint8_t *addr = crystalhd_image_data_ptr(drv, *image);
    if (!addr)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    memset(buf_info, 0, sizeof(*buf_info));
    buf_info->mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_USER_PTR;
    buf_info->type = VAImageBufferType;
    buf_info->handle = reinterpret_cast<uintptr_t>(addr);
    buf_info->mem_size = image->image.data_size;
    image->acquired = true;
    image->acquired_is_fd = false;
    image->acquired_fd = -1;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_ReleaseBufferHandle(VADriverContextP ctx, VABufferID buf_id)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    crystalhd_image_record *image = crystalhd_find_image_by_buffer(drv, buf_id);
    if (!image)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    if (image->acquired_is_fd && image->acquired_fd >= 0)
        ::close(image->acquired_fd);
    image->acquired_is_fd = false;
    image->acquired_fd = -1;
    image->acquired = false;
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
    surface.uv_stride = pitch;

    size_t y_plane_size = static_cast<size_t>(pitch) * height;
    size_t uv_plane_size = static_cast<size_t>(pitch) * ((height + 1) / 2);
    if (y_plane_size + uv_plane_size != buffer_size)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    try {
        surface.buffer.resize(buffer_size);
    } catch (const std::bad_alloc &) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    surface.uv_offset = static_cast<uint32_t>(y_plane_size);

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
    surface.dmabuf_backed = true;
    surface.dmabuf_fd = desc.dmabuf_fd;
    surface.dmabuf_index = desc.index;

    if (!crystalhd_surface_map_dmabuf(surface, desc)) {
        ::close(desc.dmabuf_fd);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

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
    crystalhd_surface_release_dmabuf(drv, *surface);
    crystalhd_surface_unmap_dmabuf(*surface);
    surface->dmabuf_context = VA_INVALID_ID;
    surface->dmabuf_registered = false;
    surface->dmabuf_in_use = false;
    if (surface->dmabuf_fd >= 0)
        ::close(surface->dmabuf_fd);
    drv->surfaces.erase(std::remove_if(drv->surfaces.begin(), drv->surfaces.end(),
                                       [id](const crystalhd_surface &surf) {
                                           return surf.id == id;
                                       }),
                        drv->surfaces.end());
    for (auto &ctx : drv->contexts)
        ctx.surface_states.erase(id);
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
    for (auto &surface : drv->surfaces) {
        crystalhd_surface_release_dmabuf(drv, surface);
        crystalhd_surface_unmap_dmabuf(surface);
        if (surface.dmabuf_fd >= 0)
            ::close(surface.dmabuf_fd);
    }
    if (drv->surface_device)
        DtsDeviceClose(drv->surface_device);
    drv->contexts.clear();
    drv->configs.clear();
    drv->images.clear();
    drv->surfaces.clear();
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
    HANDLE surf_dev = kCrystalhdEnableDmabufSurfaces ? crystalhd_get_surface_device(drv) : nullptr;
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
            for (auto &surface : drv->surfaces) {
                if (surface.last_context == context_id) {
                    crystalhd_surface_release_dmabuf(drv, surface);
                    if (surface.dmabuf_backed && surface.dmabuf_context == context_id) {
                        surface.dmabuf_context = VA_INVALID_ID;
                        surface.dmabuf_registered = false;
                    }
                }
            }
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
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 0;
    ctx->max_image_formats = 1;

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
    ctx->vtable->vaAcquireBufferHandle = crystalhd_AcquireBufferHandle;
    ctx->vtable->vaReleaseBufferHandle = crystalhd_ReleaseBufferHandle;
    ctx->vtable->vaBeginPicture = crystalhd_BeginPicture;
    ctx->vtable->vaRenderPicture = crystalhd_RenderPicture;
    ctx->vtable->vaEndPicture = crystalhd_EndPicture;
    ctx->vtable->vaSyncSurface = crystalhd_SyncSurface;
    ctx->vtable->vaQuerySurfaceStatus = crystalhd_QuerySurfaceStatus;
    ASSIGN_VTABLE_STUB(ctx->vtable, vaQuerySurfaceError);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaPutSurface);
    ctx->vtable->vaQueryImageFormats = crystalhd_QueryImageFormats;
    ctx->vtable->vaCreateImage = crystalhd_CreateImage;
    ctx->vtable->vaDestroyImage = crystalhd_DestroyImage;
    ASSIGN_VTABLE_STUB(ctx->vtable, vaSetImagePalette);
    ctx->vtable->vaGetImage = crystalhd_GetImage;
    ctx->vtable->vaPutImage = crystalhd_PutImage;
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
    ctx->vtable->vaDeriveImage = crystalhd_DeriveImage;
    ctx->vtable->vaCreateSurfaces2 = crystalhd_CreateSurfaces2;
    ctx->vtable->vaExportSurfaceHandle = crystalhd_ExportSurfaceHandle;
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
