# Rebuilding the compiler

The release firmware is built with Espressif's prebuilt toolchain (`xtensa-esp-elf`
`esp-14.2.0_20260121`, in the ESP-IDF v5.5.4 container). These scripts build that toolchain
again from Espressif's published sources, then the firmware with it, and compare: if our
compiler gives the same firmware as Espressif's, their compiler adds nothing its source does not
explain (*diverse double-compiling*).

```bash
toolchain/rebuild.sh ubuntu       # on Ubuntu 22.04 (host GCC 11), about an hour
toolchain/rebuild.sh guix         # every host tool from Guix 1.5.0 (host GCC 14), the same
toolchain/rebuild.sh firmware ubuntu dev-1234abcd path/to/SHA256SUMS   # compare with a reference
```

- crosstool-NG at the tag `esp-14.2.0_20260121` with its submodules; GCC, binutils, newlib and
  picolibc at their tagged commits (Espressif's forks on GitHub). The steps are those of
  Espressif's CI (`.gitlab-ci.yml` in their crosstool-NG), without their internal mirrors.
- The build runs in **`/builds/idf/crosstool-NG`**, Espressif's path: newlib's `assert()`
  messages keep source paths, and another path gives other strings in the firmware.
- `guix`: the Guix binary release is checked against its maintainer's key (pinned in the script,
  listed in Guix's `guix-install.sh`); Guix's host gcc gets its search paths from small wrappers,
  because crosstool-NG refuses `LIBRARY_PATH` and friends; Espressif's binary wrappers are built
  with Guix's Rust instead of a `curl … | sh` rustup download (`system-cargo.patch`).
- Work files go to `WORK` (default `~/.cache/stackchan-toolchain`): the checkouts, the source
  tarballs (shared by both builds), Guix's store (about 3 GB).

**Result (2026-10-04),** for the release of commit `dc0a017`: the firmware built with either of
our compilers is byte-identical to GitHub Actions' build with Espressif's compiler, all seven
files. Our two compilers also give identical target libraries (195 files). Espressif's own
libraries differ from ours in 21 complex-maths functions of `libm.a` (register choices; not linked
into the firmware); their compiler was built with host GCC 6.3.0, ours with GCC 11 and 14.
Details: [device setup design](https://github.com/mj41/home-w42-eu/blob/main/docs/device-setup.md), §6.4.
