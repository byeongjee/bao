#ifndef SETUP_H
#define SETUP_H

#include <stdint.h>

#define INLINE static inline
#define DEBUG_OUT_STR(...)
#define DEBUG_OUT_U16(x)
#define DEBUG_OUT_HEX(x)
#define DEBUG_OUT_CHAR(c)

// GPIO register stubs for MSP430 compatibility
static volatile uint8_t P1OUT;
static volatile uint8_t P1DIR;
static volatile uint8_t P4OUT;
static volatile uint8_t P4DIR;

static inline void initialize(void) {}
static inline void __enable_interrupt(void) {}
static inline void begin_measurement_window(void) {}
static inline void end_measurement_window(void) {}
static inline void begin_event(void) {}
static inline void end_event(void) {}
static inline void delay(int x) {}

#endif
