/*
 * jxs_controls.h - the OBS-side client for the camera node's control API.
 *
 * OBS-independent on purpose: the plugin (src/jxs-camera.c) and the standalone
 * mapping harness (tools/jxs_controls_probe.c) both compile this file. The
 * harness proves, against the LIVE node, exactly which advertised control maps
 * to which widget and with which bounds - the same mapping the properties pane
 * renders. Nothing here touches libobs, and nothing here is on the video path.
 *
 * The API (measured against the node's jxs-webui.py):
 *   GET  /api/controls -> {"hot":bool, "advertised":[{name,type,array,min,max,
 *                          default?,enum?}], "current":{name:value},
 *                          "desired":{name:str}, "rejected":[name],
 *                          "af":{...}, "restart_required":[{key,what}]}
 *   POST /api/controls  <- a flat JSON object {name: value}. Hot: applied to
 *                          the next frame, never a service restart. Unknown
 *                          names are refused with HTTP 400. Response:
 *                          {"message":..., "hot":true, "restart":false,
 *                           "changed":[name,...]}
 *
 * restart_required reports stream-pipeline keys (JXS_DEST, ...), not image
 * controls; a control is hot unless its name appears there or the document's
 * top-level "hot" is false. Those are never offered for editing here.
 */
#ifndef JXS_CONTROLS_H
#define JXS_CONTROLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This is a C API. Consumers that are C++ (the recorder) must see C linkage, or
 * the declarations take C++ linkage and the linker asks for mangled names the C
 * translation unit never exports - every symbol undefined, at link time only.
 * Self-guarding here means a consumer cannot forget the extern "C" wrapper.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* The widget an advertised control descriptor maps to. */
typedef enum {
	JC_W_UNKNOWN = 0,
	JC_W_BOOL,  /* Bool                     -> checkbox            */
	JC_W_ENUM,  /* Integer32 with an enum    -> dropdown (int)      */
	JC_W_INT,   /* Integer32 without an enum -> bounded int field   */
	JC_W_FLOAT, /* Float scalar              -> bounded float field */
	JC_W_ARRAY, /* array (Float[]/Rectangle[]) -> token text field  */
} jc_widget_t;

/*
 * ZOOM - a CAPABILITY plus a VALUE, not a bare factor.
 *
 * Neither of this rig's two cameras has optical zoom today, but the contract
 * must not assume that: a UVC/USB camera (V4L2_CID_ZOOM_ABSOLUTE) or a PTZ/IP
 * camera is a plausible node, and the capability is DISCOVERED FROM THE DEVICE
 * (libcamera's advertised ScalerCrop; Camera2's zoom ratio range /
 * LENS_INFO_AVAILABLE_FOCAL_LENGTHS), never hardcoded here. See
 * docs/zoom-control.md.
 *
 * The capability rides the Zoom control's OWN descriptor as an extra JSON key
 * (JC_ZOOM_TYPE_KEY). That is a NON-BREAKING extension: the JSON reader and the
 * advertised-set mapper in older consumers ignore keys they do not know, so an
 * old pane still renders and posts the Zoom float unchanged, while a new one
 * labels it "digital"/"optical". A node that reports no capability (an old
 * node) parses as JC_ZOOM_TYPE_UNKNOWN - never as digital.
 *
 * Mechanism precedence is decided in ONE place (the node's zoom policy), not
 * inline: optical wins when present; "optical+digital" drives the lens inside
 * its optical range and the crop beyond it; a digital-only device says so.
 */
typedef enum {
	JC_ZOOM_TYPE_UNKNOWN = 0, /* node did not report a capability (old node) */
	JC_ZOOM_TYPE_NONE = 1,    /* device has no zoom mechanism at all         */
	JC_ZOOM_TYPE_DIGITAL = 2, /* crop/scale only (no lens movement)          */
	JC_ZOOM_TYPE_OPTICAL = 3, /* a real zoom lens / motorised zoom           */
	JC_ZOOM_TYPE_OPTICAL_DIGITAL = 4, /* both; optical first, then crop      */
} jc_zoom_type_t;

