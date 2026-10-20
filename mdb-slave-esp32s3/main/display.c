#include "display.h"

#include <stdlib.h>
#include <string.h>

int display_build(const char *args, uint8_t columns, uint8_t rows, uint8_t *out) {
	if (args == NULL || columns == 0 || rows == 0) return 0;

	// "<text>,<seconds>": the last comma separates the time (the text may have commas).
	const char *comma = strrchr(args, ',');
	if (comma == NULL || comma == args) return 0;
	char *end;
	long seconds = strtol(comma + 1, &end, 10);
	if (end == comma + 1 || *end != '\0' || seconds < 1 || seconds > 25) return 0;

	size_t text_len = (size_t) (comma - args);
	if (text_len > DISPLAY_TEXT_LEN) text_len = DISPLAY_TEXT_LEN;

	out[0] = 0x02;                      // DISPLAY REQUEST
	out[1] = (uint8_t) (seconds * 10);  // display time, 0.1 s units
	memset(out + 2, ' ', DISPLAY_TEXT_LEN);

	// The server sends 2 lines of 16; each one starts a row of `columns` characters.
	for (size_t line = 0; line < 2; line++) {
		size_t row_start = line * (size_t) columns;
		if (line >= rows || row_start >= DISPLAY_TEXT_LEN) break;
		for (size_t i = 0; i < 16 && i < columns && row_start + i < DISPLAY_TEXT_LEN; i++) {
			size_t src = line * 16 + i;
			char c = src < text_len ? args[src] : ' ';
			out[2 + row_start + i] = (c >= 0x20 && c <= 0x7e) ? (uint8_t) c : '?';   // displays are ASCII
		}
	}
	return DISPLAY_PAYLOAD_LEN;
}
