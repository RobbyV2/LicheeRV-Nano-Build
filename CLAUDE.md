# Notes for agents working in this repository

Fork of `sipeed/LicheeRV-Nano-Build`, the buildroot/kernel/u-boot SDK that builds
the NanoKVM firmware image. Our work lives on **`nanokvm-custom`**. The companion
application repository is `RobbyV2/NanoKVM`, which has its own `CLAUDE.md`.

| Remote     | URL                                                  |
| ---------- | ---------------------------------------------------- |
| `origin`   | `https://github.com/RobbyV2/LicheeRV-Nano-Build.git` |
| `upstream` | `https://github.com/sipeed/LicheeRV-Nano-Build.git`  |
| `ota`      | OTA publishing remote                                |

## Merge `upstream/NanoKVM`, never `upstream/main`

Upstream keeps two lines, diverged at `74169741e`:

- **`upstream/NanoKVM`** — the NanoKVM line. This is our upstream.
- **`upstream/main`** — the general LicheeRV-Nano / MaixCAM SDK line. Display
  panels, touchscreens, backlight PWM, PyQT5, camera ISP tuning. NanoKVM has none
  of that hardware.

Sipeed has never merged `main` into `NanoKVM`; they cherry-pick from it, and have
taken two of its commits since the fork point, both aic8800 WiFi fixes. Do the
same. Merging `main` wholesale drags in a MaixCAM-tuned rewrite of
`ramdisk/initramfs/musl_riscv64/init` that moves recovery entry off the physical
User Key GPIO onto a `boot_key=` kernel cmdline argument, among much else.

```sh
git fetch upstream
git log --oneline nanokvm-custom..upstream/NanoKVM   # what is pending
git merge upstream/NanoKVM
```

To take something specific off `main`, cherry-pick that one commit and say in the
message why it applies to NanoKVM.

## Conflict resolutions to keep

These recur on every merge and are all deliberate.

- **Boot scripts.** `buildroot/board/cvitek/SG200X/overlay/etc/init.d/` holds only
  `S00pmu`, `S02config`, `S04fb`, `S10uuid`, `S25wifimod`, `S30rndis`. NanoKVM
  ships its own boot scripts from the NanoKVM repo's `kvmapp/system/init.d/`,
  copied onto the device after compilation — including an `S00kmod` that loads
  `soph_wdt.ko`, `soph_clock_cooling.ko`, `soph_rtc.ko` and `soph_mon.ko`, and an
  `S01fs` that mounts `configfs` and `debugfs`. When upstream modifies or adds
  scripts here (`S00kmod`, `S01fs`, `S04backlight`, `S05tp`, `S08usbdev`,
  `S07kmod2`, `S07fs2`), keep them out.
- **`buildroot/configs/cvitek_SG200X_musl_riscv64_defconfig` — keep ours.** A
  ~290-line minimal `savedefconfig`, deliberately trimmed (`eccf81f11`,
  `49d168aed`). Upstream `main` carries a ~4700-line expanded `.config` with
  PyQT5, numpy, pillow, vsftpd, lftp and android-tools adbd. Never resolve this
  file toward `main`.
- **`build/boards/.../partition/partition_sd.xml` — ROOTFS stays `1581056` KB**
  (from `0cff6d9d4`, upstream's own NanoKVM branch). `main`'s larger `1638400`
  exists only to fit packages we do not build. It pairs with
  `BR2_TARGET_ROOTFS_EXT2_SIZE="1536M"` (1572864 KB) in the buildroot defconfig;
  if either changes, keep the filesystem inside the partition.
- **`sg2002_licheervnano_sd_defconfig` — keep `CONFIG_ZSMALLOC` and
  `CONFIG_ZRAM`.** The NanoKVM repo's `make kernelint-zram` tests depend on them.
- **`.gitignore` — do not take upstream's `kvm/` ignore rule.** Upstream treats
  `kvm/` as build output; we track it (NanoKVM data, frp, tailscale).
- **`kvm/merge_nanokvm_app.sh` — two script loops, both listing ours.** A
  pre-flight `need_file` pass and a later install pass. `S02abtrial` and `S02zram`
  are ours and must appear in both lists; upstream's copy has neither.

## Working on macOS: 38 permanently "modified" files

On a case-insensitive filesystem, ~38 tracked files always show as modified and
cannot be cleaned. They are pairs differing only in case — for example
`linux_5.10/include/uapi/linux/netfilter/xt_MARK.h` and `.../xt_mark.h` — which
collide on checkout, so one overwrites the other. Mostly netfilter uapi headers
and their copies under `ramdisk/initramfs/glibc_riscv64/usr/include/`, plus a
`Makefile`/`makefile` pair under
`freertos/Demo/CORTEX_A53_64-bit_UltraScale_MPSoC/`.

**A filesystem artifact, not real edits. Never commit them, never try to fix
them** — `git checkout` on those paths just reproduces the collision. They also
hide real changes to those files, so before merging confirm the incoming commits
do not touch any of them:

```sh
git status --porcelain | awk '{print $2}' | sort > /tmp/dirty.txt
git diff --name-only nanokvm-custom...upstream/NanoKVM | sort > /tmp/incoming.txt
comm -12 /tmp/dirty.txt /tmp/incoming.txt   # empty means safe to merge
```

If that is ever non-empty, the merge will refuse to run over the local changes and
those paths need handling by hand.
