# CrystalHD driver modernization notes

## Repository overview
- Legacy Broadcom BCM70012/BCM70015 driver with DKMS metadata, firmware blobs, and userland samples.
- Kernel module sources live under `driver/linux`; userland libraries/plugins under `linux_lib`, `filters`, and `examples` appear unmaintained.

## Build/testing notes
- Kernel 6.19 headers under `/usr/lib/modules/$(uname -r)/build` are read-only in this environment; clone them into a workspace directory and build with `make -C <cloned tree> M=$PWD modules`.
- After building, run `make clean` in `driver/linux` to drop artifacts; avoid checking in `Module.symvers` or `.mod` files.

## Recent updates
- Updated makefile to use `M=` builds and `ccflags-y` include paths so DKMS works with current kernels.
- Switched DMA API usage to `dma_*` helpers and `pin_user_pages`/`unpin_user_pages` for Linux ≥ 5.10.
- Replaced legacy `rdtscll` calls with `ktime_get_ns()` and removed obsolete PCI APIs.
- VA-API shim now builds Annex-B bitstreams (with start codes) and submits them via `DtsProcInput`, tracks DMA/malloc surfaces through decode, and logs whether output came back via DMA-BUF or memcpy. DMA buffers/contexts are released on teardown to keep the RX pool healthy.

## Follow-ups / cleanup ideas
- Audit non-driver directories (`examples`, `filters/gst`, `linux_lib`) to decide whether to archive or drop them from DKMS packaging.
- Simplify the remaining `#if LINUX_VERSION_CODE` guards now that only 5.x/6.x kernels are in scope.
- Refresh README instructions for DKMS + modern distros and ensure firmware install script still matches current udev paths.
- Consider adding CI or a simple `make dkms` wrapper to automate the kernel tree copy + module build.
- Finish H.264 plumbing in `linux_lib/libvaapi/va_crystalhd.cpp`: cache SPS/PPS, prepend them before IDR slices, recycle CrystalHD buffers immediately after `vaSyncSurface`, and pipe PIB metadata back to VA queries. VLC still needs to be coaxed into using VA decode; current traces stop after surface creation.
