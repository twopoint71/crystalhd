#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <new>
#include <vector>

#include <va/va_backend.h>
#include <va/va_backend_vpp.h>

#include "7411d.h"
#include "bc_dts_glob_lnx.h"
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
};

struct crystalhd_driver_state {
    std::vector<crystalhd_config> configs;
    std::vector<crystalhd_context> contexts;
    VAConfigID next_config_id = 1;
    VAContextID next_context_id = 1;
};

static inline crystalhd_driver_state *crystalhd_drv(VADriverContextP ctx)
{
    return ctx ? reinterpret_cast<crystalhd_driver_state *>(ctx->pDriverData) : nullptr;
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

static VAStatus crystalhd_terminate(VADriverContextP ctx)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_SUCCESS;

    for (auto &context : drv->contexts) {
        if (context.device)
            DtsDeviceClose(context.device);
    }
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
    drv->contexts.push_back(ctx_info);

    if (context_id)
        *context_id = ctx_info.id;

    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_DestroyContext(VADriverContextP ctx, VAContextID context_id)
{
    auto *drv = crystalhd_drv(ctx);
    if (!drv)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    for (auto it = drv->contexts.begin(); it != drv->contexts.end(); ++it) {
        if (it->id == context_id) {
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
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateSurfaces);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaDestroySurfaces);
    ctx->vtable->vaCreateContext = crystalhd_CreateContext;
    ctx->vtable->vaDestroyContext = crystalhd_DestroyContext;
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateBuffer);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaBufferSetNumElements);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaMapBuffer);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaUnmapBuffer);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaDestroyBuffer);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaBeginPicture);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaRenderPicture);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaEndPicture);
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
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateSurfaces2);
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

VAStatus __vaDriverInit_1_22(VADriverContextP ctx)
{
    return crystalhd_driver_init(ctx);
}
