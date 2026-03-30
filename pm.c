#include "pm.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

/* ========== GLOBAL PROCESS TABLE ========== */
ProcessTable g_process_table;  /* Shared with monitor thread */
static int g_pid_counter = 2;  /* Next PID to allocate (1 is reserved for init) */

/* Increment change counter and notify monitor/waiters. table_lock must be held. */
static void pm_notify_change_locked(void) {
    g_process_table.last_change_time++;
    pthread_cond_broadcast(&g_process_table.table_changed);
}

/* Free all child list nodes owned by a PCB. */
static void pm_free_children_list(PCB *pcb) {
    ChildNode *current = pcb->children;
    while (current != NULL) {
        ChildNode *next = current->next;
        free(current);
        current = next;
    }
    pcb->children = NULL;
    pcb->num_children = 0;
}

/* Reap a child process and free its PCB slot. table_lock must be held. */
static void pm_reap_child_locked(pid_t parent_pid, PCB *child) {
    if (!child || !child->in_use) {
        return;
    }

    pm_remove_child(parent_pid, child->pid);
    pm_free_children_list(child);

    child->state = TERMINATED;
    child->exit_status = 0;
    child->pid = 0;
    child->ppid = 0;
    child->in_use = 0;

    if (g_process_table.num_processes > 0) {
        g_process_table.num_processes--;
    }
}

/* ========== INITIALIZATION ========== */

/*
 * pm_init: Initialize the global process table and create the init process
 */
void pm_init(void) {
    memset(&g_process_table, 0, sizeof(ProcessTable));
    pthread_mutex_init(&g_process_table.table_lock, NULL);
    pthread_cond_init(&g_process_table.table_changed, NULL);
    
    /* Initialize all PCBs */
    for (int i = 0; i < MAX_PROCESSES; i++) {
        pthread_mutex_init(&g_process_table.process_table[i].pcb_lock, NULL);
        pthread_cond_init(&g_process_table.process_table[i].state_changed, NULL);
        g_process_table.process_table[i].in_use = 0;
        g_process_table.process_table[i].children = NULL;
    }
    
    /* Create the init process (PID 1, PPID 0) */
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
    
    printf("[PM] Process table initialized. Init process (PID 1) created.\n");
}

/*
 * pm_shutdown: Clean up all process entries and synchronization objects
 */
void pm_shutdown(void) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *pcb = &g_process_table.process_table[i];
        if (pcb->in_use) {
            pm_free_children_list(pcb);
        }
        pthread_mutex_destroy(&pcb->pcb_lock);
        pthread_cond_destroy(&pcb->state_changed);
    }
    
    pthread_mutex_unlock(&g_process_table.table_lock);
    pthread_mutex_destroy(&g_process_table.table_lock);
    pthread_cond_destroy(&g_process_table.table_changed);
    
    printf("[PM] Process manager shut down.\n");
}

/* ========== HELPER FUNCTIONS ========== */

/*
 * pm_find_pcb: Find a PCB by PID
 * Assumes table_lock is held by caller
 * Returns: Pointer to PCB if found, NULL otherwise
 */
PCB* pm_find_pcb_locked(pid_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_process_table.process_table[i].in_use && 
            g_process_table.process_table[i].pid == pid) {
            return &g_process_table.process_table[i];
        }
    }
    return NULL;
}

/*
 * pm_find_pcb: Thread-safe wrapper to find a PCB by PID
 */
PCB* pm_find_pcb(pid_t pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    PCB *pcb = pm_find_pcb_locked(pid);
    pthread_mutex_unlock(&g_process_table.table_lock);
    return pcb;
}

/*
 * pm_find_free_slot: Find the first free PCB slot
 * Assumes table_lock is held by caller
 */
static int pm_find_free_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!g_process_table.process_table[i].in_use) {
            return i;
        }
    }
    return -1;  /* No free slot */
}

/*
 * pm_state_to_string: Convert process state to string
 */
const char* pm_state_to_string(ProcessState state) {
    switch (state) {
        case RUNNING:    return "RUNNING";
        case BLOCKED:    return "BLOCKED";
        case ZOMBIE:     return "ZOMBIE";
        case TERMINATED: return "TERMINATED";
        default:         return "UNKNOWN";
    }
}

/*
 * pm_add_child: Add a child to a parent's children list
 * Assumes parent's pcb_lock is held by caller
 */
int pm_add_child(pid_t parent_pid, pid_t child_pid) {
    PCB *parent = pm_find_pcb_locked(parent_pid);
    if (!parent) {
        return -1;  /* Parent not found */
    }
    
    if (parent->num_children >= MAX_CHILDREN) {
        return -1;  /* Too many children */
    }
    
    ChildNode *new_child = (ChildNode *)malloc(sizeof(ChildNode));
    if (!new_child) {
        return -1;
    }
    
    new_child->child_pid = child_pid;
    new_child->next = parent->children;
    parent->children = new_child;
    parent->num_children++;
    
    return 0;
}

