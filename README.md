# Crystal HD Hardware Decoder Driver

Out-of-tree DKMS-friendly driver for the Broadcom BCM70012/BCM70015 "Crystal HD"
hardware decoder. The tree focuses on the kernel module; older user-space samples
and GStreamer filters now live under `archive/`.

## Prerequisites

Install your distro's kernel headers, compiler toolchain, and DKMS tooling. The
modernized driver has been tested on Linux 6.18/6.19 and keeps compatibility
paths for kernels down to 4.6, but anything older is untested. For Arch Linux:

```bash
sudo pacman -S base-devel dkms linux-headers git autoconf
```

For other distributions install the equivalent `gcc`, `make`, `autoconf`,
`dkms`, and kernel-header packages.

## Building the module

1. Clone and enter the repository:
   ```bash
   git clone https://github.com/twopoint71/crystalhd.gi
   cd crystalhd
   ```
2. Generate the makefiles:
   ```bash
   cd driver/linux
   autoconf
   ./configure --with-kernel-path=/lib/modules/$(uname -r)/build
   ```
3. Build against the current kernel tree:
   ```bash
   make -C /lib/modules/$(uname -r)/build M=$PWD modules
   ```

> **Tip:** If your `/lib/modules/.../build` tree is read-only, copy it into a
> writable directory and point `./configure --with-kernel-path=/path/to/copy` at
> that location before running `make`.

## Installing without DKMS

From the repository root run:

```bash
sudo make -C driver/linux install
sudo modprobe crystalhd
```

Check `lsmod | grep crystalhd` or `dmesg | grep crystalhd` to confirm the driver
loaded.

## Using DKMS

Follow the steps in [README.dkms](README.dkms) to `dkms add`, `build`, and
`install` the module so it automatically rebuilds for new kernels.

## Archived components

The legacy user-space library, GStreamer filters, and sample applications are no
longer maintained but have been preserved under the `archive/` directory:

- `archive/linux_lib`
- `archive/filters`
- `archive/examples`

These projects are optional and not required for building or packaging the
kernel module.

## Firmware

The firmware binaries shipped under `firmware/` should be installed automatically
by `make install` or the DKMS post-install hook. If you need to install them
manually run `sudo ./install_firmware_dkms.sh` from the repository root.

## Modern kernel updates

Recent work to keep this driver building on Linux 6.1x includes:

- Switching the module build to the supported `M=<path>` kbuild flow (`driver/linux/Makefile.in`).
- Updating DMA interactions to the `dma_*` APIs and replacing `get_user_pages()` with
  `pin_user_pages()`/`unpin_user_pages()` plus scatter/gather mapping via
  `dma_map_sg()` (see `driver/linux/crystalhd_misc.c`).
- Using `dma_set_mask_and_coherent()` and `class_create()`'s new signature to silence
  kernel deprecation errors (`driver/linux/crystalhd_lnx.c`).
- Replacing `rdtscll()` with `ktime_get_ns()` and marking internal helpers `static`
  to satisfy modern compilers (`driver/linux/crystalhd_hw.c` and
  `driver/linux/crystalhd_fleafuncs.c/h`).

If you pick up a newer kernel and hit additional API churn, check the kernel
`Documentation/process/changes.rst` file and the driver sources above for examples
of how these transitions were done. Expect best results on kernels 6.1x but the
compatibility shims should keep everything working back to Linux 4.6.

## History

Forked from https://github.com/dbason/crystalhd 

See [HISTORY.md](HISTORY.md) for the background on the various driver versions.
