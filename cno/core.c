#include <ctype.h>
#include <stdio.h>

#include "core.h"
#include "../picohttpparser/picohttpparser.h"

enum CNO_CONNECTION_STATE {
    CNO_STATE_CLOSED,
    CNO_STATE_H2_INIT,
    CNO_STATE_H2_PREFACE,
    CNO_STATE_H2_SETTINGS,
    CNO_STATE_H2_FRAME,
    CNO_STATE_H1_HEAD,
    CNO_STATE_H1_BODY,
    CNO_STATE_H1_TAIL,
    CNO_STATE_H1_CHUNK,
    CNO_STATE_H1_CHUNK_BODY,
    CNO_STATE_H1_CHUNK_TAIL,
    CNO_STATE_H1_TRAILERS,
};

enum CNO_STREAM_STATE {
    CNO_STREAM_HEADERS,
    CNO_STREAM_DATA,
    CNO_STREAM_CLOSED,
};

struct cno_stream_t {
    struct cno_stream_t *next; // in hashmap bucket
    uint32_t id;
    uint8_t /* enum CNO_STREAM_STATE */ r_state;
    uint8_t /* enum CNO_STREAM_STATE */ w_state;
    uint8_t writing_chunked : 1;
    uint8_t reading_head_response : 1;
     int64_t window_recv;
     int64_t window_send;
    uint64_t remaining_payload;
};

static inline uint32_t read4(const void *v) {
    const uint8_t *p = (const uint8_t *)v;
    return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

#define PACK(...) ((struct cno_buffer_t) { (char *) (uint8_t []) { __VA_ARGS__ }, sizeof((uint8_t []) { __VA_ARGS__ }) })
#define I8(x)  (x)
#define I16(x) (x) >> 8,  (x)
#define I24(x) (x) >> 16, (x) >> 8,  (x)
#define I32(x) (x) >> 24, (x) >> 16, (x) >> 8, (x)

#define CNO_FIRE(ob, cb, ...) (ob->cb_code && ob->cb_code->cb && ob->cb_code->cb(ob->cb_data, ##__VA_ARGS__))

#define CNO_WRITEV(c, ...) CNO_FIRE(c, on_writev, (struct cno_buffer_t[]){__VA_ARGS__}, \
    sizeof((struct cno_buffer_t[]){__VA_ARGS__}) / sizeof(struct cno_buffer_t))

// Fake http "request" sent by the client at the beginning of a connection.
static const struct cno_buffer_t CNO_PREFACE = { "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24 };

// Standard-defined pre-initial-SETTINGS values
static const struct cno_settings_t CNO_SETTINGS_STANDARD = {{{
    .header_table_size      = 4096,
    .enable_push            = 1,
    .max_concurrent_streams = -1,
    .initial_window_size    = 65535,
    .max_frame_size         = 16384,
    .max_header_list_size   = -1,
}}};

// A somewhat more conservative version assumed to be used by the remote side at first.
// (In case we want to send some frames before ACK-ing the remote settings, but don't want to get told.)
static const struct cno_settings_t CNO_SETTINGS_CONSERVATIVE = {{{
    .header_table_size      = 4096,
    .enable_push            = 0,
    .max_concurrent_streams = 100,
    .initial_window_size    = 65535,
    .max_frame_size         = 16384,
    .max_header_list_size   = -1,
}}};

// Actual values to send in the first SETTINGS frame.
static const struct cno_settings_t CNO_SETTINGS_INITIAL = {{{
    .header_table_size      = 4096,
    .enable_push            = 1,
    .max_concurrent_streams = 1024,
    .initial_window_size    = 65535,
    .max_frame_size         = 16384,
    .max_header_list_size   = -1, // actually (CNO_MAX_CONTINUATIONS * max_frame_size - 32 * CNO_MAX_HEADERS)
}}};

static int cno_stream_is_local(const struct cno_connection_t *c, uint32_t sid) {
    return sid % 2 == c->client;
}

static struct cno_stream_t * cno_stream_new(struct cno_connection_t *c, uint32_t sid, int local) {
    if (cno_stream_is_local(c, sid) != local)
        return (local ? CNO_ERROR(INVALID_STREAM, "incorrect stream id parity")
                      : CNO_ERROR(PROTOCOL, "incorrect stream id parity")), NULL;

    if (sid <= c->last_stream[local])
        return (local ? CNO_ERROR(INVALID_STREAM, "nonmonotonic stream id")
                      : CNO_ERROR(PROTOCOL, "nonmonotonic stream id")), NULL;

    // TODO h1 pipelining (need to select stream with least id in cno_when_h1_*)
    if (c->stream_count[local] >= (c->mode == CNO_HTTP2 ? c->settings[!local].max_concurrent_streams : 1))
        return (local ? CNO_ERROR(WOULD_BLOCK, "wait for on_stream_end")
                      : CNO_ERROR(PROTOCOL, "peer exceeded stream limit")), NULL;

    struct cno_stream_t *s = malloc(sizeof(struct cno_stream_t));
    if (!s)
        return CNO_ERROR(NO_MEMORY, "%zu bytes", sizeof(struct cno_stream_t)), NULL;
    *s = (struct cno_stream_t) {
        .id     = c->last_stream[local] = sid,
        .next   = c->streams[sid % CNO_STREAM_BUCKETS],
        .r_state = sid % 2 || !local ? CNO_STREAM_HEADERS : CNO_STREAM_CLOSED,
        .w_state = sid % 2 ||  local ? CNO_STREAM_HEADERS : CNO_STREAM_CLOSED,
    };

    // Gotta love C for not having any standard library to speak of.
    c->streams[sid % CNO_STREAM_BUCKETS] = s;
    c->stream_count[local]++;

    if (CNO_FIRE(c, on_stream_start, sid)) {
        c->streams[sid % CNO_STREAM_BUCKETS] = s->next;
        c->stream_count[local]--;
        free(s);
        return (void)CNO_ERROR_UP(), NULL;
    }
    return s;
}

static struct cno_stream_t *cno_stream_find(const struct cno_connection_t *c, uint32_t sid) {
    struct cno_stream_t *s = c->streams[sid % CNO_STREAM_BUCKETS];
    while (s && s->id != sid) s = s->next;
    return s;
}

static int cno_stream_end(struct cno_connection_t *c, struct cno_stream_t *s) {
    uint32_t sid = s->id;
    struct cno_stream_t **sp = &c->streams[sid % CNO_STREAM_BUCKETS];
    while (*sp != s) sp = &(*sp)->next;
    *sp = s->next;
    free(s);
    c->stream_count[cno_stream_is_local(c, sid)]--;
    return CNO_FIRE(c, on_stream_end, sid);
}

static int cno_stream_end_by_local(struct cno_connection_t *c, struct cno_stream_t *s) {
    // HEADERS, DATA, WINDOW_UPDATE, and RST_STREAM may arrive on streams we have already reset
    // simply because the other side sent the frames before receiving ours. This is not
    // a protocol error according to the standard. (FIXME kinda broken with trailers...)
    if (s->r_state != CNO_STREAM_CLOSED) {
        // Very convenient that this bit is reserved. (For what, we shall never know.)
        uint32_t is_headers = (s->r_state == CNO_STREAM_HEADERS);
        c->recently_reset[c->recently_reset_next++] = s->id | is_headers << 31;
        c->recently_reset_next %= CNO_STREAM_RESET_HISTORY;
    }
    return cno_stream_end(c, s);
}

// Send a single non-flow-controlled* frame, splitting DATA/HEADERS if they are too big.
// (*meaning that it isn't counted; in case of DATA, this must be done by the caller.)
static int cno_frame_write(struct cno_connection_t *c, const struct cno_frame_t *f) {
    size_t length = f->payload.size;
    size_t limit  = c->settings[CNO_REMOTE].max_frame_size;

    if (length <= limit)
        return CNO_WRITEV(c, PACK(I24(length), I8(f->type), I8(f->flags), I32(f->stream)), f->payload);

    if (f->type != CNO_FRAME_HEADERS && f->type != CNO_FRAME_PUSH_PROMISE && f->type != CNO_FRAME_DATA)
        // A really unexpected outcome, considering that the *lowest possible* limit is 16 KiB.
        return CNO_ERROR(ASSERTION, "control frame too big");

    if (f->flags & CNO_FLAG_PADDED)
        // TODO split padded frames.
        return CNO_ERROR(NOT_IMPLEMENTED, "don't know how to split padded frames");

    struct cno_frame_t part = *f;
    // When splitting HEADERS/PUSH_PROMISE, the last CONTINUATION must carry the END_HEADERS flag,
    // but the HEADERS frame itself retains END_STREAM if set. When splitting DATA,
    // END_STREAM must be moved to the last frame in the sequence.
    uint8_t carry = f->flags & (f->type == CNO_FRAME_DATA ? CNO_FLAG_END_STREAM : CNO_FLAG_END_HEADERS);
    part.flags &= ~carry;

    for (part.payload.size = limit; length > limit; length -= limit, part.payload.data += limit) {
        if (cno_frame_write(c, &part))
            return CNO_ERROR_UP();
        if (part.type != CNO_FRAME_DATA)
            part.type = CNO_FRAME_CONTINUATION;
        part.flags &= ~(CNO_FLAG_PRIORITY | CNO_FLAG_END_STREAM);
    }

    part.flags |= carry;
    part.payload.size = length;
    return cno_frame_write(c, &part);
}

static int cno_frame_write_goaway(struct cno_connection_t *c, uint32_t /* enum CNO_RST_STREAM_CODE */ code) {
    if (!c->goaway_sent)
        c->goaway_sent = c->last_stream[CNO_REMOTE];
    struct cno_frame_t error = { CNO_FRAME_GOAWAY, 0, 0, PACK(I32(c->goaway_sent), I32(code)) };
    return cno_frame_write(c, &error);
}

// Shut down a connection and *then* throw a PROTOCOL error.
#define cno_frame_write_error(c, code, ...) \
    (cno_frame_write_goaway(c, code) ? CNO_ERROR_UP() : CNO_ERROR(PROTOCOL, __VA_ARGS__))

// Ignore frames on reset streams, as the spec requires. See `cno_stream_end_by_local`.
static int cno_frame_handle_invalid_stream(struct cno_connection_t *c, struct cno_frame_t *f) {
    if (f->stream && f->stream <= c->last_stream[cno_stream_is_local(c, f->stream)])
        for (uint8_t i = 0; i < CNO_STREAM_RESET_HISTORY; i++)
            if ((f->type != CNO_FRAME_HEADERS && c->recently_reset[i] == f->stream)
             || (f->type != CNO_FRAME_DATA && c->recently_reset[i] == (f->stream | (1ul << 31))))
                return CNO_OK;
    return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "invalid stream");
}

