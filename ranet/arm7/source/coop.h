// Cooperative threads for the calico driver code (coop.c)
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Memory for the threads' stacks (split evenly, 8-byte aligned)
void coopInit(void* mem, uint32_t size);

// Runs the threads that can run; the number that ran
unsigned coopRun(void);

// Debug: logs every thread's state
void coopDump(void);

// Every started thread has returned
bool coopAllDone(void);
