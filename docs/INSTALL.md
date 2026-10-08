# Installing freebsd-media (V4L2 + Cedrus) on an Allwinner A64 FreeBSD arm64 system

What you get: `dmabuf_base.ko`, `media.ko`, `cedrus.ko` (and, if you want, `vtest.ko`), the nodes
`/dev/video0` and `/dev/media0`, hardware H.264 and MPEG-2 decoding, and FFmpeg (a patched branch) as the
first client. Everything below the "Status" line was written on a host that has no FreeBSD machine of its
own: what was actually run is listed in section 9.

## 0. The ABI warning (read this first)

A FreeBSD kernel module is compiled against the kernel's headers. `struct device`, `struct cdev`,
`struct sglist` and friends change between kernel builds, and a module built against other headers
**loads without complaint and reads the wrong offsets** (this cost a panic once: "struct device ABI
drift", see `HANDOVER.md`). So:

- build the modules against the sources, or the headers (`/usr/src/sys`), **of the kernel that will load
  them**, same branch, same `uname -v`, same kernel config options (`options VIMAGE`, `SMP`, `MAC` ...
  show up in `opt_*.h`);
- on the hypervisor guest ("the board") that tree is `/opt/bzdos/freebsd-src-earlyboot-wt`, and nothing else
  (see `TREES.md` of the bzdos workspace and `docs/cedrus-facts/HANDOVER.md` section 2);
- after upgrading the kernel (`freebsd-update`, a new `buildkernel`), rebuild and reinstall the modules.

## 1. What the system must provide

On the stock FreeBSD 15.1 arm64 kernel with an A64 device tree (the Banana Pi M64 under the bzdk hypervisor
provides all of these; on real hardware check each):

| Requirement | Why | How to check |
|---|---|---|
| FreeBSD 15.1 arm64, kernel with `FDT`, `VIMAGE`, `SMP` as in GENERIC | module ABI | `uname -a`, `sysctl kern.conftxt` |
| A device tree node `allwinner,sun50i-a64-video-engine` with `status = "okay"`, one register window, clocks `ahb`, `mod`, `ram`, one reset | the driver probes on it | `ofwdump -a \| grep -i video` |
| the A64 CCU driver (`aw_ccu`, `ccu_a64`) and `hwreset`, `clk` | clocks/reset of the engine | `kldstat`, `sysctl -a \| grep clock` |
| 4 KB pages, DRAM based at 0x40000000, the first 256 MiB reachable by the engine | the engine's DMA window (`DRAM_BASE`, `DMA_WINDOW` in `cedrus_priv.h`) | `sysctl hw.physmem`, `dmesg \| grep -i "real memory"` |
| contiguous free memory | 1080p OUTPUT buffers are 3 MB contiguous; capture buffers too | see section 5 (load early) |
| root can load kernel modules (`kern.securelevel` <= 0 when loading late) | `kldload` | `sysctl kern.securelevel` |

`pll_ve` is declared in the stock `ccu_a64.c` without `AW_CLK_HAS_GATE`; the driver switches the PLL on
itself, so no kernel patch is required (`hal/lima/patches` in the bsdOS tree has the proper fix, optional).

Exactly what the **bzdk hypervisor guest** provides, from the project's facts (`docs/cedrus-facts`,
HANDOVER): a FreeBSD 15.1 arm64 guest on four Cortex-A53 cores with about 850 MB of usable RAM; the video engine with its
clocks and reset as a device tree node (the hypervisor can log the engine's register accesses, but does not
emulate it);
a root file system that is read-only, `/var` on the SD card, `/tmp` on tmpfs (**wiped at every reboot**,
which is why nothing may live in `/tmp`); a display owned by the hypervisor (the guest window is driven
through `bzkms`, a KMS driver, with `lima` for the Mali-400); `libdrm` from packages in `/usr/local`.
Anything not in this paragraph is not promised.

## 2. Build

### 2a. Natively on the FreeBSD machine

You need the sources (or at least `sys/`) of the running kernel in `/usr/src`, e.g. `git clone -b <your
release branch> https://git.freebsd.org/src.git /usr/src` (`/usr/src/sys` is enough for modules), and this
repository checked out anywhere:

    cd freebsd-media
    make                       # dmabuf_base.ko media.ko cedrus.ko    (WITH_VTEST=1: also vtest.ko)
    make install               # to /boot/modules, then kldxref (bsd.kmod.mk does it)
    make install-man           # man pages to /usr/local/share/man (MANPATH must include it)
    make install-conf          # /boot/loader.conf.d/v4l2-cedrus.conf; samples to share/examples

