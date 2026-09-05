# Notes for agents working in this repository

This is a fork of `sipeed/LicheeRV-Nano-Build`, the buildroot/kernel/u-boot SDK
that produces the NanoKVM firmware image. Our work lives on the
**`nanokvm-custom`** branch. The companion application repository is
`RobbyV2/NanoKVM`, which has its own `CLAUDE.md`.

Remotes:

| Remote     | URL                                            |
| ---------- | ---------------------------------------------- |
| `origin`   | `https://github.com/RobbyV2/LicheeRV-Nano-Build.git` |
| `upstream` | `https://github.com/sipeed/LicheeRV-Nano-Build.git`  |
| `ota`      | (OTA publishing remote)                              |

## Which upstream branch to merge — read this before merging anything

**Merge `upstream/NanoKVM`. Do not merge `upstream/main`.**

Upstream maintains two lines that diverged at `74169741e` (2025-01-14):

- **`upstream/NanoKVM`** — the NanoKVM-specific line. This is our upstream.
- **`upstream/main`** — the general LicheeRV-Nano / MaixCAM SDK line. It carries
  display panels, touchscreens, backlight PWM, PyQT5, camera ISP tuning and
  similar hardware NanoKVM does not have.

In the 18 months after the fork point, sipeed **never merged `main` into
`NanoKVM`**. They cherry-pick from it selectively instead: of the 18 commits on
`main` since the fork point, exactly two were taken across — `27b96adb5` (aic8800
driver SDK bump) and `f3639b07d` (aic8800 SDIO clock 25MHz), both WiFi driver
fixes. The other sixteen were deliberately left out.

So the correct routine is:

```sh
git fetch upstream
git merge upstream/NanoKVM          # the actual upstream for this branch
```

and, only when something on `main` is genuinely needed, a targeted
`git cherry-pick` of that one commit with a note in the commit message saying why
it applies to NanoKVM.

Verify before merging — if this prints nothing, there is no upstream work pending
and you should not go looking for some on `main` instead:

```sh
git log --oneline nanokvm-custom..upstream/NanoKVM
```

## The 2026-09-05 `upstream/main` merge (`6c9d872ff`)

This rule was written *because* of a merge that broke it. On 2026-09-05,
`upstream/main` was merged into `nanokvm-custom` as `6c9d872ff`, pulling in the
sixteen commits sipeed had deliberately excluded. **The decision was to keep it
for now**, on the grounds that it appears to work, but it is not something to
repeat, and it is the first thing to suspect if the image starts misbehaving at
boot.

At the time of writing this merge has **not been build-tested or run on
hardware.** Nothing below was verified on a device.

To undo the whole thing:

```sh
git revert -m 1 6c9d872ff    # first parent b5437415a is our pre-merge tip
```

### What the merge actually changed (20 files)

Most of `main`'s divergence was already present via earlier cherry-picks, so the
real footprint is small. Ordered by how much it could bite:

- **`ramdisk/initramfs/musl_riscv64/init`** — the largest behavioural change.
  Recovery/mass-storage entry no longer detects the physical **User Key over
  GPIO**; it now reads a `boot_key=` argument from `/proc/cmdline`. It also now
  enters recovery when mounting `/boot` *fails*, moves the `configfs` mount into
  `msc()`, stops mounting `debugfs`, and adds MaixCAM backlight/panel PWM setup —
  including an unconditional `devmem 0x030010AC 32 0x04` pinmux write — inside
  `msc()`. If physical-button recovery matters on NanoKVM, check this first.
- **`buildroot/.../overlay/etc/init.d/S07fs2` and `S07kmod2`** — two upstream boot
  scripts added to an overlay our fork deliberately keeps empty of them (see the
  boot script policy below). They duplicate what NanoKVM's own `S00kmod` and
  `S01fs` already do, and run afterwards, so the second `insmod` and `mount` are
  expected to fail harmlessly. Redundant rather than dangerous, but unintended.
- **`u-boot-2021.10/include/configs/mars-asic.h`** — boot logo now loads from the
  `${logo}` u-boot env var instead of a hardcoded `logo.jpeg`, and `START_VO` is
  reordered ahead of `LOAD_LOGO`. Cosmetic, and only if logo display is enabled.
- **`linux_5.10/kernel/dma/pool.c`** — minimum DMA coherent pool raised from
  `SZ_128K` to `SZ_1M` via a new overridable `DEFAULT_DMA_COHERENT_POOL_SIZE`.
  Costs a little RAM; probably beneficial.
- **`u-boot-2021.10/cmd/i2c.c`** — CVE fix for a stack buffer overflow in `i2c md`
  (`198086d16`). Real, but only reachable from the u-boot console. This is the one
  change here worth keeping if the rest is ever reverted.
