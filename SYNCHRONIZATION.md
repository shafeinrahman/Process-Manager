# Synchronization & Concurrency Design Document

## Overview

This document explains the detailed synchronization strategy used in the Multithreaded Process Manager Simulator, including why specific design choices were made and how they prevent race conditions.

## Problem Statement

### Shared Resources
1. **Process Table**: Global array of PCBs (64 entries)
2. **PCB State**: PID, PPID, state, exit_status, children list
3. **Child List**: Linked list of child processes per PCB

### Concurrent Operations
1. **Multiple Worker Threads**: Reading commands, executing fork/exit/wait/kill
2. **Monitor Thread**: Reading table state and writing snapshots every 100ms
3. **Interleaved Operations**: E.g., one thread forking while another is waiting

## Synchronization Objects

### 1. Global Table Lock (table_lock)

**Protected Resources**:
- `process_table[]` array structure
- `num_processes` counter
- All active PCBs and their allocation status
- Children linked lists

**Acquisition Order**:
- Always acquire FIRST in any lock sequence
- Never acquire another lock while holding table_lock without releasing it first

**Usage Pattern**:
```c
pthread_mutex_lock(&g_process_table.table_lock);
{
    // Read/modify table structure
    PCB *pcb = pm_find_pcb_locked(pid);  // Uses table_lock context
}
pthread_mutex_unlock(&g_process_table.table_lock);
```

### 2. Per-Process Lock (pcb_lock)

**Protected Resources**:
- Process state (RUNNING, BLOCKED, ZOMBIE, TERMINATED)
- Exit status
- Individual process properties

**Acquisition Order**:
- Always acquire SECOND, after table_lock
- Can be released before table_lock

**Usage Pattern**:
```c
pthread_mutex_lock(&g_process_table.table_lock);
PCB *pcb = pm_find_pcb_locked(pid);
pthread_mutex_lock(&pcb->pcb_lock);
pthread_mutex_unlock(&g_process_table.table_lock);  // Safe to release table_lock
{
    // Modify process state
    pcb->state = ZOMBIE;
}
pthread_mutex_unlock(&pcb->pcb_lock);
```

### 3. Condition Variables

#### table_changed
- **Signaled by**: pm_fork, pm_exit, pm_kill (any table modification)
- **Waited on by**: Monitor thread
- **Protected by**: table_lock
- **Purpose**: Wake monitor thread to write snapshot

#### state_changed
- **Signaled by**: pm_exit (when process becomes ZOMBIE)
- **Waited on by**: pm_wait (parent waiting for child)
- **Protected by**: pcb_lock (for the specific process)
- **Purpose**: Wake waiting parents when child state changes

## Critical Algorithms

### Algorithm 1: pm_fork() - Safe Child Creation

```
ACQUIRE table_lock
    FIND parent PCB in table
    IF parent not found:
        ERROR
    FIND free PCB slot
    IF no free slot:
        ERROR
    ALLOCATE new PID
    INITIALIZE new PCB
    ADD child to parent's children list
    INCREMENT num_processes
    BROADCAST table_changed  // Notify monitor
RELEASE table_lock
RETURN new PID
```

**Why This Works**:
- Table modification is atomic (all done within single lock)
- Parent-child link established before returning
- Monitor will be notified of change

### Algorithm 2: pm_exit() - Safe State Transition

```
ACQUIRE table_lock
    FIND target PCB
ACQUIRE pcb_lock
RELEASE table_lock  // Can release early since we have pcb_lock
    VALIDATE process not already TERMINATED
    CHANGE state to ZOMBIE
    STORE exit_status
    BROADCAST state_changed  // Wake waiting parents
RELEASE pcb_lock
ACQUIRE table_lock
    BROADCAST table_changed  // Notify monitor
RELEASE table_lock
```

**Why This Works**:
- Table lookup is quick and protected
- Process state modification is serialized by pcb_lock
- Waiting parents woken immediately when state changes
- Monitor notified of table change

### Algorithm 3: pm_wait() - Safe Parent-Child Synchronization

```
ACQUIRE table_lock
    FIND parent
    IF child_pid != -1:
        FIND specific child
    ELSE:
        MARK waiting for any child
ACQUIRE child's pcb_lock
RELEASE table_lock

WHILE child is not ZOMBIE:
    WAIT on child's state_changed (atomic with pcb_lock)
    // Re-acquire pcb_lock automatically after wakeup

// Now child is ZOMBIE, reap it
CHANGE child state to TERMINATED
BROADCAST state_changed  // For any other waiters
RELEASE pcb_lock

ACQUIRE table_lock
    REMOVE child from parent's list
    DECREMENT num_processes
    BROADCAST table_changed  // Notify monitor
RELEASE table_lock

RETURN child_pid
```

**Why This Works**:
- Lookup phase protected by table_lock
- Waiting phase protected by pcb_lock
- State change of child awakens parent's wait
- Automatic lock reacquisition after condition variable wakeup
- Reaping (state change) is atomic

