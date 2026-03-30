# Architecture & Design Overview

## System Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                    PROCESS MANAGER SIMULATOR                     │
└─────────────────────────────────────────────────────────────────┘
                                 │
                    ┌────────────┼────────────┐
                    │            │            │
            ┌───────▼────┐ ┌────▼───────┐ ┌─▼──────────┐
            │   Worker   │ │   Worker   │ │   Monitor  │
            │  Thread 1  │ │  Thread 2  │ │   Thread   │
            └────────────┘ └────────────┘ └────────────┘
                    │            │            │
                    └────────────┼────────────┘
                                 │
                    ┌────────────▼────────────┐
                    │   Process Table (64)    │
                    │   ┌──────────────────┐  │
                    │   │   table_lock     │  │
                    │   │ table_changed CV │  │◄───── Global Sync
                    │   └──────────────────┘  │
                    │                         │
                    │   ┌─────────────────┐   │
                    │   │ PCB[0] (PID 1)  │   │
                    │   │ Init Process    │   │
                    │   └─────────────────┘   │
                    │   ┌─────────────────┐   │
                    │   │ PCB[1] (PID 2)  │   │
                    │   │ Child of init   │   │
                    │   │ ┌─────────────┐ │   │
                    │   │ │ pcb_lock    │◄│───── Per-Process Sync
                    │   │ │state_changed│ │   │
                    │   │ └─────────────┘ │   │
                    │   └─────────────────┘   │
                    │   ...                   │
                    │   ┌─────────────────┐   │
                    │   │ PCB[63]         │   │
                    │   └─────────────────┘   │
                    └─────────────────────────┘
                                 │
                    ┌────────────┼────────────┐
                    │            │            │
            ┌───────▼────────────▼──────┐ ┌─▼──────────┐
            │   snapshots.txt            │ │ Console    │
            │   (Change-Driven Snapshots)│ │ Output     │
            └────────────────────────────┘ └────────────┘
```

## Thread Architecture

### Worker Threads

```
Worker Thread N
│
├─ Read command file (commands_N.txt)
│
├─ For each line:
│  ├─ Parse command
│  ├─ Execute pm_* function
│  │  ├─ Acquire table_lock
│  │  ├─ Check validity
│  │  ├─ Update table state
│  │  ├─ Release table_lock
│  │  └─ Optionally wait on condition variable
│  │
│  └─ Report result (printf to stdout)
│
└─ Exit worker thread
```

### Monitor Thread

```
Monitor Thread
│
├─ Initialize (write header to snapshots.txt)
│
├─ Loop while running:
│  │
│  ├─ Acquire table_lock
│  │
│  ├─ Wait on table_changed condition
│  │  └─ until signal received from worker
│  │
│  ├─ Read current table state (locked)
│  │
│  ├─ Release table_lock
│  │
│  ├─ Write snapshot to snapshots.txt
│  │  └─ Process table dump (PID, PPID, STATE, EXIT_STATUS)
│  │
│  └─ [Back to wait]
│
└─ Write final snapshot & exit
```

## Synchronization Model

### Lock Hierarchy (Prevents Deadlock)

```
Level 1: Global Lock
┌──────────────────────────────────────────┐
│ pthread_mutex_t table_lock               │
│ Protects: PCB array, num_processes       │
│ Acquired: First, always                  │
│ Held: Brief, for table modifications     │
└──────────────────────────────────────────┘
         │
         └──────────────────────────────────┐
                                            │
Level 2: Per-Process Locks                  │
┌────────────────────────────────────────────▼──┐
│ [PCB[0]]  [PCB[1]]  [PCB[2]]  ...  [PCB[63]] │
│ ┌──────┐  ┌──────┐  ┌──────┐      ┌──────┐ │
│ │ Lock │  │ Lock │  │ Lock │ ...  │ Lock │ │
│ │  CV  │  │  CV  │  │  CV  │      │  CV  │ │
│ └──────┘  └──────┘  └──────┘      └──────┘ │
│ Protects: pid, state, exit_status, etc.   │
│ Acquired: After table_lock only            │
│ Held: During state modifications           │
└─────────────────────────────────────────────┘
```

### Condition Variable Signaling

```
Event 1: Process State Change
┌─────────────────────────┐
│ Process E exists        │
│ Changes: state→ZOMBIE   │
│ Broadcasts: state_changed
└─────────────────────────┘
           │
           ▼