- **Inert additions** — `python-pyqt5-sip` buildroot package, `adbd_monitor.sh`,
  and the `cvi_sdr_bin.gc02m1` / `cvi_sdr_bin_90fps.os04a10` camera ISP blobs.
  None are enabled by our defconfig; they are dead weight, not active changes.
- **`S00pmu`**, **`u-boot-2021.10/cmd/cvi_vo.c`** — MaixCAM-Pro PMU and video
  output changes that came along with the above.

## Conflict resolutions to preserve on future merges

These came up in `6c9d872ff` and will come up again. They are deliberate.

- **Boot script policy.** `2ad0073a4` removed the additional boot scripts from
  `buildroot/board/cvitek/SG200X/overlay/etc/init.d/`, because *NanoKVM ships its
  own* and copies them onto the device after compilation — they live in the
  NanoKVM repo at `kvmapp/system/init.d/`, which already contains an `S00kmod`
  that loads `soph_wdt.ko`, `soph_clock_cooling.ko`, `soph_rtc.ko` and
  `soph_mon.ko`, and an `S01fs` that mounts `configfs` and `debugfs`. When
  upstream modifies `S00kmod`, `S01fs`, `S04backlight`, `S05tp` or `S08usbdev`,
  **keep them deleted**. Upstream's changes there are panel and touchscreen
  concerns for hardware NanoKVM does not have.

  Note that `6c9d872ff`'s own commit message is **wrong** on this point: it claims
  `S07kmod2` and `S07fs2` were already ours and merely carried the lines upstream
  had moved out of `S00kmod`/`S01fs`. They were not — both files came in with that
  merge, from `upstream/main`. Our pre-merge overlay held only `S00pmu`,
  `S02config`, `S04fb`, `S10uuid`, `S25wifimod` and `S30rndis`.
- **`buildroot/configs/cvitek_SG200X_musl_riscv64_defconfig` — keep ours.**
  Ours is a ~290-line minimal `savedefconfig`, deliberately trimmed (see
  `eccf81f11`, `49d168aed`). Upstream `main` replaced theirs with a ~4700-line
  expanded `.config` and added PyQT5, numpy, pillow, vsftpd, lftp and
  android-tools adbd. None of that belongs on a NanoKVM. Never resolve this file
  toward `main`.
- **`build/boards/.../partition/partition_sd.xml` — keep `1581056` KB.** That
  ROOTFS size comes from upstream's *own* NanoKVM branch (`0cff6d9d4`).
  `upstream/main`'s larger `1638400` exists only to fit the PyQT5/numpy packages
  we do not build. Our `BR2_TARGET_ROOTFS_EXT2_SIZE="1536M"` (= 1572864 KB) fits
  inside it; keep the pair consistent if either ever changes.
- **`sg2002_licheervnano_sd_defconfig` — keep `CONFIG_ZSMALLOC` and
  `CONFIG_ZRAM`.** The NanoKVM repo has zram tests (`make kernelint-zram`) that
  depend on them.
- **`.gitignore` — do not take upstream's `kvm/` ignore rule.** Upstream treats
  `kvm/` as a build output directory; our fork *tracks* it (NanoKVM data, frp,
  tailscale, init scripts).
- **aic8800 driver files.** As of `6c9d872ff` the entire
  `osdrv/extdrv/wireless/aic8800/` tree is byte-identical to upstream again. Three
  files (`aic8800d80x2_compat.c`, `aic8800d80x2_compat.h`, `aic_priv_cmd.h`) had
  drifted to LF while upstream ships CRLF; they were reset to upstream's bytes.
  Keep it that way so future SDK bumps merge cleanly.

## Working on macOS: 38 permanently "modified" files

On a case-insensitive filesystem (APFS), ~38 tracked files appear modified and
cannot be cleaned. They are pairs whose names differ only in case — for example
`linux_5.10/include/uapi/linux/netfilter/xt_MARK.h` and `.../xt_mark.h`, which
collide on checkout so one overwrites the other. Mostly netfilter uapi headers
plus their copies under `ramdisk/initramfs/glibc_riscv64/usr/include/`, and one
`Makefile`/`makefile` pair under `freertos/Demo/CORTEX_A53_64-bit_UltraScale_MPSoC/`.

**These are a filesystem artifact, not real edits. Never commit them, and never
"fix" them.** `git checkout` on those paths just reproduces the collision. Before
merging, confirm the incoming commits do not touch them:

```sh
git status --porcelain | awk '{print $2}' | sort > /tmp/dirty.txt
git diff --name-only nanokvm-custom...upstream/NanoKVM | sort > /tmp/incoming.txt
comm -12 /tmp/dirty.txt /tmp/incoming.txt   # empty means the merge is safe
```

If that overlap is ever non-empty, the merge will refuse to run over the local
changes and you will need to deal with those specific paths by hand.
