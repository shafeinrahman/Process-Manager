#include "pm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ProcessTable g_process_table;
static int next_pid = 2; // PID 1 is init

// --- Helpers ---
static PCB* find_pcb(pid_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_process_table.process_table[i].in_use &&
            g_process_table.process_table[i].pid == pid) {
            return &g_process_table.process_table[i];
        }
    }
    return NULL;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!g_process_table.process_table[i].in_use) return i;
    return -1;
}

static void notify_change(void) {
    g_process_table.last_change_time++;
    pthread_cond_broadcast(&g_process_table.table_changed);
}

// --- Init & Shutdown ---
void pm_init(void) {
    memset(&g_process_table, 0, sizeof(g_process_table));
    pthread_mutex_init(&g_process_table.table_lock, NULL);
    pthread_cond_init(&g_process_table.table_changed, NULL);

    PCB *init = &g_process_table.process_table[0];
    init->pid = 1; init->ppid = 0;
    init->state = RUNNING; init->in_use = 1;
    pthread_mutex_init(&init->pcb_lock, NULL);
    pthread_cond_init(&init->state_changed, NULL);

    g_process_table.num_processes = 1;
    printf("[PM] Init process created (PID 1)\n");
}

void pm_shutdown(void) {
    pthread_mutex_lock(&g_process_table.table_lock);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_process_table.process_table[i].in_use) {
            ChildNode *c = g_process_table.process_table[i].children;
            while (c) { ChildNode *n = c->next; free(c); c = n; }
        }
    }
    pthread_mutex_unlock(&g_process_table.table_lock);
    pthread_mutex_destroy(&g_process_table.table_lock);
    pthread_cond_destroy(&g_process_table.table_changed);
    printf("[PM] Shutdown complete\n");
}

// --- Core Functions ---
pid_t pm_fork(pid_t parent_pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    PCB *parent = find_pcb(parent_pid);
    if (!parent) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }

    int slot = find_free_slot();
    if (slot == -1) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }

    PCB *child = &g_process_table.process_table[slot];
    child->pid = next_pid++; child->ppid = parent_pid;
    child->state = RUNNING; child->in_use = 1;
    pthread_mutex_init(&child->pcb_lock, NULL);
    pthread_cond_init(&child->state_changed, NULL);

    g_process_table.num_processes++;
    notify_change();
    pthread_mutex_unlock(&g_process_table.table_lock);

    printf("[PM] Forked child PID %d (parent %d)\n", child->pid, parent_pid);
    return child->pid;
}

int pm_exit(pid_t pid, int status) {
    pthread_mutex_lock(&g_process_table.table_lock);
    PCB *p = find_pcb(pid);
    if (!p || p->state == TERMINATED) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }
    p->state = ZOMBIE; p->exit_status = status;
    notify_change();
    pthread_mutex_unlock(&g_process_table.table_lock);
    printf("[PM] Process %d exited (status %d)\n", pid, status);
    return 0;
}

pid_t pm_wait(pid_t parent_pid, pid_t child_pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    PCB *parent = find_pcb(parent_pid);
    if (!parent) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }

    PCB *child = NULL;
    if (child_pid == -1) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (g_process_table.process_table[i].in_use &&
                g_process_table.process_table[i].ppid == parent_pid &&
                g_process_table.process_table[i].state == ZOMBIE) {
                child = &g_process_table.process_table[i];
                break;
            }
        }
    } else {
        child = find_pcb(child_pid);
    }

    if (!child || child->state != ZOMBIE) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }

    pid_t reaped = child->pid;
    child->state = TERMINATED; child->in_use = 0;
    g_process_table.num_processes--;
    notify_change();
    pthread_mutex_unlock(&g_process_table.table_lock);

    printf("[PM] Parent %d reaped child %d\n", parent_pid, reaped);
    return reaped;
}

int pm_kill(pid_t pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    PCB *p = find_pcb(pid);
    if (!p || p->state == TERMINATED) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }
    p->state = ZOMBIE; p->exit_status = -1;
    notify_change();
    pthread_mutex_unlock(&g_process_table.table_lock);
    printf("[PM] Process %d killed\n", pid);
    return 0;
}

void pm_ps(char *buf, size_t size) {
    pthread_mutex_lock(&g_process_table.table_lock);
    int off = snprintf(buf, size, "PID\tPPID\tSTATE\tEXIT\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *p = &g_process_table.process_table[i];
        if (p->in_use && p->state != TERMINATED) {
            off += snprintf(buf+off, size-off, "%d\t%d\t%s\t%d\n",
                            p->pid, p->ppid, pm_state_to_string(p->state), p->exit_status);
        }
    }
    pthread_mutex_unlock(&g_process_table.table_lock);
}

void pm_print_process_table(void) {
    char buf[1024];
    pm_ps(buf, sizeof(buf));
    printf("%s", buf);
}

const char* pm_state_to_string(ProcessState s) {
    switch (s) {
        case RUNNING: return "RUNNING";
        case BLOCKED: return "BLOCKED";
        case ZOMBIE: return "ZOMBIE";
        case TERMINATED: return "TERMINATED";
        default: return "UNKNOWN";
    }
}
#include "pm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ProcessTable g_process_table;
static int next_pid = 2; // PID 1 is init

