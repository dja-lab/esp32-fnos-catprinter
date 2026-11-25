/*
 * Minimal IPP (Internet Printing Protocol) 1.1 message codec.
 * Only what AirPrint discovery + Print-Job need on a microcontroller.
 */
#ifndef IPP_H
#define IPP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPP_VERSION_1_1              0x0101
#define IPP_OP_PRINT_JOB             0x0002
#define IPP_OP_VALIDATE_JOB          0x0004
#define IPP_OP_CANCEL_JOB            0x0008
#define IPP_OP_GET_JOB_ATTRIBUTES    0x0009
#define IPP_OP_GET_JOBS              0x000A
#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000B
#define IPP_STATUS_OK                0x0000
#define IPP_STATUS_SERVER_ERROR_BUSY 0x0502

/* IPP value/group tags (RFC 8011 §4.1.3 — these values matter, a wrong tag
 * makes iOS parse the attribute as a different type and reject the printer) */
#define IPP_TAG_OPERATION       0x01
#define IPP_TAG_JOB             0x02
#define IPP_TAG_END             0x03
#define IPP_TAG_PRINTER         0x04
#define IPP_TAG_UNSUPPORTED     0x05
#define IPP_TAG_INTEGER         0x21
#define IPP_TAG_BOOLEAN         0x22
#define IPP_TAG_ENUM            0x23
#define IPP_TAG_OCTET_STRING    0x30
#define IPP_TAG_RESOLUTION      0x32
#define IPP_TAG_RANGE           0x33
#define IPP_TAG_BEGIN_COLLECTION 0x34   /* value length MUST be 0 */
#define IPP_TAG_END_COLLECTION  0x37    /* value length MUST be 0 */
#define IPP_TAG_TEXT            0x41
#define IPP_TAG_NAME            0x42
#define IPP_TAG_KEYWORD         0x44
#define IPP_TAG_URI             0x45
#define IPP_TAG_URI_SCHEME      0x46
#define IPP_TAG_CHARSET         0x47
#define IPP_TAG_NATURAL_LANG    0x48
#define IPP_TAG_MIMETYPE        0x49

typedef struct {
    uint16_t version;
    uint16_t operation_id;
    uint32_t request_id;
    size_t attrs_end;          /* offset just past end-of-attributes-tag */
    const uint8_t *document;   /* points into the original request buffer */
    size_t document_len;
} ipp_request_t;

/* Parse a request. Returns 0 on success. `buf` must stay alive while
 * `document` is used. */
int ipp_parse_request(const uint8_t *buf, size_t len, ipp_request_t *out);

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    /* RFC 8011 4.1.2: an attribute is a 1set of values and the name is written
     * ONCE - extra values carry a zero-length name. Repeating the name makes
     * Apple's PrintKit treat the reply as malformed and retry Get-Printer-
     * Attributes forever (the print sheet spins, then falls back to "select a
     * printer"). Remember the previous attribute so repeats encode correctly. */
    char last_name[40];
} ipp_writer_t;

void ipp_writer_init(ipp_writer_t *w, uint8_t *buf, size_t cap);

int ipp_resp_begin(ipp_writer_t *w, uint16_t version, uint16_t status, uint32_t request_id);
int ipp_add_group(ipp_writer_t *w, uint8_t group_tag);
int ipp_add_attr(ipp_writer_t *w, uint8_t tag, const char *name,
                 const uint8_t *value, size_t value_len);
int ipp_add_string(ipp_writer_t *w, uint8_t tag, const char *name, const char *s);
int ipp_add_integer(ipp_writer_t *w, uint8_t tag, const char *name, int32_t v);
int ipp_add_bool(ipp_writer_t *w, uint8_t tag, const char *name, int v);
int ipp_resp_finish(ipp_writer_t *w);

#ifdef __cplusplus
}
#endif
#endif
