#ifndef PM_H
#define PM_H

#include <pthread.h>
#include <time.h>
#include <sys/types.h>

/* ========== CONSTANTS ========== */
#define MAX_PROCESSES 64
#define MAX_CHILDREN 64

/* ========== PROCESS STATES ========== */
typedef enum {
    RUNNING,
    BLOCKED,
    ZOMBIE,
    TERMINATED
} ProcessState;

/* ========== PROCESS CONTROL BLOCK (PCB) ========== */
typedef struct ChildNode {
    pid_t child_pid;
    struct ChildNode *next;
} ChildNode;

typedef struct {
    pid_t pid;                    /* Process ID */
    pid_t ppid;                   /* Parent Process ID */
    ProcessState state;           /* Current state of process */
    int exit_status;              /* Exit status when terminated */
    ChildNode *children;          /* Linked list of child PIDs */
    int num_children;             /* Count of children */
    pthread_mutex_t pcb_lock;     /* Lock for this PCB */
    pthread_cond_t state_changed; /* Signaled when state changes */
    int in_use;                   /* Flag: 1 if PCB is in use, 0 if free */
} PCB;

/* ========== GLOBAL PROCESS TABLE ========== */
typedef struct {
    PCB process_table[MAX_PROCESSES];
    pthread_mutex_t table_lock;   /* Protects table structure */
    pthread_cond_t table_changed; /* Signals when table changes (for monitor) */
    int num_processes;            /* Count of active processes */
    int last_change_time;         /* Timestamp of last change */
} ProcessTable;

/* Extern declaration for global process table (defined in pm.c) */
extern ProcessTable g_process_table;

/* ========== FUNCTION PROTOTYPES ========== */

/* Initialization */
void pm_init(void);
void pm_shutdown(void);

/* Core Process Manager Functions */
pid_t pm_fork(pid_t parent_pid);
int pm_exit(pid_t pid, int status);
pid_t pm_wait(pid_t parent_pid, pid_t child_pid);
int pm_kill(pid_t pid);
void pm_ps(char *output_buffer, size_t buffer_size);

/* Helper Functions */
PCB* pm_find_pcb(pid_t pid);
int pm_add_child(pid_t parent_pid, pid_t child_pid);
int pm_remove_child(pid_t parent_pid, pid_t child_pid);
int pm_count_active_children(pid_t parent_pid);
void pm_print_process_table(void);
const char* pm_state_to_string(ProcessState state);

#endif /* PM_H */