/*
 * pm_remove_child: Remove a child from a parent's children list
 * Assumes parent's pcb_lock is held by caller
 */
int pm_remove_child(pid_t parent_pid, pid_t child_pid) {
    PCB *parent = pm_find_pcb_locked(parent_pid);
    if (!parent) {
        return -1;
    }
    
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
    
    return -1;  /* Child not found */
}

/*
 * pm_count_active_children: Count the number of non-terminated children
 */
int pm_count_active_children(pid_t parent_pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    PCB *parent = pm_find_pcb_locked(parent_pid);
    if (!parent) {
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    int count = 0;
    ChildNode *current = parent->children;
    
    while (current != NULL) {
        PCB *child = pm_find_pcb_locked(current->child_pid);
        if (child && child->state != TERMINATED) {
            count++;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&g_process_table.table_lock);
    return count;
}

/* ========== CORE PROCESS MANAGER FUNCTIONS ========== */

/*
 * pm_fork: Create a new child process
 * parent_pid: The PID of the parent process
 * Returns: The PID of the newly created child, or -1 on error
 *
 * Synchronization:
 * - Acquires table_lock to allocate new slot and update parent's children list
 * - Signals table_changed condition to notify monitor thread
 */
pid_t pm_fork(pid_t parent_pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    /* Find parent */
    PCB *parent = pm_find_pcb_locked(parent_pid);
    if (!parent) {
        printf("[ERROR] pm_fork: Parent PID %d not found\n", parent_pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    /* Find free slot */
    int slot = pm_find_free_slot();
    if (slot == -1) {
        printf("[ERROR] pm_fork: No free process slots\n");
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    /* Allocate new PID */
    pid_t new_pid = g_pid_counter++;
    
    /* Initialize new PCB */
    PCB *new_pcb = &g_process_table.process_table[slot];
    new_pcb->pid = new_pid;
    new_pcb->ppid = parent_pid;
    new_pcb->state = RUNNING;
    new_pcb->exit_status = 0;
    new_pcb->children = NULL;
    new_pcb->num_children = 0;
    new_pcb->in_use = 1;
    
    /* Add child to parent's children list */
    pm_add_child(parent_pid, new_pid);
    
    g_process_table.num_processes++;
    pm_notify_change_locked();
    
    pthread_mutex_unlock(&g_process_table.table_lock);
    
    printf("[PM] Process forked: PID %d (parent: %d)\n", new_pid, parent_pid);
    return new_pid;
}

/*
 * pm_exit: Transition a process to ZOMBIE state and notify waiting parents
 * pid: The PID of the process exiting
 * status: The exit status
 * Returns: 0 on success, -1 on error
 *
 * Synchronization:
 * - Acquires table_lock to find the process
 * - Acquires pcb_lock on the process being reaped
 * - Broadcasts on the process's state_changed condition
 * - Signals table_changed to notify monitor
 */
int pm_exit(pid_t pid, int status) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    PCB *pcb = pm_find_pcb_locked(pid);
    if (!pcb) {
        printf("[ERROR] pm_exit: PID %d not found\n", pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    if (pcb->state == TERMINATED || !pcb->in_use) {
        printf("[ERROR] pm_exit: PID %d already terminated\n", pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }

    if (pcb->state == ZOMBIE) {
        pthread_mutex_unlock(&g_process_table.table_lock);
        return 0;
    }

    pcb->state = ZOMBIE;
    pcb->exit_status = status;
    
    pthread_cond_broadcast(&pcb->state_changed);
    pm_notify_change_locked();
    pthread_mutex_unlock(&g_process_table.table_lock);
    
    printf("[PM] Process exited: PID %d with status %d\n", pid, status);
    return 0;
}

/*
 * pm_wait: Wait for a child process to exit
 * parent_pid: The PID of the waiting parent
 * child_pid: The PID of the child to wait for (-1 to wait for any child)
 * Returns: The PID of the reaped child, or -1 on error
 *
 * Synchronization:
 * - Acquires table_lock to find child(ren)
 * - If child is RUNNING/BLOCKED, waits on child's state_changed condition
 * - If child is ZOMBIE, reaps it immediately (marks as TERMINATED, removes from parent)
 * - Handles child_pid = -1 to wait for any child
 */
pid_t pm_wait(pid_t parent_pid, pid_t child_pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    PCB *parent = pm_find_pcb_locked(parent_pid);
    if (!parent) {
        printf("[ERROR] pm_wait: Parent PID %d not found\n", parent_pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }

    /* If there are no children at all, return immediately (trivial wait). */
    if (parent->children == NULL) {
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }

    if (parent->state != BLOCKED) {
        parent->state = BLOCKED;
        pm_notify_change_locked();
    }
    
    /* Case 1: Wait for a specific child */
    if (child_pid != -1) {
        while (1) {
            PCB *child = pm_find_pcb_locked(child_pid);
            if (!child || child->ppid != parent_pid || !child->in_use) {
                parent->state = RUNNING;
                pm_notify_change_locked();
            pthread_mutex_unlock(&g_process_table.table_lock);
            return -1;
        }

            if (child->state == ZOMBIE) {
                int exit_status = child->exit_status;
                pm_reap_child_locked(parent_pid, child);
                parent->state = RUNNING;
                pm_notify_change_locked();
                pthread_mutex_unlock(&g_process_table.table_lock);

                printf("[PM] Parent %d reaped child %d (status: %d)\n",
                       parent_pid, child_pid, exit_status);
                return child_pid;
            }

            if (child->state == TERMINATED) {
                parent->state = RUNNING;
                pm_notify_change_locked();
                pthread_mutex_unlock(&g_process_table.table_lock);
                return -1;
            }

            pthread_cond_wait(&g_process_table.table_changed, &g_process_table.table_lock);
        }
    }
    
    /* Case 2: Wait for any child (child_pid == -1) */
    while (1) {
        PCB *zombie_child = NULL;
        
        /* Find any ZOMBIE child */
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
            int exit_status = zombie_child->exit_status;
            pid_t reaped_pid = zombie_child->pid;
            pm_reap_child_locked(parent_pid, zombie_child);
            parent->state = RUNNING;
            pm_notify_change_locked();
            
            pthread_mutex_unlock(&g_process_table.table_lock);
            
            printf("[PM] Parent %d reaped child %d (status: %d)\n", 
                   parent_pid, reaped_pid, exit_status);
            return reaped_pid;
        }
        
        /* Check if parent has any non-terminated children */
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
            pm_notify_change_locked();
            pthread_mutex_unlock(&g_process_table.table_lock);
            return -1;
        }
        
        /* Wait for any child to change state, then loop back */
        pthread_cond_wait(&g_process_table.table_changed, &g_process_table.table_lock);
    }
    
    pthread_mutex_unlock(&g_process_table.table_lock);
    return -1;
}

/*
 * pm_kill: Terminate a process gracefully
 * pid: The PID of the process to kill
 * Returns: 0 on success, -1 on error
 *
 * Synchronization:
 * - Acquires table_lock to find the process
 * - Acquires pcb_lock on the target process
 */
int pm_kill(pid_t pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    
    PCB *pcb = pm_find_pcb_locked(pid);
    if (!pcb) {
        printf("[ERROR] pm_kill: PID %d not found\n", pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }
    
    if (pcb->state == TERMINATED || !pcb->in_use) {
        printf("[ERROR] pm_kill: PID %d already terminated\n", pid);
        pthread_mutex_unlock(&g_process_table.table_lock);
        return -1;
    }

    if (pcb->state == ZOMBIE) {
        pthread_mutex_unlock(&g_process_table.table_lock);
        return 0;
    }
    
    /* Transition to ZOMBIE */
    pcb->state = ZOMBIE;
    pcb->exit_status = -1;  /* Killed */
    
    pthread_cond_broadcast(&pcb->state_changed);
    pm_notify_change_locked();
    pthread_mutex_unlock(&g_process_table.table_lock);
    
    printf("[PM] Process killed: PID %d\n", pid);
    return 0;
}

/*
 * pm_ps: Generate a snapshot of the process table
 * output_buffer: Buffer to write the snapshot to
 * buffer_size: Size of the output buffer
 *
 * Synchronization:
 * - Acquires table_lock to prevent changes during snapshot
 */
void pm_ps(char *output_buffer, size_t buffer_size) {
    if (!output_buffer || buffer_size == 0) {
        return;
    }
    
    pthread_mutex_lock(&g_process_table.table_lock);
    
    int offset = 0;
    offset += snprintf(output_buffer + offset, buffer_size - offset,
        "PID\t\tPPID\t\tSTATE\t\tEXIT_STATUS\n");
    offset += snprintf(output_buffer + offset, buffer_size - offset,
        "----------------------------------------------\n");
    
    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *pcb = &g_process_table.process_table[i];
        if (pcb->in_use && pcb->state != TERMINATED) {
            char exit_status_str[16];
            if (pcb->state == ZOMBIE) {
                snprintf(exit_status_str, sizeof(exit_status_str), "%d", pcb->exit_status);
            } else {
                snprintf(exit_status_str, sizeof(exit_status_str), "-");
            }

            offset += snprintf(output_buffer + offset, buffer_size - offset,
                "%d\t\t%d\t\t%s\t\t%s\n",
                pcb->pid, pcb->ppid, pm_state_to_string(pcb->state),
                exit_status_str);

            if ((size_t)offset >= buffer_size) {
                break;
            }
        }
    }
    
    pthread_mutex_unlock(&g_process_table.table_lock);
}

/*
 * pm_print_process_table: Print the entire process table to stdout
 */
void pm_print_process_table(void) {
    char buffer[4096];
    pm_ps(buffer, sizeof(buffer));
    printf("%s", buffer);
}
