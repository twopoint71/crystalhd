# CrystalHD VA-API Work Notes

## Current Status (Mar 7, 2026)
- `linux_lib/libvaapi/va_crystalhd.cpp` now tracks configs/contexts internally and wires up basic VA entrypoints for profile/query plus context open/close using `DtsDeviceOpen`/`DtsDeviceClose`.
- All surface/buffer/picture operations are still routed through the "unimplemented" stub helper, so VA clients cannot actually submit streams or retrieve decoded frames yet.
- Decoder state is initialized only when contexts are created; there is not yet any codec-specific configuration (no `DtsOpenDecoder`, `DtsSetInputFormat`, etc.).

## Near-Term TODOs
1. **VASurface plumbing**
   - Add per-context surface table storing CrystalHD backing buffers, dimensions, and ownership state.
   - Implement `vaCreateSurfaces`/`vaDestroySurfaces` (and 2-variant) to allocate/free those buffers.
   - Decide how decoded frames will land in VA surfaces (copy from `BC_DTS_PROC_OUT` into surface buffer vs. share DMA buffer).
2. **Buffer ingestion**
   - Track submitted VA buffers (picture params, IQ matrices, slices) and translate them into CrystalHD bitstream packets.
3. **Picture submission**
   - Implement `vaBeginPicture`/`vaRenderPicture`/`vaEndPicture` to feed aggregated bitstreams to `DtsProcInput` and remember which surface to fill.
4. **Output path**
   - Implement `vaSyncSurface`/`vaQuerySurfaceStatus` to call `DtsProcOutput`, copy data into the pending surface, and propagate metadata/errors.
5. **Codec coverage**
   - Start with H.264 Main/High/Constrained Baseline as implemented today; leave hooks to extend to MPEG-2/VC-1 later.

## Design Notes
- Keep structures POD-friendly so they can be reused from C entrypoints if needed.
- Surfaces should probably be software buffers (malloc) initially; DMA export can be explored later if performance demands it.
- Map VA surface IDs to indices in a context-local vector to avoid sharing surfaces across contexts.
- Track outstanding decode submissions so we know which surface `DtsProcOutput` should fill once a frame is ready.

## DMA Export Exploration
The malloc-backed surface path is safe but leaves a full-frame memcpy on every `DtsProcOutput`, which is painful on very slow CPUs. Exporting the RX buffers that the driver already fills into DMA-BUFs lets VA surfaces talk directly to CrystalHD memory, eliminating copies and lowering cache pollution.

### Goals
- Zero-copy path for `NV12`/`YV12` outputs via `VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME`.
- Keep legacy software surfaces as a fallback so we can still run on kernels without DMA-BUF helpers or when imports fail.
- Bound CPU involvement on low-end hardware by preallocating, pinning, and recycling buffers entirely in the driver.

### Proposed Flow
1. **Driver allocation/export**
   - Allocate RX capture buffers out of `dma_alloc_attrs(..., DMA_ATTR_WRITE_COMBINE)` so both CrystalHD and the CPU can touch them cheaply.
   - For each capture buffer, create a `dma_buf` via `dma_buf_export()` and keep the `struct dma_buf` handle inside the RX pool metadata (near `struct crystalhd_rx_dma_pkt`).
   - Implement a lightweight IOCTL (`BCM_IOC_RX_DMABUF_EXPORT`) that returns an array of `dma_buf` file descriptors plus the matching Y/UV offsets, strides, and capacity so userland can pre-build VASurfaces without touching the firmware yet.
2. **Userspace surface setup**
   - Extend `linux_lib/libvaapi/va_crystalhd.cpp` to request DMA-BUF-backed surfaces inside `vaCreateSurfaces()`. Each VASurface keeps: dma-buf fd, plane offsets, VA image format, and the RX tag that must be acknowledged with `DtsReleaseOutputBuffs`.
   - When DMA export is unavailable (old kernel, fd exhaustion, etc.), fall back to malloc surfaces automatically.
3. **Decode submission**
   - Use `DtsProcOutputNoCopy()` so the firmware hands us a filled RX buffer without copying. The VA surface just records which dma-buf is now "pending present".
4. **Synchronization & lifetime**
   - `vaSyncSurface` / `vaQuerySurfaceStatus` invoke `DtsReleaseOutputBuffs()` only after VA clients signal completion. We may need a refcount per RX buffer to avoid re-queuing it to hardware while VA still owns it; start with a simple fence that keeps one outstanding decode per surface.
   - Use explicit cache maintenance (`dma_sync_single_for_cpu`/`_for_device`) in the driver before CrystalHD writes to a buffer and before handing it to VA (mainly needed once we rely on WC pages).

### Low-End Hardware Considerations
- **Batch exports**: Sending all fd metadata in a single IOCTL amortizes syscalls vs. per-surface roundtrips.
- **Small working set**: Keep the RX pool size minimal (e.g., 6–8 buffers) and tie `vaMaxNumSurfaces` to whatever the firmware can keep decoded so we do not waste RAM.
- **Power/thermal**: DMA export avoids memcpy, which tends to dominate CPU watts on these platforms.
- **Fallback safety**: If DMA-BUF negotiation fails mid-stream, drop back to the malloc path instead of wedging the decoder.

### Open Questions / Follow-Ups
- Do we need two dma-bufs per surface (one for Y, one for UV) or can we expose a single fd with plane offsets? Kernel RX descriptors already hold both addresses, so a single export with metadata seems doable.
- What format do downstream compositors expect? If they insist on modifiers, we may need to advertise `DRM_FORMAT_NV12` with linear modifier.
- We probably need a userspace helper to dup/close the fds per VA context lifetime; figure out how to integrate that with the existing `DtsInitInterface` teardown path.

For now we should prototype the IOCTL + exporter in the driver and teach `va_crystalhd` to advertise DMA-BUF support under a feature flag. Once that is stable we can flip the default for low-end builds where memcpy is too expensive.

### Implementation Notes (WIP)
- `BCM_IOC_EXPORT_DMABUF` exports RX capture buffers once per request and returns DMA-BUF file descriptors plus basic layout metadata; surfaces close those fds when destroyed.
- `va_crystalhd` now calls `DtsExportRxDmabufs()` when creating NV12 surfaces and automatically falls back to host malloc buffers if the IOCTL fails or the kernel returns fewer fds than requested.
- The VA shim opens a dedicated CrystalHD handle for surface allocation so surface creation still works before a context is established.
- VA contexts now open/configure/start the H.264 decoder immediately, track VA buffers in a reuse-friendly pool, and expose `vaCreateBuffer`/`vaRenderPicture`/`vaEndPicture` to push aggregated slice data through `DtsProcInput`.
- Each surface records a lightweight state machine (idle/submitted/pending output) so `vaSyncSurface` can call `DtsProcOutputNoCopy` only when a surface is expected to receive a frame, minimizing polling overhead on low-end CPUs.

## Open Questions
- How should we stage SPS/PPS/SEI blobs for CrystalHD? (Need to inspect `BC_INPUT_FORMAT` expectations.)
- Do we need to use the "NoCopy" interface (`DtsProcOutputNoCopy`) for better performance, or is copying into VA surfaces acceptable for now?
- Does the firmware require specific alignment/size for Y/UV buffers beyond DWORD units already exposed in `BC_DTS_PROC_OUT`?

---
Feel free to append updates or TODOs here so we can recover context quickly after interruptions.
