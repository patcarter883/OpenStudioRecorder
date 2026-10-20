/*
 * jxs_controls.c - the client + advertised-set-to-widget mapping, shared
 * between the OBS source and the standalone harness. See jxs_controls.h.
 *
 * Deliberately dependency-free: a compact JSON reader, a bounded raw-socket
 * HTTP/1.1 client, and the mapping. If any of it fails the caller gets ok=false
 * and an error string - the video path in jxs-camera.c never consults this.
 */
#include "jxs_controls.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* HTTP/1.1 over a bounded socket                                      */
/* ------------------------------------------------------------------ */

#define JC_CONNECT_MS 900
#define JC_TOTAL_MS 1500
#define JC_IO_MAX (512 * 1024)

static int set_nonblock(int fd, bool on)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return -1;
	return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

/* Resolve base_url into host (may be a name or IP), port and base path. */
static int split_url(const char *base_url, char *host, size_t hostcap,
		     int *port, char *basepath, size_t pathcap, char *err,
		     size_t errcap)
{
	const char *u = base_url ? base_url : "";
	if (strncmp(u, "http://", 7) == 0) {
		u += 7;
	} else if (strncmp(u, "https://", 8) == 0) {
		snprintf(err, errcap, "https is not supported by this client");
		return -1;
	} else if (strncmp(u, "//", 2) == 0) {
		u += 2;
	}
	if (!*u) {
		snprintf(err, errcap, "empty control API URL");
		return -1;
	}

	const char *slash = strchr(u, '/');
	size_t hostlen = slash ? (size_t)(slash - u) : strlen(u);
	if (hostlen == 0 || hostlen >= hostcap) {
		snprintf(err, errcap, "bad host in URL");
		return -1;
	}
	memcpy(host, u, hostlen);
	host[hostlen] = 0;

	*port = 80;
	const char *colon = memchr(u, ':', hostlen);
	if (colon) {
		size_t hl = (size_t)(colon - u);
		if (hl == 0 || hl >= hostcap) {
			snprintf(err, errcap, "bad host in URL");
			return -1;
		}
		host[hl] = 0;
		char *endp = NULL;
		long p = strtol(colon + 1, &endp, 10);
		if (endp == colon + 1 || p < 1 || p > 65535) {
			snprintf(err, errcap, "bad port in URL");
			return -1;
		}
		*port = (int)p;
	}

	basepath[0] = 0;
	if (slash) {
		size_t n = strlen(slash);
		if (n >= pathcap) {
			snprintf(err, errcap, "base path too long");
			return -1;
		}
		memcpy(basepath, slash, n + 1);
	}
	return 0;
}

/* Connect to host:port within JC_CONNECT_MS. Returns fd or -1. */
static int connect_to(const char *host, int port, char *err, size_t errcap)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char portstr[16];
	snprintf(portstr, sizeof portstr, "%d", port);

	struct addrinfo *res = NULL;
	int g = getaddrinfo(host, portstr, &hints, &res);
	if (g != 0) {
		snprintf(err, errcap, "resolve %s: %s", host, gai_strerror(g));
		return -1;
	}

	int fd = -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (set_nonblock(fd, true) != 0) {
			close(fd);
			fd = -1;
			continue;
		}
		int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (r == 0)
			break;
		if (errno == EINPROGRESS) {
			fd_set wf;
			FD_ZERO(&wf);
			FD_SET(fd, &wf);
			struct timeval tv;
			tv.tv_sec = JC_CONNECT_MS / 1000;
			tv.tv_usec = (JC_CONNECT_MS % 1000) * 1000;
			int s = select(fd + 1, NULL, &wf, NULL, &tv);
			if (s > 0) {
				int soerr = 0;
				socklen_t sl = sizeof soerr;
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr,
					   &sl);
				if (soerr == 0)
					break;
				errno = soerr;
			} else if (s == 0) {
				errno = ETIMEDOUT;
			}
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd < 0) {
		snprintf(err, errcap, "connect %s:%d: %s", host, port,
			 strerror(errno));
		return -1;
	}
	return fd;
}

static void set_rcv_timeout(int fd, int ms)
{
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static int send_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				fd_set wf;
				FD_ZERO(&wf);
				FD_SET(fd, &wf);
				struct timeval tv = {0, 50000};
				if (select(fd + 1, NULL, &wf, NULL, &tv) <= 0)
					return -1;
				continue;
			}
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

/* Find header end (\r\n\r\n); returns offset after it, or 0 if not present. */
static size_t header_end(const char *b, size_t n)
{
	for (size_t i = 0; i + 3 < n; i++) {
		if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' &&
		    b[i + 3] == '\n')
			return i + 4;
	}
	return 0;
}

static int parse_status(const char *b, size_t n)
{
	if (n < 12 || strncmp(b, "HTTP/", 5) != 0)
		return -1;
	const char *sp = memchr(b, ' ', n);
	if (!sp)
		return -1;
	return (int)strtol(sp + 1, NULL, 10);
}

static long parse_content_length(const char *b, size_t n)
{
	const char *p = b;
	while (p && (size_t)(p - b) < n) {
		const char *eol = memchr(p, '\n', n - (size_t)(p - b));
		if (!eol)
			break;
		if ((size_t)(eol - p) > 15 &&
		    strncasecmp(p, "content-length:", 15) == 0) {
			return strtol(p + 15, NULL, 10);
		}
		p = eol + 1;
	}
	return -1;
}

/*
 * Perform one request. out receives the response body. Returns the HTTP status
 * (>=100) or -1 on failure.
 */