// Send a delta between two configs as a SETTINGS frame.
static int cno_frame_write_settings(struct cno_connection_t *c,
                              const struct cno_settings_t   *old,
                              const struct cno_settings_t   *new)
{
    uint8_t payload[CNO_SETTINGS_UNDEFINED - 1][6];
    uint8_t (*ptr)[6] = payload;
    for (size_t i = 0; i + 1 < CNO_SETTINGS_UNDEFINED; i++) {
        if (old->array[i] == new->array[i])
            continue;
        struct cno_buffer_t buf = PACK(I16(i + 1), I32(new->array[i]));
        memcpy(ptr++, buf.data, buf.size);
    }
    struct cno_frame_t f = { CNO_FRAME_SETTINGS, 0, 0, { (char *) payload, (ptr - payload) * 6 } };
    return cno_frame_write(c, &f);
}

static int cno_frame_write_rst_stream_by_id(struct cno_connection_t *c, uint32_t sid, uint32_t code) {
    struct cno_frame_t error = { CNO_FRAME_RST_STREAM, 0, sid, PACK(I32(code)) };
    return cno_frame_write(c, &error);
}

static int cno_frame_write_rst_stream(struct cno_connection_t *c,
                                      struct cno_stream_t     *s,
                                      uint32_t /* enum CNO_RST_STREAM_CODE */ code)
{
    // Note that if HEADERS have not yet arrived, they may still do, in which case not decoding them
    // would break compression state. Setting CNO_RESET_STREAM_HISTORY is recommended.
    return cno_frame_write_rst_stream_by_id(c, s->id, code) ? CNO_ERROR_UP() : cno_stream_end_by_local(c, s);
}

static int cno_frame_handle_end_stream(struct cno_connection_t *c,
                                       struct cno_stream_t     *s,
                                       struct cno_message_t    *trailers)
{
    if (!s->reading_head_response && s->remaining_payload && s->remaining_payload != (uint64_t) -1)
        return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);
    if (CNO_FIRE(c, on_message_tail, s->id, trailers))
        return CNO_ERROR_UP();
    s->r_state = CNO_STREAM_CLOSED;
    return s->w_state == CNO_STREAM_CLOSED ? cno_stream_end(c, s) : CNO_OK;
}

static int cno_is_informational(int code) {
    return 100 <= code && code < 200;
}

static uint64_t cno_parse_uint(struct cno_buffer_t value) {
    uint64_t prev = 0, ret = 0;
    for (const char *ptr = value.data, *end = ptr + value.size; ptr != end; ptr++, prev = ret)
        if (*ptr < '0' || '9' < *ptr || (ret = prev * 10 + (*ptr - '0')) < prev)
            return (uint64_t) -1;
    return ret;
}

static const char CNO_HEADER_TRANSFORM[] =
    "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0!\0#$%&'\0\0*+\0-.\0"
    "0123456789\0\0\0\0\0\0\0abcdefghijklmnopqrstuvwxyz\0\0\0^_`abcdefghijklmnopqrstuvwxyz"
    "\0|\0~\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
    "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
    "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

static int cno_frame_handle_message(struct cno_connection_t *c,
                                    struct cno_stream_t     *s,
                                    struct cno_frame_t      *f,
                                    struct cno_message_t    *m)
{
    int is_response = c->client && f->type != CNO_FRAME_PUSH_PROMISE;

    struct cno_header_t *it  = m->headers;
    struct cno_header_t *end = m->headers + m->headers_len;

