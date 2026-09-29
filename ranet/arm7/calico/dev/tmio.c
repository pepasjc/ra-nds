// SPDX-License-Identifier: ZPL-2.1
// SPDX-FileCopyrightText: Copyright fincs, devkitPro
#include <calico/types.h>
#include <calico/arm/common.h>
#include <calico/dev/tmio.h>

//#define TMIO_DEBUG

#ifdef TMIO_DEBUG
#include <calico/system/dietprint.h>
#else
#define dietPrint(...) ((void)0)
#endif

#define REG_TMIO(_type,_name) (*(_type volatile*)(ctl->reg_base + TMIO_##_name))

#define REG_TMIO_CMD        REG_TMIO(u16, CMD)
#define REG_TMIO_PORTSEL    REG_TMIO(u16, PORTSEL)
#define REG_TMIO_CMDARG     REG_TMIO(u32, CMDARG)
#define REG_TMIO_CMDARGLO   REG_TMIO(u16, CMDARG+0)
#define REG_TMIO_CMDARGHI   REG_TMIO(u16, CMDARG+2)
#define REG_TMIO_STOP       REG_TMIO(u16, STOP)
#define REG_TMIO_BLKCNT     REG_TMIO(u16, BLKCNT)
#define REG_TMIO_CMDRESP    REG_TMIO(TmioResp, CMDRESP)
#define REG_TMIO_STAT       REG_TMIO(u32, STAT)
#define REG_TMIO_STATLO     REG_TMIO(u16, STAT+0)
#define REG_TMIO_STATHI     REG_TMIO(u16, STAT+2)
#define REG_TMIO_MASK       REG_TMIO(u32, MASK)
#define REG_TMIO_MASKLO     REG_TMIO(u16, MASK+0)
#define REG_TMIO_MASKHI     REG_TMIO(u16, MASK+2)
#define REG_TMIO_CLKCTL     REG_TMIO(u16, CLKCTL)
#define REG_TMIO_BLKLEN     REG_TMIO(u16, BLKLEN)
#define REG_TMIO_OPTION     REG_TMIO(u16, OPTION)
#define REG_TMIO_ERROR      REG_TMIO(u32, ERROR)
#define REG_TMIO_FIFO16     REG_TMIO(u16, FIFO16)
#define REG_TMIO_SDIOCTL    REG_TMIO(u16, SDIOCTL)
#define REG_TMIO_SDIO_STAT  REG_TMIO(u16, SDIO_STAT)
#define REG_TMIO_SDIO_MASK  REG_TMIO(u16, SDIO_MASK)
#define REG_TMIO_FIFOCTL    REG_TMIO(u16, FIFOCTL)
#define REG_TMIO_RESET      REG_TMIO(u16, RESET)
#define REG_TMIO_VERSION    REG_TMIO(u16, VERSION)
#define REG_TMIO_POWER      REG_TMIO(u16, POWER)
#define REG_TMIO_EXT_SDIO   REG_TMIO(u16, EXT_SDIO)
#define REG_TMIO_EXT_WRPROT REG_TMIO(u16, EXT_WRPROT)
#define REG_TMIO_EXT_STAT   REG_TMIO(u32, EXT_STAT)
#define REG_TMIO_EXT_MASK   REG_TMIO(u32, EXT_MASK)
#define REG_TMIO_EXT_MASKLO REG_TMIO(u16, EXT_MASK+0)
#define REG_TMIO_EXT_MASKHI REG_TMIO(u16, EXT_MASK+2)
#define REG_TMIO_CNT32      REG_TMIO(u16, CNT32)
#define REG_TMIO_BLKLEN32   REG_TMIO(u16, BLKLEN32)
#define REG_TMIO_BLKCNT32   REG_TMIO(u16, BLKCNT32)

#define ERROR_IRQ_BITS ( \
	TMIO_STAT_BAD_CMD_INDEX | \
	TMIO_STAT_BAD_CRC | \
	TMIO_STAT_BAD_STOP_BIT | \
	TMIO_STAT_DATA_TIMEOUT | \
	TMIO_STAT_RX_OVERFLOW | \
	TMIO_STAT_TX_UNDERRUN | \
	TMIO_STAT_CMD_TIMEOUT | \
	TMIO_STAT_ILL_ACCESS )

