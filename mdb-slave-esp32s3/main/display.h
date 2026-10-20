#pragma once
// MDB cashless DISPLAY REQUEST (POLL response 0x02): asks the VMC to show a text
// for a while. The "display" RPC sends "<32 chars = 2 lines of 16>,<seconds>".
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_TEXT_LEN    32   // MDB: display data is always 32 bytes
#define DISPLAY_PAYLOAD_LEN (2 + DISPLAY_TEXT_LEN)

// Builds [0x02, time in 0.1 s, 32 display bytes] from the RPC args, laid out in
// rows of `columns` characters (the VMC's display, from SETUP CONFIG_DATA).
// Returns DISPLAY_PAYLOAD_LEN, or 0 if the VMC declared no display
// (columns or rows 0) or the args are malformed.
int display_build(const char *args, uint8_t columns, uint8_t rows, uint8_t *out);