    // >HTTP/2 uses special pseudo-header fields beginning with ':' character
    // >(ASCII 0x3a) [to convey the target URI, ...]
    for (; it != end && cno_buffer_startswith(it->name, CNO_BUFFER_STRING(":")); it++)
        // >Pseudo-header fields MUST NOT appear in trailers.
        if (s->r_state != CNO_STREAM_HEADERS)
            return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);

    struct cno_header_t *first_non_pseudo = it;
    // Pseudo-headers are checked in reverse order because those that are used to fill fields
    // of `cno_message_t` are then erased, and shifting the two remaining headers up is cheaper
    // than moving all the normal headers down.
    int has_scheme = 0;
    int has_authority = 0;
    for (struct cno_header_t *h = it; h-- != m->headers;) {
        if (is_response) {
            if (cno_buffer_eq(h->name, CNO_BUFFER_STRING(":status")) && !m->code) {
                if ((m->code = cno_parse_uint(h->value)) > 0xFFFF) // kind of an arbitrary limit, really
                    return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);
                continue;
            }
        } else {
            if (cno_buffer_eq(h->name, CNO_BUFFER_STRING(":path")) && !m->path.data) {
                m->path = h->value;
                continue;
            }

            if (cno_buffer_eq(h->name, CNO_BUFFER_STRING(":method")) && !m->method.data) {
                m->method = h->value;
                continue;
            }

            if (cno_buffer_eq(h->name, CNO_BUFFER_STRING(":authority")) && !has_authority) {
                has_authority = 1;
                struct cno_header_t tmp = *--it;
                *it = *h;
                *h = tmp;
                continue;
            }

            if (cno_buffer_eq(h->name, CNO_BUFFER_STRING(":scheme")) && !has_scheme) {
                has_scheme = 1;
                struct cno_header_t tmp = *--it;
                *it = *h;
                *h = tmp;
                continue;
            }
        }

        // >Endpoints MUST NOT generate pseudo-header fields other than those defined in this document.
        return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);
    }

    m->headers = it;
    m->headers_len = end - it;

    s->remaining_payload = (uint64_t) -1;
    for (it = first_non_pseudo; it != end; ++it) {
        // >All pseudo-header fields MUST appear in the header block before regular
        // >header fields. [...] However, header field names MUST be converted
        // >to lowercase prior to their encoding in HTTP/2.
        for (uint8_t *p = (uint8_t *) it->name.data, *e = p + it->name.size; p != e; p++)
            if (CNO_HEADER_TRANSFORM[*p] != *p) // this also rejects invalid symbols, incl. `:`
                return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);

        // >HTTP/2 does not use the Connection header field to indicate
        // >connection-specific header fields.
        if (cno_buffer_eq(it->name, CNO_BUFFER_STRING("connection")))
            return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);

        // >The only exception to this is the TE header field, which MAY be present
        // > in an HTTP/2 request; when it is, it MUST NOT contain any value other than "trailers".
        if (cno_buffer_eq(it->name, CNO_BUFFER_STRING("te"))
        && !cno_buffer_eq(it->value, CNO_BUFFER_STRING("trailers")))
            return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);

        if (cno_buffer_eq(it->name, CNO_BUFFER_STRING("content-length")))
            if ((s->remaining_payload = cno_parse_uint(it->value)) == (uint64_t) -1)
                return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);
    }

    if (s->r_state != CNO_STREAM_HEADERS)
        // Already checked for `CNO_FLAG_END_STREAM` in `cno_frame_handle_headers`.
        return cno_frame_handle_end_stream(c, s, m);

    // >All HTTP/2 requests MUST include exactly one valid value for the :method, :scheme,
    // >and :path pseudo-header fields, unless it is a CONNECT request (Section 8.3).
    if (is_response ? !m->code : !cno_buffer_eq(m->method, CNO_BUFFER_STRING("CONNECT")) &&
            (!m->path.size || !m->method.size || !has_scheme))
        return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);

    if (f->type == CNO_FRAME_PUSH_PROMISE)
        return CNO_FIRE(c, on_message_push, s->id, m, f->stream);

    if (!cno_is_informational(m->code))
        s->r_state = CNO_STREAM_DATA;
    else if (f->flags & CNO_FLAG_END_STREAM || s->remaining_payload != (uint64_t) -1)
        return cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR);

    if (CNO_FIRE(c, on_message_head, s->id, m))
        return CNO_ERROR_UP();

    if (f->flags & CNO_FLAG_END_STREAM)
        return cno_frame_handle_end_stream(c, s, NULL);

    return CNO_OK;
}

static int cno_frame_handle_end_headers(struct cno_connection_t *c,
                                        struct cno_stream_t     *s,
                                        struct cno_frame_t      *f)
{
    if (!(f->flags & CNO_FLAG_END_HEADERS))
        return CNO_ERROR(ASSERTION, "HEADERS/PUSH_PROMISE not merged with CONTINUATION");

    struct cno_header_t headers[CNO_MAX_HEADERS];
    struct cno_message_t m = { 0, {}, {}, headers, CNO_MAX_HEADERS };
    if (cno_hpack_decode(&c->decoder, f->payload, headers, &m.headers_len)) {
        cno_frame_write_goaway(c, CNO_RST_COMPRESSION_ERROR);
        return CNO_ERROR_UP();
    }

    const size_t nheaders = m.headers_len;
    // Just ignore the message if the stream has already been reset.
    int ret = s ? cno_frame_handle_message(c, s, f, &m) : CNO_OK;
    for (size_t i = 0; i < nheaders; i++)
        cno_hpack_free_header(&headers[i]);
    return ret;
}

static int cno_frame_handle_padding(struct cno_connection_t *c, struct cno_frame_t *f) {
    if (f->flags & CNO_FLAG_PADDED) {
        if (f->payload.size == 0)
            return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "no padding found");

        size_t padding = ((const uint8_t *)f->payload.data)[0] + 1;

        if (padding > f->payload.size)
            return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "more padding than data");

        f->payload.data += 1;
        f->payload.size -= padding;
    }

    return CNO_OK;
}

static int cno_frame_handle_priority(struct cno_connection_t *c,
                                     struct cno_stream_t     *s,
                                     struct cno_frame_t      *f)
{
    if ((f->flags & CNO_FLAG_PRIORITY) || f->type == CNO_FRAME_PRIORITY) {
        if (f->payload.size < 5 || (f->type == CNO_FRAME_PRIORITY && f->payload.size != 5))
            return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "PRIORITY of invalid size");

        if (!f->stream)
            return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "PRIORITY on stream 0");

        if (f->stream == (read4(f->payload.data) & 0x7FFFFFFFUL))
            return s ? cno_frame_write_rst_stream(c, s, CNO_RST_PROTOCOL_ERROR)
                     : cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "PRIORITY depends on itself");

        // TODO implement prioritization
        f->payload = cno_buffer_shift(f->payload, 5);
    }
    return CNO_OK;
}

static int cno_frame_handle_headers(struct cno_connection_t *c,
                                    struct cno_stream_t     *s,
                                    struct cno_frame_t      *f)
{
    if (cno_frame_handle_padding(c, f))
        return CNO_ERROR_UP();

    if (cno_frame_handle_priority(c, s, f))
        return CNO_ERROR_UP();

    if (s == NULL) {
        if (c->client || f->stream <= c->last_stream[CNO_REMOTE]) {
            if (cno_frame_handle_invalid_stream(c, f))
                return CNO_ERROR_UP();
            // else this frame must be decompressed, but ignored.
        } else if (c->goaway_sent || c->stream_count[CNO_REMOTE] >= c->settings[CNO_LOCAL].max_concurrent_streams) {
            if (cno_frame_write_rst_stream_by_id(c, f->stream, CNO_RST_REFUSED_STREAM))
                return CNO_ERROR_UP();
        } else {
            s = cno_stream_new(c, f->stream, CNO_REMOTE);
            if (s == NULL)
                return CNO_ERROR_UP();
        }
    } else if (s->r_state == CNO_STREAM_DATA) {
        if (!(f->flags & CNO_FLAG_END_STREAM))
            return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "trailers without END_STREAM");
    } else if (s->r_state != CNO_STREAM_HEADERS) {
        return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "unexpected HEADERS");
    }

    return cno_frame_handle_end_headers(c, s, f);
}

static int cno_frame_handle_push_promise(struct cno_connection_t *c,
                                         struct cno_stream_t     *s,
                                         struct cno_frame_t      *f)
{
    if (cno_frame_handle_padding(c, f))
        return CNO_ERROR_UP();

    if (f->payload.size < 4)
        return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "PUSH_PROMISE too short");

    // XXX stream may have been reset by us, in which case do what?
    if (!c->settings[CNO_LOCAL].enable_push || !cno_stream_is_local(c, f->stream)
     || !s || s->r_state == CNO_STREAM_CLOSED)
        return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "unexpected PUSH_PROMISE");

    struct cno_stream_t *child = cno_stream_new(c, read4(f->payload.data), CNO_REMOTE);
    if (child == NULL)
        return CNO_ERROR_UP();
    f->payload = cno_buffer_shift(f->payload, 4);
    return cno_frame_handle_end_headers(c, child, f);
}

static int cno_frame_handle_continuation(struct cno_connection_t *c,
                                         struct cno_stream_t     *s __attribute__((unused)),
                                         struct cno_frame_t      *f __attribute__((unused)))
{
    // There were no HEADERS (else `cno_when_h2_frame` would've merge the frames).
    return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "unexpected CONTINUATION");
}

static int cno_frame_handle_data(struct cno_connection_t *c,
                                 struct cno_stream_t     *s,
                                 struct cno_frame_t      *f)
{
    // For purposes of flow control, padding counts.
    uint32_t flow = f->payload.size;
    if (cno_frame_handle_padding(c, f))
        return CNO_ERROR_UP();

    // Frames on invalid streams still count against the connection-wide flow control window.
    // TODO allow manual connection flow control?
    if (flow && cno_frame_write(c, &(struct cno_frame_t) { CNO_FRAME_WINDOW_UPDATE, 0, 0, PACK(I32(flow)) }))
        return CNO_ERROR_UP();