#define TX_IRQ_BITS ( \
	TMIO_STAT_CMD_RESPEND | \
	TMIO_STAT_CMD_DATAEND | \
	ERROR_IRQ_BITS )

#define PORT0_INSREM_BITS (TMIO_STAT_PORT0_REMOVE|TMIO_STAT_PORT0_INSERT)

static ThrListNode s_tmioIrqQueue;
static ThrListNode s_tmioTxEndQueue;

bool tmioInit(TmioCtl* ctl, uptr reg_base, uptr fifo_base, u32* mbox_slots, unsigned num_mbox_slots)
{
	*ctl = (TmioCtl){0};
	ctl->reg_base = reg_base;
	ctl->fifo_base = fifo_base;

	// Fast fail if the TMIO controller is either inaccessible
	// (SCFG-blocked) or no ports are present
	unsigned num_ports = TMIO_PORTSEL_NUMPORTS(REG_TMIO_PORTSEL);
	dietPrint("TMIO @ %#.8x - %u ports\n", reg_base, num_ports);
	if (num_ports == 0) {
		return false;
	}

	// Initialize dummy current port information
	ctl->cur_port.clock = 0xffff;
	ctl->cur_port.num = 0xff;
	ctl->cur_port.width = 0xff;

	// Initialize mailbox
	mailboxPrepare(&ctl->mbox, mbox_slots, num_mbox_slots);

	// Perform a soft reset. Resets the following regs:
	//   TMIO_STOP      = 0
	//   TMIO_CMDRESP   = 0
	//   TMIO_ERROR     = 0
	//   TMIO_CLKCTL   &= ~(TMIO_CLKCTL_ENABLE | TMIO_CLKCTL_UNK10)
	//   TMIO_OPTION    = 0x40ee
	//   TMIO_STAT      = 0 (everything acknowledged)
	//   TMIO_SDIO_STAT = 0
	//   Presumably:
	//     TMIO_EXT_SDIO irq bits?
	//     TMIO_EXT_STAT?
	REG_TMIO_RESET &= ~1;
	REG_TMIO_RESET |= 1;

	// Mask all interrupts
	REG_TMIO_MASKLO = ~0;
	REG_TMIO_MASKHI = ~0;
	REG_TMIO_SDIOCTL = 0; // port0 disable sdio
	REG_TMIO_SDIO_MASK = ~0;
	REG_TMIO_EXT_SDIO = 7<<8; // also: port1..3 disable sdio, acknowledge irqs
	REG_TMIO_EXT_MASKLO = ~0;
	REG_TMIO_EXT_MASKHI = ~0;

	// Initialize 32-bit FIFO
	REG_TMIO_CNT32 = TMIO_CNT32_ENABLE | TMIO_CNT32_FIFO_CLEAR;
	REG_TMIO_FIFOCTL = TMIO_FIFOCTL_MODE32;

	// Initialize options (not technically needed)
	REG_TMIO_OPTION = TMIO_OPTION_DETECT(14) | TMIO_OPTION_TIMEOUT(14) | TMIO_OPTION_NO_C2;

	return true;
}

MK_INLINE void _tmioSetPort0CardIrqEnable(TmioCtl* ctl, bool enable)
{
	if (enable) {
		REG_TMIO_SDIO_MASK &= ~1;
		REG_TMIO_SDIOCTL |= 1;
	} else {
		REG_TMIO_SDIO_MASK |= 1;
		REG_TMIO_SDIOCTL &= ~1;
	}
}

void tmioSetPortInsRemHandler(TmioCtl* ctl, unsigned port, TmioInsRemHandler isr)
{
	if (port != 0) return;
	ArmIrqState st = armIrqLockByPsr();

	ctl->port_isr[0].insrem = isr;
	if (isr) {
		REG_TMIO_MASKLO &= ~PORT0_INSREM_BITS;
	} else {
		REG_TMIO_MASKLO |= PORT0_INSREM_BITS;
	}
	REG_TMIO_STATLO = (u16)~PORT0_INSREM_BITS;

	armIrqUnlockByPsr(st);
}

