# Quick Start Guide

## Overview
This project implements a Multithreaded Process Manager Simulator in C with POSIX threads. It demonstrates concurrent programming, synchronization, and process management concepts.

## Project Structure

```
proj/
├── pm.h                      # Header file (structures & prototypes)
├── pm.c                      # Core process manager implementation  
├── main.c                    # Application layer (threads & CLI)
├── Makefile                  # Build automation
├── README.md                 # Full user guide
├── SYNCHRONIZATION.md        # Technical synchronization details
├── IMPLEMENTATION_GUIDE.md   # This project summary
├── commands1.txt             # Test commands (basic)
├── commands2.txt             # Test commands (concurrent)
├── simple_test.txt           # Test commands (non-blocking)
├── ultra_simple.txt          # Test commands (minimal)
└── test.sh                   # Automated test script
```

## Quick Build & Run

### Step 1: Compile
```bash
cd /home/endmin/proj

# Compile all source files
gcc -Wall -Wextra -pthread -std=c11 -O2 -c pm.c -o pm.o
gcc -Wall -Wextra -pthread -std=c11 -O2 -c main.c -o main.o
gcc pm.o main.o -o pm_sim -pthread
```

### Step 2: Run Examples
```bash
# Ultra-simple example (2 commands)
./pm_sim ultra_simple.txt

# Minimal non-blocking example  
./pm_sim simple_test.txt

# Concurrent example with 2 workers
./pm_sim commands1.txt commands2.txt
```

### Step 3: View Output
```bash
# Check console output above
# Check process snapshots
cat snapshots.txt
```

## Understanding the Code

### 1. Data Structures (pm.h)

**PCB (Process Control Block)**
```c
typedef struct {
    pid_t pid;                    // Process ID  
    pid_t ppid;                   // Parent ID
    ProcessState state;           // RUNNING, BLOCKED, ZOMBIE, TERMINATED
    int exit_status;              // Exit code
    ChildNode *children;          // Linked list of child PIDs
    pthread_mutex_t pcb_lock;     // Per-process lock
    pthread_cond_t state_changed; // Wakeup condition
} PCB;
```

**Process Table**
```c
typedef struct {
    PCB process_table[64];          // Max 64 processes
    pthread_mutex_t table_lock;     // Global lock
    pthread_cond_t table_changed;   // Monitor wakeup
} ProcessTable;
```

### 2. Core Operations (pm.c)

| Function | Purpose | Synchronization |
|--|--|--|
| `pm_fork(ppid)` | Create child | Atomic table update |
| `pm_exit(pid, status)` | Terminate process | Broadcast state_changed |
| `pm_wait(ppid, cpid)` | Wait for child | Cond_wait on zombie |
| `pm_kill(pid)` | Force terminate | Mark ZOMBIE |
| `pm_ps()` | Snapshot table | Atomic read |

### 3. Threads (main.c)

**Worker Threads**
- Read commands from files
- Execute fork, exit, wait, kill, sleep, ps
- One thread per input file

**Monitor Thread**
- Maintains snapshots of process table
- Uses event-driven condition variables
- Writes snapshots for each recorded table change
- Writes to snapshots.txt

## Command Syntax

```
fork <parent_pid>              # Create child of parent
exit <pid> <status>            # Exit with status code
wait <parent_pid> [child_pid]  # Wait for child (-1 = any)
kill <pid>                     # Terminate process
sleep <ms>                     # Sleep milliseconds
ps                             # Print process table
# comment                      # Comment line
```

## Example: Single File with Comments

```bash
# Create 3 children from init (PID 1)
fork 1
fork 1
fork 1

# Print current state
ps

# Terminate child 2
exit 2 0

# Wait for any child
wait 1 -1

# Print final state
ps
```

## Synchronization Highlights

### Lock Ordering (Prevents Deadlock)
1. Acquire `table_lock` first
2. Then acquire `pcb_lock`
3. Always release in reverse order

### Condition Variable Signaling
- **table_changed**: Signals monitor thread on any table change
- **state_changed**: Signals waiting parent when child becomes ZOMBIE

### Event-Driven Monitoring
- Monitor thread uses `pthread_cond_wait()`
- Wakes on table change notifications
- No polling loop → efficient

## Testing Scenarios

### Scenario 1: Process Creation  
```bash
fork 1
fork 1
ps
```
Expected: Two children created, shown in ps output

### Scenario 2: Process Termination
```bash
fork 1
exit 2 42
ps
```
Expected: Child marked ZOMBIE, not reaped until parent waits

### Scenario 3: Concurrency
```bash
# In commands1.txt:
fork 1
sleep 100

# In commands2.txt:
fork 1

./pm_sim commands1.txt commands2.txt
```
Expected: Both workers execute concurrently and snapshots update on table changes

## Output Interpretation

### Console Output Format
```
[PM] Process forked: PID 2 (parent: 1)       ← Fork result
[MONITOR] Initial snapshot written           ← Monitor notification
[WORKER 1] Executing: exit 2 42              ← Worker action
```

### snapshots.txt Format
```
Initial Process Table
PID             PPID            STATE           EXIT_STATUS
----------------------------------------------
1               0               RUNNING         -

Process Table Updated
PID             PPID            STATE           EXIT_STATUS
----------------------------------------------
1               0               RUNNING         -
2               1               RUNNING         -
```

## Common Issues & Solutions

| Issue | Cause | Solution |
|-------|-------|----------|
| "No free process slots" | Created 64 processes | Modify MAX_PROCESSES in pm.h |
| Program blocks on wait | Child not marked ZOMBIE yet | Ensure exit is called |
| snapshots.txt empty | Monitor not woken up | Check table-changed broadcast |
| Compilation errors | Missing headers | Add `-pthread` to gcc flags |

## Performance Tips

1. **Reduce logging volume** - remove unnecessary state-changing test commands
2. **More processes** - Increase `MAX_PROCESSES` in pm.h  
3. **Debug output** - Add printf calls in pm.c functions
4. **Lock contention** - Monitor with `perf lock` tool

## Next Steps

1. Read [README.md](README.md) for detailed user guide
2. Review [SYNCHRONIZATION.md](SYNCHRONIZATION.md) for design details
3. Study [pm.c](pm.c) to understand implementation patterns
4. Modify test commands to experiment with scenarios
5. Add new features (process groups, signals, etc.)

## Key Takeaways

✅ **Concurrency**: Multiple threads safely access shared process table
✅ **Synchronization**: Mutexes protect critical sections, condition variables signal events
✅ **Deadlock Prevention**: Careful lock ordering prevents circular waits
✅ **Event-driven**: Monitor thread doesn't poll, uses condition variable signaling
✅ **Process Management**: Parent tracks children, reaps zombies
✅ **Error Handling**: Edge cases handled gracefully

## Support

For more details:
- Architecture: See [README.md](README.md) section "Architecture Overview"
- Synchronization: See [SYNCHRONIZATION.md](SYNCHRONIZATION.md)
- Implementation: See [IMPLEMENTATION_GUIDE.md](IMPLEMENTATION_GUIDE.md)
- Source code: See [pm.c](pm.c) and [main.c](main.c) comments

---

**Ready to start?** Run `./pm_sim ultra_simple.txt` and check the output!