`SYSDIR=/path/to/sys` points at a different source tree. `make` builds the modules in the order the
dependencies need: `dmabuf_base`, then `media`, then `cedrus`. The module Makefiles follow
`sys/modules/*` conventions (`KMOD=`, `SRCS=`, `.PATH`, `bsd.kmod.mk`; `KMODDIR` defaults to
`/boot/modules`; dependencies are `MODULE_DEPEND` records in the code: `media` on `dmabuf_base`, `cedrus` on
`media`) and also work if this repository is dropped into `/usr/src` (the layout mirrors it).
They install to `/boot/modules` rather than `/boot/kernel` so that `installkernel` does not remove them.

### 2b. Cross-build on a Linux host (what the project does)

Needs `bmake`, `clang`, `lld`, `llvm-objcopy`, `llvm-nm`, and the kernel's source tree:

    FBSD_SRC=/opt/bzdos/freebsd-src-earlyboot-wt \
    DESTDIR=/path/to/stage OBJ=/path/to/objdir \
        sys/dev/media/tools/stage.sh

or `make stage DESTDIR=/path/to/stage` (variables `FBSD_SRC`, `OBJ`, `PREFIX`, `MODULES` as environment).
One module at a time: `LC_ALL=C OBJ=<dir>/<module> sys/dev/media/tools/xbuild.sh dmabuf|media|cedrus|vtest`,
which prints the `.ko`; `... xbuild.sh <module> install DESTDIR=<stage>` installs it (GNU `install` through
`gnu-install-wrap.sh`, no `kldxref`). Build output may be in Russian on this project's hosts: use `LC_ALL=C`
and check the exit code.

