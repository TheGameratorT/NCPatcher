# CLI11

Vendored, not fetched. A build-time download is unusable offline and unusable
for distro packaging, and CLI11 is not packaged widely enough to rely on
`find_package` finding it -- so the single header lives here instead.

- Version: 2.5.0
- Source: https://github.com/CLIUtils/CLI11/releases/tag/v2.5.0
- Licence: 3-clause BSD, see `LICENSE`

`CLI11.hpp` is the released single-header amalgamation, unmodified. It is
included by `source/app/cli.cpp` and nowhere else, so nothing but the argument
parser pays its compile time.

To update: replace `CLI11.hpp` and `LICENSE` from a newer release and change the
version above. There are no local patches to carry forward.