/* The value control's name, protocol floor/ceiling and default. Each NODE
 * advertises its own device-derived [min..max]; these are the protocol bounds
 * the consumers clamp to, not a promise about any device. 1.0 = full frame. */
#define JC_ZOOM_NAME "Zoom"
#define JC_ZOOM_MIN 1.0
#define JC_ZOOM_MAX 4.0
#define JC_ZOOM_DEFAULT 1.0

#define JC_ZOOM_TYPE_KEY "zoom_type"             /* descriptor capability   */
#define JC_ZOOM_TYPE_SOURCE_KEY "zoom_type_source" /* where it came from    */

/* "unknown"|"none"|"digital"|"optical"|"optical+digital" (JSON spelling). */
const char *jc_zoom_type_name(jc_zoom_type_t t);
jc_zoom_type_t jc_zoom_type_parse(const char *s);
/* Human label for a pane: "not reported", "none", "Digital zoom (crop)", ... */
const char *jc_zoom_type_label(jc_zoom_type_t t);
/* True when the type means a lens actually moves (optical, or both). */
bool jc_zoom_type_is_optical(jc_zoom_type_t t);

/*
 * Centred crop of full_w x full_h for normalised factor z, clamped to
 * [JC_ZOOM_MIN, JC_ZOOM_MAX] and to >= 1 px. This is the ONE mapping from the
 * protocol's factor to a rectangle, shared by the sensor-space (libcamera) and
 * active-array (Camera2) nodes so the two cannot drift.
 *
 * A factor is CLAMPED, not refused: z below MIN (including 0) yields the full
 * frame and z above MAX the protocol ceiling. The ONLY false return is a
 * degenerate SIZE (full_w or full_h < 1). Callers must therefore reject a
 * non-finite factor themselves - NaN satisfies neither comparison, so it would
 * otherwise slip through to a 1x1 rectangle. Every node's policy does this
 * before it gets here (see jxs_zoom_policy.h: `if (!(z >= JC_ZOOM_MIN))`).
 * On success w/h are >= 1 and <= full.
 */
bool jc_zoom_crop_rect(double z, int full_w, int full_h, int *x, int *y, int *w,
		int *h);

#define JC_MAX_FIELDS 64
#define JC_MAX_ENUM 16
#define JC_NAME_MAX 64
#define JC_LABEL_MAX 64
#define JC_TEXT_MAX 256
#define JC_ERR_MAX 256

typedef struct {
	int value;
	char label[JC_LABEL_MAX];
} jc_enum_t;

typedef struct {
	char name[JC_NAME_MAX];
	char type[32];
	bool is_array;
	jc_widget_t widget;

	bool has_min, has_max;
	double fmin, fmax; /* Float bounds              */
	int imin, imax;    /* Integer bounds (clamped)  */

	int enum_count;
	jc_enum_t enums[JC_MAX_ENUM];

	/* advertised but not hot: read-only in the pane, never POSTed */
	bool restart_required;

	/* applied value: from "current", else the descriptor's "default" */
	bool has_value;
	bool val_bool;
	double val_num;           /* scalar numeric value       */
	double val_arr[8];        /* array value                */
	int val_arr_len;
	char val_text[128];       /* canonical token string of  */
	                          /* the array (or the raw str) */

	/* ZOOM capability (only meaningful on the Zoom control). Discovered from
	 * the device by the node and carried on the descriptor as the extra
	 * JC_ZOOM_TYPE_KEY. false == the node did not report one (an old node),
	 * which is NOT the same as "digital". */
	bool has_zoom_type;
	jc_zoom_type_t zoom_type;
	char zoom_type_source[64]; /* e.g. "libcamera ScalerCrop" */
} jc_field_t;

