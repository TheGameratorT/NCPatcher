# Modules

A module is a self-contained feature (its own sources, include directories,
defines and filesystem entries) that a project switches on by name. It exists
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

targets:                 # arm7, arm9, arm7(N), arm9(N), arm9(rt:NAME)
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

`targets:` is the catch-all, a directory swept into one region. `components:`
carve exceptions out of it: a component's sources go where *it* says, and are
removed from whatever the catch-all would have done with them. Switch a
component off and its sources leave the build entirely.

The project side:

```yaml
modules:
  dir: modules                        # default
  dump: build/generated/modules.json  # optional; see below
  enabled:
    - coop
    - mini-hacks
    - dsimodewarn:
        components:
          DSiModeScene: { target: arm9(9) }   # move it into an overlay
    - debug: false                             # listed, switched off
    - nitrosdk: { optional: true }             # may simply not be installed
```

A component override takes `enabled`, `target`, and `defines`, the last being
values for defines the component already declares, not a place to invent new
ones. A target the module wrote with a leading `!` is locked, and an override of
it is reported rather than quietly dropped.

`MODULE_<ID>` is defined for every processor a module reaches, so code can ask
whether a module is present without the project having to say so twice.

## Regions

A component targeting `arm9(58)` needs the ARM9 target to have an `ov58` region.
It is an error if it does not: a mistyped overlay id would otherwise become an
overlay full of code the game never loads, and an invented region has no size
limit worth the name, so the first thing it would do is let that code run past
the end of its overlay into the next one.

Declaring one region per overlay a module might ever reach is not the answer
either. That is what `region-catalog` is for.

## Runtime modules

A target of `arm9(rt:<name>)` puts code in a [runtime module](runtime.md), an
image the game loads and unloads while it runs, instead of in the binary or an
overlay. Unlike an overlay, a runtime module has no size the game gives it, so a
module may create one the project never declared. The module that creates it
says where the image is written:

```yaml
id: Bosses

runtime:
  bosses:
    file: z_new/ncprt/bosses.ncrt    # a NitroFS path, or a list of them

targets:
  - arm9(rt:bosses):
      sources: "rt/**"

components:
  - Petey:
      target: arm9(rt:bosses)
      sources: "petey/**"
```

Other modules can put code in `rt:bosses` too, with the same target; only one
may declare it under `runtime:`. A runtime module whose code all belongs to
components that are switched off is not built, and its file is not added.

A project region with the same `dest`, `rt:bosses`, is the same runtime module:
the modules' code goes into it, and its own `defines` and `flags` apply. If it
gives a `file` or an `output`, the image goes there instead of to the module's
`file`. A runtime module that neither a module nor the project says where to
write is an error.

The file id each image gets is in the [file manifest](file-manifest.md), under
`runtime-module`, so a generator reading the dump finds which runtime module
holds a component's code (its `target.runtime`) and the manifest says which
file that is.

## Region catalogs

Which overlays a game has, and how far each one may grow before it runs into
whatever the game placed after it, is a property of the game, not of your
project. One table serves every project built against that game, so it lives
outside the project and is referenced rather than copied. A copied table goes
stale silently, and a stale ceiling is an overlay that overruns its neighbor.

```yaml
targets:
  arm9:
    region-catalog: ${env.NSMBREF_ROOT}/overlays9.yaml
    regions:
      - dest: main
        overwrites: [[0x02026CE0, 0x02039170]]
      - dest: ov58
        maxsize: 0x4000      # this project knows better than the catalog
```

The catalog itself holds nothing but the overlays:

```yaml
version: 1
regions:
  - dest: ov0
    maxsize: 0x33C00
  - dest: ov1
    maxsize: 0x56400
```

Only `dest`, `address`, `maxsize` and `compress` are accepted there. Sources,
flags and defines are the project's business, so a catalog cannot smuggle them
in.

**A catalog entry is an offer, not a declaration.** An overlay nothing is built
into is dropped rather than written out, which is what makes listing all 131 of
them cost nothing. Name a region in the target and it is yours: whatever you say
wins, and whatever you leave out still comes from the catalog, so naming an
overlay to put sources in it does not mean restating a size limit you have no
opinion about.

An overlay in neither the catalog nor the target is still an error, naming the
overlay and the catalog that failed to list it.

For a project that uses modules, a region the target declared itself and that
nothing ended up in is dropped too, but only when it does nothing but append.
A `replace` region reserves space and an `overwrites` region blanks code, and
both mean something with no sources at all.

## The dump

`ncpatcher modules dump` prints the resolved graph as JSON, and
`modules.dump:` writes the same file before the pre-build commands run. That is
the boundary: everything game-specific (object id allocation, profile tables,
filesystem maps) belongs to a tool that reads this file, not to NCPatcher.
Component keys NCPatcher does not know are kept verbatim under `extra` and
re-emitted for exactly that reason.

```
ncpatcher modules list                     # what is on, and what each contributes
ncpatcher modules explain Coop.SpikeBassFix
ncpatcher modules explain coop
```

`explain` answers where a component's target came from, why it is disabled, and
which files and defines it accounts for.

The schema is `schema/modules.schema.json`.

## Keys NCPatcher does not define

`extra` is the extension point, and it exists at two levels. Component keys have
always been kept; module root keys are kept the same way:

```yaml
# module.yaml
id: Glue

level-data:                  # NCPatcher has no idea what this is
  stageObjects: Glue::StageObject::ModuleStageObject[?]

components:
  - Vanilla:
      objects:               # nor this
        - name: CoopFlagActor
          type: actor
```

Both come back out of the dump under `extra`, with YAML's scalar types resolved
the way YAML would resolve them, so a number stays a number and `yes` stays a
string. Nothing else happens to them. That is the whole of how a game-specific generator
extends a module without NCPatcher acquiring any knowledge of the game:

```jsonc
{ "id": "Glue",
  "extra": { "level-data": { "stageObjects": "Glue::StageObject::ModuleStageObject[?]" } },
  "components": [] }
```

A root key that NCPatcher *does* define is validated as usual, so `extra` is not
an escape from spelling `targets` correctly, only a place for keys that were
never NCPatcher's to check.