void tmioSetPortCardIrqHandler(TmioCtl* ctl, unsigned port, TmioCardIrqHandler isr)
{
	if (port != 0) return;
	ArmIrqState st = armIrqLockByPsr();

	ctl->port_isr[0].cardirq = isr;
	if (!ctl->cardirq_deferred) {
		_tmioSetPort0CardIrqEnable(ctl, isr != NULL);
	}

	armIrqUnlockByPsr(st);
}

void tmioAckPortCardIrq(TmioCtl* ctl, unsigned port)
{
	if (port != 0) return;
	ArmIrqState st = armIrqLockByPsr();

	if (ctl->cardirq_deferred && !ctl->cardirq_ack_deferred) {
		if (ctl->cur_tx) {
			ctl->cardirq_ack_deferred = true;
			armIrqUnlockByPsr(st);
			dietPrint("[TMIO] Deferring SDIO IRQ ack\n");
			return;
		}

		ctl->cardirq_deferred = false;
		if (ctl->port_isr[0].cardirq != NULL) {
			REG_TMIO_SDIO_STAT = ~1;
			_tmioSetPort0CardIrqEnable(ctl, true);
		}
	}

	armIrqUnlockByPsr(st);
}

void tmioIrqHandler(TmioCtl* ctl)
{
	// Handle port0 card irq
	u16 sdiostat = ((~REG_TMIO_SDIO_MASK) & REG_TMIO_SDIO_STAT) & 1;
	if (sdiostat) {
		ctl->cardirq_deferred = true;
		_tmioSetPort0CardIrqEnable(ctl, false);
		ctl->port_isr[0].cardirq(ctl, 0);
	}

	// Handle port0 insert/remove irq
	u16 insremstat = ((~REG_TMIO_MASKLO) & REG_TMIO_STATLO) & PORT0_INSREM_BITS;
	if (insremstat) {
		REG_TMIO_STATLO = ~insremstat;
		ctl->port_isr[0].insrem(ctl, 0, (insremstat & TMIO_STAT_PORT0_INSERT) != 0);
	}

	if (ctl->num_pending_blocks) {
		bool need_xfer = false;

		// Check for transfer IRQ according to the appropriate FIFO mode
		u16 fifo16_bits = (~REG_TMIO_MASKHI) & ((TMIO_STAT_FIFO16_RECV|TMIO_STAT_FIFO16_SEND)>>16);
		if_likely (fifo16_bits == 0) {
			// Using 32-bit FIFO
			u32 fifostat = REG_TMIO_CNT32 ^ TMIO_CNT32_STAT_NOT_SEND;
			fifostat &= fifostat>>3; // mask out if IRQs are disabled
			need_xfer = (fifostat & (TMIO_CNT32_STAT_RECV|TMIO_CNT32_STAT_NOT_SEND)) != 0;
		} else {
			// Using 16-bit FIFO
			u32 fifostat = REG_TMIO_STATHI & fifo16_bits;
			need_xfer = fifostat != 0;
			if (need_xfer) {
				REG_TMIO_STATHI = ~fifostat; // Acknowledge IRQs
			}
		}

		// If above deemed it necessary, run the block transfer callback
		if_likely (need_xfer) {
			TmioTx* tx = ctl->cur_tx;
			tx->xfer_isr(ctl, tx);
			ctl->num_pending_blocks --;
		}
	}

	TmioTx* tx = ctl->cur_tx;
	if (tx) do {
		// Read transaction IRQ bits
		u32 bits = REG_TMIO_STAT & (~REG_TMIO_MASK) & TX_IRQ_BITS;

		// Update status
		REG_TMIO_STATLO = ~bits; // acknowledge bits (by writing 0)
		REG_TMIO_STATHI = (~bits)>>16;
		tx->status |= bits;

		// If errors occurred, cancel any pending block transfers
		if (tx->status & ERROR_IRQ_BITS) {
			// Cancel any pending block transfers
			ctl->num_pending_blocks = 0;
			REG_TMIO_CNT32 |= TMIO_CNT32_FIFO_CLEAR;

			// Cancel the transaction
			tx->status &= ERROR_IRQ_BITS;
		} else {
			// Read response if needed
			if (bits & TMIO_STAT_CMD_RESPEND) {
				switch (tx->type & TMIO_CMD_RESP_MASK) {
					default: break;

					case TMIO_CMD_RESP_48:
					case TMIO_CMD_RESP_48_BUSY:
					case TMIO_CMD_RESP_48_NOCRC:
						tx->resp.value[0] = REG_TMIO_CMDRESP.value[0];
						break;

					case TMIO_CMD_RESP_136: {
						TmioResp resp = REG_TMIO_CMDRESP;

						unsigned cmd_id = TMIO_CMD_INDEX(REG_TMIO_CMD);
						if_likely (cmd_id != 2 && cmd_id != 10) {
							// Regular 136-bit response, needs some shifting...
							tx->resp.value[0] = (resp.value[0]<<8);
							tx->resp.value[1] = (resp.value[1]<<8) | (resp.value[0]>>24);
							tx->resp.value[2] = (resp.value[2]<<8) | (resp.value[1]>>24);
							tx->resp.value[3] = (resp.value[3]<<8) | (resp.value[2]>>24);
						} else {
							// Special case for cmd2/cmd10 (all_get_cid/get_cid)
							tx->resp = resp;
						}

						break;
					}
				}
			}

			// Check if the command has finished
			// XX: Normally DATAEND is used to check if a block transfer, however TMIO
			//     sometimes misses asserting a DATAEND IRQ (even if it is later reflected in STAT).
			//     For this reason, use the pending block counter to track completion.
			// Also note that above is not applicable to pure DMA transfers - the user is
			// responsible for waiting for DMA to finish after a successful transaction.
			if ((tx->status & TMIO_STAT_CMD_RESPEND) && !ctl->num_pending_blocks) {
				// Command is done, clear bits in the transaction struct
				tx->status &= ERROR_IRQ_BITS;
			}
		}

		// Wake up thread if the transaction is done
		if (!(tx->status & TMIO_STAT_CMD_BUSY)) {
			threadUnblockOneByValue(&s_tmioIrqQueue, (u32)ctl);
		}
	} while (0);
}