┌─────────────────────────────────────────┐
│ Parent P waiting on:                     │
│ pm_wait(P, E):                           │
│   while (E.state != ZOMBIE)              │
│     pthread_cond_wait(&E.state_changed)  │
└─────────────────────────────────────────┘
           │
           ▼
    Parent wakes up,
    reaps E (E→TERMINATED)


Event 2: Table Change
┌──────────────────────────────┐
│ Worker forks new process     │
│ Modifies: process_table      │
│ Broadcasts: table_changed    │
└──────────────────────────────┘
           │
           ▼
┌──────────────────────────────────┐
│ Monitor thread wakes from:        │
│ pthread_cond_wait(&table_changed) │
└──────────────────────────────────┘
           │
           ▼
  Write new snapshot to file
```

## Process Lifecycle State Machine

```
                         ┌──────────┐
                         │  CREATE  │
                         └────┬─────┘
                              │
                              ▼
                    ┌─────────────────────┐
                    │      RUNNING        │◄─── Initial state after fork
                    └───────┬─────┬───────┘
                            │     │
           wait(no zombie)  │     │ exit()/kill()
                            │     │
                            ▼     ▼
                      ┌────────┐ ┌────────┐
                      │BLOCKED │ │ ZOMBIE │
                      └────┬───┘ └───┬────┘
                           │         │
             child exits ->│         │ pm_wait() reaps
                           ▼         ▼
                       ┌─────────────────────┐
                       │      RUNNING        │
                       └─────────────────────┘
                                     │
                                     ▼
                               ┌─────────────┐
                               │ TERMINATED  │ ◄─── Removed from table
                               │(Reaped)     │
                               └─────────────┘
```

## Command Execution Flow

```
┌─────────────────────────────────────────────────┐
│ File: commands.txt                              │
│ fork 1                                          │
│ sleep 100                                       │
│ exit 2 42                                       │
│ wait 1 -1                                       │
│ ps                                              │
└────────────┬────────────────────────────────────┘
             │
             ▼ (Worker Thread reads)
┌────────────────────────────────────────────────┐
│1. Parse "fork 1"                                │
│   Acquire table_lock                            │
│   Find parent (PID 1)                           │
│   Allocate new PCB (PID 2)                      │
│   Add to parent's children list                 │
│   Broadcast table_changed                       │
│   Release table_lock                            │
└────────────┬────────────────────────────────────┘
             │
             ▼
┌────────────────────────────────────────────────┐
│2. Parse "sleep 100"                             │
│   Call usleep(100000) - block for 100ms        │
└────────────┬────────────────────────────────────┘
             │
             ▼
┌────────────────────────────────────────────────┐
│3. Parse "exit 2 42"                             │
│   Acquire table_lock                            │
│   Find PID 2                                    │
│   Acquire pcb[2].lock                           │
│   Release table_lock (early!)                   │
│   Set state = ZOMBIE, exit_status = 42          │
│   Broadcast pcb[2].state_changed                │
│   Release pcb_lock                              │
│   Broadcast table_changed                       │
└────────────┬────────────────────────────────────┘
             │
             ▼
┌────────────────────────────────────────────────┐
│4. Parse "wait 1 -1"                             │
│   Acquire table_lock                            │
│   Find parent (PID 1)                           │
│   Search children for any ZOMBIE                │
│   Found PID 2 (ZOMBIE)                          │
│   Acquire pcb[2].lock                           │
│   Release table_lock (early!)                   │
│   Acquired: pcb[2] is ZOMBIE                    │
│   Set pcb[2].state = TERMINATED                 │
│   Release pcb_lock                              │
│   Acquire table_lock                            │
│   Remove PID 2 from parent's list               │
│   Decrement num_processes                       │
│   Broadcast table_changed                       │
│   Release table_lock                            │
└────────────┬────────────────────────────────────┘
             │
             ▼