    if (!s)
        return cno_frame_handle_invalid_stream(c, f);

    if (s->r_state != CNO_STREAM_DATA)
        return cno_frame_write_rst_stream(c, s, CNO_RST_STREAM_CLOSED);

    if (flow && flow > s->window_recv + c->settings[CNO_LOCAL].initial_window_size)
        return cno_frame_write_rst_stream(c, s, CNO_RST_FLOW_CONTROL_ERROR);

    if (s->remaining_payload != (uint64_t) -1)
        s->remaining_payload -= f->payload.size;

    if (f->payload.size && CNO_FIRE(c, on_message_data, f->stream, f->payload.data, f->payload.size))
        return CNO_ERROR_UP();

    if (f->flags & CNO_FLAG_END_STREAM)
        return cno_frame_handle_end_stream(c, s, NULL);

    if (c->manual_flow_control) {
        s->window_recv -= f->payload.size;
        // If there was padding, increase the window by its length right now anyway.
        flow -= f->payload.size;
    }

    struct cno_frame_t update = { CNO_FRAME_WINDOW_UPDATE, 0, s->id, PACK(I32(flow)) };
    return flow ? cno_frame_write(c, &update) : CNO_OK;
}

static int cno_frame_handle_ping(struct cno_connection_t *c,
                                 struct cno_stream_t     *s __attribute__((unused)),
                                 struct cno_frame_t      *f)
{
    if (f->stream)
        return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "PING on a stream");

    if (f->payload.size != 8)
        return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "bad PING frame");

    if (f->flags & CNO_FLAG_ACK)
        return CNO_FIRE(c, on_pong, f->payload.data);

    struct cno_frame_t response = { CNO_FRAME_PING, CNO_FLAG_ACK, 0, f->payload };
    return cno_frame_write(c, &response);
}

static int cno_frame_handle_goaway(struct cno_connection_t *c,
                                   struct cno_stream_t     *s __attribute__((unused)),
                                   struct cno_frame_t      *f)
{
    if (f->stream)
        return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "GOAWAY on a stream");

    if (f->payload.size < 8)
        return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "bad GOAWAY");

    const uint32_t error = read4(f->payload.data + 4);
    if (error != CNO_RST_NO_ERROR)
        return CNO_ERROR(PROTOCOL, "disconnected with error %u", error);
    // TODO: clean shutdown: reject all streams higher than indicated in the frame
    return CNO_ERROR(DISCONNECT, "disconnected");
}

static int cno_frame_handle_rst_stream(struct cno_connection_t *c,
                                       struct cno_stream_t     *s,
                                       struct cno_frame_t      *f)
{
    if (!s)
        return cno_frame_handle_invalid_stream(c, f);

    if (f->payload.size != 4)
        return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "bad RST_STREAM");

    // TODO parse the error code and do something with it.
    return cno_stream_end(c, s);
}

static int cno_frame_handle_settings(struct cno_connection_t *c,
                                     struct cno_stream_t     *s __attribute__((unused)),
                                     struct cno_frame_t      *f)
{
    if (f->stream)
        return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "SETTINGS on a stream");

    if (f->flags & CNO_FLAG_ACK) {
        // XXX should use the previous SETTINGS (except for stream limit) before receiving this
        if (f->payload.size)
            return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "bad SETTINGS ack");
        return CNO_OK;
    }

    if (f->payload.size % 6)
        return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "bad SETTINGS");

    struct cno_settings_t *cfg = &c->settings[CNO_REMOTE];
    const uint32_t old_window = cfg->initial_window_size;

    for (const char *p = f->payload.data, *e = p + f->payload.size; p != e; p += 6) {
        uint16_t setting = read4(p) >> 16;
        if (setting && setting < CNO_SETTINGS_UNDEFINED)
            cfg->array[setting - 1] = read4(p + 2);
    }

    if (cfg->enable_push > 1)
        return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "enable_push out of bounds");

    if (cfg->initial_window_size > 0x7FFFFFFFL)
        return cno_frame_write_error(c, CNO_RST_FLOW_CONTROL_ERROR, "initial_window_size too big");

    if (cfg->max_frame_size < 16384 || cfg->max_frame_size > 16777215)
        return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "max_frame_size out of bounds");

    if (cfg->initial_window_size > old_window && CNO_FIRE(c, on_flow_increase, 0))
        return CNO_ERROR_UP();

    size_t limit = c->encoder.limit_upper = cfg->header_table_size;
    if (limit > c->settings[CNO_LOCAL].header_table_size)
        limit = c->settings[CNO_LOCAL].header_table_size;
    if (cno_hpack_setlimit(&c->encoder, limit))
        return CNO_ERROR_UP();

    struct cno_frame_t ack = { CNO_FRAME_SETTINGS, CNO_FLAG_ACK, 0, {} };
    return cno_frame_write(c, &ack) || CNO_FIRE(c, on_settings);
}

static int cno_frame_handle_window_update(struct cno_connection_t *c,
                                          struct cno_stream_t     *s,
                                          struct cno_frame_t      *f)
{
    if (f->payload.size != 4)
        return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "bad WINDOW_UPDATE");

    uint32_t delta = read4(f->payload.data);
    if (delta == 0 || delta > 0x7FFFFFFFL)
        return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "window increment out of bounds");

    if (!f->stream) {
        if ((c->window_send += delta) > 0x7FFFFFFFL)
            return cno_frame_write_error(c, CNO_RST_FLOW_CONTROL_ERROR, "window increment too big");
    } else if (s) {
        if ((s->window_send += delta) + c->settings[CNO_REMOTE].initial_window_size > 0x7FFFFFFFL)
            return cno_frame_write_rst_stream(c, s, CNO_RST_FLOW_CONTROL_ERROR);
    } else {
        return cno_frame_handle_invalid_stream(c, f);
    }

    return CNO_FIRE(c, on_flow_increase, f->stream);
}

typedef int cno_frame_handler_t(struct cno_connection_t *,
                                struct cno_stream_t     *,
                                struct cno_frame_t      *);

static cno_frame_handler_t * const CNO_FRAME_HANDLERS[] = {
    // Should be synced to enum CNO_FRAME_TYPE.
    &cno_frame_handle_data,
    &cno_frame_handle_headers,
    &cno_frame_handle_priority,
    &cno_frame_handle_rst_stream,
    &cno_frame_handle_settings,
    &cno_frame_handle_push_promise,
    &cno_frame_handle_ping,
    &cno_frame_handle_goaway,
    &cno_frame_handle_window_update,
    &cno_frame_handle_continuation,
};

int cno_configure(struct cno_connection_t *c, const struct cno_settings_t *settings) {
    if (settings->enable_push != 0 && settings->enable_push != 1)
        return CNO_ERROR(ASSERTION, "enable_push neither 0 nor 1");

    if (settings->max_frame_size < 16384 || settings->max_frame_size > 16777215)
        return CNO_ERROR(ASSERTION, "maximum frame size out of bounds (2^14..2^24-1)");

    if (c->state != CNO_STATE_H2_INIT && c->mode == CNO_HTTP2)
        // If not yet in HTTP2 mode, `cno_when_h2_init` will send the SETTINGS frame.
        if (cno_frame_write_settings(c, &c->settings[CNO_LOCAL], settings))
            return CNO_ERROR_UP();

    c->decoder.limit_upper = settings->header_table_size;
    memcpy(&c->settings[CNO_LOCAL], settings, sizeof(*settings));
    return CNO_OK;
}

void cno_init(struct cno_connection_t *c, enum CNO_CONNECTION_KIND kind) {
    *c = (struct cno_connection_t) {
        .client      = CNO_CLIENT == kind,
        .window_recv = CNO_SETTINGS_STANDARD.initial_window_size,
        .window_send = CNO_SETTINGS_STANDARD.initial_window_size,
        .settings    = { /* remote = */ CNO_SETTINGS_CONSERVATIVE,
                         /* local  = */ CNO_SETTINGS_INITIAL, },
        .disallow_h2_upgrade = 1,
    };

    cno_hpack_init(&c->decoder, CNO_SETTINGS_INITIAL .header_table_size);
    cno_hpack_init(&c->encoder, CNO_SETTINGS_STANDARD.header_table_size);
}

