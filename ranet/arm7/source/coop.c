// Cooperative stand-ins for the calico kernel calls the DSi WiFi driver and
// sgIP make (threads, blocking queues, sleep, mailboxes, mutexes).  Where
// the driver runs (a card engine inside a game) there is no scheduler: the
// host calls coopRun() now and then, which runs every thread that can run
// until each blocks again.  A blocked thread just yields back to coopRun();
// "unblocking" only marks it runnable, so interrupt handlers may do it.
#include <calico/types.h>
#include <calico/system/thread.h>
#include <calico/system/mailbox.h>
#include <calico/system/mutex.h>
#include <calico/system/tick.h>
#include <calico/system/dietprint.h>
#include "coop.h"

typedef enum { CoFree, CoReady, CoSleep, CoBlocked, CoWaiting, CoDone } CoState;

typedef struct Co {
	Thread* t;
	ThreadFunc fn;
	void* arg;
	u32* sp;               // saved stack pointer while switched out
	vu32 state;            // CoState
	ThrListNode* queue;    // CoBlocked: queue and token
	u32 token;
	vu32 unblocked;
	u64 wake;              // CoSleep: tick
	int rc;
} Co;

#define CO_MAX 6
static Co s_co[CO_MAX];
static Co* s_cur;
static u32* s_hostSp;

// Each thread's stack, from coopInit(): not the one the caller passes.
// Interrupts that arrive while a thread runs use its stack too (the host's
// or the game's handlers, in system mode), and calico's own stacks are
// sized for its kernel, which gives interrupts a stack of their own.
static u8* s_stackMem;
static u32 s_stackSize; // per thread

void coopInit(void* mem, u32 size)
{
	s_stackMem = (u8*)mem;
	s_stackSize = (size / CO_MAX) & ~7;
}

ThrSchedState __sched_state;

// coop_switch.s: saves r4-r11/lr on the current stack, stores sp in *save,
// switches to next and restores its registers
extern void coopSwitch(u32** save, u32* next);
extern u32 coopCpsr(void);

static Co* coFind(Thread* t)
{
	for (int i = 0; i < CO_MAX; i ++) {
		if (s_co[i].t == t && s_co[i].state != CoFree) return &s_co[i];
	}
	return NULL;
}

// Back to coopRun(); returns when coopRun() picks this thread again
static void coYield(void)
{
	Co* self = s_cur;
	coopSwitch(&self->sp, s_hostSp);
}

static void coEntry(void)
{
	Co* self = s_cur;
	self->rc = self->fn(self->arg);
	self->state = CoDone;
	for (;;) coYield();
}

void threadPrepare(Thread* t, ThreadFunc entrypoint, void* arg, void* stack_top, u8 prio)
{
	(void)prio;
	Co* c = NULL;
	for (int i = 0; i < CO_MAX && !c; i ++) {
		if (s_co[i].state == CoFree || s_co[i].t == t) c = &s_co[i];
	}
	if (!c) return;
	c->t = t;
	c->fn = entrypoint;
	c->arg = arg;
	c->queue = NULL;
	c->unblocked = 0;
	c->rc = 0;
	// Initial frame for coopSwitch: cpsr (interrupts on), r4-r11, then
	// lr = coEntry
	(void)stack_top;
	u32* sp = (u32*)(s_stackMem + (c - s_co + 1) * s_stackSize);
	*--sp = (u32)coEntry;
	for (int i = 0; i < 8; i ++) *--sp = 0;
	*--sp = coopCpsr() & ~0xC0; // I and F clear
	c->sp = sp;
	c->state = CoWaiting; // until threadStart()
	t->status = ThrStatus_Uninitialized;
}

void threadStart(Thread* t)
{
	Co* c = coFind(t);
	if (c) {
		c->state = CoReady;
		t->status = ThrStatus_Running;
	}
}

int threadJoin(Thread* t)
{
	Co* c = coFind(t);
	if (!c) return 0;
	while (c->state != CoDone) {
		if (s_cur) coYield(); else coopRun();
	}
	c->state = CoFree;
	t->status = ThrStatus_Finished;
	return c->rc;
}

void threadYield(void)
{
	if (s_cur) coYield();
}

void threadSleepTicks(u32 ticks)
{
	if (!s_cur) return;
	s_cur->wake = tickGetCount() + ticks;
	s_cur->state = CoSleep;
	coYield();
}

