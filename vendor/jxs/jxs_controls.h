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

/* The widget an advertised control descriptor maps to. */
typedef enum {
	JC_W_UNKNOWN = 0,
	JC_W_BOOL,  /* Bool                     -> checkbox            */
	JC_W_ENUM,  /* Integer32 with an enum    -> dropdown (int)      */
	JC_W_INT,   /* Integer32 without an enum -> bounded int field   */
	JC_W_FLOAT, /* Float scalar              -> bounded float field */
	JC_W_ARRAY, /* array (Float[]/Rectangle[]) -> token text field  */
} jc_widget_t;

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

/* ---- multicast last-octet: read-only auto-assignment probe -------------- */

/*
 * The plugin's multicast block is FIXED at 239.1.1.0/24; the operator-facing
 * knob is a single last-octet index in 1..254, rendering as 239.1.1.<n>. These
 * are the one place the base is written down.
 */
#define JC_GROUP_BASE_A 239
#define JC_GROUP_BASE_B 1
#define JC_GROUP_BASE_C 1
#define JC_GROUP_INDEX_MIN 1
#define JC_GROUP_INDEX_MAX 254
#define JC_GROUP_INDEX_DEFAULT 1

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
 * destination of 239.1.1.<n> with n in 1..254, return that index in
 * *out_index. Returns true only for a report inside our base block; a
 * unicast/garbage/absent dest is false. Bounded like every jc_http_get.
 */
bool jc_node_status_group_index(const char *base_url, int *out_index);

/*
 * Auto-assign the multicast last octet from what the cameras on the LAN are
 * already using. For each endpoint in cams[0..n_cams-1] it reads /api/status
 * (read-only) and notes the group index reported.
 *
 *   - If `self_instance` names a camera that reports a group, that index is
 *     KEPT (stability: re-picking a camera must not churn its group).
 *   - Otherwise the LOWEST free index in 1..254 is chosen.
 *   - When no camera answers at all, *out_index is the default (1).
 *
 * `occ` (255 bools, slots 1..254) receives the occupied set, or may be NULL.
 * Returns the number of cameras that REPORTED a group (not merely ones that
 * answered HTTP). Every fetch is bounded (~0.9 s connect, ~1.5 s read), so a
 * powered-off camera cannot hang the caller.
 */
int jc_autopick_group_index(const jc_cam_endpoint_t *cams, int n_cams,
			    const char *self_instance, bool occ[255],
			    int *out_index);

/* Normalise a whitespace/comma/semicolon-separated token list to "a,b,c". */
void jc_canon_tokens(const char *s, char *out, size_t cap);

const char *jc_widget_name(jc_widget_t w);

#endif /* JXS_CONTROLS_H */
