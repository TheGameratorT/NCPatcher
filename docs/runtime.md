# The NCP Runtime

Every `ncp_` patch is written into the binary when the ROM is built. The runtime
is for patches that are applied while the game runs, and taken back out again:

- A **patch group** is a set of ordinary patches, linked into the binary like
  any other code, that the game switches on and off. A versus menu that changes
  how the title screen behaves while it is open is one.
- A **runtime module** is code that is not in the binary at all. It is linked
  into an image file of its own, which the game loads when it needs it: the
  code for one course, or the actors one level uses. Its patches apply when it
  loads and revert when it unloads.

Both need `ncprt.c`, which NCPatcher compiles into the target's main region on
its own whenever the code uses the runtime. It does its own cache maintenance
and needs nothing from the game, except, for runtime modules, a way to read a
file.

## Patch groups

Every patch has a grouped form, `ncprt_<kind>(group, address[, overlay])`,
which takes a group name before the address. The name is an identifier, and
needs no declaration.

```cpp
ncprt_call(mvsl, 0x020A2CCC)
void tMvsLMenu::onUpdateTop() { /* ... */ }

ncprt_set_jump(mvsl, 0x020CC1D4, tMvsLMenu::onUpdateBottom)

ncprt_over(mvsl, 0x020A2768) static const u32 noTimer = 0xE1A00000;

ncprt_repl(mvsl, 0x020A2800, "MOV R0, #0")
```

`ncprt_jump`, `call`, `hook`, `tjump`, `tcall`, `thook`, `over`, `repl` and the
`ncprt_set_` forms all work, and do what their `ncp_` form does, with the same
bridges. In assembly, the label forms take the group the same way. Function
tags stack, as `ncp_` tags do.

Nothing is written until the code asks:

```cpp
void onSceneCreate()
{
    if (inVersusMenu)
        ncprt_apply(mvsl);
    else
        ncprt_revert(mvsl);
}
```

| Call | Does |
|---|---|
| `ncprt_apply(g)` | Saves the bytes at every patch address, then writes the patches. Does nothing if the group is applied. |
| `ncprt_revert(g)` | Puts the saved bytes back, last patch first. Does nothing if it is not applied. |
| `ncprt_is_applied(g)` | Whether it is applied. |
| `ncprt_get_group(g)` | The `ncprt_group` itself. |

Both run with interrupts masked and leave the caches in order, so an interrupt
handler never runs half-patched code.

The bytes are saved when the group is applied, not when the ROM is built, so a
group may patch over an `ncp_` patch and revert to it. Two groups may patch the
same address, with a warning: that is only correct if they are never applied at
the same time, or are reverted in the reverse of the order they were applied in.
A group patching an overlay must be applied while that overlay is loaded, and
reverted before it is unloaded.

The group's table and the room for the saved bytes are reserved in the main
binary: 8 bytes per group, then 16 bytes per patch plus the bytes each one
writes.

## Runtime modules

A region whose `dest` is `rt:<name>` is a runtime module:

```yaml
targets:
  arm9:
    regions:
      - dest: main
        sources: ["source/**", "!source/rt/**"]
      - dest: rt:bosses
        sources: source/rt/bosses/**
        file: z_new/ncprt/bosses.ncrt
```

