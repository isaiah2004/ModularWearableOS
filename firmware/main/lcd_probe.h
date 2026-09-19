#ifndef SMARTWATCH_LCD_PROBE_H
#define SMARTWATCH_LCD_PROBE_H

#include <stdint.h>

uint8_t board_lcd_probe(void);

/* Cache-only JSON report; safe after the panel owns the SPI bus. */
void board_lcd_probe_print(void);

#endif