void cno_fini(struct cno_connection_t *c) {
    cno_buffer_dyn_clear(&c->buffer);
    cno_hpack_clear(&c->encoder);
    cno_hpack_clear(&c->decoder);

    for (size_t i = 0; i < CNO_STREAM_BUCKETS; i++)
        for (struct cno_stream_t *s; (s = c->streams[i]); free(s))
            c->streams[i] = s->next;
}

static size_t cno_remove_chunked_te(struct cno_buffer_t *buf) {
    // assuming the request is valid, chunked can only be the last transfer-encoding
    if (cno_buffer_endswith(*buf, CNO_BUFFER_STRING("chunked"))) {
        buf->size -= 7;
        while (buf->size && buf->data[buf->size - 1] == ' ')
            buf->size--;
        if (buf->size && buf->data[buf->size - 1] == ',')
            buf->size--;
    }
    return buf->size;
}

// NOTE these functions have tri-state returns now: negative for errors, 0 (CNO_OK)
//      to wait for more data, and positive (CNO_CONNECTION_STATE) to switch to another state.
static int cno_when_closed(struct cno_connection_t *c __attribute__((unused))) {
    return CNO_ERROR(DISCONNECT, "connection closed");
}

static int cno_when_h2_init(struct cno_connection_t *c) {
    c->mode = CNO_HTTP2;
    if (c->client && CNO_FIRE(c, on_writev, &CNO_PREFACE, 1))
        return CNO_ERROR_UP();
    if (cno_frame_write_settings(c, &CNO_SETTINGS_STANDARD, &c->settings[CNO_LOCAL]))
        return CNO_ERROR_UP();
    return CNO_STATE_H2_PREFACE;
}

static int cno_when_h2_preface(struct cno_connection_t *c) {
    if (!c->client) {
        if (strncmp(c->buffer.data, CNO_PREFACE.data, c->buffer.size))
            return CNO_ERROR(PROTOCOL, "invalid HTTP 2 client preface");
        if (c->buffer.size < CNO_PREFACE.size)
            return CNO_OK;
        cno_buffer_dyn_shift(&c->buffer, CNO_PREFACE.size);
    }
    return CNO_STATE_H2_SETTINGS;
}

static int cno_when_h2_settings(struct cno_connection_t *c) {
    if (c->buffer.size < 5)
        return CNO_OK;
    if (c->buffer.data[3] != CNO_FRAME_SETTINGS || c->buffer.data[4] != 0)
        return CNO_ERROR(PROTOCOL, "invalid HTTP 2 preface: no initial SETTINGS");
    size_t len = read4(c->buffer.data) >> 8;
    if (len > CNO_SETTINGS_INITIAL.max_frame_size) // couldn't have ACKed our settings yet!
        return CNO_ERROR(PROTOCOL, "invalid HTTP 2 preface: initial SETTINGS too big");
    if (c->buffer.size < 9 + len)
        return CNO_OK;
    // Now that we know the *actual* values, they should be applied as deltas to this.
    c->settings[CNO_REMOTE] = CNO_SETTINGS_INITIAL;
    return CNO_STATE_H2_FRAME;
}

static int cno_when_h2_frame(struct cno_connection_t *c) {
    const uint8_t *base = (const uint8_t *) c->buffer.data;
    if (c->buffer.size < 9)
        return CNO_OK;

    struct cno_buffer_t payload = { c->buffer.data + 9, read4(base) >> 8 };
    if (payload.size > c->settings[CNO_LOCAL].max_frame_size)
        return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "frame too big");
    if (c->buffer.size < payload.size + 9)
        return CNO_OK;

    struct cno_frame_t f = { base[3], base[4], read4(&base[5]) & 0x7FFFFFFFUL, payload };
    if ((f.type == CNO_FRAME_HEADERS || f.type == CNO_FRAME_PUSH_PROMISE) && !(f.flags & CNO_FLAG_END_HEADERS)) {
        size_t i = 0;
        for (size_t offset = f.payload.size + 9;;) {
            if (++i > CNO_MAX_CONTINUATIONS)
                return cno_frame_write_error(c, CNO_RST_ENHANCE_YOUR_CALM, "too many CONTINUATIONs");
            if (c->buffer.size < offset + 9)
                return CNO_OK;

            size_t size = read4(&base[offset]) >> 8;
            if (size > c->settings[CNO_LOCAL].max_frame_size)
                return cno_frame_write_error(c, CNO_RST_FRAME_SIZE_ERROR, "frame too big");
            if (base[offset + 3] != CNO_FRAME_CONTINUATION)
                return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "expected CONTINUATION");
            if (base[offset + 4] & ~CNO_FLAG_END_HEADERS)
                return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "invalid CONTINUATION flags");
            if ((read4(&base[offset + 5]) & 0x7FFFFFFFUL) != f.stream)
                return cno_frame_write_error(c, CNO_RST_PROTOCOL_ERROR, "invalid CONTINUATION stream");

            if (c->buffer.size < offset + 9 + size)
                return CNO_OK;
            if (base[offset + 4])
                break;
            offset += size + 9;
        }
        f.flags |= CNO_FLAG_END_HEADERS;

        size_t offset = f.payload.size + 9;
        for (size_t size; i--; f.payload.size += size, offset += size + 9)
            memmove((char *) f.payload.data + f.payload.size, base + offset + 9, size = read4(&base[offset]) >> 8);
        memmove((char *) f.payload.data + f.payload.size, base + offset, c->buffer.size - offset);
        c->buffer.size -= (offset - f.payload.size - 9);
    }

    cno_buffer_dyn_shift(&c->buffer, f.payload.size + 9);
    if (CNO_FIRE(c, on_frame, &f))
        return CNO_ERROR_UP();
    // >Implementations MUST ignore and discard any frame that has a type that is unknown.
    if (f.type < CNO_FRAME_UNKNOWN && CNO_FRAME_HANDLERS[f.type](c, cno_stream_find(c, f.stream), &f))
        return CNO_ERROR_UP();
    return CNO_STATE_H2_FRAME;
}