int tmioThreadMain(TmioCtl* ctl)
{
	dietPrint("TMIO thread start %p\n", ctl);

	// Disable interrupts in this thread
	armIrqLockByPsr();

	for (;;) {
		// Receive transaction request
		TmioTx* tx = (TmioTx*)mailboxRecv(&ctl->mbox);
		ctl->cur_tx = tx;
		if_unlikely (!tx) {
			break;
		}

		unsigned cmd_id = tx->type & 0x3f;
		dietPrint("TMIO cmd%2u arg=%.8lx\n", cmd_id, tx->arg);

		// Ensure TMIO is idle
		while (REG_TMIO_STAT & TMIO_STAT_CMD_BUSY) {
			dietPrint("[TMIO] Spurious busy");
			threadSleep(1000);
		}

		// Invoke transaction callback if needed
		if (tx->callback) {
			tx->callback(ctl, tx);
		}

		// Mask SDIO card interrupt if needed
		bool has_cardirq = tx->port.num == 0 && !ctl->cardirq_deferred && ctl->port_isr[0].cardirq != NULL;
		if (has_cardirq) {
			_tmioSetPort0CardIrqEnable(ctl, false);
		}

		// Apply port number if changed
		if (ctl->cur_port.num != tx->port.num) {
			ctl->cur_port.num = tx->port.num;
			REG_TMIO_PORTSEL = (REG_TMIO_PORTSEL &~ TMIO_PORTSEL_PORT(3)) | TMIO_PORTSEL_PORT(ctl->cur_port.num);
		}

		// Apply bus width if changed
		if (ctl->cur_port.width != tx->port.width) {
			ctl->cur_port.width = tx->port.width;
			REG_TMIO_OPTION = (REG_TMIO_OPTION &~ TMIO_OPTION_BUS_WIDTH1) | (ctl->cur_port.width == 1 ? TMIO_OPTION_BUS_WIDTH1 : TMIO_OPTION_BUS_WIDTH4);
		}

		// Apply clock if changed
		if (ctl->cur_port.clock != tx->port.clock) {
			ctl->cur_port.clock = tx->port.clock;

			u16 clkmask = TMIO_CLKCTL_DIV(0xff) | TMIO_CLKCTL_AUTO;
			u16 clk0 = REG_TMIO_CLKCTL &~ TMIO_CLKCTL_ENABLE;
			u16 clk1 = (clk0 &~ clkmask) | (ctl->cur_port.clock & clkmask);
			u16 clk2 = clk1 | TMIO_CLKCTL_ENABLE;

			REG_TMIO_CLKCTL = clk0; // Disable clock
			REG_TMIO_CLKCTL = clk1; // Apply new clock settings
			REG_TMIO_CLKCTL = clk2; // Reenable clock
		}

		// GO_IDLE requires 1ms delay between applying clock and sending the command
		if (cmd_id == 0) {
			threadSleep(1000);
		}

		// Set up block transfer FIFO if needed
		u32 isr_bits = 0;
		u16 fifo16_bits = 0;
		if (tx->type & TMIO_CMD_TX) {
			u16 stop = REG_TMIO_STOP &~ (TMIO_STOP_DO_STOP|TMIO_STOP_AUTO_STOP);
			if (tx->type & TMIO_CMD_TX_MULTI) {
				stop |= TMIO_STOP_AUTO_STOP;
			}

			REG_TMIO_STOP = stop;
			REG_TMIO_BLKLEN = tx->block_size;
			REG_TMIO_BLKCNT = tx->num_blocks;
			REG_TMIO_BLKLEN32 = tx->block_size;
			REG_TMIO_BLKCNT32 = tx->num_blocks; // this is purely informational

			// Enable corresponding interrupt if block transfer callback is used
			if (tx->xfer_isr) {
				// Check if the transfer buffer/size are aligned
				bool aligned = (((uptr)tx->user | tx->block_size) & 3) == 0;

				// Select which FIFO to use
				if_likely (tx->block_size >= 0x80 && aligned) {
					// Use 32-bit FIFO
					isr_bits = (tx->type & TMIO_CMD_TX_READ) ? TMIO_CNT32_IE_RECV : TMIO_CNT32_IE_SEND;
					REG_TMIO_CNT32 |= isr_bits | TMIO_CNT32_FIFO_CLEAR;

				} else {
					// Use 16-bit FIFO (also for small transfers, see hardware bug #2 in tmio.h)
					fifo16_bits = ((tx->type & TMIO_CMD_TX_READ) ? TMIO_STAT_FIFO16_RECV : TMIO_STAT_FIFO16_SEND)>>16;
					REG_TMIO_CNT32 &= ~TMIO_CNT32_ENABLE;
					REG_TMIO_FIFOCTL = 0;
					REG_TMIO_STATHI = ~fifo16_bits;
					REG_TMIO_MASKHI &= ~fifo16_bits;
				}

				ctl->num_pending_blocks = tx->num_blocks;
			}
		}

		// Clear number of pending blocks if not using block transfer callback
		if (!isr_bits && !fifo16_bits) {
			ctl->num_pending_blocks = 0;
		}

		// Enable transaction-related interrupts
		REG_TMIO_STATLO = (u16)~TX_IRQ_BITS; // acknowledge them first just in case
		REG_TMIO_STATHI = (~TX_IRQ_BITS)>>16;
		REG_TMIO_MASKLO &= ~TX_IRQ_BITS;
		REG_TMIO_MASKHI &= (~TX_IRQ_BITS)>>16;

		// Feed command to the controller
		REG_TMIO_CMDARGLO = tx->arg;
		REG_TMIO_CMDARGHI = tx->arg >> 16;
		REG_TMIO_CMD = tx->type;

		// Wait for the command to be done processing
		threadBlock(&s_tmioIrqQueue, (u32)ctl);

		// Disable transaction-related interrupts
		REG_TMIO_MASKLO |= TX_IRQ_BITS;
		REG_TMIO_MASKHI |= TX_IRQ_BITS>>16;

		// Disable interrupt if block transfer callback is used
		if (isr_bits) {
			// Disable 32-bit FIFO IRQs
			REG_TMIO_CNT32 &= ~isr_bits;
		} else if (fifo16_bits) {
			// Disable 16-bit FIFO IRQs
			REG_TMIO_STATHI = ~fifo16_bits;
			REG_TMIO_MASKHI |= fifo16_bits;

			// Reenable 32-bit FIFO
			REG_TMIO_CNT32 |= TMIO_CNT32_ENABLE;
			REG_TMIO_FIFOCTL = TMIO_FIFOCTL_MODE32;
		}

		// Unmask SDIO card interrupt if needed
		if (has_cardirq) {
			_tmioSetPort0CardIrqEnable(ctl, true);
		}

		// Invoke transaction callback if needed
		if (tx->callback) {
			tx->callback(ctl, tx);
		}

		// Notify that we're done with this transaction
		ctl->cur_tx = NULL;

		// Handle deferred SDIO IRQ acks
		// (must be done *after* clearing ctl->cur_tx)
		if (ctl->cardirq_ack_deferred) {
			ctl->cardirq_ack_deferred = false;
			dietPrint("[TMIO] Deferred SDIO IRQ ack\n");
			tmioAckPortCardIrq(ctl, 0);
		}

		threadUnblockOneByValue(&s_tmioTxEndQueue, (u32)tx);
	}

	return 0;
}

