# NCPatcher
A universal Nintendo DS code maker/patcher.

NCPatcher is a program that modifies the executable binaries of a Nintendo DS ROM. \
It was created because of the need to have more flexible patching features that other patchers did not have.

## Credits
This program was made with the help of the [Mamma Mia Team](https://github.com/MammaMiaTeam) members. \
NCPatcher was heavily inspired by Fireflower.

## Installing

Grab a release, or build from source. Either way the binary can live anywhere:
it locates `ncp.h` and its companions relative to itself, so there is no longer
anything to add to `PATH` beyond the binary, and nothing to reboot for.

**Windows** — run the installer and tick *Add NCPatcher to the system PATH*, or
unzip the portable archive anywhere and put that directory on `PATH` yourself.
With [Scoop](https://scoop.sh): `scoop install ncpatcher`.

**Linux and macOS** — unpack the release archive, or install from source:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build              # /usr/local/bin + /usr/local/share/ncpatcher
```

`--prefix` puts it somewhere else; the lookup is relative, so a prefix under
`$HOME` works without any further configuration. Packagers will want
`-DNCP_USE_SYSTEM_DEPS=ON`, which turns a missing yaml-cpp into a configure
error rather than a download. See [packaging/README.md](packaging/README.md).

### Where its data files go

`ncp.h`, `ncp_ide.h` and `ncprt.c` are compiled into your ARM code, not into
ncpatcher, so they are installed as program data rather than as host headers —
`/usr/share/ncpatcher`, or beside the binary on Windows. They are looked for in:

1. `$NCPATCHER_DATA_DIR`, if set
2. `<directory of the binary>/../share/ncpatcher`
3. the directory of the binary, and its `include` subdirectory

The version of `ncp.h` found is checked against the one the binary expects. A
mismatch is an error rather than a warning: a stale header compiles perfectly
well, emits sections nothing is looking for any more, and hands you a ROM with
the patches silently missing.

## Building

CMake 3.20 and a C++20 compiler. yaml-cpp is used if the system has it and
fetched if not.

```sh
git clone https://github.com/TheGameratorT/NCPatcher.git
cd NCPatcher
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

On Windows, name the generator instead of the build type:
`cmake -B build -G "Visual Studio 17 2022" -A x64`, then
`cmake --build build --config Release`. The binary lands in `build`, with the
runtime files copied beside it so it can be run from there without installing.

Useful switches:

| Switch | Meaning |
|---|---|
| `-DNCP_BUILD_TESTS=ON` | Build the unit tests; run them with `ctest --test-dir build` |
| `-DNCP_PORTABLE_LAYOUT=ON` | Install everything into one flat directory (the default on Windows) |
| `-DNCP_USE_SYSTEM_DEPS=ON` | Require yaml-cpp from the system rather than fetching it |

## Running

Configure the project as described below, then run `ncpatcher` in the directory
holding its configuration file — or from anywhere with `ncpatcher -C <that
directory>`.

NCPatcher patches a `.nds` directly:

```yaml
rom:
  file: roms/nsmb.nds     # patched in place
  output: build/out.nds   # ...unless you say where to put the result
  backup: backup
```

It only ever touches the header, the two ARM binaries, the two overlay tables
and the overlay files, and it edits the ROM rather than rebuilding it — so
levels, textures, sounds, the banner, the secure area and anything else in
there come out exactly as they went in.

An extracted directory works too, and is what a level editor hands it:

```yaml
rom:
  dir: __tmp
  backup: backup
```

`ncpatcher rom extract <dir>` writes that directory out of a `.nds`, and
`ncpatcher rom pack <dir>` folds it back in — which is the job most projects
currently do with a script of their own. Neither one takes the NitroFS apart;
`ndstool`, or `nds-extract` from
[Fireflower](https://github.com/MammaMiaTeam/Fireflower/releases/latest), is
the tool for that.

Patching a directory does not rewrite its `header.bin`: the header is an input
the patcher has never owned, and every tool that repacks one of these
directories works the sizes out for itself. `ncpatcher rom pack` does too — it
takes the header from the ROM it is packing into and updates it there.

If your extraction names the files differently, say so rather than renaming
them:

```yaml
rom:
  dir: dump
  layout: ndstool            # or a mapping of names
  # layout: { preset: ndstool, arm7-ovt: y7.bin }
```

The presets are `ncpatcher` (the default, and what a level editor writes) and
`ndstool`. The names a mapping may set are `header`, `arm9`, `arm7`,
`arm9-ovt`, `arm7-ovt`, `overlay9-dir`, `overlay7-dir`, `overlay9-name`,
`overlay7-name`, `fnt`, `fat`, `banner` and `data-dir`; the two `*-name` keys
take `{id}` for the overlay number, or `{id:4}` to zero-pad it.

### Growing ARM9

An `append` region adds code to the end of ARM9, and ARM9 cannot be moved: the
secure area is encrypted against its being at the offset the header names. So
the first build that outgrows the gap before the overlay table lays the ROM out
again, leaving `rom.arm9-slack` bytes (64 KiB by default) of room behind it.
Because every build re-applies its patches to the *pristine* ARM9 rather than
to the last build's output, the patched size is a function of the code and not
of how many times you have built — so that happens once, and later builds write
in place and produce a byte-identical ROM.

## Command line

Running `ncpatcher` with no subcommand builds the project in the current
directory, which is how it has always been invoked and still is.

```
ncpatcher [build]                    compile and patch
          clean [--backups]          delete the build directories
          restore                    put the ROM binaries back and drop the backups
          config dump [--explain] [--json]
          config validate
          config path
          migrate [--write]          convert ncpatcher.json to ncpatcher.yaml
          modules list               show which modules are enabled
          modules dump [-o PATH]     print the resolved module graph as JSON
          modules explain NAME       say why a module or component is where it is
          rom info                   print what the ROM header says
          rom extract DIR            write the ROM's code binaries into DIR
          rom pack DIR               fold a directory of code binaries into the ROM
          version
```

Options, which may be written before or after the subcommand:

| Option | Meaning |
|---|---|
| `-C, --project PATH` | Project directory, or the configuration file itself |
| `--rom PATH` | ROM to patch: a `.nds` or an extracted directory |
| `--out PATH` | Write the patched ROM here instead of patching in place |
| `-D, --define NAME[=VAL]` | Define a preprocessor macro |
| `--var NAME=VAL` | Override a `vars:` entry |
| `--toolchain PREFIX` | Cross-compiler prefix |
| `-j, --jobs N` | Compile jobs; 0 means one per hardware thread |
| `-v, --verbose` / `--verbose-tag TAG` | Verbose output, all of it or one category |
| `--color auto\|always\|never` | Console styling. `NO_COLOR` is honoured |
| `--log PATH` / `--no-log` | Where the log file goes, or that there is none |
| `--message-format human\|json` | See below |
| `--result PATH` | Write a JSON summary of the run |

`-C` is what removes the "must be launched from the project directory"
constraint, so a ROM editor or a build script no longer has to `cd` first.

Three settings also read the environment, which the command line still beats:
`NCPATCHER_TOOLCHAIN`, `NCPATCHER_JOBS` and `NCPATCHER_LOG`. The precedence is
command line, then environment, then the project file, then the built-in
default, and `ncpatcher config dump --explain` says which of them won for each
setting. `NCPATCHER_DATA_DIR` is separate from that chain: it says where
`ncp.h` lives, which is a property of the installation rather than of the
project.

The log goes to `<buildDir>/ncpatcher.log` — the ARM9 target's build directory,
or the only enabled one's — so `clean` takes it away with everything else it
made. Only a build writes one; `config dump` and friends have no build
directory to write into and are not worth creating one for. `--log` puts it
somewhere else, `--no-log` turns it off.

### Machine-readable output

`--message-format json` writes one JSON object per line on stdout and moves the
human log to stderr, so a caller never has to match on English:

```json
{"type":"progress","phase":"compile","current":43,"total":210,"item":"source/Coop.cpp"}
{"type":"artifact","kind":"overlay","proc":"arm9","id":58,"action":"modified","size":4788,"ram-address":"0x021726C0","file-id":117,"name":"overlay9/overlay9_58.bin"}
{"type":"diagnostic","level":"error","code":"NCP0001","message":"...","location":{"file":"ncpatcher.yaml","line":88,"col":9,"path":"targets.arm9.regions[12].maxsize"}}
{"type":"result","status":"error","exit-code":3,"duration-ms":8123,"errors":1,"warnings":3}
```

`--result PATH` writes the same run as a single JSON object, including every
diagnostic and artifact. Both work with or without `--message-format json`.

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | Internal error |
| 2 | The command line did not parse |
| 3 | Configuration |
| 4 | Module resolution |
| 5 | Toolchain or runtime header not found |
| 6 | Compilation |
| 7 | Linking |
| 8 | Patching |
| 9 | ROM file I/O |
| 10 | A pre-build or post-build command failed |


## Configuration

For the program to run at least one configuration file must exist with at least one target specified.
This configuration file must be named "ncpatcher.json" and looks somewhat like this:
```json
{
  "$arm_flags": "-masm-syntax-unified -mno-unaligned-access -mfloat-abi=soft -mabi=aapcs",
  "$c_flags": "-Os -fomit-frame-pointer -ffast-math -fno-builtin -nostdlib -nodefaultlibs -nostartfiles -DSDK_GCC -DSDK_FINALROM",
  "$cpp_flags": "-fno-rtti -fno-exceptions -std=c++20",
  "$asm_flags": "-Os -x assembler-with-cpp -fomit-frame-pointer",
  "$ld_flags": "-lgcc -lc -lstdc++ --use-blx",
  
  "backup": "backup",
  "filesystem": "fs-data",
  "toolchain": "arm-none-eabi-",
  
  "arm7": {},
  "arm9": {
  	"target": "arm9.json",
  	"build": "build"
  },
  
  "pre-build": [],
  "post-build": [],
  
  "thread-count": 0
}
```

Structure:
 - backup - The folder to where files needed to re-patch are stored.
 - filesystem - The folder that contains the ROM data to patch.
 - toolchain - The location/prefix of your GCC toolchain executable.
 - arm7 - The ARM7 target.
   - target - The location of the target configuration.
   - build - The folder to where files generated from the build are stored.
 - arm9 - The ARM9 target.
   - target - The location of the target configuration.
   - build - The folder to where files generated from the build are stored.
 - pre-build - An array of commands to run before building.
 - post-build - An array of commands to run after building.
 - thread-count - The amount of jobs to use simultaneously while building. (Use 0 for maximum)

The target configuration file, which is specified in the ncpatcher.json looks somewhat like this:
```json
{
  "$arm_flags": "-march=armv5te -mtune=arm946e-s $${arm_flags}",
  "$c_flags": "${arm_flags} $${c_flags} -DSDK_ARM9 -Darm9_start=0x021901E0",
  "$cpp_flags": "${c_flags} $${cpp_flags}",
  "$asm_flags": "${arm_flags} $${asm_flags}",
  "$ld_flags": "$${ld_flags}",
  
  "c_flags": "${c_flags}",
  "cpp_flags": "${cpp_flags}",
  "asm_flags": "${asm_flags}",
  "ld_flags": "${ld_flags}",

  "includes": [
    "include",
    "source"
  ],
  "regions": [{
    "dest": "main",
    "compress": false,
    "sources": [
      "source/*"
    ]
  }, {
    "dest": "ov9",
    "mode": "append",
    "compress": false,
    "sources": [
      "source/ov9/**"
    ],
    "c_flags": "${c_flags} -DOVERLAY_ID=9",
    "cpp_flags": "${cpp_flags} -DOVERLAY_ID=9",
    "asm_flags": "${asm_flags} -DOVERLAY_ID=9"
  }],
  
  "arenaLo": "0x02065F10",
  "symbols": "symbols9.x"
}
```

Structure:
 - c_flags - The flags used when building C source files. (Can be overwritten per region)
 - cpp_flags - The flags used when building C++ source files. (Can be overwritten per region)
 - asm_flags - The flags used when building Assembly files. (Can be overwritten per region)
 - ld_flags - The flags used when linking.
 - includes - Array of paths or glob patterns that resolve to directories containing headers/includes.
 - regions - An array of sections to build separately.
   - dest - "main" if the code should go in the main binary, "ovX" if the code should go in overlay X.
   - mode - The mode that specifies how code should be inserted.
     - "append" adds code to the end of an existing overlay (Only option for "main").
     - "replace" deletes all the contents of an existing overlay and places your code instead.
     - "create" creates a new overlay with your code.
   - address - The address in memory for this overlay. (Optional, except for "create" mode. In "replace" mode it can be used to set a new address for the overlay)
   - length - The max length that this overlay can have. (Optional)
   - compress - If the overlay should be Backwards LZ compressed. Overlays only; a main region asking for it is warned about and written uncompressed. An overlay whose data does not get smaller is stored as it is, rather than "compressed" into something bigger.
   - sources - Array of paths or glob patterns that resolve to source files.
   - c_flags, cpp_flags, asm_flags - Region overwriteable flags. (Optional)
 - arenaLo - The address of the value holding the address end of the main binary code in memory. (Usually the value being loaded in the first LDR of OS_GetInitArenaLo)
 - symbols - A file containing symbol definitions to include when linking. (Optional)

The "$" symbol allows to define or access a variable that is for its own file scope. \
The "$$" symbol allows a target to access a variable that is defined in the ncpatcher.json file scope. \
The "${env:ENV_VARIABLE}" syntax allows to access a variable that is defined in the system's environment variable list.

## Modules

A module is a self-contained feature — its own sources, include directories,
defines and filesystem entries — that a project switches on by name. It exists
so a project stops having to say *where* every file goes: the module says that
once, and the project says only whether it wants it.

Modules live in one directory, one subdirectory each, and each carries a
`module.yaml`:

```yaml
id: Coop                 # becomes MODULE_COOP, and names it in `requires`
name: NSMB Co-op
description: Adds co-op multiplayer support to the game.
authors: [TheGameratorT, Shadey21]
repo: https://github.com/ShaneDoyle/nsmb-coop/

targets:                 # arm7, arm9, arm7(N), arm9(N)
  - arm9:
      includes: include
      sources: "source9/**"

components:
  - SpikeBassFix:
      target: arm9(58)
      sources: "source9/fixes/SpikeBass.cpp"
      defines:
        - COOP_FIX_SPIKE_BASS_ADD_ZONE_ID_FIELD=1
        - COOP_FIX_SPIKE_BASS_ZONE_ID_FIELD_OFFSET=0x4B8

  - SpikeBassSpawnerFix:
      target: arm9(58)
      requires: SpikeBassFix
      sources: "source9/fixes/SpikeBassSpawner.cpp"

  - PauseMenuFix:        # no target: a switch, and nothing else
      target: arm9
      defines: COOP_FIX_PAUSE_MENU
```

`targets:` is the catch-all — a directory swept into one region. `components:`
carve exceptions out of it: a component's sources go where *it* says, and are
removed from whatever the catch-all would have done with them. Switch a
component off and its sources leave the build entirely.

The project side:

```yaml
modules:
  dir: modules                        # default
  dump: build/generated/modules.json  # optional; see below
  auto-create-regions: false
  enabled:
    - coop
    - mini-hacks
    - dsimodewarn:
        components:
          DSiModeScene: { target: arm9(9) }   # move it into an overlay
    - debug: false                             # listed, switched off
    - nitrosdk: { optional: true }             # may simply not be installed
```

A component override takes `enabled`, `target`, and `defines` — the last being
values for defines the component already declares, not a place to invent new
ones. A target the module wrote with a leading `!` is locked, and an override of
it is reported rather than quietly dropped.

`MODULE_<ID>` is defined for every processor a module reaches, so code can ask
whether a module is present without the project having to say so twice.

### Regions

A component targeting `arm9(58)` needs the ARM9 target to have an `ov58` region.
It is an error if it does not — a mistyped overlay id would otherwise become an
overlay full of code the game never loads. `modules.auto-create-regions: true`
lifts that, creating an appending region with the target's own flags; declare
the region yourself when you want a `maxsize` enforced, because an auto-created
one gets the 1 MiB default.

For a project that uses modules, a declared region that nothing ended up in is
dropped rather than written as an empty overlay — but only when it does nothing
but append. A `replace` region reserves space and an `overwrites` region blanks
code, and both mean something with no sources at all.

### The dump

`ncpatcher modules dump` prints the resolved graph as JSON, and
`modules.dump:` writes the same file before the pre-build commands run. That is
the boundary: everything game-specific — object id allocation, profile tables,
filesystem maps — belongs to a tool that reads this file, not to NCPatcher.
Component keys NCPatcher does not know are kept verbatim under `extra` and
re-emitted for exactly that reason.

```
ncpatcher modules list                     # what is on, and what each contributes
ncpatcher modules explain Coop.SpikeBassFix
ncpatcher modules explain coop
```

`explain` answers where a component's target came from, why it is disabled, and
which files and defines it accounts for.


## Patches

Some patch types come defined to allow the programmer to define how the code should be modified. \
Such patches are defined by using one of the following patterns on a source file. \
*NOTE: The word "hook" can be used to refer both to any patch that causes a jump to your code and to the hook patch type.*

Patches are divided into 2 types, static and dynamic patches. \
**Static patches**: Patches that allow you to change the instructions at "compile time". These are directly applied to the binary. \
**Dynamic patches**: Patches that can be stored in the binary unapplied and only be applied at runtime when requested by the code.

### Static Patching

**To replace instructions in the binary with jumps, calls or hooks use:**

Note: \
If the code you want to hook from is THUMB then for `ncp_jump` and `ncp_call`,
you can prefix them with a "t" (eg. `ncp_tjump`, `ncp_thook`) or use the address+1. \
This is valid for the `ncp_set` variants as well (eg. `ncp_set_tjump`, `ncp_set_tcall`). \
"hook" patch type only supports hooking from ARM mode.

〇 Function tags (not stackable - only 1 hook per function)

C/C++:

```
ncp_jump(int address, [int overlay])
ncp_call(int address, [int overlay])
ncp_hook(int address, [int overlay])
```

Example:
```
ncp_jump(0x02000000)
void MyFunction1() {
    // A jump to this function will be placed at 0x02000000
}

ncp_call(0x02010000, 0)
void MyFunction2() {
    // A call to this function will be placed at 0x02010000 in overlay 0
}
```

〇 Labels (stackable - multiple hooks per function)

C/C++:

```
ncp_set_jump(int address, [int overlay], void* function)
ncp_set_call(int address, [int overlay], void* function)
ncp_set_hook(int address, [int overlay], void* function)
```

Example:
```
void MyFunction() {}

ncp_set_jump(0x02000000, MyFunction)    // A jump to MyFunction will be placed at 0x02000000
ncp_set_jump(0x02000004, MyFunction)    // A jump to MyFunction will be placed at 0x02000004
ncp_set_call(0x02010000, 0, MyFunction) // A call to MyFunction will be placed at 0x02010000 in overlay 0
ncp_set_call(0x02010004, 0, MyFunction) // A call to MyFunction will be placed at 0x02010004 in overlay 0
```

Assembly:

```
ncp_jump(int address, [int overlay])
ncp_call(int address, [int overlay])
ncp_hook(int address, [int overlay])
```

Example:
```
ncp_jump(0x02000000)    // A jump to MyFunction will be placed at 0x02000000
ncp_jump(0x02000004)    // A jump to MyFunction will be placed at 0x02000004
ncp_jump(0x02010000, 0) // A call to MyFunction will be placed at 0x02010000 in overlay 0
MyFunction:
    BX      LR
```

**To overwrite chunks of the binary with raw data or setting the destination address of a function use:**

C/C++:

```
ncp_over(int address, [int overlay])
ncp_repl(int address, [int overlay], char assembly[])
```

Example:
```
ncp_over(0x02000000)
void MyFunction() {} // This function will be placed at 0x02000000

ncp_over(0x02010000, 4)
int MyArray[] = {}; // This array will be placed at 0x02010000 in overlay 4

ncp_repl(0x02000000, "MOV R0, R0") // This instruction will be placed at 0x02000000

ncp_repl(0x02010000, 0, R"(
    MOV     R0, R0
    BX      LR
    .int    0
    .int    0
)") // This assembly code will be placed at 0x02010000 in overlay 0
```

Assembly:

```
ncp_over(int address, [int overlay])
ncp_endover()
```

Example:
```
ncp_over(0x02000000) // Places the following code at 0x02000000
    MOV     R0, #1
    MOV     R1, R0
    BX      LR
ncp_endover():
```

### Dynamic Patching

C/C++:
```
void ncprt_set(int address, int value);
void ncprt_set_jump(int address, void* function);
void ncprt_set_call(int address, void* function);
void ncprt_repl(int address, char name[]);

ncprt_repl_type(name)
```
	
Example:
```
class MyClass
{
public:
    void MyFunction(); // -Wno-pmf-conversions flag must be set to be used in ncprt_set_jump/call/hook
    static void MyStaticFunction();
};

void MyFunction() {}
void MyClass::MyFunction() {}
void MyClass::MyStaticFunction() {}

ncprt_repl_type(patch0) // Must be a unique name for each patch
void MyPatch()
{
asm(R"(
    MOV     R0, #0
    BX      LR
)");
}

void MyPatcher()
{
    ncprt_set(0x02000000, 0);                              // Sets the value at 0x02000000 to 0
    ncprt_set_jump(0x02000004, MyFunction);                // Sets the value at 0x02000004 to a jump to MyFunction
    ncprt_set_call(0x02000008, &MyClass::MyFunction);      // Sets the value at 0x02000008 to a jump to MyClass::MyFunction
    ncprt_set_call(0x0200000C, MyClass::MyStaticFunction); // Sets the value at 0x0200000C to a jump to MyClass::MyStaticFunction
    ncprt_repl(0x02000010, patch0);                        // Writes the contents of patch0 (MyPatch) to 0x02000010
}
```

### Technical Details

These are the sizes occupied by branch type patches: \
The number of bytes is how many bytes are overwritten at the specified address, the ones after
the "+" sign are bridge bytes. Those are automatically generated instructions from which the
address to patch will branch to.

| Patch Type | ARM->ARM                  | ARM->THUMB                | THUMB->ARM | THUMB->THUMB |
|------------|---------------------------|---------------------------|------------|--------------|
| jump       | 4 bytes                   | 4 bytes + 8 bridge bytes  | 8 bytes    | 8 bytes      |
| call       | 4 bytes                   | 4 bytes                   | 4 bytes    | 4 bytes      |
| hook       | 4 bytes + 20 bridge bytes | 4 bytes + 20 bridge bytes | -          | -            |

**IMPORTANT NOTE** \
This means that all hooks made from ARM mode to any target will always be safe because it only
ever overwrites one instruction, but when hooking from THUMB, 4 or 8 bytes are always
overwritten depending on the hook type used and not just 2 bytes. So be careful because
you might accidentally overwrite more instructions than you intended to!

A jump patch is equivalent to a branch instruction (`B srcAddr`). \
A call patch is equivalent to a linked branch instruction (`BL srcAddr`).

If the patch type is a ARM->THUMB jump, the instruction at
`destAddr` becomes a jump to a ARM->THUMB jump bridge generated
by NCPatcher and it should look as such:

```
arm2thumb_jump_bridge:
    LDR   PC, [PC,#-4]
    .int: srcAddr+1
```

If the patch type is a hook, the instruction at
`destAddr` becomes a jump to a hook bridge generated
by NCPatcher and it should look as such:

```
hook_bridge:
    PUSH {R0-R3,R12}
    BL   srcAddr        @ BLX if srcAddr is THUMB
    POP  {R0-R3,R12}
    @<unpatched destAddr's instruction>
    B    (destAddr + 4)
```