typedef struct {
	bool ok;          /* GET + parse succeeded                */
	bool hot;         /* top-level "hot"                      */
	char error[JC_ERR_MAX];
	char base_url[JC_TEXT_MAX];

	int n_fields;
	jc_field_t fields[JC_MAX_FIELDS];

	int n_restart;
	char restart_keys[32][JC_NAME_MAX];

	char status[JC_TEXT_MAX]; /* one-line human summary      */
} jc_controls_t;

/* ---- HTTP client (plain sockets; no libcurl, no libobs) ----------------- */

/*
 * GET base_url+path. On HTTP 2xx returns 0 and fills out/out_len (NUL
 * terminated). Otherwise returns -1 and fills err. Bounded by connect and
 * read timeouts: an unreachable node fails fast rather than hanging a caller.
 */
int jc_http_get(const char *base_url, const char *path, char *out, size_t cap,
		size_t *out_len, char *err, size_t errcap);

/*
 * POST a JSON body to base_url+path. Returns the HTTP status code, or -1 on a
 * transport failure (err filled). The response body is copied to resp.
 */
int jc_http_post_json(const char *base_url, const char *path,
		      const char *json_body, char *resp, size_t cap,
		      size_t *resp_len, char *err, size_t errcap);

/* ---- mapping ------------------------------------------------------------ */

/* Parse a /api/controls document and map the advertised set to widgets. */
bool jc_controls_parse(const char *json, size_t len, jc_controls_t *out);

/* GET base_url/api/controls and parse it (a thin, honest wrapper). */
bool jc_controls_fetch(const char *base_url, jc_controls_t *out);

/* POST a flat {name:value} body of changed controls; logs nothing. */
bool jc_controls_post(const char *base_url, const char *json_body,
		      char *resp, size_t cap, char *err, size_t errcap);

/*
 * Point the camera's stream at dest:port via the node's POST /api/config
 * (the destructive write that configures the camera - it restarts the node's
 * stream). dest may be a bare address or a group; port is a decimal string.
 * Returns true on HTTP 2xx, else false with err set to the node's message.
 */
bool jc_node_set_destination(const char *base_url, const char *dest,
			     const char *port, char *resp, size_t cap,
			     char *err, size_t errcap);

/* ---- multicast group: base block + per-camera index --------------------- */

/*
 * MULTICAST MODE. One fixed UDP port for every camera; each camera is separated
 * by a distinct GROUP, formed by incrementing the last octet of a base /24 block.
 * Full rationale: docs/multicast-mode.md.
 *
 * Base block: a configurable "a.b.c" prefix, DEFAULT "239.1.1" (i.e. 239.1.1.0/24).
 *   The default matches (a) the node's own documented default group (239.1.1.1)
 *   and (b) the block the OBS plugin has always used, so every consumer on the
 *   LAN agrees. Change it only when two rigs share one L2 (a fixed block collides).
 * Index: n in 1..254  ->  "<base>.<n>".  0 (the subnet address) and 255 (broadcast)
 *   are not valid host groups and are excluded by the bounds.
 * Port: JC_MCAST_DEFAULT_PORT (5000), shared by every camera - NOT per camera.
 *
 * These are the ONE place the base/index/port are written down; the plugin and
 * the recorder both compile this file, so they cannot disagree.
 */
#define JC_GROUP_BASE_DEFAULT "239.1.1"
#define JC_GROUP_INDEX_MIN 1
#define JC_GROUP_INDEX_MAX 254
#define JC_GROUP_INDEX_DEFAULT 1
#define JC_MCAST_DEFAULT_PORT 5000

/* Compose "<base>.<idx>" into out. base == NULL or "" -> JC_GROUP_BASE_DEFAULT.
 * idx is clamped into JC_GROUP_INDEX_MIN..MAX. Returns false only when the base
 * is malformed or out is too small. */
bool jc_group_addr(const char *base, int idx, char *out, size_t cap);

