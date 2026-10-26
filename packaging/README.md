# Packaging

The install rules are the single source of truth. Everything here stages
`cmake --install` output rather than assembling its own file list, so a file
added to the install cannot go missing from one channel and not another.

## Layouts

`NCP_PORTABLE_LAYOUT=ON` puts the binary and its data files in one directory.
That is the Windows default, and what the release zips have always contained.
Off, the GNU layout applies: `bin/ncpatcher`, `share/ncpatcher/ncp.h`, a man
page and a bash completion. The binary finds its data files relative to itself
either way, so neither layout compiles a path in and both can be relocated.

## Archives, DEB and RPM

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd build && cpack -G "DEB;RPM;TGZ"
```

## Windows installer

An NSIS setup exe, built by the `installer` target from a portable install it
stages in the build dir, so the installer and the zip ship the same files:

```
cmake -B build -A x64
cmake --build build --config Release --target installer
# -> build/ncpatcher-<version>-setup.exe
```

Install NSIS from <https://nsis.sourceforge.io/> (or `choco install nsis`);
`installer/build_installer.cmake` looks in its default install directory and
then on `PATH`. Don't use MSYS2's NSIS package: its plugins are installed
under `share/nsis/Plugins/unicode`, which makensis 3.13 doesn't search.

What it does:

- installs into `Program Files\NCPatcher`, adds that directory to the machine
  `PATH` and sets `NCPATCHER_ROOT` to it. Both are broadcast to the shell, so a
  newly opened terminal sees them without signing out;
- optionally downloads the Arm GNU Toolchain (`arm-none-eabi-gcc`, 276 MB),
  checks it against the SHA-256 pinned in `installer/ncpatcher.nsi`, unpacks it
  into `NCPatcher\arm-none-eabi` and adds its `bin` to `PATH`. The component is
  unticked when an `arm-none-eabi-gcc` is already on `PATH`, and an upgrade that
  finds the same toolchain version installed does not download it again. The
  download uses the `curl.exe` and `tar.exe` that ship with Windows 10 1803 and
  later, so no NSIS plugins are needed;
- the uninstaller reverses all of that and removes exactly the staged files
  (the list is generated from the stage at build time), leaving anything else
  in the directory alone.

`PATH` is edited by `installer/path.ps1` rather than by NSIS itself, because a
stock NSIS build holds strings of at most 1024 characters and would truncate a
longer `PATH` when writing it back.

To move to a newer toolchain, update `TOOLCHAIN_VERSION`, `TOOLCHAIN_URL` and
`TOOLCHAIN_SHA256` at the top of `ncpatcher.nsi` (Arm publishes the hash beside
each zip as `<zip>.sha256asc`), and the sizes in the messages if they changed.

## Scoop

`scoop/ncpatcher.json` is a manifest for a Scoop bucket. It carries `checkver`
and `autoupdate`, so a bucket carrying it picks up new releases and computes
the hashes itself; the hashes in the committed copy are placeholders and are
replaced the first time `scoop update` runs against a real release.

## winget

winget manifests live in `microsoft/winget-pkgs`, not here, and each one pins
the SHA256 of a published installer, so there is nothing to commit in this
repository that would stay true. At release time:

```
wingetcreate update TheGameratorT.NCPatcher --version <x.y.z> \
    --urls https://github.com/TheGameratorT/NCPatcher/releases/download/v<x.y.z>/ncpatcher-<x.y.z>-setup.exe \
    --submit
```