// --- Helpers ---
static PCB* find_pcb(pid_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_process_table.process_table[i].in_use &&
            g_process_table.process_table[i].pid == pid) {
            return &g_process_table.process_table[i];
        }
    }
    return NULL;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (!g_process_table.process_table[i].in_use) return i;
    return -1;
}

static void notify_change(void) {
    g_process_table.last_change_time++;
    pthread_cond_broadcast(&g_process_table.table_changed);
}

// --- Init & Shutdown ---
void pm_init(void) {
    memset(&g_process_table, 0, sizeof(g_process_table));
    pthread_mutex_init(&g_process_table.table_lock, NULL);
    pthread_cond_init(&g_process_table.table_changed, NULL);

    PCB *init = &g_process_table.process_table[0];
    init->pid = 1; init->ppid = 0;
    init->state = RUNNING; init->in_use = 1;
    pthread_mutex_init(&init->pcb_lock, NULL);
    pthread_cond_init(&init->state_changed, NULL);

    g_process_table.num_processes = 1;
    printf("[PM] Init process created (PID 1)\n");
}

void pm_shutdown(void) {
    pthread_mutex_lock(&g_process_table.table_lock);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (g_process_table.process_table[i].in_use) {
            ChildNode *c = g_process_table.process_table[i].children;
            while (c) { ChildNode *n = c->next; free(c); c = n; }
        }
    }
    pthread_mutex_unlock(&g_process_table.table_lock);
    pthread_mutex_destroy(&g_process_table.table_lock);
    pthread_cond_destroy(&g_process_table.table_changed);
    printf("[PM] Shutdown complete\n");
}

// --- Core Functions ---
pid_t pm_fork(pid_t parent_pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    PCB *parent = find_pcb(parent_pid);
    if (!parent) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }

    int slot = find_free_slot();
    if (slot == -1) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }

    PCB *child = &g_process_table.process_table[slot];
    child->pid = next_pid++; child->ppid = parent_pid;
    child->state = RUNNING; child->in_use = 1;
    pthread_mutex_init(&child->pcb_lock, NULL);
    pthread_cond_init(&child->state_changed, NULL);

    g_process_table.num_processes++;
    notify_change();
    pthread_mutex_unlock(&g_process_table.table_lock);

    printf("[PM] Forked child PID %d (parent %d)\n", child->pid, parent_pid);
    return child->pid;
}

int pm_exit(pid_t pid, int status) {
    pthread_mutex_lock(&g_process_table.table_lock);
    PCB *p = find_pcb(pid);
    if (!p || p->state == TERMINATED) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }
    p->state = ZOMBIE; p->exit_status = status;
    notify_change();
    pthread_mutex_unlock(&g_process_table.table_lock);
    printf("[PM] Process %d exited (status %d)\n", pid, status);
    return 0;
}

pid_t pm_wait(pid_t parent_pid, pid_t child_pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    PCB *parent = find_pcb(parent_pid);
    if (!parent) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }

    PCB *child = NULL;
    if (child_pid == -1) {
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (g_process_table.process_table[i].in_use &&
                g_process_table.process_table[i].ppid == parent_pid &&
                g_process_table.process_table[i].state == ZOMBIE) {
                child = &g_process_table.process_table[i];
                break;
            }
        }
    } else {
        child = find_pcb(child_pid);
    }

    if (!child || child->state != ZOMBIE) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }

    pid_t reaped = child->pid;
    child->state = TERMINATED; child->in_use = 0;
    g_process_table.num_processes--;
    notify_change();
    pthread_mutex_unlock(&g_process_table.table_lock);

    printf("[PM] Parent %d reaped child %d\n", parent_pid, reaped);
    return reaped;
}

int pm_kill(pid_t pid) {
    pthread_mutex_lock(&g_process_table.table_lock);
    PCB *p = find_pcb(pid);
    if (!p || p->state == TERMINATED) { pthread_mutex_unlock(&g_process_table.table_lock); return -1; }
    p->state = ZOMBIE; p->exit_status = -1;
    notify_change();
    pthread_mutex_unlock(&g_process_table.table_lock);
    printf("[PM] Process %d killed\n", pid);
    return 0;
}

void pm_ps(char *buf, size_t size) {
    pthread_mutex_lock(&g_process_table.table_lock);
    int off = snprintf(buf, size, "PID\tPPID\tSTATE\tEXIT\n");
    for (int i = 0; i < MAX_PROCESSES; i++) {
        PCB *p = &g_process_table.process_table[i];
        if (p->in_use && p->state != TERMINATED) {
            off += snprintf(buf+off, size-off, "%d\t%d\t%s\t%d\n",
                            p->pid, p->ppid, pm_state_to_string(p->state), p->exit_status);
        }
    }
    pthread_mutex_unlock(&g_process_table.table_lock);
}

void pm_print_process_table(void) {
    char buf[1024];
    pm_ps(buf, sizeof(buf));
    printf("%s", buf);
}

const char* pm_state_to_string(ProcessState s) {
    switch (s) {
        case RUNNING: return "RUNNING";
        case BLOCKED: return "BLOCKED";
        case ZOMBIE: return "ZOMBIE";
        case TERMINATED: return "TERMINATED";
        default: return "UNKNOWN";
    }
}
