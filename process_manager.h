#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <pthread.h> //pthread library
#include <stdbool.h> //boolean
#include <stdio.h> //file i/o

#define PM_MAX_PROCESSES 64 //total processes
#define PM_INIT_PID 1 //root process id
#define PM_INIT_PPID 0 //no parent for root process
#define PM_ANY_CHILD (-1) //used for wait function to wait for any child
#define PM_ACTION_LOG_CAP 256 //max capacity for action log
#define PM_SNAPSHOT_TEXT_CAP 8192 //max capacity for snapshot text buffer

typedef enum {
    PM_STATE_RUNNING = 0, //running
    PM_STATE_BLOCKED, //waiting
    PM_STATE_ZOMBIE, //zomboooooooooooooooooo
    PM_STATE_TERMINATED //dead 
} pm_state_t; 

typedef struct pm_pcb {
    int pid; //process id
    int ppid; //parent id
    pm_state_t state; //state
    int exit_status;

    int children[PM_MAX_PROCESSES]; //array of children, 64
    int child_count;

    pthread_cond_t child_exit_cv; //parent waits on this when waiting for child to die
} pm_pcb_t;

typedef struct pm_manager {
    pm_pcb_t *table[PM_MAX_PROCESSES]; //array of pointers for pcbs
    int process_count;
    int next_pid;

    pthread_mutex_t table_lock; //mutex lock

    pthread_cond_t snapshot_cv; //condition for snapshot updates
    unsigned long snapshot_version; 
    bool shutting_down; //flag for shutdown
    bool monitor_started;
    bool monitor_exited;

    int action_actor[PM_ACTION_LOG_CAP]; //array of thread ids who are acting
    char action_text[PM_ACTION_LOG_CAP][128]; //2d array for descriptio
    unsigned long action_version[PM_ACTION_LOG_CAP]; //version number of all actions
    unsigned long action_read; //log entry to be read
    unsigned long action_write; //log entry to be written

    int pending_actor; //id of thread who initiated pending action 
    char pending_action[128]; //description of pendng actioon

    FILE *snapshot_file;
} pm_manager_t;

typedef struct pm_worker_args {
    int thread_id; //id of worker thread
    const char *script_path; //path to script
} pm_worker_args_t;

int pm_init(const char *snapshot_path); //initialize pm
void pm_shutdown(void); //shitdown

int pm_fork(int parent_pid); //fork
int pm_exit(int pid, int status); //exit
int pm_wait(int parent_pid, int child_pid, int *reaped_pid, int *exit_status); //wait parent
int pm_kill(int pid); //killlllllllllllllllllllllllllllllllllllllllllllll
void pm_ps(FILE *out); //print snapshot

void *pm_worker_thread(void *arg); //worker thread 
void *pm_monitor_thread(void *arg); //monitor thrweaf

int pm_run_script(int thread_id, const char *script_path); //run script 

const char *pm_state_to_string(pm_state_t state); //state to string for printing

#endif
