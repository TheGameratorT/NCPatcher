# NCPatcher
A universal Nintendo DS code maker/patcher.

NCPatcher is a program that modifies the executable binaries of a Nintendo DS ROM. \
It was created because of the need to have more flexible patching features that other patchers did not have.

## Quick start

Install a release (on Windows, the installer or `scoop install ncpatcher`), or
build from source as described in [Installing](docs/installing.md). Then create
a project and build it:

```sh
ncpatcher -C my-project init                  # or: init --template nsmb
# put the ROM at my-project/game.nds, and code in my-project/source/
ncpatcher -C my-project build
```

`ncpatcher build` run inside the directory holding `ncpatcher.yaml` does the
same without `-C`.

## Documentation

| Page | Covers |
|---|---|
| [Installing](docs/installing.md) | Releases, building from source, where the SDK files go |
| [Configuration](docs/configuration.md) | `ncpatcher.yaml`, targets and regions, variables, the environment file, build variants |
| [The ROM](docs/rom.md) | `.nds` and extracted directories, `rom extract`/`rom pack`, the banner, DSi-enhanced and trimmed output |
| [Command line](docs/command-line.md) | Subcommands and options, console and JSON output, exit codes, cancelling a build |
| [Build hooks](docs/hooks.md) | Running commands before, during and after a build |
| [NitroFS files](docs/nitrofs.md) | Replacing and adding files, archive members, file trees |
| [The file manifest](docs/file-manifest.md) | The file id table a build writes, `files plan`, id stability |
| [Modules](docs/modules.md) | Self-contained features, components, region catalogs, the module dump |
| [Patches](docs/patches.md) | `ncp_jump`, `ncp_hook`, `ncp_over` and the runtime `ncprt_*` patches |

`man ncpatcher` summarizes the command line, and the JSON schemas for the
configuration and every document NCPatcher writes are in [schema/](schema).

## Credits
This program was made with the help of the [Mamma Mia Team](https://github.com/MammaMiaTeam) members. \
NCPatcher was heavily inspired by Fireflower.