static int http_request(const char *method, const char *base_url,
			const char *path, const char *body, char *out,
			size_t cap, size_t *out_len, char *err, size_t errcap)
{
	char host[256], basepath[JC_TEXT_MAX];
	int port = 80;
	if (split_url(base_url, host, sizeof host, &port, basepath,
		      sizeof basepath, err, errcap) != 0)
		return -1;

	char reqpath[JC_TEXT_MAX * 2];
	if (basepath[0] == 0) {
		snprintf(reqpath, sizeof reqpath, "%s", path);
	} else {
		size_t bl = strlen(basepath);
		while (bl > 0 && basepath[bl - 1] == '/')
			basepath[--bl] = 0;
		snprintf(reqpath, sizeof reqpath, "%s%s", basepath, path);
	}

	int fd = connect_to(host, port, err, errcap);
	if (fd < 0)
		return -1;
	set_nonblock(fd, false); /* back to blocking; read bound by SO_RCVTIMEO */
	set_rcv_timeout(fd, JC_TOTAL_MS);
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

	size_t blen = body ? strlen(body) : 0;
	char head[1024];
	int hl;
	if (blen) {
		hl = snprintf(head, sizeof head,
			      "%s %s HTTP/1.1\r\nHost: %s\r\n"
			      "User-Agent: obs-jxs-camera-controls/1\r\n"
			      "Content-Type: application/json\r\n"
			      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
			      method, reqpath, host, blen);
	} else {
		hl = snprintf(head, sizeof head,
			      "%s %s HTTP/1.1\r\nHost: %s\r\n"
			      "User-Agent: obs-jxs-camera-controls/1\r\n"
			      "Connection: close\r\n\r\n",
			      method, reqpath, host);
	}
	if (hl <= 0 || (size_t)hl >= sizeof head || send_all(fd, head, (size_t)hl) != 0) {
		snprintf(err, errcap, "send failed: %s", strerror(errno));
		close(fd);
		return -1;
	}
	if (blen && send_all(fd, body, blen) != 0) {
		snprintf(err, errcap, "send body failed: %s", strerror(errno));
		close(fd);
		return -1;
	}

	char *rbuf = malloc(JC_IO_MAX);
	if (!rbuf) {
		snprintf(err, errcap, "out of memory");
		close(fd);
		return -1;
	}
	size_t rn = 0;
	int status = -1;
	long clen = -1;
	size_t hdr_end = 0;
	for (;;) {
		if (rn >= JC_IO_MAX)
			break;
		ssize_t n = recv(fd, rbuf + rn, JC_IO_MAX - rn, 0);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == ETIMEDOUT) {
				snprintf(err, errcap, "read timed out");
				break;
			}
			snprintf(err, errcap, "read failed: %s",
				 strerror(errno));
			break;
		}
		if (n == 0)
			break; /* connection closed */
		rn += (size_t)n;
		if (!hdr_end) {
			hdr_end = header_end(rbuf, rn);
			if (hdr_end) {
				status = parse_status(rbuf, rn);
				clen = parse_content_length(rbuf, rn);
			}
		}
		if (hdr_end && clen >= 0 &&
		    rn >= hdr_end + (size_t)clen)
			break;
	}
	close(fd);

	if (!hdr_end || status < 0) {
		if (!err[0])
			snprintf(err, errcap, "malformed HTTP response");
		free(rbuf);
		return -1;
	}
	size_t avail = rn - hdr_end;
	size_t take = (clen >= 0 && (size_t)clen < avail) ? (size_t)clen
							  : avail;
	if (take >= cap)
		take = cap - 1;
	memcpy(out, rbuf + hdr_end, take);
	out[take] = 0;
	if (out_len)
		*out_len = take;
	free(rbuf);
	return status;
}

int jc_http_get(const char *base_url, const char *path, char *out, size_t cap,
		size_t *out_len, char *err, size_t errcap)
{
	if (err && errcap)
		err[0] = 0;
	int st = http_request("GET", base_url, path, NULL, out, cap, out_len,
			      err, errcap);
	if (st < 0)
		return -1;
	if (st < 200 || st >= 300) {
		snprintf(err, errcap, "HTTP %d", st);
		return -1;
	}
	return 0;
}

int jc_http_post_json(const char *base_url, const char *path,
		      const char *json_body, char *resp, size_t cap,
		      size_t *resp_len, char *err, size_t errcap)
{
	if (err && errcap)
		err[0] = 0;
	return http_request("POST", base_url, path, json_body, resp, cap,
			    resp_len, err, errcap);
}

/* ------------------------------------------------------------------ */
/* a compact JSON reader                                              */
/* ------------------------------------------------------------------ */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jnode {
	jtype type;
	int boolean;
	double num;
	char *str;
	struct jnode **items;
	char **keys;
	int n;
	int cap;
} jnode;

typedef struct {
	const char *p;
	const char *end;
	char err[128];
} jparser;

static jnode *jnode_new(jtype t)
{
	jnode *n = calloc(1, sizeof *n);
	if (n)
		n->type = t;
	return n;
}

static void jnode_free(jnode *n)
{
	if (!n)
		return;
	free(n->str);
	if (n->keys) {
		for (int i = 0; i < n->n; i++)
			free(n->keys[i]);
		free(n->keys);
	}
	if (n->items) {
		for (int i = 0; i < n->n; i++)
			jnode_free(n->items[i]);
		free(n->items);
	}
	free(n);
}

static void skip_ws(jparser *j)
{
	while (j->p < j->end &&
	       (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' ||
		*j->p == '\r'))
		j->p++;
}

static jnode *parse_value(jparser *j);

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static char *parse_string_raw(jparser *j)
{
	if (j->p >= j->end || *j->p != '"') {
		snprintf(j->err, sizeof j->err, "expected string");
		return NULL;
	}
	j->p++;
	size_t cap = 32, len = 0;
	char *s = malloc(cap);
	if (!s)
		return NULL;
	while (j->p < j->end && *j->p != '"') {
		unsigned char c = (unsigned char)*j->p++;
		if (c == '\\') {
			if (j->p >= j->end)
				break;
			char e = *j->p++;
			switch (e) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case '/': c = '/'; break;
			case '\\': c = '\\'; break;
			case '"': c = '"'; break;
			case 'u': {
				int v = 0;
				for (int k = 0; k < 4 && j->p < j->end; k++)
					v = v * 16 + hexval((unsigned char)*j->p++);
				c = (v < 128) ? (unsigned char)v : '?';
				break;
			}
			default: c = (unsigned char)e; break;
			}
		}
		if (len + 1 >= cap) {
			cap *= 2;
			char *ns = realloc(s, cap);
			if (!ns) {
				free(s);
				return NULL;
			}
			s = ns;
		}
		s[len++] = (char)c;
	}
	if (j->p >= j->end) {
		free(s);
		snprintf(j->err, sizeof j->err, "unterminated string");
		return NULL;
	}
	j->p++; /* closing quote */
	s[len] = 0;
	return s;
}