bool tmioTransact(TmioCtl* ctl, TmioTx* tx)
{
	ArmIrqState st = armIrqLockByPsr();

	tx->status = TMIO_STAT_CMD_BUSY;
	if_unlikely (!mailboxTrySend(&ctl->mbox, (u32)tx)) {
		tx->status = TMIO_STAT_RX_OVERFLOW;
		armIrqUnlockByPsr(st);
		return false;
	}

	if_likely (tx->status & TMIO_STAT_CMD_BUSY) {
		threadBlock(&s_tmioTxEndQueue, (u32)tx);
	}

	armIrqUnlockByPsr(st);
	return tx->status == 0;
}

void tmioThreadCancel(TmioCtl* ctl)
{
	while (!mailboxTrySend(&ctl->mbox, 0)) {
		threadSleep(1000);
	}
}

typedef union _TmioXferBuf {
	u32* buf32;
	u8* buf8;
	void* buf;
} _TmioXferBuf;

void tmioXferRecvByCpu(TmioCtl* ctl, TmioTx* tx)
{
	_TmioXferBuf u = { tx->user };

	u16 fifo16_bits = (~REG_TMIO_MASKHI) & (TMIO_STAT_FIFO16_RECV>>16);
	if_likely (fifo16_bits == 0) {
		// 32-bit FIFO ("fast" path)
		vu32* fifo = (vu32*)ctl->fifo_base;
		for (u32 i = 0; i < tx->block_size; i += 4) {
			*u.buf32++ = *fifo;
		}
	} else {
		// 16-bit FIFO (slow path)
		for (u32 i = 0; i < tx->block_size; i += 2) {
			u16 data = REG_TMIO_FIFO16;
			*u.buf8++ = data & 0xff;
			if ((i+1) < tx->block_size) {
				*u.buf8++ = (data >> 8);
			}
		}
	}

	tx->user = u.buf;
}

