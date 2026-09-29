// What the in-game network stack needs from whoever runs it (the test app
// now, nds-bootstrap-ra's card engine later)
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Free-running counter at TICK_FREQ (33.5 MHz / 64); may wrap
uint32_t ranetHostTicks(void);

// Anything that varies (for the WPA nonce)
uint32_t ranetHostEntropy(void);

// DSi I2C (the MCU holds the WiFi reset line)
uint8_t ranetHostI2cRead(uint8_t dev, uint8_t reg);
bool ranetHostI2cWrite(uint8_t dev, uint8_t reg, uint8_t data);

// Debug text (dietPrint output), not 0-terminated when buf is set; buf
// NULL means that many spaces
void ranetHostLog(const char* buf, unsigned size);

// platform.c
void ranetPollIrq2(void);
void ranetNetbufInit(void* arena);
unsigned ranetNetbufArenaSize(void);