static jnode *parse_number(jparser *j)
{
	const char *start = j->p;
	while (j->p < j->end &&
	       (*j->p == '-' || *j->p == '+' || *j->p == '.' ||
		*j->p == 'e' || *j->p == 'E' ||
		(*j->p >= '0' && *j->p <= '9')))
		j->p++;
	if (j->p == start) {
		snprintf(j->err, sizeof j->err, "bad number");
		return NULL;
	}
	char tmp[64];
	size_t n = (size_t)(j->p - start);
	if (n >= sizeof tmp)
		n = sizeof tmp - 1;
	memcpy(tmp, start, n);
	tmp[n] = 0;
	jnode *node = jnode_new(J_NUM);
	if (node)
		node->num = strtod(tmp, NULL);
	return node;
}

static int push_item(jnode *n, jnode *child)
{
	if (n->n == n->cap) {
		int nc = n->cap ? n->cap * 2 : 8;
		jnode **ni = realloc(n->items, (size_t)nc * sizeof *ni);
		if (!ni)
			return -1;
		n->items = ni;
		n->cap = nc;
	}
	n->items[n->n++] = child;
	return 0;
}

static int parse_literal_tail(jparser *j, const char *lit, jtype t,
			      jnode **out)
{
	size_t n = strlen(lit);
	if ((size_t)(j->end - j->p) < n || strncmp(j->p, lit, n) != 0) {
		snprintf(j->err, sizeof j->err, "bad literal");
		return -1;
	}
	j->p += n;
	jnode *node = jnode_new(t);
	if (!node)
		return -1;
	*out = node;
	return 0;
}

static jnode *parse_object(jparser *j)
{
	j->p++; /* { */
	jnode *obj = jnode_new(J_OBJ);
	if (!obj)
		return NULL;
	obj->keys = NULL;
	int kcap = 0;
	skip_ws(j);
	if (j->p < j->end && *j->p == '}') {
		j->p++;
		return obj;
	}
	for (;;) {
		skip_ws(j);
		char *key = parse_string_raw(j);
		if (!key)
			goto fail;
		skip_ws(j);
		if (j->p >= j->end || *j->p != ':') {
			free(key);
			snprintf(j->err, sizeof j->err, "expected ':'");
			goto fail;
		}
		j->p++;
		jnode *val = parse_value(j);
		if (!val) {
			free(key);
			goto fail;
		}
		if (obj->n == kcap) {
			int nc = kcap ? kcap * 2 : 8;
			char **nk = realloc(obj->keys, (size_t)nc * sizeof *nk);
			if (!nk) {
				free(key);
				jnode_free(val);
				goto fail;
			}
			obj->keys = nk;
			kcap = nc;
		}
		obj->keys[obj->n] = key;
		if (push_item(obj, val) != 0)
			goto fail;
		skip_ws(j);
		if (j->p < j->end && *j->p == ',') {
			j->p++;
			continue;
		}
		if (j->p < j->end && *j->p == '}') {
			j->p++;
			return obj;
		}
		snprintf(j->err, sizeof j->err, "expected ',' or '}'");
		goto fail;
	}
fail:
	jnode_free(obj);
	return NULL;
}

static jnode *parse_array(jparser *j)
{
	j->p++; /* [ */
	jnode *arr = jnode_new(J_ARR);
	if (!arr)
		return NULL;
	skip_ws(j);
	if (j->p < j->end && *j->p == ']') {
		j->p++;
		return arr;
	}
	for (;;) {
		jnode *val = parse_value(j);
		if (!val)
			goto fail;
		if (push_item(arr, val) != 0) {
			jnode_free(val);
			goto fail;
		}
		skip_ws(j);
		if (j->p < j->end && *j->p == ',') {
			j->p++;
			continue;
		}
		if (j->p < j->end && *j->p == ']') {
			j->p++;
			return arr;
		}
		snprintf(j->err, sizeof j->err, "expected ',' or ']'");
		goto fail;
	}
fail:
	jnode_free(arr);
	return NULL;
}

static jnode *parse_value(jparser *j)
{
	skip_ws(j);
	if (j->p >= j->end) {
		snprintf(j->err, sizeof j->err, "unexpected end of input");
		return NULL;
	}
	char c = *j->p;
	if (c == '{')
		return parse_object(j);
	if (c == '[')
		return parse_array(j);
	if (c == '"') {
		char *s = parse_string_raw(j);
		if (!s)
			return NULL;
		jnode *n = jnode_new(J_STR);
		if (!n) {
			free(s);
			return NULL;
		}
		n->str = s;
		return n;
	}
	if (c == 't') {
		jnode *n = NULL;
		if (parse_literal_tail(j, "true", J_BOOL, &n) != 0)
			return NULL;
		n->boolean = 1;
		return n;
	}
	if (c == 'f') {
		jnode *n = NULL;
		if (parse_literal_tail(j, "false", J_BOOL, &n) != 0)
			return NULL;
		n->boolean = 0;
		return n;
	}
	if (c == 'n') {
		jnode *n = NULL;
		if (parse_literal_tail(j, "null", J_NULL, &n) != 0)
			return NULL;
		return n;
	}
	if (c == '-' || (c >= '0' && c <= '9'))
		return parse_number(j);
	snprintf(j->err, sizeof j->err, "unexpected character '%c'", c);
	return NULL;
}