static int cno_when_h1_head(struct cno_connection_t *c) {
    if (!c->buffer.size)
        return CNO_OK;

    struct cno_stream_t *s = cno_stream_find(c, c->last_stream[c->client]);
    if (c->client) {
        if (!s || s->r_state != CNO_STREAM_HEADERS)
            return CNO_ERROR(PROTOCOL, "server sent an HTTP/1.x response, but there was no request");
    } else {
        if (!s) {
            // Only allow upgrading with prior knowledge if no h1 requests have yet been received.
            if (!c->disallow_h2_prior_knowledge && c->last_stream[CNO_REMOTE] == 0)
                if (!strncmp(c->buffer.data, CNO_PREFACE.data, c->buffer.size))
                    return c->buffer.size < CNO_PREFACE.size ? CNO_OK : CNO_STATE_H2_INIT;
            if (!(s = cno_stream_new(c, (c->last_stream[CNO_REMOTE] + 1) | 1, CNO_REMOTE)))
                return CNO_ERROR_UP();
        }
        if (s->r_state != CNO_STREAM_HEADERS)
            return CNO_ERROR(WOULD_BLOCK, "already handling an HTTP/1.x message");
    }

    struct cno_header_t headers[CNO_MAX_HEADERS + 2]; // + :scheme and :authority
    struct cno_message_t m = { 0, {}, {}, headers, CNO_MAX_HEADERS };
    struct phr_header headers_phr[CNO_MAX_HEADERS];

    int minor = 0;
    int ok = c->client
        ? phr_parse_response(c->buffer.data, c->buffer.size,
            &minor, &m.code, &m.method.data, &m.method.size,
            headers_phr, &m.headers_len, 1)
        : phr_parse_request(c->buffer.data, c->buffer.size,
            &m.method.data, &m.method.size, &m.path.data, &m.path.size, &minor,
            headers_phr, &m.headers_len, 1);

    if (ok == -2) {
        if (c->buffer.size > (CNO_MAX_CONTINUATIONS + 1) * c->settings[CNO_LOCAL].max_frame_size)
            return CNO_ERROR(PROTOCOL, "HTTP/1.x message too big");
        return CNO_OK;
    }

    if (ok == -1)
        return CNO_ERROR(PROTOCOL, "bad HTTP/1.x message");

    if (minor != 0 && minor != 1)
        // HTTP/1.0 is probably not really supported either tbh.
        return CNO_ERROR(PROTOCOL, "HTTP/1.%d not supported", minor);

    int upgrade = 0;
    c->remaining_h1_payload = 0;
    struct cno_header_t *it = headers;
    if (!c->client) {
        *it++ = (struct cno_header_t) { CNO_BUFFER_STRING(":scheme"), CNO_BUFFER_STRING("unknown"), 0 };
        *it++ = (struct cno_header_t) { CNO_BUFFER_STRING(":authority"), CNO_BUFFER_STRING("unknown"), 0 };
    }
    for (size_t i = 0; i < m.headers_len; i++) {
        *it = (struct cno_header_t) {
            .name  = { headers_phr[i].name,  headers_phr[i].name_len  },
            .value = { headers_phr[i].value, headers_phr[i].value_len },
        };

        for (uint8_t *p = (uint8_t *) it->name.data, *e = p + it->name.size; p != e; p++)
            if (!(*p = CNO_HEADER_TRANSFORM[*p]))
                return CNO_ERROR(PROTOCOL, "invalid character in h1 header");

        if (!c->client && cno_buffer_eq(it->name, CNO_BUFFER_STRING("host"))) {
            headers[1].value = it->value;
            continue;
        } else if (cno_buffer_eq(it->name, CNO_BUFFER_STRING("http2-settings"))) {
            // TODO decode & emit on_frame
            continue;
        } else if (cno_buffer_eq(it->name, CNO_BUFFER_STRING("upgrade"))) {
            if (c->mode != CNO_HTTP1) {
                continue; // If upgrading to h2c, don't notify the application of any upgrades.
            } else if (cno_buffer_eq(it->value, CNO_BUFFER_STRING("h2c"))) {
                // TODO: client-side h2 upgrade
                if (c->disallow_h2_upgrade || c->client || s->id != 1 || upgrade)
                    continue;

                // Technically, server should refuse if HTTP2-Settings are not present. We'll let this slide.
                if (CNO_WRITEV(c, CNO_BUFFER_STRING("HTTP/1.1 101 Switching Protocols\r\nconnection: upgrade\r\nupgrade: h2c\r\n\r\n"))
                 || cno_when_h2_init(c) < 0)
                    return CNO_ERROR_UP();
                continue;
            } else if (!c->client) {
                // FIXME technically, http supports upgrade requests with payload (see h2c above).
                //       the api does not allow associating 2 streams of data with a message, though.
                upgrade = 1;
            }
        } else if (cno_buffer_eq(it->name, CNO_BUFFER_STRING("content-length"))) {
            if (c->remaining_h1_payload == (uint64_t) -1)
                continue; // Ignore content-length with chunked transfer-encoding.
            if (c->remaining_h1_payload)
                return CNO_ERROR(PROTOCOL, "multiple content-lengths");
            if ((c->remaining_h1_payload = cno_parse_uint(it->value)) == (uint64_t) -1)
                return CNO_ERROR(PROTOCOL, "invalid content-length");
        } else if (cno_buffer_eq(it->name, CNO_BUFFER_STRING("transfer-encoding"))) {
            if (cno_buffer_eq(it->value, CNO_BUFFER_STRING("identity")))
                continue; // (This value is probably not actually allowed.)
            // Any non-identity transfer-encoding requires chunked (which should also be listed).
            // (This part is a bit non-compatible with h2. Proxies should probably decode TEs.)
            c->remaining_h1_payload = (uint64_t) -1;
            if (!cno_remove_chunked_te(&it->value))
                continue;
        }

        it++;
    }
    m.headers_len = it - m.headers;

    if (m.code == 101)
        // Just forward everything else (well, 18 exabytes at most...) to stream 1 as data.
        c->remaining_h1_payload = (uint64_t) -2;
    else if (cno_is_informational(m.code) && c->remaining_h1_payload)
        return CNO_ERROR(PROTOCOL, "informational response with a payload");

    // XXX can a HEAD request with `upgrade` trigger an upgrade? This prevents it:
    if (s->reading_head_response)
        c->remaining_h1_payload = 0;

    // If on_message_head triggers asynchronous handling, this is expected to block until
    // either 101 has been sent or the server decides not to upgrade.
    if (CNO_FIRE(c, on_message_head, s->id, &m) || (upgrade && CNO_FIRE(c, on_upgrade)))
        return CNO_ERROR_UP();

    cno_buffer_dyn_shift(&c->buffer, (size_t) ok);

    if (cno_is_informational(m.code) && m.code != 101)
        return CNO_STATE_H1_HEAD;

    s->r_state = CNO_STREAM_DATA;
    return c->remaining_h1_payload == (uint64_t) -1 ? CNO_STATE_H1_CHUNK
         : c->remaining_h1_payload ? CNO_STATE_H1_BODY
         : CNO_STATE_H1_TAIL;
}

static int cno_when_h1_body(struct cno_connection_t *c) {
    while (c->remaining_h1_payload) {
        if (!c->buffer.size)
            return CNO_OK;
        struct cno_buffer_t b = CNO_BUFFER_VIEW(c->buffer);
        if (b.size > c->remaining_h1_payload)
            b.size = c->remaining_h1_payload;
        c->remaining_h1_payload -= b.size;
        cno_buffer_dyn_shift(&c->buffer, b.size);
        struct cno_stream_t *s = cno_stream_find(c, c->last_stream[c->client]);
        if (s && CNO_FIRE(c, on_message_data, s->id, b.data, b.size))
            return CNO_ERROR_UP();
    }
    return c->state == CNO_STATE_H1_BODY ? CNO_STATE_H1_TAIL : CNO_STATE_H1_CHUNK_TAIL;
}

static int cno_when_h1_tail(struct cno_connection_t *c) {
    struct cno_stream_t *s = cno_stream_find(c, c->last_stream[c->client]);
    if (s) {
        if (CNO_FIRE(c, on_message_tail, s->id, NULL))
            return CNO_ERROR_UP();
        // FIXME on_message_tail may call cno_write_reset and destroy the stream, leaving
        //       a dangling pointer. (Also check all other CNO_FIREs.)
        s->r_state = CNO_STREAM_CLOSED;
        if (s->w_state == CNO_STREAM_CLOSED && cno_stream_end(c, s))
            return CNO_ERROR_UP();
    }
    return c->mode == CNO_HTTP2 ? CNO_STATE_H2_PREFACE : CNO_STATE_H1_HEAD;
}

static int cno_when_h1_chunk(struct cno_connection_t *c) {
    const char *eol = memchr(c->buffer.data, '\n', c->buffer.size);
    if (eol == NULL)
        return c->buffer.size >= c->settings[CNO_LOCAL].max_frame_size
             ? CNO_ERROR(PROTOCOL, "too many h1 chunk extensions") : CNO_OK;
    const char *p = c->buffer.data;
    size_t length = 0;
    do {
        size_t prev = length;
        length = length * 16 + ('0' <= *p && *p <= '9' ? *p - '0' :
                                'A' <= *p && *p <= 'F' ? *p - 'A' + 10 :
                                'a' <= *p && *p <= 'f' ? *p - 'a' + 10 : -1);
        if (length < prev)
            return CNO_ERROR(PROTOCOL, "invalid h1 chunk length");
    } while (*++p != '\r' && *p != '\n' && *p != ';');
    if (*p == ';')
        p = eol + 1;
    else if (*p++ != '\r' || *p++ != '\n')
        return CNO_ERROR(PROTOCOL, "invalid h1 line separator");
    cno_buffer_dyn_shift(&c->buffer, p - c->buffer.data);
    c->remaining_h1_payload = length;
    return length ? CNO_STATE_H1_CHUNK_BODY : CNO_STATE_H1_TRAILERS;
}

