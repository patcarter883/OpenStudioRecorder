# Vendored DNS-SD parser (the encoder's exact translation units)

These files are **byte-for-byte copies** of the DNS-SD parser that already exists
in the encoder repo:

    open-broadcast-encoder/source/bridge/mdns.{h,cpp}
    open-broadcast-encoder/source/bridge/discovery.{h,cpp}

pinned at encoder commit `bf9516bd3d17ac30f8251f784f5f0ff43063e82f`
("feat(bridge): install the session's RIST key on the bridge").

## Why they are vendored rather than re-implemented

The house rule is **one parser, never a third**: the DNS-SD code above is the
single parser in the tree, reused by the bridge browser, the MC5 `obr-cam-browse`
helper, and now this plugin. Writing a fresh mDNS parser in the plugin would be a
third parser and is explicitly forbidden.

The plugin is a C11 translation unit with no C++ dependency of its own, and the
`obr-cam-browse` helper binary exists but is not installed anywhere on `PATH`
(only inside the encoder's build tree), so a subprocess dependency would be
fragile and would drag in `httplib`/`nlohmann_json`. Vendoring the **same source
files** and compiling them as C++ into the plugin keeps exactly one parser
implementation while making the plugin self-contained: `src/jxs_discovery.cpp`
is a thin C-callable marshalling shim over `bridge::discover()` — it does no DNS
parsing at all.

## Constraints that must not regress (carried over verbatim)

* `SO_REUSEADDR` only, **never `SO_REUSEPORT`** — in `discovery.cpp`.
* DNS labels are length-prefixed, so the service type is never searched for as a
  literal byte string — in `mdns.cpp`.
* A malformed record refuses the **whole** packet by design.

Do not edit these files here. If the parser must change, change it upstream in
the encoder repo and re-copy, then update the pinned commit above and the
`sha256` values below.

## sha256 of the vendored files

    mdns.h         afe065485b047145373990fa3f34230da451509a2a995700d7d44bc0ec23af85
    mdns.cpp       a45a39521e2572a64cf4a5b1326cf5fc8f8e9aa024bdc7bbbe9c980c238fbc34
    discovery.h    fa4bd73c16a6339378e25883091455907a8e000554b075db7617fabf86f47ff4
    discovery.cpp  eb4b8c38d12d241767d141b5d5a6497a18554ba1264f9c4d4800c6135ecd19e6
