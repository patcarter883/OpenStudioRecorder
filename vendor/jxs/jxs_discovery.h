/*
 * jxs_discovery.h - C-facing API over the encoder's DNS-SD parser.
 *
 * The plugin is C11; the parser (vendored verbatim from the encoder, see
 * src/mdns/VENDORED.md) is C++. jxs_discovery.cpp is the ONE boundary between
 * them: it marshals bridge::discover()'s result into a plain C struct array.
 * It performs NO DNS parsing itself - "one parser, never a third".
 *
 * Discovery is a convenience, never a dependency: a scan that finds nothing
 * returns 0, and the caller keeps its manual address fields.
 */
#ifndef JXS_DISCOVERY_H
#define JXS_DISCOVERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JXS_DISC_MAX_CAMERAS 32
#define JXS_DISC_NAME_MAX 64
#define JXS_DISC_ADDR_MAX 64
#define JXS_DISC_VAL_MAX 128

/* One camera advertising _obr-cam._udp. `address` is the A-record address (the
 * one to USE - never a re-resolved .local name, which can answer with an
 * unreachable global IPv6 first). */
typedef struct {
	char instance[JXS_DISC_NAME_MAX]; /* DNS-SD instance label          */
	char name[JXS_DISC_NAME_MAX];     /* TXT "name" (falls back to instance) */
	char address[JXS_DISC_ADDR_MAX];  /* A record: the camera's LAN IPv4 */
	char host[JXS_DISC_ADDR_MAX];     /* SRV target host name            */
	uint16_t service_port;            /* SRV port                        */
	char codec[JXS_DISC_VAL_MAX];     /* TXT "codec"        */
	char resolution[JXS_DISC_VAL_MAX];/* TXT "resolution"   */
	char fps[JXS_DISC_VAL_MAX];       /* TXT "fps"          */
	char sampling[JXS_DISC_VAL_MAX];  /* TXT "sampling"     */
	char api_port[JXS_DISC_VAL_MAX];  /* TXT "api_port" (HTTP port, NOT stream) */
	char api_version[JXS_DISC_VAL_MAX];/* TXT "api_version" */
} jxs_discovered_camera_t;

/*
 * Blocking browse of `_obr-cam._udp` for window_ms (clamped to [200,8000]).
 * Fills up to `max` entries in `out` and returns the count (0 when nothing
 * answered). It never fails: a missing socket or a quiet LAN is 0, not an
 * error. Call it from a worker thread, never a realtime path.
 */
int jxs_discovery_scan(jxs_discovered_camera_t *out, int max, int window_ms);

#ifdef __cplusplus
}
#endif

#endif /* JXS_DISCOVERY_H */
