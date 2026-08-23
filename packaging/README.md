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

Built from a staged portable install, so the installer and the zip ship the
same bytes:

```
cmake -B build -G "Visual Studio 17 2022" -A x64 -DNCP_PORTABLE_LAYOUT=ON
cmake --build build --config Release
cmake --install build --config Release --prefix stage
iscc /DStageDir=..\stage /DAppVersion=1.0.4 installer\ncpatcher.iss
```

The add-to-PATH checkbox is the point of it: adding the directory by hand and
rebooting for it to take is the step the project templates used to have to
spell out.

## Scoop

`scoop/ncpatcher.json` is a manifest for a Scoop bucket. It carries `checkver`
and `autoupdate`, so a bucket carrying it picks up new releases and computes
the hashes itself; the hashes in the committed copy are placeholders and are
replaced the first time `scoop update` runs against a real release.

## winget

winget manifests live in `microsoft/winget-pkgs`, not here, and each one pins
the SHA256 of a published installer -- so there is nothing to commit in this
repository that would stay true. At release time:

```
wingetcreate update TheGameratorT.NCPatcher --version <x.y.z> \
    --urls https://github.com/TheGameratorT/NCPatcher/releases/download/v<x.y.z>/ncpatcher-<x.y.z>-setup.exe \
    --submit
```
