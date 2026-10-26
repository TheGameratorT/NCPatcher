# Build hooks

Version 2 projects use one named `hooks:` list. `when` chooses the build phase
a hook runs in:

| `when` | Runs | Sees |
|---|---|---|
| `pre-build` | before NitroFS insertion | the module graph |
| `post-files` | after insertion, before compilation | the file ids this build assigned |
| `post-build` | after the patched ROM is committed | the finished ROM |

`post-files` exists because a generator that turns file ids into source code has
nowhere else to run: `pre-build` is too early, since nothing has been inserted
yet, and `post-build` is too late, since the code referring to those ids has
already been compiled.

```yaml
modules:
  dump: build/generated/modules.json

hooks:
  - name: Generate module headers
    run: python3 scripts/module_gen.py --graph "${ncp.moduleDump}"
    cwd: tools
    env:
      GENERATED_DIR: "${project.root}/build/generated"
    when: pre-build

  - name: Generate file id header
    run: python3 scripts/fid.py
    when: post-files

  - name: Summarize build
    run: python3 scripts/report.py
    when: post-build
```

`cwd` is relative to the project directory and defaults to that directory.
`env` overrides variables for the child process only; all other environment
variables are inherited. Hook `name`, `run`, `cwd`, `env` values, and `when`
support the same `${vars.NAME}`, `${env.NAME}`, `${project.root}`, and other
configuration references as the rest of the file. `${ncp.moduleDump}` is the
absolute path configured by `modules.dump`, and `${ncp.fileDump}` the one
configured by `files-dump`; each is available when its setting is present.

Three references are resolved when the hook runs rather than when the file is
read, because they are not known any earlier: `${variant.name}` is the variant
being built, empty for a project with none, `${rom.output}` is the ROM this
build wrote, including the `_<variant>` suffix a variant build derives, and
`${rom.file}` is the `.nds` it patched -- `rom.file`, or what `--rom` named --
empty for an extracted directory. That is what lets one post-build hook produce
a patch per variant, whether or not the project names its ROM:

```yaml
  - name: Patch
    run: xdelta3 -e -f -s "${rom.file}" "${rom.output}" build/xdelta/${variant.name}.xdelta
    when: post-build
```

For the JSON a `post-files` hook usually reads, see
[The file manifest](file-manifest.md).

The old `pre-build` and `post-build` string arrays remain accepted in both
configuration versions. Do not combine those compatibility keys with `hooks:`
in one version 2 file.

## The environment a hook inherits

Every hook inherits an environment that tells
it what the console it is writing into looks like, so it can print lines that
read as NCPatcher's own instead of falling back to its own tags:

| Variable | Meaning |
| --- | --- |
| `NCPATCHER` | The running NCPatcher's version. |
| `NCPATCHER_LOG_STYLE` | `ncp`: drop bracketed severity tags; use `warning: ` / `error: ` prefixes. |
| `NCPATCHER_LOG_COLUMN` | Width of the right-aligned verb column a milestone line uses. |
| `NCPATCHER_LOG_INDENT` | Column a detail line under a milestone should indent to. |
| `NCPATCHER_COLOR` | `always` or `never`: whether the pipe the hook is writing into renders ANSI escapes. |

A hook's own `env:` entries always win over these, so a project can override
any of them.
