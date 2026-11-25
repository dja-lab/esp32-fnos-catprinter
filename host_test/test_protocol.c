/*
 * Host-side protocol test. Build: make -C host_test test
 * Verifies the C implementation against the Python reference byte-for-byte.
 */
#include "printer_protocol.h"

#include <stdio.h>
#include <string.h>

static int s_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", (msg), __LINE__); s_fail++; } \
} while (0)

/* Independent polynomial implementation - cross-checks the CRC table. */
static uint8_t crc_poly_ref(const uint8_t *d, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 0x80u) ? (uint8_t)((c << 1) ^ 0x07u) : (uint8_t)(c << 1);
        }
    }
    return c;
}

static void dump(const char *label, const uint8_t *b, size_t n)
{
    printf("  %s (%zu): ", label, n);
    for (size_t i = 0; i < n; i++) printf("%02x ", b[i]);
    printf("\n");
}

int main(void)
{
    uint8_t out[PP_FRAME_MAX];
    size_t n;

    /* 1. CRC table must equal polynomial implementation over varied data */
    for (size_t len = 0; len <= 64; len++) {
        uint8_t buf[64];
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(i * 37 + len);
        CHECK(pp_crc8(buf, len) == crc_poly_ref(buf, len), "crc8 table == poly");
    }
    CHECK(pp_crc8((const uint8_t *)"", 0) == 0, "crc8(empty) == 0");
    CHECK(pp_crc8((const uint8_t *)"\x01", 1) == 0x07, "crc8(0x01) == 0x07");

    /* 2. start_printing() exact bytes from commander.py */
    const uint8_t expected_start[9] = {0x51,0x78,0xa3,0x00,0x01,0x00,0x00,0x00,0xff};
    n = pp_cmd_start_printing(out, sizeof(out), 0);
    CHECK(n == 9, "start_printing length == 9");
    CHECK(n == 9 && memcmp(out, expected_start, 9) == 0, "start_printing bytes");
    dump("start_printing", out, n);

    /* 3. start_printing_new() prepends 0x12 */
    n = pp_cmd_start_printing(out, sizeof(out), 1);
    CHECK(n == 10, "start_printing_new length == 10");
    CHECK(n == 10 && out[0] == 0x12 && memcmp(&out[1], expected_start, 9) == 0,
          "start_printing_new prefix 0x12");

    /* 4. flow control notifications */
    const uint8_t exp_pause[9]  = {0x51,0x78,0xae,0x01,0x01,0x00,0x10,0x70,0xff};
    const uint8_t exp_resume[9] = {0x51,0x78,0xae,0x01,0x01,0x00,0x00,0x00,0xff};
    CHECK(memcmp(PP_FLOW_PAUSE, exp_pause, 9) == 0, "flow pause bytes");
    CHECK(memcmp(PP_FLOW_RESUME, exp_resume, 9) == 0, "flow resume bytes");

    /* 5. bit reversal */
    CHECK(pp_reverse_bits(0x80) == 0x01, "reverse 0x80 -> 0x01");
    CHECK(pp_reverse_bits(0x01) == 0x80, "reverse 0x01 -> 0x80");
    CHECK(pp_reverse_bits(0xf0) == 0x0f, "reverse 0xf0 -> 0x0f");
    CHECK(pp_reverse_bits(0x55) == 0xaa, "reverse 0x55 -> 0xaa");

    /* 6. set_energy(0x6000) -> little-endian payload {00 60} */
    n = pp_cmd_set_energy(out, sizeof(out), 0x6000);
    CHECK(n == 10, "set_energy length == 10");
    CHECK(n == 10 && out[2] == PP_CMD_SET_ENERGY && out[4] == 2 && out[5] == 0,
          "set_energy header");
    CHECK(n == 10 && out[6] == 0x00 && out[7] == 0x60, "set_energy LE payload 00 60");
    CHECK(n == 10 && out[8] == crc_poly_ref(&out[6], 2), "set_energy CRC covers payload only");
    CHECK(n == 10 && out[9] == 0xff, "set_energy tail");

    /* 7. feed_paper(128) -> payload {80 00} */
    n = pp_cmd_feed(out, sizeof(out), 128);
    CHECK(n == 10 && out[2] == PP_CMD_FEED && out[6] == 0x80 && out[7] == 0x00,
          "feed_paper LE payload 80 00");

    /* 8. draw_row: 48-byte payload, each byte bit-reversed */
    uint8_t row[PP_ROW_BYTES];
    for (size_t i = 0; i < PP_ROW_BYTES; i++) row[i] = (uint8_t)i;
    n = pp_cmd_draw_row(out, sizeof(out), row);
    CHECK(n == 8 + PP_ROW_BYTES, "draw_row length == 56");
    CHECK(n == 56 && out[0] == 0x51 && out[1] == 0x78 &&
          out[2] == PP_CMD_DRAW_BITMAP && out[4] == PP_ROW_BYTES && out[55] == 0xff,
          "draw_row header/tail");
    if (n == 56) {
        int ok = 1;
        for (size_t i = 0; i < PP_ROW_BYTES; i++) {
            if (out[6 + i] != pp_reverse_bits(row[i])) { ok = 0; break; }
        }
        CHECK(ok, "draw_row payload is bit-reversed");
        CHECK(out[54] == crc_poly_ref(&out[6], PP_ROW_BYTES), "draw_row CRC covers payload only");
    }

    /* 9. lattice frames: 11-byte payload */
    n = pp_cmd_start_lattice(out, sizeof(out));
    CHECK(n == 19 && out[2] == PP_CMD_LATTICE && out[4] == 11 && out[6] == 0xaa && out[7] == 0x55,
          "start_lattice frame");
    n = pp_cmd_end_lattice(out, sizeof(out));
    CHECK(n == 19 && out[2] == PP_CMD_LATTICE && out[4] == 11 &&
          out[6] == 0xaa && out[7] == 0x55 && out[16] == 0x17, "end_lattice frame");

    /* 10. DPI and generic builder boundary */
    n = pp_cmd_set_dpi_200(out, sizeof(out));
    CHECK(n == 9 && out[2] == PP_CMD_SET_DPI && out[6] == 50, "set_dpi_as_200 payload 50");
    CHECK(pp_make_command(out, 8, 0xa2, row, 48) == 0, "undersized buffer rejected");
    {
        uint8_t big[300];
        memset(big, 0, sizeof(big));
        CHECK(pp_make_command(out, sizeof(out), 0xa2, big, 256) == 0, "payload >255 rejected");
    }

    if (s_fail == 0) {
        printf("ALL PROTOCOL TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", s_fail);
    return 1;
}
