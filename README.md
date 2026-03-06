# Wraith

[![License: BSD-3-Clause](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](LICENSE.txt)

A minimal [Lilu](https://github.com/acidanthera/Lilu) kernel extension that forces
`kern.hv_vmm_present` to return a fixed value — **0** or **1** — unconditionally.

Named after the Wraith — present but unseen.

---

## What it does

Intercepts the `kern.hv_vmm_present` sysctl handler and returns the value you
specify via boot-arg or NVRAM. Every process that queries the sysctl gets the same
answer — there is no per-process logic.

| `wrtvmm` | `sysctl kern.hv_vmm_present` reports |
|---|---|
| `on` | `1` — system appears to be a VM |
| `off` | `0` — system does not appear to be a VM |
| `none` | unchanged — Wraith passes through the real kernel value |
| *(absent)* | unchanged — Wraith does nothing |

---

## Requirements

| | Minimum |
|---|---|
| macOS | Big Sur 11.3 |
| Lilu | 1.6.0 |
| OpenCore | 0.7.0 |

---

## Installation

1. Place `Wraith.kext` in `OC/Kexts/` alongside `Lilu.kext`.
2. Snapshot your `config.plist` with ProperTree (or add the entry manually).
3. `Lilu.kext` must appear **before** `Wraith.kext` in the kext list.

---

## Configuration

### Boot arguments

| Argument | Effect |
|---|---|
| `wrtvmm=on` | Force `kern.hv_vmm_present` to return `1` |
| `wrtvmm=off` | Force `kern.hv_vmm_present` to return `0` |
| `wrtvmm=none` | Explicitly disable patching (same as not setting it) |
| `-wrtoff` | Disable Wraith entirely |
| `-wrtdbg` | Verbose logging (DEBUG builds only) |
| `-wrtbeta` | Allow loading on macOS beyond the supported range |

### NVRAM variable

The same setting can be stored in NVRAM under the Lilu vendor GUID.
Boot-args take priority over NVRAM.

```
# Force VMM present (wrtvmm=on):
sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:wrtvmm=on

# Force VMM absent (wrtvmm=off):
sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:wrtvmm=off

# Explicitly pass through (wrtvmm=none):
sudo nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:wrtvmm=none

# Remove the variable entirely:
sudo nvram -d 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:wrtvmm
```

> String values are stored as plain ASCII — no binary encoding needed.
> In OpenCore's `config.plist` NVRAM section, set the value type to `String`.

If `wrtvmm` is not set (neither boot-arg nor NVRAM), or is set to `none`, Wraith loads but makes no changes — the kernel returns whatever value it naturally has.

---

## Building from source

```
git clone --recursive https://github.com/<your-fork>/Wraith.git
```

Requires `Lilu.kext` (DEBUG build for development) and `MacKernelSDK` submodules.
Open `Wraith.xcodeproj` in Xcode and build.

The entire implementation lives in a single file: `Wraith/Wraith.cpp`.

---

## Credits

- [vit9696](https://github.com/vit9696) — [Lilu](https://github.com/acidanthera/Lilu)
  and the original sysctl OID walking technique from
  [RestrictEvents](https://github.com/acidanthera/RestrictEvents).
- [acidanthera](https://github.com/acidanthera) — RestrictEvents, MacKernelSDK.
- [Apple](https://www.apple.com) — macOS.

---

## Licence

BSD 3-Clause. See [LICENSE.txt](LICENSE.txt).