void tmioXferSendByCpu(TmioCtl* ctl, TmioTx* tx)
{
	_TmioXferBuf u = { tx->user };

	u16 fifo16_bits = (~REG_TMIO_MASKHI) & (TMIO_STAT_FIFO16_SEND>>16);
	if_likely (fifo16_bits == 0) {
		// 32-bit FIFO ("fast" path)
		vu32* fifo = (vu32*)ctl->fifo_base;
		for (u32 i = 0; i < tx->block_size; i += 4) {
			*fifo = *u.buf32++;
		}
	} else {
		// 16-bit FIFO (slow path)
		for (u32 i = 0; i < tx->block_size; i += 2) {
			u16 data = *u.buf8++;
			if ((i+1) < tx->block_size) {
				data |= (*u.buf8++) << 8;
			}
			REG_TMIO_FIFO16 = data;
		}
	}

	tx->user = u.buf;
}

unsigned tmioDecodeTranSpeed(u8 tran_speed)
{
	// https://android.googlesource.com/device/google/accessory/adk2012/+/e0f114ab1d645caeac2c30273d0b693d72063f54/MakefileBasedBuild/Atmel/sam3x/sam3x-ek/libraries/memories/sdmmc/sdmmc.c#212
	static const u16 units[] = { 10, 100, 1000, 10000 }; // in 10Kb/s units
	static const u8 multipliers[] = { 0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80 }; // in 1/10 units
	u8 unit = tran_speed & 7;
	u8 multiplier = (tran_speed >> 3) & 0xF;
	if (unit < 4) {
		return 1000*units[unit]*multipliers[multiplier];
	}

	return 0;
}