### Algorithm 4: Monitor Thread - Non-Polling Event Notification

```
WHILE running:
    wait for table_changed signal
    ACQUIRE table_lock
        WAIT on table_changed
        // Wait returns when:
        // 1. Signaled (table change occurred)
        // wakes when notified about a change
    // at this point, a change notification was received
    ACQUIRE snapshot state
    RELEASE table_lock
    WRITE snapshot to file
```

**Why This Works**:
- No polling loop (CPU efficient)
- Responds as soon as change notifications are delivered
- Change counter prevents missed snapshots when updates happen quickly
- Mutex automatically held during wait, released during sleep

## Common Pitfall: Deadlock Prevention

### The Forbidden Pattern (DEADLOCK)
```c
// WRONG: This can deadlock!
ACQUIRE table_lock
    ACQUIRE pcb_lock
        // ...
    RELEASE pcb_lock  
RELEASE table_lock

// Later in different thread:
ACQUIRE pcb_lock        // DEADLOCK if first thread still needs table_lock
    ACQUIRE table_lock  // Waiting for table_lock held by first thread
```

### The Safe Pattern (CORRECT)
```c
// CORRECT: Consistent ordering
ACQUIRE table_lock
    ACQUIRE pcb_lock
        // ...
    RELEASE pcb_lock
RELEASE table_lock

// Always table_lock -> pcb_lock, never reverse
```

## Race Condition Scenarios & Solutions

### Scenario 1: Fork During Exit

**Timeline**:
```
Thread A (exit):  ACQUIRE table_lock, find PID 5
Thread B (fork):  ACQUIRE table_lock (blocked, waiting)
Thread A (exit):  ACQUIRE pcb_lock(5), RELEASE table_lock
Thread B (fork):  ACQUIRE table_lock (now succeeds), creates PID 6
Thread A (exit):  RELEASE pcb_lock(5), BROADCAST table_changed
Thread B (fork):  BROADCAST table_changed
```

**Why Safe**:
- Each table operation fully completes before next begins
- Monitor gets notified of both changes
- No interference between operations

### Scenario 2: Multiple Waits on Same Child

**Timeline**:
```
Thread A (wait):   ACQUIRE table_lock, FIND child, ACQUIRE pcb_lock(child)
Thread B (wait):   ACQUIRE table_lock (blocked)
Thread A (wait):   RELEASE table_lock, now waiting on state_changed
Thread B (wait):   ACQUIRE table_lock, FIND child, ACQUIRE pcb_lock(child)
Thread C (exit):   [child exits] ACQUIRE table_lock, ACQUIRE pcb_lock(child),
                   CHANGE state to ZOMBIE, BROADCAST state_changed
Thread A & B:      Both wake up from state_changed broadcast
```

**Why Safe**:
- Both parents acquire pcb_lock in sequence
- Broadcast wakes all waiters
- Both parents will reap the child correctly

### Scenario 3: Monitor Writes During Modifications

**Timeline**:
```
Thread A (fork):   ACQUIRE table_lock, modify table,
                   BROADCAST table_changed, RELEASE table_lock
Monitor Thread:    AWAKENS from cond_wait, still holds table_lock,
                   READS stable table state
Monitor Thread:    RELEASE table_lock, WRITE snapshot
```

**Why Safe**:
- Condition variable automatially reacquires table_lock before returning
- Monitor reads table in atomic fashion
- Snapshot is consistent point-in-time view

## Verification Checklist

For any new operation that modifies table/process state:

- [ ] Acquisition order: table_lock before pcb_lock
- [ ] All table modifications atomic (within single table_lock section)
- [ ] All process state changes atomic (within single pcb_lock section)
- [ ] Condition variables signaled WITHIN their protecting mutex
- [ ] No nested locks of the same type (no deadlock)
- [ ] Long operations don't hold both locks
- [ ] Signals broadcast to wake appropriate waiters

## Performance Implications

### Lock Contention
- **Low**: Each operation acquires table_lock briefly
- **Scalability**: Multiple processes can proceed concurrently
- **Monitor Thread**: Waits on condition variable, doesn't spin

### Critical Sections
- **Table lookup/modification**: ~O(n) where n=num_processes, but brief
- **Process state change**: O(1) per process
- **Monitor I/O**: Triggered by table-change notifications

### Worst Case
- All threads trying to fork simultaneously
- Result: serialized at table_lock, ~64 forks possible, monitor is event-driven
- This is acceptable for a simulator (not a production OS)

## Testing Strategy

### Race Condition Testing
1. Run with many concurrent workers
2. Create, exit, wait operations interleaved
3. Monitor thread output for consistency
4. Check snapshots.txt for logical consistency

### Correctness Testing
- Expected parent-child relationships maintained
- All children reaped before parent exits
- No zombie processes remain unreaped
- Process count accurate throughout

### Stress Testing
- Max out 64 processes
- Concurrent forks when at capacity
- Multiple parents with multiple children
- Wait for any child (-1 scenario)


