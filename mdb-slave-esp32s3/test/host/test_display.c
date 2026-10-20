// Host test for the DISPLAY REQUEST builder (display.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../main/display.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int main(void) {
	uint8_t out[DISPLAY_PAYLOAD_LEN];

	// 1. 2 x 16 (the usual): 2 lines of 16, time in 0.1 s.
	CHECK(display_build("Fidelidade 8/15 Mais 7 = gratis ,8", 16, 2, out) == DISPLAY_PAYLOAD_LEN);
	CHECK(out[0] == 0x02 && out[1] == 80);
	CHECK(!memcmp(out + 2, "Fidelidade 8/15 Mais 7 = gratis ", 32));

	// 2. Short text is padded with spaces.
	CHECK(display_build("GANHOU!,5", 16, 2, out) == DISPLAY_PAYLOAD_LEN);
	CHECK(!memcmp(out + 2, "GANHOU!                         ", 32));

	// 3. No display declared in SETUP: nothing to send.
	CHECK(display_build("Fidelidade 8/15 Mais 7 = gratis ,8", 0, 0, out) == 0);
	CHECK(display_build("Fidelidade 8/15 Mais 7 = gratis ,8", 16, 0, out) == 0);

	// 4. One row: only the first line.
	CHECK(display_build("Fidelidade 8/15 Mais 7 = gratis ,8", 16, 1, out) == DISPLAY_PAYLOAD_LEN);
	CHECK(!memcmp(out + 2, "Fidelidade 8/15                 ", 32));

	// 5. 20 columns: each line starts a 20-char row; the rest is blank.
	CHECK(display_build("Fidelidade 8/15 Mais 7 = gratis ,8", 20, 2, out) == DISPLAY_PAYLOAD_LEN);
	CHECK(!memcmp(out + 2, "Fidelidade 8/15     Mais 7 = gra", 32));

	// 6. Malformed args and out-of-range time are rejected.
	CHECK(display_build("sem tempo", 16, 2, out) == 0);
	CHECK(display_build("texto,0", 16, 2, out) == 0);
	CHECK(display_build("texto,26", 16, 2, out) == 0);
	CHECK(display_build("texto,8x", 16, 2, out) == 0);
	CHECK(display_build(",8", 16, 2, out) == 0);

	// 7. Non-ASCII bytes become '?'; a comma inside the text is fine (last comma = time).
	CHECK(display_build("gr\xc3\xa1tis, ok,3", 16, 2, out) == DISPLAY_PAYLOAD_LEN);
	CHECK(!memcmp(out + 2, "gr??tis, ok     ", 16) && out[1] == 30);

	printf("display: todos os testes passaram\n");
	return 0;
}
