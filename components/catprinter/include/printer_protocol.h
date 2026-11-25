/*
 * Cat printer binary protocol - pure C, no ESP-IDF dependency.
 * Byte-for-byte compatible with NaitLee/Cat-Printer printer_lib/commander.py (CC0-1.0).
 *
 * Frame layout (from make_command):
 *   51 78 | cmd | 00 | payload_size(1B) | 00 | payload... | crc8(payload) | FF
 * NOTE: payload_size is ONE byte (<=255), followed by a separate 0x00 byte.
 *       CRC covers ONLY the payload, not the header/cmd.
 */
#ifndef PRINTER_PROTOCOL_H
#define PRINTER_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PP_FRAME_H0        0x51
#define PP_FRAME_H1        0x78
#define PP_FRAME_TAIL      0xFF
#define PP_ROW_BYTES       48u              /* 384 dots / 8, GB01-class paper */
#define PP_FRAME_MAX       (6u + 255u + 2u) /* 263 bytes worst case */

/* Command bits (from commander.py) */
#define PP_CMD_RETRACT       0xA0
#define PP_CMD_FEED          0xA1
#define PP_CMD_DRAW_BITMAP   0xA2
#define PP_CMD_GET_STATE     0xA3
#define PP_CMD_SET_DPI       0xA4
#define PP_CMD_GET_INFO      0xA8
#define PP_CMD_UPDATE_DEVICE 0xA9
#define PP_CMD_LATTICE       0xA6
#define PP_CMD_SET_ENERGY    0xAF
#define PP_CMD_APPLY_ENERGY  0xBE
#define PP_CMD_SET_SPEED     0xBD

/* Flow-control notifications sent BY the printer on 0xAE02 */
extern const uint8_t PP_FLOW_PAUSE[9];
extern const uint8_t PP_FLOW_RESUME[9];
#define PP_FLOW_LEN 9

/* CRC-8 poly 0x07, init 0, table-driven - identical to Python crc8_table */
uint8_t pp_crc8(const uint8_t *data, size_t len);

/* Reverse bits in one byte (PBM P4 MSB-first -> printer LSB-first) */
uint8_t pp_reverse_bits(uint8_t b);

/* Generic frame builder. Returns frame length, or 0 on error. */
size_t pp_make_command(uint8_t *out, size_t out_cap,
                       uint8_t cmd, const uint8_t *payload, size_t payload_len);

/* Convenience command builders. Each returns frame length, or 0 on error. */
size_t pp_cmd_get_state(uint8_t *out, size_t cap);
size_t pp_cmd_get_info(uint8_t *out, size_t cap);
size_t pp_cmd_update_device(uint8_t *out, size_t cap);
/* start_printing: plain frame; new_kind=true prepends 0x12 as in start_printing_new() */
size_t pp_cmd_start_printing(uint8_t *out, size_t cap, int new_kind);
size_t pp_cmd_set_dpi_200(uint8_t *out, size_t cap);
size_t pp_cmd_set_speed(uint8_t *out, size_t cap, uint8_t speed);
/* energy is 0..0xffff; 0x6000 is the Python default for text */
size_t pp_cmd_set_energy(uint8_t *out, size_t cap, uint16_t energy);
size_t pp_cmd_apply_energy(uint8_t *out, size_t cap);
size_t pp_cmd_feed(uint8_t *out, size_t cap, uint16_t pixels);
size_t pp_cmd_retract(uint8_t *out, size_t cap, uint16_t pixels);
size_t pp_cmd_start_lattice(uint8_t *out, size_t cap);
size_t pp_cmd_end_lattice(uint8_t *out, size_t cap);
/* One 48-byte raster row; bit-reversal is applied internally. */
size_t pp_cmd_draw_row(uint8_t *out, size_t cap, const uint8_t *row);

#ifdef __cplusplus
}
#endif
#endif