// A queue's head points at one of its waiters while it has any: calico's
// code checks "queue.next" before waking anyone (ar6k_htc.c credits)
static void updateQueueHead(ThrListNode* queue)
{
	Thread* waiter = NULL;
	for (int i = 0; i < CO_MAX && !waiter; i ++) {
		if (s_co[i].state == CoBlocked && s_co[i].queue == queue && !s_co[i].unblocked) waiter = s_co[i].t;
	}
	queue->next = queue->prev = waiter;
}

u32 threadBlock(ThrListNode* queue, u32 token)
{
	Co* self = s_cur;
	if (!self) return 0;
	self->queue = queue;
	self->token = token;
	self->unblocked = 0;
	self->state = CoBlocked;
	updateQueueHead(queue);
	coYield();
	self->queue = NULL;
	updateQueueHead(queue);
	return 1;
}

static void unblock(ThrListNode* queue, u32 ref, bool all)
{
	for (int i = 0; i < CO_MAX; i ++) {
		Co* c = &s_co[i];
		if (c->state == CoBlocked && c->queue == queue && c->token == ref && !c->unblocked) {
			c->unblocked = 1;
			if (!all) break;
		}
	}
	updateQueueHead(queue);
}

void threadUnblockOneByValue(ThrListNode* queue, u32 ref) { unblock(queue, ref, false); }
void threadUnblockAllByValue(ThrListNode* queue, u32 ref) { unblock(queue, ref, true); }

void threadBlockCancel(ThrListNode* queue, Thread* t)
{
	Co* c = coFind(t);
	if (c && c->state == CoBlocked && c->queue == queue) c->unblocked = 1;
}

// Mailboxes: a ring of slots; receivers poll
bool mailboxTrySend(Mailbox* mb, u32 message)
{
	if (mb->pending_slots >= mb->num_slots) return false;
	unsigned slot = mb->cur_slot + mb->pending_slots;
	if (slot >= mb->num_slots) slot -= mb->num_slots;
	mb->slots[slot] = message;
	mb->pending_slots ++;
	return true;
}

bool mailboxTryRecv(Mailbox* mb, u32* out)
{
	if (!mb->pending_slots) return false;
	*out = mb->slots[mb->cur_slot];
	if (++mb->cur_slot >= mb->num_slots) mb->cur_slot = 0;
	mb->pending_slots --;
	return true;
}

u32 mailboxRecv(Mailbox* mb)
{
	u32 msg;
	while (!mailboxTryRecv(mb, &msg)) threadYield();
	return msg;
}

// Mutexes: only ever contended across a yield
bool mutexTryLock(Mutex* m)
{
	if (m->owner) return false;
	m->owner = threadGetSelf();
	return true;
}

void mutexLock(Mutex* m)
{
	while (m->owner && m->owner != threadGetSelf()) threadYield();
	m->owner = threadGetSelf();
}

void mutexUnlock(Mutex* m)
{
	m->owner = NULL;
}

// Runs every thread that can run, each until it blocks, sleeps or yields;
// a few rounds, so that a chain of wake-ups finishes in one call.
// Returns how many threads ran.
unsigned coopRun(void)
{
	if (s_cur) return 0; // not from inside a thread
	unsigned ran = 0;
	for (int round = 0; round < 4; round ++) {
		bool any = false;
		u64 now = tickGetCount();
		for (int i = 0; i < CO_MAX; i ++) {
			Co* c = &s_co[i];
			bool go = c->state == CoReady
				|| (c->state == CoSleep && now >= c->wake)
				|| (c->state == CoBlocked && c->unblocked);
			if (!go) continue;
			c->state = CoReady;
			s_cur = c;
			__sched_state.cur = c->t;
			coopSwitch(&s_hostSp, c->sp);
			s_cur = NULL;
			__sched_state.cur = NULL;
			if (c->state == CoReady) {
				// threadYield(): runs again next round
			}
			any = true;
			ran ++;
		}
		if (!any) break;
	}
	return ran;
}

// Debug: every thread's state (0 free, 1 ready, 2 sleep, 3 blocked,
// 4 not started, 5 done), where it blocks and its saved sp
void coopDump(void)
{
	for (int i = 0; i < CO_MAX; i ++) {
		Co* c = &s_co[i];
		if (c->state == CoFree) continue;
		dietPrint("[co] %d st%lu q%p t%lx u%lu sp%p\n", i, (unsigned long)c->state, c->queue,
			(unsigned long)c->token, (unsigned long)c->unblocked, c->sp);
	}
}

bool coopAllDone(void)
{
	for (int i = 0; i < CO_MAX; i ++) {
		if (s_co[i].state != CoFree && s_co[i].state != CoDone) return false;
	}
	return true;
}
