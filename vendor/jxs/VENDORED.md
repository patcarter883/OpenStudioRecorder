# Vendored JPEG XS units in `vendor/jxs/`

These files are **copies**, not forks. The rig's rule is **one definition per constant**: a
constant re-typed here is a second parser waiting to disagree with the first, and the two carrier
points of the JPEG XS colour declaration (the PMT's `JXS_video_descriptor` and the per-frame `jxes`
box) are exactly the kind of thing that silently drifts.

## `jxs_ts.{c,h}` and `s302m.{c,h}`

| file | taken from | commit | sha256 |
|---|---|---|---|
| `jxs_ts.c` | `obs-jxs-camera` `src/jxs_ts.c` | `2253d50` | `938147ea6b24cae8ae6a6b3a3e7aed4d0aa933187709875cec54b2671d47e280` |
| `jxs_ts.h` | `obs-jxs-camera` `src/jxs_ts.h` | `2253d50` | `ee24ed9f509ef3e17730f104fff97061c6e8c48da28cc23b37d70ee0d2efe439` |
| `s302m.c`  | `obs-jxs-camera` `src/s302m.c`  | `2253d50` | `ab700097f046486dac69caf10fb3405bf5e6b1f441ab8e40effb582b67e60cfc` |
| `s302m.h`  | `obs-jxs-camera` `src/s302m.h`  | `2253d50` | `c41bc7d4ee0fd086e1e9cabf37a10465318908353a938537542632dccef58649` |

`2253d50` is the head of `fix/colour-range-truth`: the change that made the plugin **read** the
stream's declared range/colour from both carriage points instead of hardcoding it. `s302m.{c,h}` are
included because `jxs_ts.c` includes them; copying a translation unit without its dependencies is
how you get a copy that cannot be compiled.

Why the whole demux and not just "the four bytes": the plugin's `jxs_ts.c` is the canonical
implementation of where those bytes live and which of the two carriage points wins. Lifting just the
offsets into the recorder would be a second definition of them.

The recorder drives it through `source/jxs_range_probe.{c,h}` — thin glue that feeds the vendored
demux and reports what it declared. The glue holds **no** parsing constants; the demux's PID and
descriptor constants stay in the vendored files.

## Verifying

`test/vendored_jxs_hash.cmake` fails the build if any of the four files stops matching the hashes
above. It is registered as a CTest test by `test/CMakeLists.txt`:

```
ctest --test-dir <build> -R vendored-jxs-hash -V
```

## Re-copying after a deliberate change upstream

```sh
cp <plugin>/src/{jxs_ts.c,jxs_ts.h,s302m.c,s302m.h} vendor/jxs/
sha256sum vendor/jxs/jxs_ts.c vendor/jxs/jxs_ts.h vendor/jxs/s302m.c vendor/jxs/s302m.h
# then update the table above and the expected_* values in test/vendored_jxs_hash.cmake
```