/* True when `base` is a usable "a.b.c" IPv4 /24 prefix (first octet in 224..239,
 * the administratively-scoped multicast range used here). NULL/"" is valid and
 * means "use the default". */
bool jc_group_base_valid(const char *base);

/* One discovered camera as the auto-assigner sees it: how to reach its control
 * API (A-record address + TXT api_port). OBS-independent on purpose, so the
 * same logic is exercised by the standalone probe. */
typedef struct {
	char instance[JC_NAME_MAX];
	char address[64];
	char api_port[16];
} jc_cam_endpoint_t;

/*
 * Read base_url/api/status (READ-ONLY) and, when the node reports a stream
 * destination of "<base>.<n>" with n in 1..254, return that index in
 * *out_index. base == NULL/"" uses JC_GROUP_BASE_DEFAULT. Returns true only for
 * a report inside the base block; a unicast/garbage/absent dest is false.
 * Bounded like every jc_http_get.
 */
bool jc_node_status_group_index(const char *base_url, const char *base,
				int *out_index);

/*
 * Read base_url/api/status (READ-ONLY) and report whether the node's stream
 * carries the SMPTE 302M audio elementary stream (node.stream.audio.enabled).
 * A camera without audio (or unreachable, or with no audio block in its
 * status) reports *enabled = 0. Returns true when the status was parsed.
 */
bool jc_node_status_audio(const char *base_url, bool *enabled);

/*
 * Auto-assign the multicast last octet from what the cameras on the LAN are
 * already using. For each endpoint in cams[0..n_cams-1] it reads /api/status
 * (read-only) and notes the group index reported inside the `base` block.
 *
 *   - If `self_instance` names a camera that reports a group, that index is
 *     KEPT (stability: re-picking a camera must not churn its group).
 *   - Otherwise the LOWEST free index in 1..254 is chosen.
 *   - When no camera answers at all, *out_index is the default (1).
 *
 * base == NULL/"" uses JC_GROUP_BASE_DEFAULT. `occ` (255 bools, slots 1..254)
 * receives the occupied set, or may be NULL. Returns the number of cameras that
 * REPORTED a group (not merely ones that answered HTTP). Every fetch is bounded
 * (~0.9 s connect, ~1.5 s read), so a powered-off camera cannot hang the caller.
 */
int jc_autopick_group_index(const char *base, const jc_cam_endpoint_t *cams,
			    int n_cams, const char *self_instance, bool occ[255],
			    int *out_index);

/* Normalise a whitespace/comma/semicolon-separated token list to "a,b,c". */
void jc_canon_tokens(const char *s, char *out, size_t cap);

const char *jc_widget_name(jc_widget_t w);

#ifdef __cplusplus
}
#endif

#endif /* JXS_CONTROLS_H */

/* ---- image match: making two cameras look like one ---------------------- *
 *
 * The phone's tone curve is the only part of its picture that is deterministic
 * enough to match: TonemapMode 0 hands the device OUR points and switches off
 * the ISP's local/nonglobal tone work, while modes 1 and 2 are the device's own
 * adaptive map and report a two-point identity curve - not copyable, not
 * matchable. Everything here is about that one curve.
 *
 * It lives in this file, rather than in either consumer, for the same reason as
 * the rest of it: one definition. The recorder and the OBS source each supply
 * only the thing they uniquely have - a frame from each camera.
 *
 * Three findings are baked into this API, each one paid for with measurements:
 *
 *  - MATCH CLOSED-LOOP. The control points are not in the output-code domain: a
 *    4% change to the mid-curve moved the measured mean by 44 code values. A
 *    curve DERIVED from a histogram is therefore wrong however carefully it is
 *    derived. Apply it, measure it, keep the best.
 *  - SEARCH A SHAPE, NOT A GAMMA. x^gamma is a log-type curve - below 1 it is
 *    washed (blacks lifted, the "looks like a log recording" complaint) and
 *    above 1 it is dark. Neither is natural. Natural is the display encode with
 *    a little contrast on top, so that is the family.
 *  - SCORE THE ENDPOINTS TOO. Shape alone (each camera normalised to its own
 *    black/white) is blind to contrast, and standard deviation alone REWARDS
 *    lifting the blacks, because that inflates the deviation. Score shape and
 *    level together.
 */

