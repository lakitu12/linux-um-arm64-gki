# Guest feature set

What the default build enables for guests beyond the board bring-up set, why
each piece is there, and how to verify it end to end.  CI merges these
fragments on every build and audits them symbol by symbol in the assert step
(`ci/cfg-check.py`); this page is the narrative behind them.

## Fragments

| fragment | where | enables | consumers |
|---|---|---|---|
| `config/docker.config` | the port's patch stack | docker's `check-config.sh` set: namespaces (incl. `USER_NS`), cgroups (+`MEMCG`, `CGROUP_BPF`), overlayfs, veth/bridge/netfilter/nftables, seccomp, keys, POSIX mqueues, `IKCONFIG` for `/proc/config.gz`, ext4 ACL/security xattrs | docker / containers in Linux guests |
| `ci/docker-completions.config` | this repo | the companions `docker.config` needs so none of its requests silently drops: `NET_SCHED` (gates the classifier menu), `NFT_FIB_IPV6` (`NFT_FIB_INET` depends on it) | completeness of the above |
| `ci/snap-flatpak.config` | this repo | squashfs + zlib/lzo/xz/zstd + xattr (snap images are loop-mounted squashfs; `BLK_DEV_LOOP` is already in the defconfig); `FUSE_FS` (flatpak's document/revoke portals — and the same driver Android's sdcard daemon needs) | snap, flatpak; Android guests |
| `ci/android-guest.config` | this repo | binder (`ANDROID_BINDER_IPC/FS/DEVICES`), the `FTRACE` + `SCHED_TRACER` pair that brings `EVENT_TRACING` on (this tree's `ANDROID_BINDER_IPC` carries `depends on EVENT_TRACING`), `PSI` (lmkd), netd's remaining netfilter faces | redroid-class Android guests |

## Deliberately not here

- `CPUSETS` and SMP-dependent bits: this baseline is UP-only
  (`docs/EXCLUDED-SMP.md`); `docker info` reports its usual cpuset warning.
- AppArmor: not wired into guest boot; snapd runs without confinement
  profiles.  Adding an LSM means choosing a story for every guest — out of
  scope until something asks.
- Display / GPU fragments (fb-vfb, virtio-gpu): they live with the display
  workstream, not the container set.
- Loadable modules: this kernel is built without module support; everything
  is `=y` and guest images carry no modutils.

## Verifying inside a guest

What kernel the guest really runs (IKCONFIG_PROC is part of the set):

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
  locally; it is also part of the CI assert step, so a dropped request is a
  red run naming the symbol, not a surprise on a device.
- When the port's `docker.config` grows a companion for `NET_CLS_CGROUP` /
  `NFT_FIB_IPV6` itself, delete the line from `ci/docker-completions.config`
  — the audit stays green either way.
- The 4 KB variant (`pages4k`) merges on top of the same set; nothing here
  is page-size dependent.