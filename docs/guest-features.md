# Guest feature set

What the default build enables for guests beyond the board bring-up set, why
each piece is there, and how to verify it inside a guest. CI merges these
fragments on every build and audits them symbol by symbol (`ci/cfg-check.py`);
this page is the narrative behind them.

## Fragments

All of them live in `tools/um-arm64/config/`, and `build-bionic.sh` takes them
by name in `EXTRA_CONFIG` (resolved from `tools/um-arm64/`).

| fragment | enables | consumers |
|---|---|---|
| `config/docker.config` | docker's `check-config.sh` set: namespaces (incl. `USER_NS`), cgroups (`MEMCG`, `CGROUP_BPF`), overlayfs, veth/bridge/netfilter/nftables, seccomp, keys, POSIX mqueues, `IKCONFIG` for `/proc/config.gz`, ext4 ACL/security xattrs | docker and container runtimes in Linux guests |
| `config/docker-completions.config` | the companions `docker.config` needs so none of its requests silently drops: `NET_SCHED` (gates the classifier menu), `NFT_FIB_IPV6` (`NFT_FIB_INET` depends on it) | completeness of the above |
| `config/snap-flatpak.config` | squashfs + zlib/lzo/xz/zstd + xattr (snap images are loop-mounted squashfs; `BLK_DEV_LOOP` is already in the defconfig); `FUSE_FS` (flatpak's document and revoke portals — and the same driver Android's sdcard daemon needs) | snap, flatpak, Android guests |
| `config/android-guest.config` | binder (`ANDROID_BINDER_IPC/FS/DEVICES`), the `FTRACE` + `SCHED_TRACER` pair that brings `EVENT_TRACING` on (this tree's `ANDROID_BINDER_IPC` depends on it), `PSI` (lmkd), netd's remaining netfilter faces | redroid-class Android guests |
| `config/umfb.config` | the display sink: `FB`/`FB_DEVICE` behind `UML_IOMEM_EMULATION`, plus `UML_FB` (dormant unless `umlfb=` is on the guest command line) | Linux guests (Xorg/weston on fbdev), Android guests (SurfaceFlinger/gralloc), the app's display screen |

Variants, not part of the default set:

- `config/page4k.config` — 4 KB guest pages instead of 16 KB.
- `config/vt.config` — `CONFIG_EXPERT` + `CONFIG_VT` with `VT_CONSOLE` off, for
  a guest that needs a VT for a real X server. It is not in `umfb.config` on
  purpose: `vty_init()` takes major 4, the UML console then owns no tty
  devices, `/dev/console` disappears for userland and the marker gate reads
  nothing (measured 2026-10-18). A VT build's gate verdict has to come from the
  channel console.
- `config/virtio-gpu.config` — DRM and virtio-gpu over `UML_PCI_OVER_VIRTIO`
  (DRM only gets `HAS_DMA` through `UML_DMA_EMULATION`; asking for `CONFIG_DRM`
  without the PCI path silently yields "not set"). `ci/vugpu2d.py` is the 2D
  backend the gate boots against.
- `config/virtio-snd.config`, `config/usb-wifi.config` — the guest-side sound
  card and the USB/IP wifi path.

## Deliberately not here

- `CPUSETS` and any SMP-dependent controller: the kernel has SMP now
  (`docs/smp.md`), but the controller has not been through a docker run on a
  device yet; `docker info` reports its usual cpuset warning and runs.
- AppArmor: not wired into guest boot; snapd runs without confinement profiles.
  Adding an LSM means choosing a story for every guest.
- Loadable modules: the kernel is built without module support; everything is
  `=y` and guest images carry no modutils.

## Verifying inside a guest

What kernel the guest really runs (`IKCONFIG_PROC` is part of the set):

    zcat /proc/config.gz | grep -E 'USER_NS|OVERLAY_FS|SQUASHFS|FUSE_FS|VETH|BPF_SYSCALL'

docker, with the same yardstick docker itself uses:

    apt-get update && apt-get install -y docker.io
    docker info                 # expect one WARNING: No cpuset support (UP kernel)
    docker run --rm hello-world # needs the guest's network (umnet + passt)

snap, kernel side — snap images are squashfs, loop-mounted:

    apt-get install -y squashfs-tools
    mksquashfs /etc /tmp/etc.sqfs && mkdir -p /mnt && mount -o loop /tmp/etc.sqfs /mnt && ls /mnt

flatpak, sandbox side — bubblewrap exercises `USER_NS` + seccomp, the portal
side needs `/dev/fuse`:

    apt-get install -y bubblewrap
    unshare -Urmpf true && echo userns-ok
    ls -l /dev/fuse

## Maintenance

- `python3 ci/cfg-check.py <build .config> <fragment>...` re-runs the audit
  locally. It is part of the CI assert step, so a dropped request is a red run
  naming the symbol, not a surprise on a device.
- When the port's `docker.config` grows a companion for `NET_CLS_CGROUP` or
  `NFT_FIB_IPV6` itself, delete the line from `docker-completions.config`; the
  audit stays green either way.
- The 4 KB variant merges on top of the same set; nothing here is page-size
  dependent.
- Kernel image growth is not free on the app side: the loader maps the kernel
  at `0x60000000` and sits at a fixed base just above the image end. When the
  image end crosses that base, every boot dies with exit 64 (`-EEXIST` from
  the image mapper) — the display fragment set did exactly that once, and the
  base was moved. Re-check after any config that grows the kernel.