static int cno_when_h1_chunk_tail(struct cno_connection_t *c) {
    if (c->buffer.size < 2)
        return CNO_OK;
    if (c->buffer.data[0] != '\r' || c->buffer.data[1] != '\n')
        return CNO_ERROR(PROTOCOL, "invalid h1 chunk terminator");
    cno_buffer_dyn_shift(&c->buffer, 2);
    return CNO_STATE_H1_CHUNK;
}

static int cno_when_h1_trailers(struct cno_connection_t *c) {
    // TODO actually support trailers (they come before the tail).
    int ret = cno_when_h1_chunk_tail(c);
    return ret < 0 ? CNO_ERROR_UP() : ret > 0 ? CNO_STATE_H1_TAIL : CNO_OK;
}

typedef int cno_state_handler_t(struct cno_connection_t *);

static cno_state_handler_t * const CNO_STATE_MACHINE[] = {
    // Should be synced to enum CNO_CONNECTION_STATE.
    &cno_when_closed,
    &cno_when_h2_init,
    &cno_when_h2_preface,
    &cno_when_h2_settings,
    &cno_when_h2_frame,
    &cno_when_h1_head,
    &cno_when_h1_body,
    &cno_when_h1_tail,
    &cno_when_h1_chunk,
    &cno_when_h1_body,
    &cno_when_h1_chunk_tail,
    &cno_when_h1_trailers,
};

int cno_begin(struct cno_connection_t *c, enum CNO_HTTP_VERSION version) {
    if (c->state != CNO_STATE_CLOSED)
        return CNO_ERROR(ASSERTION, "called connection_made twice");
    c->state = (version == CNO_HTTP2 ? CNO_STATE_H2_INIT : CNO_STATE_H1_HEAD);
    return cno_consume(c, NULL, 0);
}

int cno_consume(struct cno_connection_t *c, const char *data, size_t size) {
    if (cno_buffer_dyn_concat(&c->buffer, (struct cno_buffer_t) { data, size }))
        return CNO_ERROR_UP();
    for (int r; (r = CNO_STATE_MACHINE[c->state](c)) != 0; c->state = r)
        if (r < 0)
            return CNO_ERROR_UP();
    return CNO_OK;
}

int cno_shutdown(struct cno_connection_t *c) {
    return cno_write_reset(c, 0, CNO_RST_NO_ERROR);
}

int cno_eof(struct cno_connection_t *c) {
    if (c->mode != CNO_HTTP2) {
        struct cno_stream_t *s = cno_stream_find(c, c->last_stream[c->client]);
        return s && s->r_state != CNO_STREAM_CLOSED ? CNO_ERROR(DISCONNECT, "unclean http/1.x termination") : CNO_OK;
    }

    // h2 won't work over half-closed connections due to pings and flow control.
    c->state = CNO_STATE_CLOSED;
    for (size_t i = 0; i < CNO_STREAM_BUCKETS; i++)
        while (c->streams[i])
            if (cno_stream_end(c, c->streams[i]))
                return CNO_ERROR_UP();
    return CNO_OK;
}

uint32_t cno_next_stream(const struct cno_connection_t *c) {
    uint32_t last = c->last_stream[CNO_LOCAL];
    return c->client ? (last + 1) | 1 : last + 2;
}

int cno_write_reset(struct cno_connection_t *c, uint32_t sid, enum CNO_RST_STREAM_CODE code) {
    if (c->mode != CNO_HTTP2)
        return CNO_OK; // if code != NO_ERROR, this requires simply closing the transport ¯\_(ツ)_/¯
    if (!sid)
        return cno_frame_write_goaway(c, code);
    struct cno_stream_t *s = cno_stream_find(c, sid);
    return s ? cno_frame_write_rst_stream(c, s, code) : CNO_OK; // assume idle streams have already been reset
}

int cno_write_push(struct cno_connection_t *c, uint32_t sid, const struct cno_message_t *m) {
    if (c->state == CNO_STATE_CLOSED)
        return CNO_ERROR(DISCONNECT, "connection closed");
    if (c->client)
        return CNO_ERROR(ASSERTION, "clients can't push");
    if (c->mode != CNO_HTTP2 || !c->settings[CNO_REMOTE].enable_push || cno_stream_is_local(c, sid))
        return CNO_OK;

    struct cno_stream_t *s = cno_stream_find(c, sid);
    if (!s || s->w_state == CNO_STREAM_CLOSED)
        return CNO_OK;  // pushed requests are safe, so whether we send one doesn't matter

    uint32_t child = cno_next_stream(c);
    if (cno_stream_new(c, child, CNO_LOCAL) == NULL)
        return CNO_ERROR_UP();

    struct cno_buffer_dyn_t enc = {};
    struct cno_header_t head[2] = {
        { CNO_BUFFER_STRING(":method"), m->method, 0 },
        { CNO_BUFFER_STRING(":path"),   m->path,   0 },
    };
    if (cno_buffer_dyn_concat(&enc, PACK(I32(child)))
     || cno_hpack_encode(&c->encoder, &enc, head, 2)
     || cno_hpack_encode(&c->encoder, &enc, m->headers, m->headers_len)
     || cno_frame_write(c, &(struct cno_frame_t){ CNO_FRAME_PUSH_PROMISE, CNO_FLAG_END_HEADERS, sid, CNO_BUFFER_VIEW(enc) }))
        // irrecoverable (compression state desync), don't bother destroying the stream.
        // FIXME should make next `cno_consume` fail. The possible errors are NO_MEMORY
        //       or something from on_writev, so rolling back is pointless as keeping the old state
        //       will consume even more memory and on_writev should only fail on disconnect.
        return cno_buffer_dyn_clear(&enc), CNO_ERROR_UP();
    cno_buffer_dyn_clear(&enc);
    return CNO_FIRE(c, on_message_head, child, m) || CNO_FIRE(c, on_message_tail, child, NULL);
}

static int cno_discard_remaining_payload(struct cno_connection_t *c, struct cno_stream_t *s) {
    s->w_state = CNO_STREAM_CLOSED;
    if (s->r_state == CNO_STREAM_CLOSED)
        return cno_stream_end_by_local(c, s);
    if (!c->client && c->mode == CNO_HTTP2 && cno_frame_write_rst_stream(c, s, CNO_RST_NO_ERROR))
        return CNO_ERROR_UP();
    return CNO_OK;
}

static struct cno_buffer_t cno_fmt_uint(char *b, size_t s, unsigned n) {
    char *q = b + s;
    do *--q = '0' + (n % 10); while (n /= 10);
    return (struct cno_buffer_t){ q, b + s - q };
}

static struct cno_buffer_t cno_fmt_chunk_length(char *b, size_t s, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    char *q = b + s;
    *--q = '\n';
    *--q = '\r';
    do *--q = hex[n % 16]; while (n /= 16);
    return (struct cno_buffer_t){ q, b + s - q };
}

