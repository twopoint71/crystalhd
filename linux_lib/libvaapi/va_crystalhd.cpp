#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>

#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"

static VAStatus crystalhd_terminate(VADriverContextP ctx)
{
    (void)ctx;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_QueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list, int *num_profiles)
{
    (void)ctx;
    (void)profile_list;
    if (num_profiles)
        *num_profiles = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus crystalhd_QueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
                                               VAEntrypoint *entrypoint_list,
                                               int *num_entrypoints)
{
    (void)ctx;
    (void)profile;
    (void)entrypoint_list;
    if (num_entrypoints)
        *num_entrypoints = 0;
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

#define ASSIGN_VTABLE_STUB(vtbl, field) \
    do { (vtbl)->field = reinterpret_cast<decltype((vtbl)->field)>(crystalhd_stub_unimplemented); } while (0)

static VAStatus crystalhd_driver_init(VADriverContextP ctx)
{
    if (!ctx || !ctx->vtable)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    memset(ctx->vtable, 0, sizeof(*ctx->vtable));
    if (ctx->vtable_vpp)
        memset(ctx->vtable_vpp, 0, sizeof(*ctx->vtable_vpp));

    ctx->str_vendor = "Broadcom CrystalHD";
    ctx->max_profiles = 0;
    ctx->max_entrypoints = 0;
    ctx->max_attributes = 0;
    ctx->max_subpic_formats = 0;
    ctx->max_display_attributes = 0;
    ctx->max_image_formats = 0;

    ctx->vtable->vaTerminate = crystalhd_terminate;
    ctx->vtable->vaQueryConfigProfiles = crystalhd_QueryConfigProfiles;
    ctx->vtable->vaQueryConfigEntrypoints = crystalhd_QueryConfigEntrypoints;

    ASSIGN_VTABLE_STUB(ctx->vtable, vaGetConfigAttributes);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateConfig);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaDestroyConfig);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaQueryConfigAttributes);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateSurfaces);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaDestroySurfaces);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaCreateContext);
    ASSIGN_VTABLE_STUB(ctx->vtable, vaDestroyContext);
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
