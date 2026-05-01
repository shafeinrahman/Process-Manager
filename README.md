# Process-Manager

A small, educational **process manager simulator** written in **C (POSIX + pthreads)**.  
It models a simplified OS-style process table with `fork`, `exit`, `wait`, and `kill`, and produces a continuously updated **snapshot log** (`snapshots.txt`) showing process state changes over time.

This project is useful for understanding:
- Process Control Blocks (PCBs)
- Parent/child relationships
- Process states (RUNNING/BLOCKED/ZOMBIE/TERMINATED)
- Synchronization with mutexes and condition variables
- Multi-threaded event logging

---

## Features

- **Process Table Simulation**
  - Max processes: `PM_MAX_PROCESSES = 64`
  - Each process has: `pid`, `ppid`, `state`, `exit_status`, and a list of children.

- **Supported Operations**
  - `pm_fork(parent_pid)` → creates a child process entry
  - `pm_exit(pid, status)` → marks process as `ZOMBIE` and stores exit status
  - `pm_wait(parent_pid, child_pid | -1)` → parent blocks until a child becomes zombie, then reaps it
  - `pm_kill(pid)` → forces a process into `ZOMBIE` with exit status `-1`
  - `pm_ps(stdout)` → prints current process table to terminal

- **Threaded Execution Model**
  - **Worker threads** execute command scripts concurrently.
  - A **monitor thread** logs every action + updated process table into `snapshots.txt`.

- **Snapshot Logging**
  - On each action (`fork/exit/wait/kill`), the monitor prints something like:
    ```
    Thread <id> calls pm_fork <parent_pid>
    PID  PPID  STATE   EXIT_STATUS
    ...
    ```

---

## Process States

The simulator uses these states:

- `RUNNING` — process exists and is runnable
- `BLOCKED` — parent is waiting in `pm_wait(...)`
- `ZOMBIE` — process has exited/killed but not yet reaped by its parent
- `TERMINATED` — process has been reaped and removed from the process table

---

## Repository Structure (logical)

This repository contains everything in a single C source file (as provided):

- **Header section** (`process_manager.h`): types, constants, and function declarations
- **Implementation section** (`pm.c`): all process manager logic + monitor/worker threads
- **Main section** (`main.c`): starts the monitor, spawns workers, runs scripts, shuts down

---

## Build

### Requirements
- GCC or Clang
- POSIX threads (`pthread`)
- A POSIX-like environment (Linux/macOS, or WSL on Windows)

### Compile
```bash
gcc -std=c11 -Wall -Wextra -pthread -o process_manager "process manager.c"
```

---

## Run

The program expects **one or more script files** (each script runs in its own worker thread):

```bash
./process_manager script1.txt script2.txt
```

Output:
- `ps` commands print to the **terminal**
- snapshots of all actions + process table updates are written to:
  - `snapshots.txt`

---

## Script Format

Each script file is a plain text file containing one command per line.

### Supported Commands

#### `fork <parent_pid>`
Creates a child process under `parent_pid`.

Example:
```
fork 1
```

#### `exit <pid> <status>`
Marks process `pid` as `ZOMBIE` with the given status.

Example:
```
exit 2 0
```

#### `wait <parent_pid> [child_pid]`
Waits for a child of `parent_pid` to exit and reaps it.

- If `child_pid` is provided, wait for that specific child.
- If omitted, waits for **any child** (equivalent to `-1` internally).

Examples:
```
wait 1
wait 1 2
```

#### `kill <pid>`
Forces a process to become `ZOMBIE` with exit status `-1`.

Example:
```
kill 3
```

#### `ps`
Prints the current process table to stdout.

Example:
```
ps
```

#### `sleep <ms>`
Sleeps for `<ms>` milliseconds (useful to control interleavings across threads).

Example:
```
sleep 250
```

### Notes
- Lines starting with `#` are treated as comments.
- Blank lines are ignored.

---

## Example

### `script1.txt`
```
# Spawn two children from init
fork 1
fork 1
ps
sleep 200
wait 1
ps
```

### `script2.txt`
```
sleep 50
exit 2 42
sleep 50
exit 3 7
```

Run:
```bash
./process_manager script1.txt script2.txt
```

Then inspect:
```bash
cat snapshots.txt
```

---

## Design & Concurrency Notes

- A single global manager instance (`g_pm`) holds the process table and logging state.
- A single mutex (`table_lock`) protects:
  - the process table
  - process counts / PID allocation
  - snapshot log bookkeeping
- Parents waiting for children use a condition variable in the parent PCB:
  - `pthread_cond_t child_exit_cv`
- The monitor uses `snapshot_cv` and a version counter to detect changes.
- `pm_wait` uses a timed wait (1 second) to avoid deadlock if a signal is missed.

---

## Limitations / Simplifications

This is a simulator and intentionally simplifies many OS realities:

- No real OS processes are created; everything is simulated in memory.
- No scheduling, CPU time, priorities, etc.
- PID allocation is simple and does not reuse old PIDs in a robust way.
- Child list is a fixed array sized to `PM_MAX_PROCESSES`.
- `pm_kill` sets a zombie status; it does not enforce immediate removal until reaped.

---

## Troubleshooting

- If `snapshots.txt` is empty:
  - ensure `pm_init("snapshots.txt")` is successful and the program has write permission.
- If build fails with pthread errors:
  - ensure you compile with `-pthread`.

---

## License

Add a license of your choice (MIT is common for small educational projects).  
If you want, tell me which license you prefer and I’ll include the exact text.
