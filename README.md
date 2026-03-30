# Multithreaded Process Manager Simulator

A concurrent process management simulator in C using POSIX threads (pthreads) that demonstrates synchronization, process lifecycle management, and condition variables.

## Architecture Overview

### 1. Data Structures

#### Process Control Block (PCB)
```
typedef struct {
    pid_t pid;                      // Process ID
    pid_t ppid;                     // Parent Process ID
    ProcessState state;             // RUNNING, BLOCKED, ZOMBIE, TERMINATED
    int exit_status;                // Exit code when terminated
    ChildNode *children;            // Linked list of child PIDs
    int num_children;               // Count of children
    pthread_mutex_t pcb_lock;       // Protects this PCB
    pthread_cond_t state_changed;   // Signals when state changes
    int in_use;                     // Flag if PCB is allocated
} PCB;
```

#### Process Table
```
typedef struct {
    PCB process_table[64];          // Fixed array of 64 PCBs
    pthread_mutex_t table_lock;     // Protects table structure
    pthread_cond_t table_changed;   // Signals when table changes
    int num_processes;              // Count of active processes
    int last_change_time;           // Timestamp of last change
} ProcessTable;
```

### 2. Synchronization Strategy

The system uses a **fine-grained locking strategy** with careful lock ordering to prevent deadlocks:

#### Lock Hierarchy
```
1. table_lock      (global, for table structure modifications)
2. pcb_lock        (per-process, for individual process state)
```

**Critical Rule**: Always acquire `table_lock` before `pcb_lock`. Never acquire them in reverse order.

#### Condition Variables

1. **table_changed**: Signals monitor thread when table changes
2. **state_changed**: Signals waiting parents when child changes state

#### Lock Usage by Function

| Function | Locks Used | Purpose |
|----------|-----------|---------|
| pm_fork | table_lock | Create new PCB, allocate PID, add to parent |
| pm_exit | table_lock → pcb_lock | Mark process as ZOMBIE, notify waiters |
| pm_wait | table_lock → pcb_lock | Wait for child to become ZOMBIE, reap |
| pm_kill | table_lock → pcb_lock | Mark process for termination |
| pm_ps   | table_lock | Snapshot table state |

### 3. Monitor Thread Design

The monitor thread is **event-driven**:

```
while (running) {
    wait_for_table_changed_signal()
    write_snapshot_to_file()
}
```

**Key Features**:
- Uses `pthread_cond_wait()` and sleeps until signaled
- Wakes on table updates and writes snapshots without timed polling
- Uses a change counter so quick consecutive updates are not missed
- Automatically notified of changes via condition variables

### 4. Core Functions

#### pm_fork(parent_pid)
- **Atomicity**: Acquires table_lock for entire operation
- **Actions**: Allocate PCB, assign PID, add to parent's children, broadcast table_changed
- **Returns**: New PID on success, -1 on error (no free slots)

#### pm_exit(pid, status)
- **Atomicity**: Acquires table_lock then pcb_lock
- **Actions**: Mark as ZOMBIE, store exit_status, broadcast state_changed, signal table_changed
- **Returns**: 0 on success, -1 if process not found

#### pm_wait(parent_pid, child_pid)
- **Atomicity**: Careful around condition variable wait
- **Special Case**: child_pid = -1 waits for any child
- **Blocking**: Uses `pthread_cond_wait()` to sleep until child becomes ZOMBIE
- **Reaping**: Marks child as TERMINATED, removes from parent's list
- **Returns**: Reaped PID on success, -1 on error

#### pm_kill(pid)
- **Atomicity**: Acquires table_lock then pcb_lock
- **Actions**: Transition to ZOMBIE with exit_status = -1
- **Returns**: 0 on success, -1 if process not found

#### pm_ps()
- **Atomicity**: Acquires table_lock for entire snapshot
- **Output**: PID, PPID, STATE, EXIT_STATUS columns (no TERMINATED rows)

### 5. Thread Architecture

#### Worker Threads
- **Quantity**: One per input command file
- **Job**: Read commands, execute synchronously
- **Commands**: fork, exit, wait, kill, sleep, ps

