/*
 * Multithreaded Process Manager Simulator - Single File Implementation
 * 
 * Demonstrates concurrent programming with POSIX threads:
 * - Process management (fork, exit, wait, kill)
 * - Synchronization (mutexes, condition variables)
 * - Event-driven monitoring (no polling)
 * 
 * Compile: gcc -Wall -Wextra -pthread -std=c11 -O2 pm_single.c -o pm_sim
 * Run: ./pm_sim commands.txt [commands2.txt ...]
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>

/* ========== CONSTANTS ========== */
#define MAX_PROCESSES 64
#define MAX_CHILDREN 64
#define MAX_COMMAND_LENGTH 256

/* ========== PROCESS STATES ========== */
typedef enum {
    RUNNING,
    BLOCKED,
    ZOMBIE,
    TERMINATED
} ProcessState;

/* ========== DATA STRUCTURES ========== */

typedef struct ChildNode {
    pid_t child_pid;
    struct ChildNode *next;
} ChildNode;

typedef struct {
    pid_t pid;
    pid_t ppid;
    ProcessState state;
    int exit_status;
    ChildNode *children;
    int num_children;
    pthread_mutex_t pcb_lock;
    pthread_cond_t state_changed;
    int in_use;
} PCB;

typedef struct {
    PCB process_table[MAX_PROCESSES];
    pthread_mutex_t table_lock;
    pthread_cond_t table_changed;
    int num_processes;
} ProcessTable;

/* ========== GLOBAL STATE ========== */
ProcessTable g_process_table;
static int g_pid_counter = 2;
static volatile int g_monitor_running = 1;
static pthread_t g_monitor_thread;
static FILE *g_snapshot_file = NULL;

typedef struct {
    int thread_id;
    char operation[256];
} LastOperation;

static LastOperation g_last_operation = {-1, ""};
static pthread_mutex_t g_operation_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_table_version = 0;

/* ========== HELPER FUNCTIONS ========== */

const char* pm_state_to_string(ProcessState state) {
    switch (state) {
        case RUNNING:    return "RUNNING";
        case BLOCKED:    return "BLOCKED";
        case ZOMBIE:     return "ZOMBIE";
        case TERMINATED: return "TERMINATED";
        default:         return "UNKNOWN";
    }
}

PCB* pm_find_pcb_locked(pid_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_process_table.process_table[i].in_use && 
            g_process_table.process_table[i].pid == pid) {
            return &g_process_table.process_table[i];
        }
    }
    return NULL;
}

static int pm_find_free_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!g_process_table.process_table[i].in_use) {
            return i;
        }
    }
    return -1;
}

int pm_add_child(pid_t parent_pid, pid_t child_pid) {
    PCB *parent = pm_find_pcb_locked(parent_pid);
    if (!parent) return -1;
    if (parent->num_children >= MAX_CHILDREN) return -1;
    
    ChildNode *new_child = (ChildNode *)malloc(sizeof(ChildNode));
    if (!new_child) return -1;
    
    new_child->child_pid = child_pid;
    new_child->next = parent->children;
    parent->children = new_child;
    parent->num_children++;
    
    return 0;
}