static jnode *jget(const jnode *obj, const char *key)
{
	if (!obj || obj->type != J_OBJ)
		return NULL;
	for (int i = 0; i < obj->n; i++) {
		if (strcmp(obj->keys[i], key) == 0)
			return obj->items[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* mapping                                                            */
/* ------------------------------------------------------------------ */

static jc_widget_t map_widget(const char *type, bool is_array,
			      const jnode *enm)
{
	if (is_array)
		return JC_W_ARRAY;
	if (type && strcmp(type, "Bool") == 0)
		return JC_W_BOOL;
	if (enm && enm->type == J_OBJ && enm->n > 0)
		return JC_W_ENUM;
	if (type && strstr(type, "Integer"))
		return JC_W_INT;
	if (type && strcmp(type, "Float") == 0)
		return JC_W_FLOAT;
	return JC_W_UNKNOWN;
}

const char *jc_widget_name(jc_widget_t w)
{
	switch (w) {
	case JC_W_BOOL: return "bool";
	case JC_W_ENUM: return "enum";
	case JC_W_INT: return "int";
	case JC_W_FLOAT: return "float";
	case JC_W_ARRAY: return "array";
	default: return "unknown";
	}
}

/* ---- zoom capability ------------------------------------------------- */

const char *jc_zoom_type_name(jc_zoom_type_t t)
{
	switch (t) {
	case JC_ZOOM_TYPE_NONE: return "none";
	case JC_ZOOM_TYPE_DIGITAL: return "digital";
	case JC_ZOOM_TYPE_OPTICAL: return "optical";
	case JC_ZOOM_TYPE_OPTICAL_DIGITAL: return "optical+digital";
	default: return "unknown";
	}
}

jc_zoom_type_t jc_zoom_type_parse(const char *s)
{
	if (!s || !*s)
		return JC_ZOOM_TYPE_UNKNOWN;
	if (strcmp(s, "none") == 0)
		return JC_ZOOM_TYPE_NONE;
	if (strcmp(s, "digital") == 0)
		return JC_ZOOM_TYPE_DIGITAL;
	if (strcmp(s, "optical") == 0)
		return JC_ZOOM_TYPE_OPTICAL;
	/* accept the composite spellings a node may use for "both" */
	if (strcmp(s, "optical+digital") == 0 || strcmp(s, "optical_digital") == 0 ||
	    strcmp(s, "hybrid") == 0)
		return JC_ZOOM_TYPE_OPTICAL_DIGITAL;
	return JC_ZOOM_TYPE_UNKNOWN;
}

const char *jc_zoom_type_label(jc_zoom_type_t t)
{
	switch (t) {
	case JC_ZOOM_TYPE_NONE: return "no zoom";
	case JC_ZOOM_TYPE_DIGITAL: return "Digital zoom (crop, no lens movement)";
	case JC_ZOOM_TYPE_OPTICAL: return "Optical zoom (drives the lens)";
	case JC_ZOOM_TYPE_OPTICAL_DIGITAL:
		return "Optical + digital zoom (lens, then crop)";
	default: return "zoom type not reported";
	}
}

bool jc_zoom_type_is_optical(jc_zoom_type_t t)
{
	return t == JC_ZOOM_TYPE_OPTICAL || t == JC_ZOOM_TYPE_OPTICAL_DIGITAL;
}

bool jc_zoom_crop_rect(double z, int full_w, int full_h, int *x, int *y, int *w,
		int *h)
{
	if (full_w < 1 || full_h < 1)
		return false;
	if (z < JC_ZOOM_MIN)
		z = JC_ZOOM_MIN;
	if (z > JC_ZOOM_MAX)
		z = JC_ZOOM_MAX;
	int cw = (int)(full_w / z + 0.5);
	int ch = (int)(full_h / z + 0.5);
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;
	if (cw > full_w) cw = full_w;
	if (ch > full_h) ch = full_h;
	if (x) *x = (full_w - cw) / 2;
	if (y) *y = (full_h - ch) / 2;
	if (w) *w = cw;
	if (h) *h = ch;
	return true;
}

void jc_canon_tokens(const char *s, char *out, size_t cap)
{
	size_t o = 0;
	bool first = true;
	const char *p = s;
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';' ||
		       *p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != ';' &&
		       *p != '\n' && *p != '\r')
			p++;
		if (!first && o + 1 < cap)
			out[o++] = ',';
		first = false;
		size_t n = (size_t)(p - start);
		if (o + n >= cap)
			n = cap - 1 - o;
		memcpy(out + o, start, n);
		o += n;
	}
	out[o] = 0;
}

static void fill_array_value(jc_field_t *f, const jnode *v)
{
	f->val_arr_len = 0;
	if (v->type == J_ARR) {
		for (int i = 0; i < v->n && f->val_arr_len < 8; i++) {
			if (v->items[i]->type == J_NUM)
				f->val_arr[f->val_arr_len++] = v->items[i]->num;
		}
	} else if (v->type == J_NUM) {
		f->val_arr[f->val_arr_len++] = v->num;
	}
	/* canonical text for change detection */
	char tmp[128];
	size_t o = 0;
	for (int i = 0; i < f->val_arr_len && o + 24 < sizeof tmp; i++)
		o += (size_t)snprintf(tmp + o, sizeof tmp - o, "%s%.6g",
				      i ? "," : "", f->val_arr[i]);
	tmp[o] = 0;
	snprintf(f->val_text, sizeof f->val_text, "%s", tmp);
	if (v->type == J_STR)
		snprintf(f->val_text, sizeof f->val_text, "%s", v->str);
	f->has_value = true;
}

static void fill_value(jc_field_t *f, const jnode *v)
{
	if (!v)
		return;
	switch (v->type) {
	case J_BOOL:
		f->val_bool = v->boolean != 0;
		f->has_value = true;
		break;
	case J_NUM:
		f->val_num = v->num;
		f->has_value = true;
		if (f->is_array)
			fill_array_value(f, v);
		break;
	case J_STR:
		f->val_num = strtod(v->str, NULL);
		f->has_value = true;
		if (f->is_array)
			fill_array_value(f, v);
		else
			snprintf(f->val_text, sizeof f->val_text, "%s",
				 v->str);
		break;
	case J_ARR:
		if (f->is_array)
			fill_array_value(f, v);
		break;
	default:
		break;
	}
}

bool jc_controls_parse(const char *json, size_t len, jc_controls_t *out)
{
	if (!out)
		return false;
	memset(out, 0, sizeof *out);
	out->hot = true;

	if (!json || len == 0) {
		snprintf(out->error, sizeof out->error, "empty response");
		return false;
	}

	jparser j = {json, json + len, {0}};
	jnode *root = parse_value(&j);
	if (!root) {
		snprintf(out->error, sizeof out->error, "malformed JSON: %s",
			 j.err[0] ? j.err : "parse error");
		return false;
	}
	skip_ws(&j);
	if (root->type != J_OBJ) {
		snprintf(out->error, sizeof out->error,
			 "malformed JSON: root is not an object");
		jnode_free(root);
		return false;
	}

	const jnode *hot = jget(root, "hot");
	if (hot && hot->type == J_BOOL)
		out->hot = hot->boolean != 0;

	/* restart-required keys first, so a control can be flagged when mapped */
	const jnode *rr = jget(root, "restart_required");
	if (rr && rr->type == J_ARR) {
		for (int i = 0; i < rr->n && out->n_restart < 32; i++) {
			const jnode *item = rr->items[i];
			const jnode *k = item && item->type == J_OBJ
						 ? jget(item, "key")
						 : item;
			if (k && k->type == J_STR)
				snprintf(out->restart_keys[out->n_restart++],
					 JC_NAME_MAX, "%s", k->str);
		}
	}
	const jnode *adv = jget(root, "advertised");
	const jnode *cur = jget(root, "current");
	if (!adv || adv->type != J_ARR) {
		snprintf(out->error, sizeof out->error,
			 "malformed JSON: no advertised array");
		jnode_free(root);
		return false;
	}

	for (int i = 0; i < adv->n && out->n_fields < JC_MAX_FIELDS; i++) {
		const jnode *d = adv->items[i];
		if (!d || d->type != J_OBJ)
			continue;
		const jnode *nm = jget(d, "name");
		if (!nm || nm->type != J_STR)
			continue;

		jc_field_t *f = &out->fields[out->n_fields];
		memset(f, 0, sizeof *f);
		snprintf(f->name, sizeof f->name, "%s", nm->str);

		const jnode *ty = jget(d, "type");
		snprintf(f->type, sizeof f->type, "%s",
			 (ty && ty->type == J_STR) ? ty->str : "");

		const jnode *ar = jget(d, "array");
		f->is_array = (ar && ar->type == J_BOOL && ar->boolean) ||
			      (ar && ar->type == J_NUM && ar->num != 0);

		const jnode *enm = jget(d, "enum");
		f->widget = map_widget(f->type, f->is_array, enm);

		const jnode *mn = jget(d, "min");
		const jnode *mx = jget(d, "max");
		if (mn && (mn->type == J_NUM || mn->type == J_BOOL)) {
			f->has_min = true;
			f->fmin = mn->type == J_BOOL ? (mn->boolean ? 1 : 0)
						     : mn->num;
			f->imin = (int)f->fmin;
		}
		if (mx && (mx->type == J_NUM || mx->type == J_BOOL)) {
			f->has_max = true;
			f->fmax = mx->type == J_BOOL ? (mx->boolean ? 1 : 0)
						     : mx->num;
			f->imax = (int)f->fmax;
		}

		if (enm && enm->type == J_OBJ) {
			for (int k = 0; k < enm->n && f->enum_count < JC_MAX_ENUM;
			     k++) {
				jc_enum_t *e = &f->enums[f->enum_count];
				e->value = (int)strtol(enm->keys[k], NULL, 10);
				const jnode *lab = enm->items[k];
				snprintf(e->label, sizeof e->label, "%s",
					 (lab && lab->type == J_STR) ? lab->str
								     : enm->keys[k]);
				f->enum_count++;
			}
		}

		/* ZOOM capability: a non-breaking extra key on the descriptor. An
		 * older node omits it -> has_zoom_type stays false (UNKNOWN), which
		 * the pane must render as "not reported", never as "digital". */
		const jnode *zt = jget(d, JC_ZOOM_TYPE_KEY);
		if (zt && zt->type == J_STR) {
			f->has_zoom_type = true;
			f->zoom_type = jc_zoom_type_parse(zt->str);
			const jnode *zs = jget(d, JC_ZOOM_TYPE_SOURCE_KEY);
			if (zs && zs->type == J_STR)
				snprintf(f->zoom_type_source,
					 sizeof f->zoom_type_source, "%s", zs->str);
		}

		for (int k = 0; k < out->n_restart; k++) {
			if (strcmp(out->restart_keys[k], f->name) == 0)
				f->restart_required = true;
		}

		/* applied value: node's current echo, else the descriptor default */
		const jnode *v = cur ? jget(cur, f->name) : NULL;
		if (!v)
			v = jget(d, "default");
		fill_value(f, v);

		out->n_fields++;
	}

	int n_hot = 0;
	for (int i = 0; i < out->n_fields; i++)
		if (!out->fields[i].restart_required)
			n_hot++;

	if (!out->hot) {
		snprintf(out->status, sizeof out->status,
			 "%d controls advertised, node reports them NOT hot "
			 "(read-only); %d restart-required keys",
			 out->n_fields, out->n_restart);
	} else if (out->n_restart > 0) {
		snprintf(out->status, sizeof out->status,
			 "%d controls advertised (%d hot); %d restart-required "
			 "node settings are not editable here",
			 out->n_fields, n_hot, out->n_restart);
	} else {
		snprintf(out->status, sizeof out->status,
			 "%d controls advertised, all hot", out->n_fields);
	}
	jnode_free(root);
	/* The parse succeeded: this is the flag the properties pane keys on. It
	 * was never set, so a perfectly good fetch still rendered as "unavailable"
	 * and apply_controls() refused to POST anything. */
	out->ok = true;
	return true;
}

bool jc_controls_fetch(const char *base_url, jc_controls_t *out)
{
	if (!out)
		return false;
	memset(out, 0, sizeof *out);
	snprintf(out->base_url, sizeof out->base_url, "%s",
		 base_url ? base_url : "");

	char *buf = malloc(JC_IO_MAX);
	if (!buf) {
		snprintf(out->error, sizeof out->error, "out of memory");
		return false;
	}
	size_t n = 0;
	char err[JC_ERR_MAX] = {0};
	int rc = jc_http_get(base_url, "/api/controls", buf, JC_IO_MAX, &n,
			     err, sizeof err);
	if (rc != 0) {
		snprintf(out->error, sizeof out->error, "controls unavailable: %s",
			 err[0] ? err : "unreachable");
		free(buf);
		return false;
	}
	bool ok = jc_controls_parse(buf, n, out);
	free(buf);
	if (!ok) {
		char e[JC_ERR_MAX];
		snprintf(e, sizeof e, "%s", out->error);
		snprintf(out->error, sizeof out->error,
			 "controls unavailable: %.200s", e);
	}
	return ok;
}

bool jc_controls_post(const char *base_url, const char *json_body,
		      char *resp, size_t cap, char *err, size_t errcap)
{
	int st = jc_http_post_json(base_url, "/api/controls", json_body, resp,
				   cap, NULL, err, errcap);
	if (st < 0)
		return false;
	if (st < 200 || st >= 300) {
		snprintf(err, errcap, "HTTP %d: %.180s", st, resp ? resp : "");
		return false;
	}
	return true;
}

bool jc_node_set_destination(const char *base_url, const char *dest,
			     const char *port, char *resp, size_t cap,
			     char *err, size_t errcap)
{
	if (err && errcap)
		err[0] = 0;
	if (!base_url || !*base_url) {
		snprintf(err, errcap, "no control API URL");
		return false;
	}
	if (!dest || !*dest) {
		snprintf(err, errcap, "no destination address");
		return false;
	}
	/* The node's /api/config parses the port ONLY from inside the JXS_DEST
	 * string ("host:port", "udp://host:port", or a bare host meaning the
	 * current/default port); the separate JXS_PORT field is ignored. Sending
	 * the fields apart looked like success but silently kept the camera's old
	 * port (measured: the node echoed dest back with the PREVIOUS port). So
	 * the port always goes embedded. Callers pass a bare host (this recorder
	 * and the OBS plugin both derive it); a pre-built host:port/udp:// URL is
	 * passed through untouched. */
	bool dest_has_port = strchr(dest, ':') != NULL;
	const char *use_port = (port && *port) ? port : "5000";
	char body[512];
	if (dest_has_port)
		snprintf(body, sizeof body,
			 "{\"JXS_DEST\":\"%s\",\"JXS_PORT\":\"%s\","
			 "\"JXS_THREADS\":\"\",\"JXS_SLICE_HEIGHT\":\"\","
			 "\"JXS_CAM_BUFFERS\":\"\",\"JXS_IFACE\":\"\"}",
			 dest, use_port);
	else
		snprintf(body, sizeof body,
			 "{\"JXS_DEST\":\"%s:%s\",\"JXS_PORT\":\"%s\","
			 "\"JXS_THREADS\":\"\",\"JXS_SLICE_HEIGHT\":\"\","
			 "\"JXS_CAM_BUFFERS\":\"\",\"JXS_IFACE\":\"\"}",
			 dest, use_port, use_port);
int st = jc_http_post_json(base_url, "/api/config", body, resp, cap, NULL,
				   err, errcap);
	if (st < 0)
		return false;
	if (st < 200 || st >= 300) {
		snprintf(err, errcap, "HTTP %d: %.160s", st, resp ? resp : "");
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* multicast group: base block + per-camera index                       */
/* ------------------------------------------------------------------ */

/* Parse a "a.b.c" prefix into its three octets. NULL/"" means the default.
 * Returns true only when it is EXACTLY three dot-separated octets. */
static bool group_base_octets(const char *base, unsigned *a, unsigned *b,
			      unsigned *c)
{
	if (!base || !*base)
		base = JC_GROUP_BASE_DEFAULT;
	int n = 0;
	if (sscanf(base, "%u.%u.%u%n", a, b, c, &n) != 3)
		return false;
	return base[n] == '\0';
}

bool jc_group_base_valid(const char *base)
{
	if (!base || !*base)
		return true; /* empty == use the default */
	unsigned a, b, c;
	if (!group_base_octets(base, &a, &b, &c))
		return false;
	/* Administratively-scoped multicast range used on this rig. */
	return a >= 224 && a <= 239 && b <= 255 && c <= 255;
}

bool jc_group_addr(const char *base, int idx, char *out, size_t cap)
{
	unsigned a, b, c;
	if (!group_base_octets(base, &a, &b, &c))
		return false;
	if (idx < JC_GROUP_INDEX_MIN)
		idx = JC_GROUP_INDEX_MIN;
	if (idx > JC_GROUP_INDEX_MAX)
		idx = JC_GROUP_INDEX_MAX;
	int w = snprintf(out, cap, "%u.%u.%u.%d", a, b, c, idx);
	return w > 0 && (size_t)w < cap;
}

/*
 * The node's /api/status carries the stream destination it is ACTUALLY using
 * under node.dest (e.g. "239.1.1.1" when in multicast, or a unicast host like
 * "10.50.1.51" when not). We care only about a group inside the `base` block.
 */
bool jc_node_status_group_index(const char *base_url, const char *base,
				int *out_index)
{
	unsigned ga, gb, gc;
	if (!group_base_octets(base, &ga, &gb, &gc))
		return false;
	char *buf = malloc(JC_IO_MAX);
	if (!buf)
		return false;
	size_t n = 0;
	char err[JC_ERR_MAX] = {0};
	int rc = jc_http_get(base_url, "/api/status", buf, JC_IO_MAX, &n, err,
			     sizeof err);
	if (rc != 0) {
		free(buf);
		return false;
	}
	bool ok = false;
	jparser j = {buf, buf + n, {0}};
	jnode *root = parse_value(&j);
	if (root) {
		const jnode *node = jget(root, "node");
		const jnode *dest =
			node ? jget(node, "dest") : jget(root, "dest");
		if (dest && dest->type == J_STR) {
			unsigned a, b, c, d;
			if (sscanf(dest->str, "%u.%u.%u.%u", &a, &b, &c, &d) ==
			    4 &&
			    a == ga && b == gb && c == gc &&
			    d >= JC_GROUP_INDEX_MIN && d <= JC_GROUP_INDEX_MAX) {
				if (out_index)
					*out_index = (int)d;
				ok = true;
			}
		}
		jnode_free(root);
	}
	free(buf);
	return ok;
}

/*
 * Read base_url/api/status (READ-ONLY) and report whether the node's stream
 * carries the SMPTE 302M audio elementary stream (node.stream.audio.enabled).
 * A camera without audio (or one that is unreachable, or whose status lacks the
 * audio block) reports *enabled = 0. This is what lets a consumer declare the
 * audio ES for a camera ONLY when the stream actually has one - the default
 * "assume audio" was what wired a silent audiomixer input into the preview and
 * wedged the whole pipeline. Bounded like every jc_http_get.
 */
bool jc_node_status_audio(const char *base_url, bool *enabled)
{
	if (enabled)
		*enabled = 0;
	char *buf = malloc(JC_IO_MAX);
	if (!buf)
		return false;
	size_t n = 0;
	char err[JC_ERR_MAX] = {0};
	int rc = jc_http_get(base_url, "/api/status", buf, JC_IO_MAX, &n, err,
			     sizeof err);
	if (rc != 0) {
		free(buf);
		return false;
	}
	bool ok = false;
	jparser j = {buf, buf + n, {0}};
	jnode *root = parse_value(&j);
	if (root) {
		/* The phone reports audio at TOP LEVEL (measured live): the status
		 * carries "stream":{"audio":{"enabled":1,...}} AND a top-level
		 * "audio":{"enabled":true,...}. It is NOT under "node" - walking
		 * node->stream returned nothing and every camera looked absent.
		 * Check stream.audio.enabled first (numeric), then the plain
		 * audio.enabled (boolean). The Pi reports neither. */
		const jnode *stream = jget(root, "stream");
		const jnode *sa = stream ? jget(stream, "audio") : NULL;
		const jnode *en = sa ? jget(sa, "enabled") : NULL;
		if (!en) {
			const jnode *audio = jget(root, "audio");
			en = audio ? jget(audio, "enabled") : NULL;
		}
		if (en && en->type == J_NUM) {
			if (enabled)
				*enabled = (en->num != 0);
			ok = true;
		} else if (en && en->type == J_BOOL) {
			if (enabled)
				*enabled = (en->boolean != 0);
			ok = true;
		} else if (sa || jget(root, "audio")) {
			/* an audio block exists but carries no "enabled" field:
			 * treat presence of the block as "has audio" */
			if (enabled)
				*enabled = 1;
			ok = true;
		}
		jnode_free(root);
	}
	free(buf);
	return ok;
}

int jc_autopick_group_index(const char *base, const jc_cam_endpoint_t *cams,
			    int n_cams, const char *self_instance, bool occ[255],
			    int *out_index)
{
	bool local_occ[255];
	if (!occ)
		occ = local_occ;
	for (int i = 0; i < 255; i++)
		occ[i] = false;

	int self = 0;       /* the group our own camera already reports */
	int n_reported = 0; /* cameras that reported a group in the base block */

	for (int i = 0; i < n_cams; i++) {
		const jc_cam_endpoint_t *c = &cams[i];
		if (!c->address[0])
			continue;
		char url[256];
		snprintf(url, sizeof url, "http://%s:%s", c->address,
			 c->api_port[0] ? c->api_port : "8080");
		int idx = 0;
		if (!jc_node_status_group_index(url, base, &idx))
			continue;
		n_reported++;
		if (idx >= JC_GROUP_INDEX_MIN && idx <= JC_GROUP_INDEX_MAX)
			occ[idx] = true;
		if (self_instance && *self_instance &&
		    strcmp(c->instance, self_instance) == 0)
			self = idx;
	}

	int pick = JC_GROUP_INDEX_DEFAULT;
	if (self >= JC_GROUP_INDEX_MIN && self <= JC_GROUP_INDEX_MAX) {
		/* Stability: keep the group this camera already uses. */
		pick = self;
	} else {
		/* Lowest free index; with nothing occupied this is index 1. */
		for (int i = JC_GROUP_INDEX_MIN; i <= JC_GROUP_INDEX_MAX; i++) {
			if (!occ[i]) {
				pick = i;
				break;
			}
		}
	}
	if (out_index)
		*out_index = pick;
	return n_reported;
}

/* ------------------------------------------------------------------ */
/* Image match                                                         */
/* ------------------------------------------------------------------ */

bool jc_luma_from_plane(jc_luma_t *out, const unsigned char *plane, int w,
			int h, int stride)
{
	if (!out || !plane || w <= 0 || h <= 0)
		return false;
	memset(out, 0, sizeof(*out));
	for (int y = 0; y < h; y++) {
		const unsigned char *row = plane + (size_t)y * stride;

		for (int x = 0; x < w; x++)
			out->hist[row[x]] += 1.0;
	}
	out->count = w * h;
	double sum = 0.0;
	for (int i = 0; i < JC_MATCH_BINS; i++) {
		sum += out->hist[i] * i;
	}
	out->mean = sum / out->count;
	double var = 0.0;
	for (int i = 0; i < JC_MATCH_BINS; i++) {
		var += out->hist[i] * (i - out->mean) * (i - out->mean);
	}
	out->std = sqrt(var / out->count);
	static const int pct[JC_MATCH_PCTS] = {1, 5, 10, 25, 50, 75, 90, 95, 99};

	for (int k = 0; k < JC_MATCH_PCTS; k++) {
		double acc = 0.0;
		int i;

		for (i = 0; i < JC_MATCH_BINS; i++) {
			acc += out->hist[i] / out->count;
			if (acc >= pct[k] / 100.0)
				break;
		}
		out->pct[k] = (i >= JC_MATCH_BINS) ? 255.0 : (double)i;
	}
	for (int i = 0; i < JC_MATCH_BINS; i++)
		out->hist[i] /= out->count;
	return true;
}

double jc_match_score(const jc_luma_t *a, const jc_luma_t *b, bool absolute)
{
	if (!a || !b || a->count <= 0 || b->count <= 0)
		return -1.0;
	double lo_a = 0.0, lo_b = 0.0, span_a = 255.0, span_b = 255.0;

	if (!absolute) {
		lo_a = a->pct[0];
		lo_b = b->pct[0];
		span_a = a->pct[JC_MATCH_PCTS - 1] - lo_a;
		span_b = b->pct[JC_MATCH_PCTS - 1] - lo_b;
		if (span_a < 1e-6)
			span_a = 1.0;
		if (span_b < 1e-6)
			span_b = 1.0;
	}
	double acc = 0.0;

	for (int k = 0; k < JC_MATCH_PCTS; k++) {
		double va = (a->pct[k] - lo_a) / span_a * 255.0;
		double vb = (b->pct[k] - lo_b) / span_b * 255.0;

		acc += (va - vb) * (va - vb);
	}
	return sqrt(acc / JC_MATCH_PCTS);
}

void jc_match_curve(jc_curve_t *out, double gamma, double contrast)
{
	static const double xs[JC_MATCH_POINTS] = {0.0, 0.125, 0.25, 0.5, 0.75, 1.0};

	if (!out)
		return;
	if (!(gamma > 0.05))
		gamma = 0.05;
	if (contrast < 0.0)
		contrast = 0.0;
	if (contrast > 1.0)
		contrast = 1.0;
	out->n = JC_MATCH_POINTS;
	for (int i = 0; i < JC_MATCH_POINTS; i++) {
		double e = pow(xs[i], gamma);
		double s = 3.0 * e * e - 2.0 * e * e * e;

		out->x[i] = xs[i];
		out->y[i] = (1.0 - contrast) * e + contrast * s;
	}
}

int jc_match_curve_points(const jc_curve_t *c, double *xy, int max_pairs)
{
	if (!c || !xy || c->n <= 0 || c->n > max_pairs)
		return -1;
	for (int i = 0; i < c->n; i++) {
		xy[2 * i] = c->x[i];
		xy[2 * i + 1] = c->y[i];
	}
	return c->n;
}

/* Measured candidates. 0.45 gamma is the display encode and the natural
 * reference; the rest bracket it, and the contrast variants sit on it. */
static const struct { double gamma, contrast; } jc_match_plan[JC_MATCH_PLAN] = {
	{0.45, 0.00}, {0.35, 0.00}, {0.55, 0.00}, {0.70, 0.00}, {0.30, 0.00},
	{0.45, 0.25}, {0.45, 0.50}, {0.40, 0.25}, {0.50, 0.25},
};

void jc_match_begin(jc_match_t *s)
{
	if (!s)
		return;
	memset(s, 0, sizeof(*s));
	s->i = -1;
	s->best_score = -1.0;
	s->gamma = jc_match_plan[0].gamma;
	s->contrast = jc_match_plan[0].contrast;
	s->best_gamma = s->gamma;
	s->best_contrast = s->contrast;
}

bool jc_match_next(jc_match_t *s, double score, jc_curve_t *out)
{
	if (!s || s->done)
		return false;
	if (s->i >= 0 && score >= 0.0 &&
	    (s->best_score < 0.0 || score < s->best_score)) {
		s->best_score = score;
		s->best_gamma = s->gamma;
		s->best_contrast = s->contrast;
	}
	s->i++;
	if (s->i >= JC_MATCH_PLAN) {
		s->done = true;
		return false;
	}
	s->gamma = jc_match_plan[s->i].gamma;
	s->contrast = jc_match_plan[s->i].contrast;
	if (out)
		jc_match_curve(out, s->gamma, s->contrast);
	return true;
}

void jc_match_best(const jc_match_t *s, jc_curve_t *out)
{
	if (!s || !out)
		return;
	jc_match_curve(out, s->best_gamma, s->best_contrast);
}

bool jc_match_apply(const char *base_url, const jc_curve_t *c, char *err,
		    size_t errcap)
{
	double xy[2 * JC_MATCH_POINTS];
	int n;
	char body[512];
	size_t o = 0;

	if (!base_url || !c)
		return false;
	n = jc_match_curve_points(c, xy, JC_MATCH_POINTS);
	if (n <= 0) {
		if (err && errcap)
			snprintf(err, errcap, "curve does not fit the control");
		return false;
	}
	o += (size_t)snprintf(body + o, sizeof(body) - o,
			      "{\"TonemapMode\":0,\"TonemapCurvePoints\":[");
	for (int i = 0; i < n && o < sizeof(body) - 32; i++)
		o += (size_t)snprintf(body + o, sizeof(body) - o, "%s%.5f,%.5f",
				      i ? "," : "", xy[2 * i], xy[2 * i + 1]);
	snprintf(body + o, sizeof(body) - o, "]}");
	char resp[256];

	return jc_controls_post(base_url, body, resp, sizeof(resp), err, errcap);
}
