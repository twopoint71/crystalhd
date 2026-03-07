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

## Open Questions
- How should we stage SPS/PPS/SEI blobs for CrystalHD? (Need to inspect `BC_INPUT_FORMAT` expectations.)
- Do we need to use the "NoCopy" interface (`DtsProcOutputNoCopy`) for better performance, or is copying into VA surfaces acceptable for now?
- Does the firmware require specific alignment/size for Y/UV buffers beyond DWORD units already exposed in `BC_DTS_PROC_OUT`?

---
Feel free to append updates or TODOs here so we can recover context quickly after interruptions.