#define JC_MATCH_BINS 256
#define JC_MATCH_PCTS 9
#define JC_MATCH_POINTS 6
#define JC_MATCH_PLAN 9

/* One camera's luma distribution - the whole basis of the comparison. */
typedef struct {
	int count;                  /* pixels counted                            */
	double hist[JC_MATCH_BINS]; /* normalised, sums to 1                     */
	double pct[JC_MATCH_PCTS];  /* CDF at 1,5,10,25,50,75,90,95,99 percent   */
	double mean, std;           /* code values, 0..255                       */
} jc_luma_t;

/*
 * Fill from a packed 8-bit luma plane (any colour layout, reduced by the
 * caller). false means an unusable plane, so a caller skips the frame instead
 * of scoring a stale one.
 */
bool jc_luma_from_plane(jc_luma_t *out, const unsigned char *plane, int w,
			int h, int stride);

/*
 * Distance between two cameras in code values. Each camera is normalised to its
 * own black/white unless absolute is set. Shape is the honest measure while
 * both are on auto exposure, where the absolute level is the AEs' business and
 * not the curve's; absolute is the honest one once the exposure is pinned.
 * Returns a negative value if either input is empty.
 */
double jc_match_score(const jc_luma_t *a, const jc_luma_t *b, bool absolute);

/* A tone curve in the node's own points, x and y both 0..1, ends included. */
typedef struct {
	int n;
	double x[JC_MATCH_POINTS];
	double y[JC_MATCH_POINTS];
} jc_curve_t;

/*
 * The family: y = (1-c) * x^gamma + c * smoothstep(x^gamma), for contrast c in
 * 0..1. gamma carries the encode, c the contrast on top of it. The S is applied
 * to the ENCODE'S OUTPUT, not to x: blending toward smoothstep(x) instead
 * mostly darkens the picture, because those two curves differ by far more than
 * a contrast change - a brightness change wearing a contrast label, measured at
 * mean 109 falling to 52 as c went 0 to 0.75.
 */
void jc_match_curve(jc_curve_t *out, double gamma, double contrast);

/*
 * Flatten a curve to the interleaved x,y pairs the TonemapCurvePoints control
 * takes. Returns the number of PAIRS written, or -1 if it does not fit.
 */
int jc_match_curve_points(const jc_curve_t *c, double *xy, int max_pairs);

/*
 * The closed loop over a fixed, measured plan of candidates. The consumer owns
 * the frames and therefore the timing - only it can get the latest frame out of
 * each camera - and this owns the search. Every candidate costs a real capture,
 * so the budget is the plan's length rather than an iteration count.
 */
typedef struct {
	int i;                   /* candidate index                        */
	double gamma, contrast;  /* the candidate to apply and measure     */
	double best_score;
	double best_gamma, best_contrast;
	bool done;
} jc_match_t;

void jc_match_begin(jc_match_t *s);
/*
 * Advance given the score measured for the candidate just tested (pass a
 * negative score for "the capture failed"). Returns false when the plan is
 * exhausted; otherwise *out is the next candidate to apply.
 */
bool jc_match_next(jc_match_t *s, double score, jc_curve_t *out);
/* The best candidate found, for the final apply. */
void jc_match_best(const jc_match_t *s, jc_curve_t *out);

/*
 * Apply a curve to a node: POST TonemapMode 0 with the points (the curve means
 * nothing in any other mode).
 */
bool jc_match_apply(const char *base_url, const jc_curve_t *c, char *err,
		    size_t errcap);