static int cno_h1_write_head(struct cno_connection_t *c, struct cno_stream_t *s, const struct cno_message_t *m, int final) {
    if (c->client
      ? CNO_WRITEV(c, m->method, CNO_BUFFER_STRING(" "), m->path, CNO_BUFFER_STRING(" HTTP/1.1\r\n"))
      // XXX technically, the reason string is meaningless so we don't need to specify the correct one.
      //     Might not be wise to leak information about the used library, though. Security through obscurity ftw.
      : CNO_WRITEV(c, CNO_BUFFER_STRING("HTTP/1.1 "), cno_fmt_uint((char[12]){}, 12, m->code), CNO_BUFFER_STRING(" "),
                      m->method.size ? m->method : CNO_BUFFER_STRING("No Reason"), CNO_BUFFER_STRING("\r\n")))
        return CNO_ERROR_UP();

    s->writing_chunked = !cno_is_informational(m->code) && !final;
    for (const struct cno_header_t *it = m->headers, *end = it + m->headers_len; it != end; ++it) {
        struct cno_header_t h = *it;
        if (cno_buffer_eq(h.name, CNO_BUFFER_STRING(":authority"))) {
            h.name = CNO_BUFFER_STRING("host");
        } else if (cno_buffer_startswith(h.name, CNO_BUFFER_STRING(":"))) {
            continue; // :scheme, probably
        } else if (cno_buffer_eq(h.name, CNO_BUFFER_STRING("content-length"))
                || cno_buffer_eq(h.name, CNO_BUFFER_STRING("upgrade"))) {
            // XXX not writing chunked on `upgrade` is a hack so that `GET` with final = 0 still works.
            s->writing_chunked = 0;
        } else if (cno_buffer_eq(h.name, CNO_BUFFER_STRING("transfer-encoding"))) {
            // Either CNO_STREAM_H1_WRITING_CHUNKED is set, there's no body at all, or message
            // is invalid because it contains both content-length and transfer-encoding.
            if (!cno_remove_chunked_te(&h.value))
                continue;
        }
        // XXX maybe send as one call? Or at least pack ~32 buffers (~8 headers) or something.
        if (CNO_WRITEV(c, h.name, CNO_BUFFER_STRING(": "), h.value, CNO_BUFFER_STRING("\r\n")))
            return CNO_ERROR_UP();
    }
    if (CNO_WRITEV(c, s->writing_chunked ? CNO_BUFFER_STRING("transfer-encoding: chunked\r\n\r\n") : CNO_BUFFER_STRING("\r\n")))
        return CNO_ERROR_UP();

    if (m->code == 101) {
        // Only handle upgrades if still in on_message_head/on_upgrade.
        if (c->state != CNO_STATE_H1_HEAD || s->r_state == CNO_STREAM_CLOSED)
            return CNO_ERROR(ASSERTION, "accepted a h1 upgrade, but did not block in on_upgrade");
        c->remaining_h1_payload = (uint64_t) -2;
    }
    return CNO_OK;
}

static int cno_h2_write_head(struct cno_connection_t *c, struct cno_stream_t *s, const struct cno_message_t *m, int final) {
    if (m->code == 101)
        return CNO_ERROR(ASSERTION, "cannot switch protocols over an http2 connection");
    int flags = (final ? CNO_FLAG_END_STREAM : 0) | CNO_FLAG_END_HEADERS;
    struct cno_buffer_dyn_t enc = {};
    struct cno_header_t head[] = {
        { CNO_BUFFER_STRING(":status"), cno_fmt_uint((char[12]){}, 12, m->code), 0 },
        { CNO_BUFFER_STRING(":method"), m->method, 0 },
        { CNO_BUFFER_STRING(":path"),   m->path,   0 },
    };
    if (cno_hpack_encode(&c->encoder, &enc, c->client ? head + 1 : head, c->client ? 2 : 1)
     || cno_hpack_encode(&c->encoder, &enc, m->headers, m->headers_len)
     || cno_frame_write(c, &(struct cno_frame_t){ CNO_FRAME_HEADERS, flags, s->id, CNO_BUFFER_VIEW(enc) }))
        // Irrecoverable (compression state desync). FIXME: see `cno_write_push`.
        return cno_buffer_dyn_clear(&enc), CNO_ERROR_UP();
    return cno_buffer_dyn_clear(&enc), CNO_OK;
}

int cno_write_head(struct cno_connection_t *c, uint32_t sid, const struct cno_message_t *m, int final) {
    if (c->state == CNO_STATE_CLOSED)
        return CNO_ERROR(DISCONNECT, "connection closed");

    if (c->client ? !!m->code : !!m->path.size)
        return CNO_ERROR(ASSERTION, c->client ? "request with a code" : "response with a path");
    if (cno_is_informational(m->code) && final)
        return CNO_ERROR(ASSERTION, "1xx codes cannot end the stream");
    for (const struct cno_header_t *h = m->headers, *he = h + m->headers_len; h != he; h++)
        for (const char *p = h->name.data, *e = p + h->name.size; p != e; p++)
            if (isupper(*p))
                return CNO_ERROR(ASSERTION, "header names should be lowercase");

    struct cno_stream_t *s = cno_stream_find(c, sid);
    if (c->client && !s && !(s = cno_stream_new(c, sid, CNO_LOCAL)))
        return CNO_ERROR_UP();
    if (!s || s->w_state != CNO_STREAM_HEADERS)
        return CNO_ERROR(INVALID_STREAM, "this stream is not writable");

    s->reading_head_response = cno_buffer_eq(m->method, CNO_BUFFER_STRING("HEAD"));
    if ((c->mode == CNO_HTTP2 ? cno_h2_write_head : cno_h1_write_head)(c, s, m, final))
        return CNO_ERROR_UP();
    if (m->code == 101 || !cno_is_informational(m->code))
        s->w_state = CNO_STREAM_DATA;
    return final && cno_discard_remaining_payload(c, s) ? CNO_ERROR_UP() : CNO_OK;
}

static int cno_h1_write_data(struct cno_connection_t *c, struct cno_stream_t *s, struct cno_buffer_t *b, int final) {
    if (!s->writing_chunked)
        return b->size ? CNO_WRITEV(c, *b) : CNO_OK;
    if (!b->size)
        return final ? CNO_WRITEV(c, CNO_BUFFER_STRING("0\r\n\r\n")) : CNO_OK;
    struct cno_buffer_t tail = final ? CNO_BUFFER_STRING("\r\n0\r\n\r\n") : CNO_BUFFER_STRING("\r\n");
    return CNO_WRITEV(c, cno_fmt_chunk_length((char[24]){}, 24, b->size), *b, tail);
}

static int cno_h2_write_data(struct cno_connection_t *c, struct cno_stream_t *s, struct cno_buffer_t *b, int final) {
    int64_t limit = s->window_send + c->settings[CNO_REMOTE].initial_window_size;
    if (limit > c->window_send)
        limit = c->window_send;
    if (limit < 0)
        limit = 0;
    if (b->size > (uint64_t) limit) {
        b->size = limit;
        final = 0;
    }
    struct cno_frame_t frame = { CNO_FRAME_DATA, final ? CNO_FLAG_END_STREAM : 0, s->id, *b };
    if ((b->size || final) && cno_frame_write(c, &frame))
        return CNO_ERROR_UP();
    c->window_send -= b->size;
    s->window_send -= b->size;
    return CNO_OK;
}

int cno_write_data(struct cno_connection_t *c, uint32_t sid, const char *data, size_t size, int final) {
    if (c->state == CNO_STATE_CLOSED)
        return CNO_ERROR(DISCONNECT, "connection closed");

    struct cno_stream_t *s = cno_stream_find(c, sid);
    if (!s || s->w_state != CNO_STREAM_DATA)
        return CNO_ERROR(INVALID_STREAM, "this stream is not writable");

    struct cno_buffer_t b = {data, size};
    if ((c->mode == CNO_HTTP2 ? cno_h2_write_data : cno_h1_write_data)(c, s, &b, final))
        return CNO_ERROR_UP();
    return final && cno_discard_remaining_payload(c, s) ? CNO_ERROR_UP() : (int)b.size;
}

int cno_write_ping(struct cno_connection_t *c, const char data[8]) {
    if (c->mode != CNO_HTTP2)
        return CNO_ERROR(ASSERTION, "cannot ping HTTP/1.x endpoints");
    struct cno_frame_t ping = { CNO_FRAME_PING, 0, 0, { data, 8 } };
    return cno_frame_write(c, &ping);
}

int cno_write_frame(struct cno_connection_t *c, const struct cno_frame_t *f) {
    if (c->mode != CNO_HTTP2)
        return CNO_ERROR(ASSERTION, "cannot send HTTP2 frames to HTTP/1.x endpoints");
    if (f->type == CNO_FRAME_DATA)
        return CNO_ERROR(ASSERTION, "cannot send flow-controlled DATA frames this way");
    return cno_frame_write(c, f);
}

int cno_open_flow(struct cno_connection_t *c, uint32_t sid, uint32_t delta) {
    if (c->mode != CNO_HTTP2 || !sid || !delta)
        return CNO_OK; // TODO don't ignore connection flow updates
    struct cno_stream_t *s = cno_stream_find(c, sid);
    if (!s)
        return CNO_OK;
    s->window_recv += delta;
    struct cno_frame_t update = { CNO_FRAME_WINDOW_UPDATE, 0, sid, PACK(I32(delta)) };
    return cno_frame_write(c, &update);
}
