#include "ipp.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int ipp_parse_request(const uint8_t *buf, size_t len, ipp_request_t *out)
{
    if (buf == NULL || out == NULL || len < 12) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->version = rd16(&buf[0]);
    out->operation_id = rd16(&buf[2]);
    out->request_id = rd32(&buf[4]);

    size_t off = 8;
    while (off < len) {
        uint8_t tag = buf[off++];
        if (tag == IPP_TAG_END) {
            out->attrs_end = off;
            out->document = &buf[off];
            out->document_len = len - off;
            return 0;
        }
        /* Group tags (0x01..0x05) carry no name/value - just advance. */
        if (tag >= 0x01 && tag <= 0x05) {
            continue;
        }
        if (off + 2 > len) {
            return -1;
        }
        size_t name_len = rd16(&buf[off]);
        off += 2;
        if (off + name_len + 2 > len) {
            return -1;
        }
        off += name_len;
        size_t value_len = rd16(&buf[off]);
        off += 2;
        if (off + value_len > len) {
            return -1;
        }
        off += value_len;
    }
    /* Missing end-of-attributes-tag: tolerate and point document past end. */
    out->attrs_end = len;
    out->document = &buf[len];
    out->document_len = 0;
    return 0;
}

void ipp_writer_init(ipp_writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->len = 0;
    w->cap = cap;
    w->last_name[0] = '\0';
}

static int ipp_put(ipp_writer_t *w, const void *data, size_t n)
{
    if (w->buf == NULL || w->len + n > w->cap) {
        return -1;
    }
    memcpy(&w->buf[w->len], data, n);
    w->len += n;
    return 0;
}

int ipp_resp_begin(ipp_writer_t *w, uint16_t version, uint16_t status, uint32_t request_id)
{
    uint8_t hdr[8];
    /* Echo the client's version: iOS speaks IPP 2.0 and discards a 1.1 reply,
     * then retries forever (looks like an endless Get-Printer-Attributes storm). */
    wr16(&hdr[0], version ? version : IPP_VERSION_1_1);
    wr16(&hdr[2], status);
    wr32(&hdr[4], request_id);
    if (ipp_put(w, hdr, 8) != 0) {
        return -1;
    }
    return ipp_add_group(w, IPP_TAG_OPERATION);
}

int ipp_add_group(ipp_writer_t *w, uint8_t group_tag)
{
    /* A new group starts a fresh attribute scope: the next value may not reuse
     * the previous name with a zero-length encoding. */
    w->last_name[0] = '\0';
    return ipp_put(w, &group_tag, 1);
}

int ipp_add_attr(ipp_writer_t *w, uint8_t tag, const char *name,
                 const uint8_t *value, size_t value_len)
{
    if (name == NULL || value_len > 0xffff) {
        return -1;
    }
    size_t name_len = strlen(name);
    if (name_len > 0xffff) {
        return -1;
    }
    /* Second and later values of the same attribute: tag + 0x0000 name length,
     * no name bytes. See the comment in ipp.h - this is what makes PrintKit
     * accept the attribute set. */
    int repeat = (w->last_name[0] != '\0' && strcmp(w->last_name, name) == 0);
    if (repeat) {
        name_len = 0;
    } else {
        size_t c = name_len < sizeof(w->last_name) - 1 ? name_len : sizeof(w->last_name) - 1;
        memcpy(w->last_name, name, c);
        w->last_name[c] = '\0';
    }
    uint8_t hdr[5];
    hdr[0] = tag;
    wr16(&hdr[1], (uint16_t)name_len);
    if (ipp_put(w, hdr, 3) != 0) {
        return -1;
    }
    if (name_len > 0 && ipp_put(w, name, name_len) != 0) {
        return -1;
    }
    uint8_t lenhdr[2];
    wr16(lenhdr, (uint16_t)value_len);
    if (ipp_put(w, lenhdr, 2) != 0) {
        return -1;
    }
    if (value_len > 0 && ipp_put(w, value, value_len) != 0) {
        return -1;
    }
    return 0;
}

int ipp_add_string(ipp_writer_t *w, uint8_t tag, const char *name, const char *s)
{
    if (s == NULL) {
        return -1;
    }
    return ipp_add_attr(w, tag, name, (const uint8_t *)s, strlen(s));
}

int ipp_add_integer(ipp_writer_t *w, uint8_t tag, const char *name, int32_t v)
{
    uint8_t b[4];
    wr32(b, (uint32_t)v);
    return ipp_add_attr(w, tag, name, b, 4);
}

int ipp_add_bool(ipp_writer_t *w, uint8_t tag, const char *name, int v)
{
    uint8_t b = v ? 1 : 0;
    return ipp_add_attr(w, tag, name, &b, 1);
}

int ipp_resp_finish(ipp_writer_t *w)
{
    uint8_t end = IPP_TAG_END;
    return ipp_put(w, &end, 1);
}
