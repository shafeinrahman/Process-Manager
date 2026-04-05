#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#define PM_MAX_PROCESSES 64
#define PM_INIT_PID 1
#define PM_INIT_PPID 0
#define PM_ANY_CHILD (-1)

/* Process lifecycle states for the simulator. */
typedef enum {
    PM_STATE_RUNNING = 0,
    PM_STATE_BLOCKED,
    PM_STATE_ZOMBIE,
    PM_STATE_TERMINATED
} pm_state_t;

/* Process Control Block (PCB) used by the simulated process table. */
typedef struct pm_pcb {
    int pid;
    int ppid;
    pm_state_t state;
    int exit_status;

    int children[PM_MAX_PROCESSES];
    int child_count;

    pthread_cond_t child_exit_cv;
} pm_pcb_t;

/* Global process manager state shared by all threads. */
typedef struct pm_manager {
    pm_pcb_t *table[PM_MAX_PROCESSES];
    int process_count;
    int next_pid;

    pthread_mutex_t table_lock;

    /* Monitor thread synchronization for snapshot notifications. */
    pthread_cond_t snapshot_cv;
    unsigned long snapshot_version;
    bool shutting_down;

    FILE *snapshot_file;
} pm_manager_t;

/* Worker thread argument bundle for script execution. */
typedef struct pm_worker_args {
    int thread_id;
    const char *script_path;
} pm_worker_args_t;

/* Process manager lifecycle. */
int pm_init(const char *snapshot_path);
void pm_shutdown(void);

/* Required process operations from the project spec. */
int pm_fork(int parent_pid);
int pm_exit(int pid, int status);
int pm_wait(int parent_pid, int child_pid, int *reaped_pid, int *exit_status);
int pm_kill(int pid);
void pm_ps(FILE *out);

/* Thread entry points. */
void *pm_worker_thread(void *arg);
void *pm_monitor_thread(void *arg);

/* Script interpreter helper. */
int pm_run_script(int thread_id, const char *script_path);

/* Utility helpers (optional for implementation). */
const char *pm_state_to_string(pm_state_t state);

#endif /* PROCESS_MANAGER_H */