int pm_remove_child(pid_t parent_pid, pid_t child_pid) {
    PCB *parent = pm_find_pcb_locked(parent_pid);
    if (!parent) return -1;
    
    ChildNode *current = parent->children;
    ChildNode *prev = NULL;
    
    while (current != NULL) {
        if (current->child_pid == child_pid) {
            if (prev == NULL) {
                parent->children = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            parent->num_children--;
            return 0;
        }
        prev = current;
        current = current->next;
    }
    return -1;
}

static void pm_mark_table_changed_locked(void) {
    g_table_version++;
    pthread_cond_broadcast(&g_process_table.table_changed);
}

static void sleep_ms(int milliseconds) {
    if (milliseconds <= 0) {
        return;
    }

    struct timespec req;
    req.tv_sec = milliseconds / 1000;
    req.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

/* ========== CORE PROCESS MANAGER FUNCTIONS ========== */

void pm_init(void) {
    memset(&g_process_table, 0, sizeof(ProcessTable));
    pthread_mutex_init(&g_process_table.table_lock, NULL);
    pthread_cond_init(&g_process_table.table_changed, NULL);
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        pthread_mutex_init(&g_process_table.process_table[i].pcb_lock, NULL);
        pthread_cond_init(&g_process_table.process_table[i].state_changed, NULL);
        g_process_table.process_table[i].in_use = 0;
        g_process_table.process_table[i].children = NULL;
    }
    
    PCB *init_pcb = &g_process_table.process_table[0];
    init_pcb->pid = 1;
    init_pcb->ppid = 0;
    init_pcb->state = RUNNING;
    init_pcb->exit_status = 0;
    init_pcb->children = NULL;
    init_pcb->num_children = 0;
    init_pcb->in_use = 1;
    
    g_process_table.num_processes = 1;
    g_pid_counter = 2;
    g_table_version = 1;
    
    printf("[PM] Process table initialized. Init process (PID 1) created.\n");
}

pid_t pm_fork(pid_t parent_pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    PCB *parent = pm_find_pcb_locked(parent_pid);
    if (!parent) {
        printf("[ERROR] pm_fork: Parent PID %d not found\n", parent_pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    int slot = pm_find_free_slot();
    if (slot == -1) {
        printf("[ERROR] pm_fork: No free process slots\n");
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    pid_t new_pid = g_pid_counter++;
    
    PCB *new_pcb = &g_process_table.process_table[slot];
    new_pcb->pid = new_pid;
    new_pcb->ppid = parent_pid;
    new_pcb->state = RUNNING;
    new_pcb->exit_status = 0;
    new_pcb->children = NULL;
    new_pcb->num_children = 0;
    new_pcb->in_use = 1;
    
    pm_add_child(parent_pid, new_pid);
    g_process_table.num_processes++;
    pm_mark_table_changed_locked();
    pthread_mutex_unlock(&g_process_table.table_lock);
    
    printf("[PM] Process forked: PID %d (parent: %d)\n", new_pid, parent_pid);
    return new_pid;
}

int pm_exit(pid_t pid, int status) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    PCB *pcb = pm_find_pcb_locked(pid);
    if (!pcb) {
        printf("[ERROR] pm_exit: PID %d not found\n", pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    pthread_mutex_lock(&pcb->pcb_lock);
    pthread_mutex_unlock(&g_process_table.table_lock);
    
    if (pcb->state == TERMINATED) {
        printf("[ERROR] pm_exit: PID %d already terminated\n", pid);
        pthread_mutex_unlock(&pcb->pcb_lock);
        return -1;
    }
    
    pcb->state = ZOMBIE;
    pcb->exit_status = status;
    pthread_cond_broadcast(&pcb->state_changed);
    pthread_mutex_unlock(&pcb->pcb_lock);
    
    pthread_mutex_lock(&g_process_table.table_lock);
    pm_mark_table_changed_locked();
    pthread_mutex_unlock(&g_process_table.table_lock);
    
    printf("[PM] Process exited: PID %d with status %d\n", pid, status);
    return 0;
}

pid_t pm_wait(pid_t parent_pid, pid_t child_pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    PCB *parent = pm_find_pcb_locked(parent_pid);
    if (!parent) {
        printf("[ERROR] pm_wait: Parent PID %d not found\n", parent_pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    if (child_pid != -1) {
        PCB *child = pm_find_pcb_locked(child_pid);
        if (!child || child->ppid != parent_pid) {
            printf("[ERROR] pm_wait: Child PID %d not found or wrong parent\n", child_pid);
            pthread_mutex_unlock(&g_process_table.table_lock);
            return -1;
        }

        parent->state = BLOCKED;
        pm_mark_table_changed_locked();
        
        pthread_mutex_lock(&child->pcb_lock);
        pthread_mutex_unlock(&g_process_table.table_lock);
        
        while (child->state != ZOMBIE && child->state != TERMINATED) {
            pthread_cond_wait(&child->state_changed, &child->pcb_lock);
        }
        
        if (child->state == ZOMBIE) {
            child->state = TERMINATED;
            
            pthread_mutex_lock(&g_process_table.table_lock);
            parent->state = RUNNING;
            pm_remove_child(parent_pid, child_pid);
            g_process_table.num_processes--;
            pm_mark_table_changed_locked();
            pthread_mutex_unlock(&g_process_table.table_lock);
            
            pthread_mutex_unlock(&child->pcb_lock);
            
            printf("[PM] Parent %d reaped child %d\n", parent_pid, child_pid);
            return child_pid;
        }

        pthread_mutex_lock(&g_process_table.table_lock);
        parent->state = RUNNING;
        pm_mark_table_changed_locked();
        pthread_mutex_unlock(&g_process_table.table_lock);
        
        pthread_mutex_unlock(&child->pcb_lock);
        return -1;
    }

    parent->state = BLOCKED;
    pm_mark_table_changed_locked();
    
    while (1) {
        PCB *zombie_child = NULL;
        pid_t reaped_pid = -1;
        
        ChildNode *current = parent->children;
        while (current != NULL) {
            PCB *candidate = pm_find_pcb_locked(current->child_pid);
            if (candidate && candidate->state == ZOMBIE) {
                zombie_child = candidate;
                break;
            }
            current = current->next;
        }
        
        if (zombie_child) {
            pthread_mutex_lock(&zombie_child->pcb_lock);
            reaped_pid = zombie_child->pid;
            zombie_child->state = TERMINATED;
            pthread_cond_broadcast(&zombie_child->state_changed);
            pthread_mutex_unlock(&zombie_child->pcb_lock);
            
            parent->state = RUNNING;
            pm_remove_child(parent_pid, reaped_pid);
            g_process_table.num_processes--;
            pm_mark_table_changed_locked();
            
            pthread_mutex_unlock(&g_process_table.table_lock);
            
            printf("[PM] Parent %d reaped child %d\n", parent_pid, reaped_pid);
            return reaped_pid;
        }
        
        int has_active_children = 0;
        current = parent->children;
        while (current != NULL) {
            PCB *candidate = pm_find_pcb_locked(current->child_pid);
            if (candidate && candidate->state != TERMINATED) {
                has_active_children = 1;
                break;
            }
            current = current->next;
        }
        
        if (!has_active_children) {
            parent->state = RUNNING;
            pm_mark_table_changed_locked();
            pthread_mutex_unlock(&g_process_table.table_lock);
            return 0;
        }
        
        pthread_cond_wait(&g_process_table.table_changed, &g_process_table.table_lock);
    }
    
    pthread_mutex_unlock(&g_process_table.table_lock);
    return -1;
}

int pm_kill(pid_t pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    PCB *pcb = pm_find_pcb_locked(pid);
    if (!pcb) {
        printf("[ERROR] pm_kill: PID %d not found\n", pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    pthread_mutex_lock(&pcb->pcb_lock);
    pthread_mutex_unlock(&g_process_table.table_lock);
    
    if (pcb->state == TERMINATED) {
        printf("[ERROR] pm_kill: PID %d already terminated\n", pid);
        pthread_mutex_unlock(&pcb->pcb_lock);
        return -1;
    }
    
    pcb->state = ZOMBIE;
    pcb->exit_status = -1;
    pthread_cond_broadcast(&pcb->state_changed);
    pthread_mutex_unlock(&pcb->pcb_lock);
    
    pthread_mutex_lock(&g_process_table.table_lock);
    pm_mark_table_changed_locked();
    pthread_mutex_unlock(&g_process_table.table_lock);
    
    printf("[PM] Process killed: PID %d\n", pid);
    return 0;
}

void pm_ps(char *output_buffer, size_t buffer_size) {
    if (!output_buffer || buffer_size == 0) return;
    
    pthread_mutex_lock(&g_process_table.table_lock);
    
    int offset = 0;
    offset += snprintf(output_buffer + offset, buffer_size - offset,
        "PID\t\tPPID\t\tSTATE\t\tEXIT_STATUS\n");
    offset += snprintf(output_buffer + offset, buffer_size - offset,
        "----------------------------------------------\n");
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *pcb = &g_process_table.process_table[i];
        if (pcb->in_use && pcb->state != TERMINATED) {
            if (pcb->state == ZOMBIE) {
                offset += snprintf(output_buffer + offset, buffer_size - offset,
                    "%d\t\t%d\t\t%s\t\t%d\n",
                    pcb->pid, pcb->ppid, pm_state_to_string(pcb->state),
                    pcb->exit_status);
            } else {
                offset += snprintf(output_buffer + offset, buffer_size - offset,
                    "%d\t\t%d\t\t%s\t\t-\n",
                    pcb->pid, pcb->ppid, pm_state_to_string(pcb->state));
            }
        }
    }
    
    pthread_mutex_unlock(&g_process_table.table_lock);
}

void pm_print_process_table(void) {
    char buffer[4096];
    pm_ps(buffer, sizeof(buffer));
    printf("%s", buffer);
}

void record_operation(int thread_id, const char *operation_desc) {
    pthread_mutex_lock(&g_operation_lock);
    g_last_operation.thread_id = thread_id;
    strncpy(g_last_operation.operation, operation_desc, sizeof(g_last_operation.operation) - 1);
    g_last_operation.operation[sizeof(g_last_operation.operation) - 1] = '\0';
    pthread_mutex_unlock(&g_operation_lock);
}

void pm_shutdown(void) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *pcb = &g_process_table.process_table[i];
        if (pcb->in_use) {
            ChildNode *current = pcb->children;
            while (current != NULL) {
                ChildNode *next = current->next;
                free(current);
                current = next;
            }
            pcb->children = NULL;
        }
        pthread_mutex_destroy(&pcb->pcb_lock);
        pthread_cond_destroy(&pcb->state_changed);
    }
    
    pthread_mutex_unlock(&g_process_table.table_lock);
    pthread_mutex_destroy(&g_process_table.table_lock);
    pthread_cond_destroy(&g_process_table.table_changed);
    
    printf("[PM] Process manager shut down.\n");
}

/* ========== MONITOR THREAD ========== */

void* monitor_thread_func(void *arg) {
    (void)arg;
    unsigned long last_seen_version;
    
    printf("[MONITOR] Monitor thread started\n");
    fflush(stdout);
    
    char snapshot_buffer[8192];
    pm_ps(snapshot_buffer, sizeof(snapshot_buffer));
    if (g_snapshot_file) {
        fprintf(g_snapshot_file, "Initial Process Table\n");
        fprintf(g_snapshot_file, "%s", snapshot_buffer);
        fflush(g_snapshot_file);
        printf("[MONITOR] Initial snapshot written\n");
    }
    pthread_mutex_lock(&g_process_table.table_lock);
    last_seen_version = g_table_version;
    pthread_mutex_unlock(&g_process_table.table_lock);
    
    while (g_monitor_running) {
        pthread_mutex_lock(&g_process_table.table_lock);

        while (g_monitor_running && g_table_version == last_seen_version) {
            pthread_cond_wait(&g_process_table.table_changed, &g_process_table.table_lock);
        }

        if (!g_monitor_running) {
            pthread_mutex_unlock(&g_process_table.table_lock);
            break;
        }

        last_seen_version = g_table_version;
        
        pthread_mutex_unlock(&g_process_table.table_lock);

        pm_ps(snapshot_buffer, sizeof(snapshot_buffer));

        if (g_snapshot_file) {
            pthread_mutex_lock(&g_operation_lock);
            if (g_last_operation.thread_id >= 0) {
                fprintf(g_snapshot_file, "\nThread %d calls %s\n",
                    g_last_operation.thread_id, g_last_operation.operation);
            }
            pthread_mutex_unlock(&g_operation_lock);

            fprintf(g_snapshot_file, "%s", snapshot_buffer);
            fflush(g_snapshot_file);
            printf("[MONITOR] Snapshot written after operation\n");
        }
    }
    
    printf("[MONITOR] Monitor thread stopping\n");
    return NULL;
}

/* ========== COMMAND PARSER ========== */

void parse_and_execute_command(const char *line, int thread_id) {
    char command[MAX_COMMAND_LENGTH];
    int arg1, arg2;
    
    if (!line || line[0] == '\0' || line[0] == '#') {
        return;
    }
    
    strncpy(command, line, sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';
    
    size_t len = strlen(command);
    if (len > 0 && command[len-1] == '\n') {
        command[len-1] = '\0';
    }
    
    char cmd_name[32];
    int parsed = sscanf(command, "%31s", cmd_name);
    if (parsed != 1) {
        return;
    }
    
    printf("[WORKER %d] Executing: %s\n", thread_id, command);
    
    if (strcmp(cmd_name, "fork") == 0) {
        if (sscanf(command, "%31s %d", cmd_name, &arg1) == 2) {
            char op_desc[256];
            snprintf(op_desc, sizeof(op_desc), "pm_fork %d", arg1);
            record_operation(thread_id, op_desc);
            pid_t child_pid = pm_fork(arg1);
            if (child_pid > 0) {
                printf("[WORKER %d] Fork successful: new PID %d\n", thread_id, child_pid);
            } else {
                printf("[WORKER %d] Fork failed\n", thread_id);
            }
        } else {
            printf("[WORKER %d] Invalid fork command\n", thread_id);
        }
    }
    else if (strcmp(cmd_name, "exit") == 0) {
        if (sscanf(command, "%31s %d %d", cmd_name, &arg1, &arg2) == 3) {
            char op_desc[256];
            snprintf(op_desc, sizeof(op_desc), "pm_exit %d %d", arg1, arg2);
            record_operation(thread_id, op_desc);
            int result = pm_exit(arg1, arg2);
            if (result == 0) {
                printf("[WORKER %d] Exit successful for PID %d\n", thread_id, arg1);
            } else {
                printf("[WORKER %d] Exit failed for PID %d\n", thread_id, arg1);
            }
        } else {
            printf("[WORKER %d] Invalid exit command\n", thread_id);
        }
    }
    else if (strcmp(cmd_name, "wait") == 0) {
        arg2 = -1;
        if (sscanf(command, "%31s %d %d", cmd_name, &arg1, &arg2) >= 2) {
            char op_desc[256];
            snprintf(op_desc, sizeof(op_desc), "pm_wait %d %d", arg1, arg2);
            record_operation(thread_id, op_desc);
            pid_t reaped = pm_wait(arg1, arg2);
            if (reaped > 0) {
                printf("[WORKER %d] Wait successful: reaped PID %d\n", thread_id, reaped);
            } else if (reaped == 0) {
                printf("[WORKER %d] Wait returned: no child to reap\n", thread_id);
            } else {
                printf("[WORKER %d] Wait failed\n", thread_id);
            }
        } else {
            printf("[WORKER %d] Invalid wait command\n", thread_id);
        }
    }
    else if (strcmp(cmd_name, "kill") == 0) {
        if (sscanf(command, "%31s %d", cmd_name, &arg1) == 2) {
            char op_desc[256];
            snprintf(op_desc, sizeof(op_desc), "pm_kill %d", arg1);
            record_operation(thread_id, op_desc);
            int result = pm_kill(arg1);
            if (result == 0) {
                printf("[WORKER %d] Kill successful for PID %d\n", thread_id, arg1);
            } else {
                printf("[WORKER %d] Kill failed for PID %d\n", thread_id, arg1);
            }
        } else {
            printf("[WORKER %d] Invalid kill command\n", thread_id);
        }
    }
    else if (strcmp(cmd_name, "sleep") == 0) {
        if (sscanf(command, "%31s %d", cmd_name, &arg1) == 2) {
            printf("[WORKER %d] Sleeping for %d ms\n", thread_id, arg1);
            sleep_ms(arg1);
        } else {
            printf("[WORKER %d] Invalid sleep command\n", thread_id);
        }
    }
    else if (strcmp(cmd_name, "ps") == 0) {
        char op_desc[256];
        snprintf(op_desc, sizeof(op_desc), "pm_ps");
        record_operation(thread_id, op_desc);
        printf("[WORKER %d] Printing process table:\n", thread_id);
        pm_print_process_table();
    }
    else {
        printf("[WORKER %d] Unknown command: %s\n", thread_id, cmd_name);
    }
}

/* ========== WORKER THREAD ========== */

typedef struct {
    int thread_id;
    const char *filename;
} WorkerThreadArgs;

void* worker_thread_func(void *arg) {
    WorkerThreadArgs *args = (WorkerThreadArgs *)arg;
    int thread_id = args->thread_id;
    const char *filename = args->filename;
    
    printf("[WORKER %d] Starting with file: %s\n", thread_id, filename);
    
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        printf("[WORKER %d] ERROR: Cannot open file %s\n", thread_id, filename);
        free(args);
        return NULL;
    }
    
    char line[MAX_COMMAND_LENGTH];
    while (fgets(line, sizeof(line), fp) != NULL) {
        parse_and_execute_command(line, thread_id);
    }
    
    fclose(fp);
    printf("[WORKER %d] Finished reading commands\n", thread_id);
    
    free(args);
    return NULL;
}

/* ========== MAIN ========== */

void print_usage(const char *program) {
    printf("Usage: %s <command_file1> [<command_file2> ...]\n", program);
    printf("\nCommand files should contain one command per line:\n");
    printf("  fork <parent_pid>          - Create a child process\n");
    printf("  exit <pid> <status>        - Exit a process with status\n");
    printf("  wait <parent_pid> [child_pid] - Wait for child (child_pid=-1 for any)\n");
    printf("  kill <pid>                 - Kill a process\n");
    printf("  sleep <milliseconds>       - Sleep for specified time\n");
    printf("  ps                         - Print process table\n");
    printf("  # comment                  - Comment line (ignored)\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    printf("========================================\n");
    printf("  Multithreaded Process Manager\n");
    printf("========================================\n\n");
    
    pm_init();
    
    g_snapshot_file = fopen("snapshots.txt", "w");
    if (!g_snapshot_file) {
        printf("ERROR: Cannot open snapshots.txt for writing\n");
        return 1;
    }
    
    printf("[MAIN] Creating monitor thread...\n");
    if (pthread_create(&g_monitor_thread, NULL, monitor_thread_func, NULL) != 0) {
        printf("ERROR: Cannot create monitor thread\n");
        fclose(g_snapshot_file);
        return 1;
    }
    
    int num_workers = argc - 1;
    pthread_t *worker_threads = (pthread_t *)malloc(num_workers * sizeof(pthread_t));
    
    printf("[MAIN] Creating %d worker threads...\n", num_workers);
    for (int i = 0; i < num_workers; i++) {
        WorkerThreadArgs *args = (WorkerThreadArgs *)malloc(sizeof(WorkerThreadArgs));
        args->thread_id = i;
        args->filename = argv[i + 1];
        
        if (pthread_create(&worker_threads[i], NULL, worker_thread_func, args) != 0) {
            printf("ERROR: Cannot create worker thread %d\n", i);
            free(args);
        }
    }
    
    printf("[MAIN] Waiting for worker threads to complete...\n");
    for (int i = 0; i < num_workers; i++) {
        pthread_join(worker_threads[i], NULL);
    }
    printf("[MAIN] All worker threads completed\n");
    
    sleep(1);
    
    printf("[MAIN] Stopping monitor thread...\n");
    g_monitor_running = 0;
    pthread_cond_broadcast(&g_process_table.table_changed);
    pthread_join(g_monitor_thread, NULL);
    
    printf("\n[MAIN] Final Process Table:\n");
    pm_print_process_table();
    
    pm_shutdown();
    if (g_snapshot_file) {
        fclose(g_snapshot_file);
    }
    
    free(worker_threads);
    
    printf("\n========================================\n");
    printf("  Process Manager Shut Down\n");
    printf("========================================\n");
    
    return 0;
}
