#ifndef SGL_IPC_H
#define SGL_IPC_H

#include "Frontends/DbiIpcCommon.h"
#include "global.h"

/* An implementation of interprocess communication with the Sigil2 frontend.
 * IPC includes initialization, termination, shared memory buffer writes, and
 * synchronization via named pipes */

typedef struct EventPoolSlotTuple
{
    BufferedSglEv* event_slot;
    char*          pool_slot;
    UInt           pool_idx;
} EventPoolSlotTuple;

void SGL_(init_IPC)(void);
void SGL_(term_IPC)(void);

BufferedSglEv*     SGL_(acq_event_slot)(void);
EventPoolSlotTuple SGL_(acq_event_pool_slot)(UInt size);

#endif