#### Monitor Thread
- **Quantity**: One global
- **Job**: Write snapshots when the process table changes
- **Synchronization**: Event-driven (no polling)
- **Output**: snapshots.txt file

## Building

```bash
make              # Build pm_sim
make clean        # Remove build artifacts
make run          # Build and run with sample commands
```

## Usage

```bash
./pm_sim <command_file1> [<command_file2> ...]
```

### Example
```bash
./pm_sim commands1.txt commands2.txt
```

This runs two worker threads, each processing commands from their respective files concurrently.

## Command Syntax

Commands are case-sensitive, one per line:

```
fork <parent_pid>              # Create child process
exit <pid> <status>            # Exit process with status code
wait <parent_pid> [child_pid]  # Wait for child (default: -1 for any)
kill <pid>                     # Terminate process
sleep <milliseconds>           # Pause execution
ps                             # Print process table
# This is a comment            # Comments start with #
```

### Example Commands
```
fork 1                         # Create child of init
exit 2 42                      # Exit process 2 with status 42
wait 1 -1                      # Init waits for any child to exit
wait 1 2                       # Init waits for specific child (PID 2)
sleep 100                      # Sleep 100ms
ps                             # Print process table
```

## Output Files

### Console Output
- Real-time logging of process manager operations
- Worker thread operations (fork, exit, wait, kill results)
- Monitor thread status

### snapshots.txt
- Snapshots of the process table whenever it changes
- Header and table rows per snapshot section
- Process details: PID, PPID, STATE, EXIT_STATUS

## Example Session

```
$ ./pm_sim commands1.txt commands2.txt
========================================
  Multithreaded Process Manager
========================================

[PM] Process table initialized. Init process (PID 1) created.
[MAIN] Creating monitor thread...
[MONITOR] Monitor thread started
[MAIN] Creating 2 worker threads...
[MAIN] Waiting for worker threads to complete...
[WORKER 1] Starting with file: commands1.txt
[WORKER 2] Starting with file: commands2.txt
[WORKER 1] Executing: fork 1
[PM] Process forked: PID 2 (parent: 1)
[MONITOR] Initial snapshot written
...
[WORKER 1] Finished reading commands
[WORKER 2] Finished reading commands
[MAIN] All worker threads completed
```

## Edge Cases Handled

1. **No free PCB slots**: Returns -1 when MAX_PROCESSES exceeded
2. **Invalid parent PID**: Error returned if parent doesn't exist
3. **Zombie process waiting**: Parent blocks in pm_wait until child exits
4. **Waiting for non-existent child**: Error if child not in parent's list
5. **Multiple mothers waiting**: All parents broadcast when child changes state
6. **Kill on ZOMBIE**: Can't kill already-terminated process
7. **Wait for any child (-1)**: Searches all children until finding ZOMBIE

## Race Condition Prevention

### Double-lock prevention
- Never hold multiple locks while calling functions that acquire locks
- Release table_lock before long waits (except condition variable waits)

### State consistency
- All table modifications within table_lock
- All process state changes within pcb_lock
- Condition variables paired with their protecting mutex

### Monitor thread safety
- table_changed broadcast only within table_lock
- Monitor sleeps on condition variable and wakes on table updates

## Performance Characteristics

- **Lock contention**: Minimal due to fine-grained locking
- **Monitor behavior**: Event-driven snapshots on table changes
- **Scalability**: Tested with 64 processes, 4 concurrent worker threads

## Limitations

1. Max 64 processes (fixed array)
2. Max 64 children per process
3. No inter-process communication
4. No resource limits or signal handling
5. Simplified state machine (no BLOCKED → RUNNING transitions)

## Future Enhancements

- Hierarchical priority scheduling
- Process groups and session management
- Signal support (SIGKILL, SIGSTOP, SIGCONT)
- Resource limits (memory, CPU time)
- Process groups and session leaders
- IPC primitives (pipes, sockets)

## Compilation Notes

- Requires POSIX threads: compile with `-pthread` flag
- Uses `-O2` optimization for production builds
- Tested on Linux with GCC 9.0+
- Should work on any POSIX-compliant system with pthreads
