// ranet test app, ARM7: hosts the in-game network stack the way the card
// engine will (polled, no OS) and runs one HTTP request to RA.
#include <nds.h>
#include <string.h>
#include "ranet.h"
#include "ranet_host.h"
#include "../../common.h"

static RanetLog* s_log;

// ranet_host.h
uint32_t ranetHostTicks(void)
{
	u16 hi, lo, hi2;
	do {
		hi = TIMER_DATA(3);
		lo = TIMER_DATA(2);
		hi2 = TIMER_DATA(3);
	} while (hi != hi2);
	return (u32)hi << 16 | lo;
}

uint32_t ranetHostEntropy(void)
{
	return REG_VCOUNT ^ (ranetHostTicks() << 7) ^ REG_KEYXY;
}

uint8_t ranetHostI2cRead(uint8_t dev, uint8_t reg)
{
	return i2cReadRegister(dev, reg);
}

bool ranetHostI2cWrite(uint8_t dev, uint8_t reg, uint8_t data)
{
	return i2cWriteRegister(dev, reg, data);
}

bool ranetHostNvramRead(void* dst, u32 addr, u32 len)
{
	readFirmware(addr, dst, len);
	return true;
}

void ranetHostLog(const char* buf, unsigned size)
{
	if (!s_log) return;
	for (unsigned i = 0; i < size; i ++) {
		s_log->text[s_log->head % RANET_LOG_SIZE] = buf ? buf[i] : ' ';
		s_log->head ++;
	}
}

static volatile bool s_startPending;
static RanetStartMsg s_start;

static void startHandler(void* address, void* user)
{
	memcpy(&s_start, address, sizeof(s_start));
	s_startPending = true;
}

static void VcountHandler(void) { inputGetAndSend(); }
static volatile bool exitflag;
static void powerButtonCB(void) { exitflag = true; }

extern void dietPrintSetFuncHost(void);

int main(void)
{
	readUserSettings();
	irqInit();
	fifoInit();
	touchInit();
	SetYtrigger(80);
	installSystemFIFO();
	irqSet(IRQ_VCOUNT, VcountHandler);
	irqEnable(IRQ_VBLANK | IRQ_VCOUNT);
	setPowerButtonCB(powerButtonCB);
	fifoSetAddressHandler(FIFO_USER_01, startHandler, 0);

	// Tick counter: 33.5 MHz / 64, 32 bits over timers 2 and 3
	TIMER_CR(3) = 0;
	TIMER_CR(2) = 0;
	TIMER_DATA(2) = 0;
	TIMER_DATA(3) = 0;
	TIMER_CR(3) = TIMER_ENABLE | TIMER_CASCADE;
	TIMER_CR(2) = TIMER_ENABLE | TIMER_DIV_64;

	bool requested = false, reported = false;
	int lastState = -1;
	while (!exitflag) {
		if (s_startPending) {
			s_startPending = false;
			s_log = (RanetLog*)s_start.log;
			dietPrintSetFuncHost();
			ranetHostLog("[host] start\n", 13);
			if (!ranetStart((const RaNetProfile*)s_start.profile, (void*)s_start.arena)) {
				ranetHostLog("[host] bad profile\n", 19);
			}
			ranetSetTlsSession((const RaTlsSession*)s_start.tls);
		}
		if (s_log) s_log->beat ++;
		ranetPoll();
		int state = ranetGetState();
		// Debug: a snapshot every 3 s while not online
		static u32 lastDump;
		u32 now = ranetHostTicks();
		if (state != RanetState_Off && state < RanetState_Online && now - lastDump > 3 * 523655) {
			lastDump = now;
			ranetDebugDump();
		}
		if (state == RanetState_Online && !requested) {
			requested = ranetRequest(s_start.path, NULL, (char*)s_start.reply, s_start.replySize);
		}
		int http = 0;
		if (requested && !reported && ranetRequestDone(&http)) {
			reported = true;
			fifoSendValue32(FIFO_USER_02, state | ((u32)(http & 0xFFFF) << 8) | 0x80000000);
		} else if (state != lastState) {
			fifoSendValue32(FIFO_USER_02, state);
		}
		lastState = state;
		if (state == RanetState_Off || state == RanetState_Failed) swiWaitForVBlank();
	}
	return 0;
}
