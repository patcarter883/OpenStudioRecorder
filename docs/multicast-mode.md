# JPEG XS cameras over multicast — fixed port, one group per camera

Status: adopted. Applies to the OBS source plugin (`obs-jxs-camera`), the recorder
(`OpenStudioRecorder`) and the camera nodes (Pi `jpegxs-arm-port`, Android
`android-jxs-cam`).

## The problem

The previous scheme separated cameras by **UDP port**: the Pi published to
`:5001`, the phone to `:5002`, and every consumer took a `host:port` "target" per
source. That works, but it fights the network:

* **IGMP snooping grants membership per GROUP, not per port.** A per-port scheme
  gains nothing at all from snooping — the switch either floods the group or
  drops it; the port number is invisible to it.
* Every camera costs a **port + a firewall rule** on every consumer, and a node
  pointed at a port nobody listens on is 100 % discard (looks like packet loss).
* Two cameras accidentally sharing one port both bind with `SO_REUSEADDR` and
  silently starve each other — the "added but never showing" failure.

The video LAN now has a working **IGMP querier** (the video router runs
`omcproxy`), so memberships no longer age out. That removes the historical reason
to avoid multicast, and the right model becomes the standard one.

## The decision

**One fixed UDP port for every camera; each camera gets its own multicast GROUP,
formed by incrementing the last octet of a base block.**

| knob | value | scope |
|---|---|---|
| transport | `unicast` \| `multicast` | per camera/source |
| port | `5000` (default) | **global** — one value for all cameras |
| group base | `239.1.1` (default) | configurable /24 prefix |
| group index | `1..254` (default `1`) | per camera |

* **Group mapping:** index `n` → `<base>.<n>`, i.e. `239.1.1.<n>`. `1` is the
  default and equals the node's own documented default group. `0` (the subnet
  address) and `255` (broadcast) are excluded by the `1..254` bounds.
* **Default block:** `239.1.1.0/24`. Chosen because it is *already* the block in
  every existing piece: the Pi's `deploy/jxs-stream.config` default is
  `JXS_DEST=239.1.1.1`, and the OBS plugin has shipped `239.1.1.0/24` since
  commit `28aafb5`. Agreement across consumers is the whole point — the plugin
  and the recorder must auto-pick from the *same* block.
* **The base is configurable** (a `a.b.c` string, default `239.1.1`) because two
  rigs on one flat L2 would otherwise collide. It is one value per rig, set the
  same on the plugin and the recorder.
* **The port stays configurable but is no longer per camera.** It must be the
  same value everywhere; changing it is a rig-wide change. It matches the node
  default (`JXS_PORT=5000`) and the plugin default.
* **Unicast stays a mode.** With `unicast` selected nothing changes: one port per
  camera, the destination derived from this host's address toward the camera. The
  old path is preserved so the rig keeps working while multicast is brought up.

### How a node learns its index / group

The index is **config**, assigned by the consumer and pushed to the node the same
way the destination always was: `POST /api/config` with
`JXS_DEST="<group>:<port>"`. There is no new discovery protocol. As a read-only
convenience, a consumer picks a free index by reading each discovered camera's
`GET /api/status` `node.dest` (the group the camera already reports) and taking
the lowest index not in use, keeping a camera's existing group when it already
reports one (so re-picking never churns it). This is
`jc_autopick_group_index()` in the shared `jxs_controls.*`, which both the plugin
and the recorder compile.

### Why the nodes need no transport change

Both nodes are already plain multicast senders: the Pi's `ts_mux`/`ts_udp`
`connect()`s the socket to the group and pins `IP_MULTICAST_IF`; the phone's
bridge does the same, pinning the egress with `JXS_IFACE`. Both already put the
group in `JXS_DEST`. So "camera 2 uses `239.1.1.2:5000`" is just a destination
change — no node code is involved. (The phone's out-of-box default port is 5001,
but the port is embedded in `JXS_DEST` on every steer, so it does not matter.)

## Rejected alternatives

* **Keep per-port (status quo).** No snooping benefit, a port + rule per camera
  per consumer, and a silent starvation mode when two cameras share a port.
  Rejected — the querier now makes the group model work, so the workaround is
  obsolete.
* **A second, disjoint block (e.g. `239.2.x`).** Would disagree with the node
  default and with the plugin's committed block, so the auto-pick would see two
  blocks and pick collisions. Rejected.
* **0-based index.** `.0` is a subnet address, not a host group. 1-based keeps
  index 1 == the node's documented `239.1.1.1`.
* **A discovery protocol for the index.** Unnecessary: the index is config; the
  `node.dest` read is a convenience, not a protocol.
* **Making the port per camera in multicast.** The entire reason multicast wins
  is that membership is per group and the port is common. Per-camera ports under
  multicast would reintroduce every cost of the old scheme.

## Consumer-side implementation

* **OBS plugin** (`src/jxs-camera.c`): `transport` selector; `group_base` +
  `group_index` → group via `jc_group_addr`; one `port` setting (default 5000);
  receive thread `IP_ADD_MEMBERSHIP`s the group and binds `<group>:<port>`; the
  shared PTP→OBS offset stays a single module-scope value.
* **Recorder** (`source/main.cpp`): `transport` selector per camera; in multicast
  the camera's `port` is the global 5000 and its `group` is `<base>.<n>`; the
  capture pipeline uses `udpsrc multicast-group=<group> port=<port>` (GStreamer
  joins the group itself); the steer pushes `<group>:5000` to the node.
* **Shared API** (`jxs_controls.h`, vendored into the recorder): the base block,
  index bounds and default port live in ONE place, so the two consumers cannot
  disagree.