The stage contains `boot/modules/*.ko`, `boot/loader.conf.d/v4l2-cedrus.conf`, the man pages and
`usr/local/share/examples/freebsd-media/{kld,devfs.rules}`. Copy it to the target as root, so the files
get root ownership: `tar -C stage -cf - . | ssh root@target tar -C / -xpf -` (on a read-only root,
`mount -u -w /` first, on the project's guest see its own notes), then on the target:

    kldxref /boot/modules

## 3. Load and check

    kldload dmabuf_base media cedrus        # in this order; "kldload cedrus" alone finds the others only through
                                            # /boot/modules/linker.hints (kldxref); "No such file or directory"
                                            # means a missing dependency
    kldstat | grep -E 'dmabuf_base|media|cedrus'
    dmesg | tail                            # "cedrus0: stateless decoder (MPEG-2, H.264): /dev/video0, /dev/media0"
    ls -l /dev/video0 /dev/media0
    sysctl dev.cedrus.0.pictures dev.cedrus.0.errors

A panic during a first load leaves no trace after the reboot: start a console capture before testing.

## 4. Permissions

The nodes are `root:operator 0660`. To give group `video` access: merge `etc/devfs.rules` into
`/etc/devfs.rules`, put `devfs_system_ruleset="v4l2"` into `/etc/rc.conf`, `pw groupshow video || pw groupadd video`,
`pw groupmod video -m <user>`, `service devfs restart`. `devfs.conf` does not work for this: it runs at boot
before the modules create the nodes. No `devd` rule is needed (a rule would only be a place to run a
`chgrp`; the ruleset covers nodes as they appear).

## 5. Loading at boot

Two ways, choose one.

**From the loader (recommended)**: copy `etc/loader.conf.d/v4l2-cedrus.conf` to `/boot/loader.conf.d/`
(`make install-conf` does). The modules are loaded before userland runs, when physical memory is still
unfragmented. The decoder needs large contiguous blocks (3 MB OUTPUT buffers for 1080p, and contiguous
capture buffers); after a long uptime, or with the GPU stack loaded, `VIDIOC_CREATE_BUFS` fails with `ENOMEM`
and only a reboot helps. A reserved pool tunable (`hw.cedrus.pool_mb`) is **planned**, not in the driver yet; the
snippet carries it commented out, and a tunable the driver does not read is ignored.

**From rc**: `kld_list="${kld_list} dmabuf_base media cedrus"` in `/etc/rc.conf` (or `rc.conf.d/kld`, sample
in `share/examples/freebsd-media/kld`). Later in the boot, so the memory is more fragmented.

Tunables of the driver (all optional): `hw.cedrus.pm` (1), `hw.cedrus.lab` (0), `hw.cedrus.lab_size`
(8 MiB), `hw.cedrus.lab_lo_mb` (0); the run-time sysctls are in `cedrus(4)`.

### GPU stack (lima, bzkms)

`dmabuf_base` is our native dma-buf. drm-kmod's module `dmabuf.ko` is another thing with the same job. To
show decoded frames on the GPU without a copy, the stock drm-kmod `dmabuf.ko` has to be replaced by the
bridged build (drm-kmod patch 0006, bsdOS `hal/lima/patches/drm-kmod`), which depends on `dmabuf_base`.
Load order: `dmabuf_base`, `media`, `cedrus`, `dmabuf` (bridged), `drm`, `lima`, `bzkms`. With the stock
`dmabuf.ko` the decoder works and frames can be copied out, but a GPU cannot import them.

## 6. FFmpeg

FFmpeg's releases (up to 9.0.2) have no stateless V4L2 hwaccel; Kwiboo's branch `v4l2-request-n8.1` does.
Two ways:

- **Ports skeleton** (untested, section 9): `ports/multimedia/ffmpeg-v4l2request` with
  `ports/devel/libudev-bzdos` (a 150-line libudev over devfs, `sys/dev/media/tools/ffmpeg/libudev-shim.c`)
  and `ports/devel/v4l2-uapi-bzdos` (the Linux uapi headers of `sys/dev/media/uapi` and `shim`). Copy the three
  directories into a ports tree, `make makesum` and build in poudriere. FFmpeg ends up in
  `/usr/local/ffmpeg-v4l2request/bin/ffmpeg`.
- **Cross-build as the project does**: `sys/dev/media/tools/ffmpeg/build-ffmpeg.sh all` (needs a sysroot of the
  target and the board's address: read the script) installs into `/var/tmp/ffv4l2` on the guest, because
  `/var` is the only persistent writable place there.

Use: `ffmpeg -hwaccel v4l2request -i in.mp4 -f null -`. The libudev shim must be found: with the port its
directory is in the rpath; with the manual build `LD_LIBRARY_PATH=/var/tmp/ffv4l2/lib:/var/tmp/ffv4l2/udev/lib`.
What a client has to do (and what it may not assume) is in `v4l2_stateless(7)`; speed and the zero-copy
path are in measurements M039, M040.

## 7. Uninstall

    kldunload cedrus media dmabuf_base      # dmabuf_base cannot unload while a buffer exists
    rm /boot/modules/{cedrus,media,dmabuf_base,vtest}.ko /boot/loader.conf.d/v4l2-cedrus.conf; kldxref /boot/modules

## 8. Files

| Path | What |
|---|---|
| `Makefile` | top level: `all`, `install`, `install-man`, `install-conf`, `stage` |
| `sys/modules/{dmabuf,media,cedrus,vtest}/Makefile` | module Makefiles (module names `dmabuf_base`, `media`, `cedrus`, `vtest`) |
| `sys/dev/media/tools/xbuild.sh`, `stage.sh`, `gnu-install-wrap.sh` | cross-build, install into a staging root |
| `etc/loader.conf.d/v4l2-cedrus.conf`, `etc/rc.conf.d/kld`, `etc/devfs.rules` | boot integration |
| `share/man/man4/{media,cedrus,dmabuf}.4`, `share/man/man7/v4l2_stateless.7` | manual pages |
| `ports/` | FreeBSD ports skeletons for the consumer |

## 9. Status of this document: what has and has not been tested

Run on the Linux build host: the cross-build of `dmabuf_base`, `media`, `cedrus` and `vtest` against
`freebsd-src-earlyboot-wt`; `make install` of each into a staging root (through the GNU install wrapper);
`stage.sh` end to end; `bmake -n install-man install-conf all` of the top-level Makefile; a lint of the four man
pages with `groff -mdoc -ww` (`mandoc` is not installed there).

**Not tested**: a native `make` / `make install` on a FreeBSD machine (no FreeBSD host was available; `kldxref`
was never run); loading the staged modules on the board (deliberately not touched); the loader.conf.d
snippet and the devfs.rules ruleset on a live system; the ports (no ports tree or poudriere; only the
FFmpeg distfile SHA256/size are real: `963ed87e...a1c4`, 16908150 bytes, commit `b57fbbe5`); the claim that
the stock drm-kmod `dmabuf.ko` can be replaced by the bridged one in the load order given.