┌────────────────────────────────────────────────┐
│5. Parse "ps"                                    │
│   Acquire table_lock                            │
│   Generate snapshot (ACB[0], ACB[1], etc.)      │
│   Release table_lock                            │
│   Print to stdout                               │
│   (Also appears in snapshots.txt)               │
└────────────────────────────────────────────────┘
```

## Memory Layout: PCB Structure

```
┌─────────────────────────────────────┐
│ PCB[i]                              │
├─────────────────────────────────────┤
│ pid_t pid              (4 bytes)    │
│ pid_t ppid             (4 bytes)    │
│ ProcessState state     (4 bytes)    │
│ int exit_status        (4 bytes)    │
├─────────────────────────────────────┤
│ ChildNode *children    (8 bytes)───┐│
│                                     ││
│ int num_children       (4 bytes)    ││
│ int in_use             (4 bytes)    ││
├─────────────────────────────────────┤│
│ pthread_mutex_t pcb_lock            ││
│ pthread_cond_t state_changed        ││  
└─────────────────────────────────────┘│
                                       │
                                       ▼
                         ┌─────────────────────┐
                         │ ChildNode (Linked)  │
                         ├─────────────────────┤
                         │ pid_t child_pid     │
                         │ ChildNode *next  ───┼───► (next child)
                         └─────────────────────┘
```

## Critical Section Analysis

| Operation | Lock(s) | Duration | Reason |
|-----------|---------|----------|--------|
| Fork | table_lock | Brief | Allocate & link child |
| Exit | table_lock → pcb_lock | Brief | Mark ZOMBIE |
| Wait | table_lock → pcb_lock | Varies | Parent may transition RUNNING → BLOCKED until child exit |
| Kill | table_lock → pcb_lock | Brief | Mark for termination |
| Ps | table_lock | Brief | Snapshot read |
| Monitor | table_lock | Brief | Snapshot on each signaled table change |

## Race Condition Prevention

```
Scenario: Fork while Exit
─────────────────────────

Thread A (fork):        Thread B (exit):
                        
1. Lock table
2. Find parent          
3. Allocate slot
4. Add to children
5. Broadcast table_changed
6. Unlock table         1. Lock table ──┐
                        2. Find PID      ├─ Cannot interfere
                        3. Lock PID      │  with fork (step 6
                        4. Unlock table  │  hasn't happened yet)
                        5. Set ZOMBIE
                        6. Unlock PCB
                        7. Broadcast table
                        8. Unlock table

✓ Fork completes BEFORE exit starts modifying state
✓ No race: Both operations atomic
```

## Data Flow: Process Creation

```
fork 1
  │
  ├─► pm_fork(1)
  │    │
  │    ├─► Acquire table_lock
  │    │
  │    ├─► pm_find_pcb_locked(1) → PCB[0]
  │    │
  │    ├─► Allocate PID = 2
  │    │
  │    ├─► Initialize PCB[1]:
  │    │    pid = 2
  │    │    ppid = 1
  │    │    state = RUNNING
  │    │    children = NULL
  │    │
  │    ├─► pm_add_child(1, 2)
  │    │    │
  │    │    ├─► Find parent PCB[0]
  │    │    │
  │    │    └─► Add ChildNode(2) → parent.children
  │    │
  │    ├─► num_processes++  (1 → 2)
  │    │
  │    ├─► Broadcast table_changed
  │    │    │
  │    │    └─► Wakes monitor thread!
  │    │
  │    ├─► Release table_lock
  │    │
  │    └─► Return 2
  │
  └─► Output: "Process forked: PID 2"
```

## Summary

The architecture uses:
1. **Hierarchical locking** - Global then per-process
2. **Condition variables** - Event-driven signaling
3. **Modular threads** - Workers & monitor separate
4. **Change-driven monitor** - Snapshot emitted on each table update signal
5. **Dynamic lists** - Flexible child tracking

This design ensures:
✅ No race conditions
✅ No deadlocks
✅ Efficient synchronization
✅ Clean separation of concerns
✅ Scalable to 64+ processes

