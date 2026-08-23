# BS::thread_pool

Vendored copy of [bshoshany/thread-pool](https://github.com/bshoshany/thread-pool),
version 3.3.0, commit `67fad04348b91cf93bdfad7495d298f54825602c`. MIT licensed;
see `LICENSE.txt`.

It lives in the tree rather than being fetched at build time because a download
step makes the build unusable offline and unusable for distro packaging, and
because the library is a single header with no build system of its own to gain
from.

Unmodified. To update, replace `include/BS_thread_pool.hpp` and `LICENSE.txt`
from the upstream tag and record the new version and commit above.
