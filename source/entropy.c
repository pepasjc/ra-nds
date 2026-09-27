// Entropy for mbedTLS's CTR_DRBG on a DSi.
//
// The DSi has no random number generator the ARM9 can read, so this mixes
// sources that differ from run to run and within a run: a free-running
// 33 MHz timer sampled around busy loops of varying length (the ARM9 and
// memory timing jitter against it), the scanline counter, the RTC and the
// time since boot.  Proof-of-concept grade: fine for the ephemeral keys of a
// TLS client, not for generating long-term secrets.
#include <nds.h>
#include <string.h>
#include <time.h>

static bool timer_started = false;

static void start_timer(void) {
    // Timers 2+3 cascaded: a 32-bit counter at the bus clock
    TIMER_DATA(2) = 0;
    TIMER_DATA(3) = 0;
    TIMER_CR(3) = TIMER_ENABLE | TIMER_CASCADE;
    TIMER_CR(2) = TIMER_ENABLE | TIMER_DIV_1;
    timer_started = true;
}

static u32 sample(void) {
    return (u32)TIMER_DATA(2) | ((u32)TIMER_DATA(3) << 16);
}

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    if (!timer_started) start_timer();

    u32 mix = (u32)time(NULL) ^ (REG_VCOUNT << 16);
    for (size_t i = 0; i < len; i++) {
        // A busy loop whose length depends on what came before
        volatile u32 spin = (mix & 0x3F) + 8;
        while (spin--) {}
        u32 t = sample();
        mix = (mix << 5 | mix >> 27) ^ t ^ (REG_VCOUNT << 11);
        output[i] = (unsigned char)(mix ^ (mix >> 8) ^ (mix >> 16) ^ (mix >> 24));
    }
    *olen = len;
    return 0;
}