Its code is written with the same `ncp_` patches as any other: a module's
`ncp_jump` is written when it loads and reverted when it unloads. Grouped
patches cannot be used in a module, since the module itself is the group.
A module can call the game, and the project's own code in the main binary;
the main binary cannot call the module, which may not be loaded. See
[Regions](configuration.md#runtime-modules) for the region's keys, and
[Modules](modules.md#runtime-modules) for putting module code in one.

A module's C, C++ and assembly code is compiled with `NCP_RUNTIME_MODULE`
defined to its name, `bosses` here, and the rest of the target's code is not.
A header can use it to tell which module it is part of, and to include what a
generator wrote for that module:

```cpp
#define NCP_STR_(x) #x
#define NCP_STR(x) NCP_STR_(x)
#include NCP_STR(generated/NCP_RUNTIME_MODULE.hpp)
```

### Loading

The game supplies a filesystem provider, which reads a whole module file into
memory, and the runtime does the rest:

```c
void* image = ncprt_load("ncprt/bosses.ncrt");
if (image != NULL)
{
    const struct BossApi* api = ncprt_exports_as(const struct BossApi, image);
    api->spawn(0);
    /* ... */
    ncprt_unload(image);
}
```

| Call | Does |
|---|---|
| `ncprt_set_fs_provider(p)` | Sets the provider the calls below use. |
| `ncprt_load(path)` | Reads the file by NitroFS path, then attaches it. Returns the image, or `NULL`. |
| `ncprt_load_id(id)` | The same, by file id. |
| `ncprt_unload(image)` | Detaches the image, then frees it with the provider. |
| `ncprt_attach(image)` | Prepares an image already in memory; see below. Returns `NCPRT_OK` or an `NCPRT_ERROR_` value. |
| `ncprt_detach(image)` | Undoes `ncprt_attach`. The image can then be freed, or attached again. |
| `ncprt_exports_of(image)` | The module's exports object, or `NULL`. |

Attaching relocates the image to where it is in memory, makes the code visible
to the CPU, runs the module's constructors and applies its patches. Detaching
reverts the patches and runs its destructors. A failed `ncprt_load` leaves
nothing loaded.

### The filesystem provider

```c
typedef struct ncprt_fs_provider {
    void* (*load)(const char* path);
    void* (*load_id)(unsigned int id);
    void (*unload)(void* image);
} ncprt_fs_provider;
```

`load` and `load_id` return the whole file in memory, or `NULL`. The memory
must start on an `NCPRT_MODULE_ALIGN` (32 byte) boundary: an image records the
alignment it needs, which is at most that and usually 4, and refuses to attach
anywhere less aligned. `unload` frees what they returned. A game sets whichever
of the two it can read files with, and `NULL` for the other.

New Super Mario Bros. refers to files by id, so its provider reads by id. Ids
are in the game's own numbering, which in this game is the FAT index minus
`FS::fileIDOffset`:

```cpp
static void* loadModule(unsigned int id)
{
    FSFile file;
    FS_InitFile(&file);
    if (!FS_OpenFileFast(&file, FSFileID{ FS::romArchive, id + FS::fileIDOffset }))
        return nullptr;

    s32 size = s32(file.prop.file.bottom - file.prop.file.top);
    void* image = Memory::allocate(size, NCPRT_MODULE_ALIGN);
    if (image != nullptr)
    {
        DC_FlushRange(image, size);
        if (FS_ReadFile(&file, image, size) != size)
        {
            Memory::deallocate(image);
            image = nullptr;
        }
    }
    FS_CloseFile(&file);
    return image;
}

static void freeModule(void* image) { Memory::deallocate(image); }

static const ncprt_fs_provider provider = { nullptr, loadModule, freeModule };
```

```cpp
ncprt_set_fs_provider(&provider);
void* image = ncprt_load_id("ncprt/bosses.ncrt"fid);
```

The id comes from the build: a module's `file` gets its id when the other
NitroFS files do, before anything is compiled, and the [file
manifest](file-manifest.md) lists it, marked with `runtime-module`. A generator
reading the manifest, such as the one behind NSMB's `""fid`, sees it like any
other file.

A game that already has the file in memory needs no provider. Mario Kart DS
loads each course's archive, and keeps a module in each one; its loader
attaches the module it finds there, and detaches it when the course ends:

```c
void loadCourseModule(void* data)
{
    if (((ncprt_module*)data)->magic == NCPRT_MODULE_MAGIC)
        ncprt_attach(data);
}

void unloadCourseModule(void* data)
{
    ncprt_module* module = (ncprt_module*)data;
    if (module->magic == NCPRT_MODULE_MAGIC && (module->flags & NCPRT_MODULE_ATTACHED))
        ncprt_detach(data);
}
```

Such a module is written with `output:` to a host path, for the game's own tools
to pack into the archive.

### Exports

A module gives its loader one object, of a type both sides share:

```c
/* bosses_api.h, included by the module and by the main binary */
struct BossApi
{
    void (*spawn)(int kind);
    int (*count)(void);
};
```

```c
/* in the module */
ncprt_exports(const struct BossApi) = { spawnBoss, countBosses };
```

`ncprt_exports_as(type, image)` returns it as a `type*`. The object is
relocated with the rest of the image, so its pointers are right wherever the
image is.

### Constructors and destructors

`ncprt_attach` runs the module's constructors in order: C++ static objects, and
functions marked `__attribute__((constructor))`. `ncprt_detach` runs the
destructors of C++ static objects, newest first, then functions marked
`__attribute__((destructor))`, in reverse. These replace RCM's load and unload
callbacks.

Each module keeps its own list of C++ static objects to destroy, sized at build
time for every object that registers one, so a module does not call the main
binary's `atexit`.

### What is not reset

A module's static data is not reset when it is detached. Attaching the same
image again, where it is or after moving it, sees the values it was left with;
loading the file again, with `ncprt_load`, starts over from the file.

### Patching overlays

A module may patch an overlay, with `ncp_jump(address, overlay)` and the rest.
Patches are written by address, so the overlay must be loaded when the module is
attached, and stay loaded until it is detached. This is not checked.

### Order

Two modules that patch the same address must be detached in the reverse of the
order they were attached in, as with groups: each one saves the bytes it found,
and puts those back.

### How the image is built

A module is linked twice, at two addresses far from anything the DS maps, and
the two images are compared: a word that differs holds an address inside the
module, and goes in a relocation table that `ncprt_attach` uses to move the
image. Calls from the module to the game reach it through long branches, since
the game is out of branch range of either address, and so need no relocation.

A hook in a module uses a 32-byte bridge in the module, which runs the
instruction the hook replaced. An instruction that reads the program counter is
rewritten to work from there; one the bridge cannot rewrite is refused when the
module is built.

The image is written to every `file` and `output` of its region, and a copy is
kept as `<build>/rt/<name>/<name>.ncrt`.

## Migrating from RCM

A Runtime Code Module's hook table becomes ordinary patches:

| RCM | NCPatcher |
|---|---|
| `HOOK_ANSUB(addr, f)` | `ncp_jump(addr)` on `f` |
| `HOOK_AREPL(addr, f)` | `ncp_call(addr)` on `f` |
| `HOOK_TNSUB(addr, f)` | `ncp_tjump(addr)` on `f` |
| `HOOK_TREPL(addr, f)` | `ncp_tcall(addr)` on `f` |
| `HOOK_DATA32(addr, w)`, `HOOK_DATA16`, `HOOK_DATA8` | `ncp_over(addr)` on a constant of that size |
| `HOOK_LOAD(f)` | `__attribute__((constructor))` on `f` |
| `HOOK_UNLOAD(f)` | `__attribute__((destructor))` on `f` |

A function that more than one entry pointed at takes one tag per address, or an
`ncp_set_jump(addr, f)` line for each. The separate Makefile and relocator go
away: the module's sources are a region of the project, built with it.

The loader becomes `ncprt_attach` and `ncprt_detach`. A loader that must still
read old modules can tell them apart by the header's `magic`, as above.
