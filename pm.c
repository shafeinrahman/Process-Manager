#include "process_manager.h"
#include <stdlib.h>
#include <string.h>

// Global manager instance
static pm_manager_t g_pm;

// ===== Internal Helpers =====
static pm_pcb_t *find_pcb(int pid) {
    if (pid <= 0 || pid >= PM_MAX_PROCESSES) return NULL;
    return g_pm.table[pid];
}

static void notify_snapshot(void) {
    g_pm.snapshot_version++;
    pthread_cond_broadcast(&g_pm.snapshot_cv);
}

// ===== Lifecycle =====
int pm_init(const char *snapshot_path) {
    memset(&g_pm, 0, sizeof(g_pm));
    pthread_mutex_init(&g_pm.table_lock, NULL);
    pthread_cond_init(&g_pm.snapshot_cv, NULL);
    g_pm.next_pid = PM_INIT_PID + 1;
    g_pm.process_count = 0;
    g_pm.shutting_down = false;

    // open snapshot file
    g_pm.snapshot_file = fopen(snapshot_path, "w");
    if (!g_pm.snapshot_file) return -1;

    // create init process
    pm_pcb_t *init = (pm_pcb_t *)calloc(1, sizeof(pm_pcb_t));
    init->pid = PM_INIT_PID;
    init->ppid = PM_INIT_PPID;
    init->state = PM_STATE_RUNNING;
    pthread_cond_init(&init->child_exit_cv, NULL);
    g_pm.table[PM_INIT_PID] = init;
    g_pm.process_count = 1;

    fprintf(g_pm.snapshot_file, "[PM] Init process created (PID %d)\n", PM_INIT_PID);
    fflush(g_pm.snapshot_file);
    return 0;
}

void pm_shutdown(void) {
    pthread_mutex_lock(&g_pm.table_lock);
    g_pm.shutting_down = true;
    for (int i = 0; i < PM_MAX_PROCESSES; i++) {
        if (g_pm.table[i]) {
            pthread_cond_destroy(&g_pm.table[i]->child_exit_cv);
            free(g_pm.table[i]);
            g_pm.table[i] = NULL;
        }
    }
    g_pm.process_count = 0;
    pthread_mutex_unlock(&g_pm.table_lock);

    pthread_mutex_destroy(&g_pm.table_lock);
    pthread_cond_destroy(&g_pm.snapshot_cv);

    if (g_pm.snapshot_file) fclose(g_pm.snapshot_file);
}

// ===== Core Operations =====
int pm_fork(int parent_pid) {
    pthread_mutex_lock(&g_pm.table_lock);
    pm_pcb_t *parent = find_pcb(parent_pid);
    if (!parent || parent->state != PM_STATE_RUNNING) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }

    int pid = g_pm.next_pid++;
    if (pid >= PM_MAX_PROCESSES) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }

    pm_pcb_t *child = (pm_pcb_t *)calloc(1, sizeof(pm_pcb_t));
    child->pid = pid;
    child->ppid = parent_pid;
    child->state = PM_STATE_RUNNING;
    pthread_cond_init(&child->child_exit_cv, NULL);
    g_pm.table[pid] = child;
    g_pm.process_count++;

    parent->children[parent->child_count++] = pid;

    notify_snapshot();
    pthread_mutex_unlock(&g_pm.table_lock);
    return pid;
}

int pm_exit(int pid, int status) {
    pthread_mutex_lock(&g_pm.table_lock);
    pm_pcb_t *p = find_pcb(pid);
    if (!p || p->state == PM_STATE_TERMINATED) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }
    p->state = PM_STATE_ZOMBIE;
    p->exit_status = status;
    notify_snapshot();
    pthread_mutex_unlock(&g_pm.table_lock);
    return 0;
}

int pm_wait(int parent_pid, int child_pid, int *reaped_pid, int *exit_status) {
    pthread_mutex_lock(&g_pm.table_lock);
    pm_pcb_t *parent = find_pcb(parent_pid);
    if (!parent) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }

    pm_pcb_t *child = NULL;
    if (child_pid == PM_ANY_CHILD) {
        for (int i = 0; i < parent->child_count; i++) {
            pm_pcb_t *c = find_pcb(parent->children[i]);
            if (c && c->state == PM_STATE_ZOMBIE) {
                child = c;
                break;
            }
        }
    } else {
        child = find_pcb(child_pid);
    }

    if (!child || child->state != PM_STATE_ZOMBIE) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }

    *reaped_pid = child->pid;
    *exit_status = child->exit_status;
    child->state = PM_STATE_TERMINATED;
    g_pm.table[child->pid] = NULL;
    g_pm.process_count--;

    notify_snapshot();
    pthread_mutex_unlock(&g_pm.table_lock);
    return 0;
}

int pm_kill(int pid) {
    pthread_mutex_lock(&g_pm.table_lock);
    pm_pcb_t *p = find_pcb(pid);
    if (!p || p->state == PM_STATE_TERMINATED) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }
    p->state = PM_STATE_ZOMBIE;
    p->exit_status = -1;
    notify_snapshot();
    pthread_mutex_unlock(&g_pm.table_lock);
    return 0;
}

void pm_ps(FILE *out) {
    pthread_mutex_lock(&g_pm.table_lock);
    fprintf(out, "PID\tPPID\tSTATE\tEXIT\n");
    for (int i = 0; i < PM_MAX_PROCESSES; i++) {
        pm_pcb_t *p = g_pm.table[i];
        if (p && p->state != PM_STATE_TERMINATED) {
            fprintf(out, "%d\t%d\t%s\t%d\n",
                    p->pid, p->ppid,
                    pm_state_to_string(p->state),
                    p->exit_status);
        }
    }
    pthread_mutex_unlock(&g_pm.table_lock);
}

// ===== Threads =====
void *pm_worker_thread(void *arg) {
    pm_worker_args_t *args = (pm_worker_args_t *)arg;
    pm_run_script(args->thread_id, args->script_path);
    return NULL;
}

void *pm_monitor_thread(void *arg) {
    (void)arg;
    unsigned long seen_version = 0;
    while (!g_pm.shutting_down) {
        pthread_mutex_lock(&g_pm.table_lock);
        while (seen_version == g_pm.snapshot_version && !g_pm.shutting_down) {
            pthread_cond_wait(&g_pm.snapshot_cv, &g_pm.table_lock);
        }
        seen_version = g_pm.snapshot_version;
        pthread_mutex_unlock(&g_pm.table_lock);

        if (g_pm.snapshot_file) {
            pm_ps(g_pm.snapshot_file);
            fflush(g_pm.snapshot_file);
        }
    }
    return NULL;
}

// ===== Script Runner =====
int pm_run_script(int thread_id, const char *script_path) {
    FILE *fp = fopen(script_path, "r");
    if (!fp) return -1;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char cmd[32];
        int a1, a2;
        if (sscanf(line, "%31s", cmd) != 1) continue;

        if (strcmp(cmd, "fork") == 0 && sscanf(line, "%*s %d", &a1) == 1) {
            pm_fork(a1);
        } else if (strcmp(cmd, "exit") == 0 && sscanf(line, "%*s %d %d", &a1, &a2) == 2) {
            pm_exit(a1, a2);
        } else if (strcmp(cmd, "wait") == 0 && sscanf(line, "%*s %d %d", &a1, &a2) >= 1) {
            int reaped, status;
            pm_wait(a1, (sscanf(line, "%*s %d %d", &a1, &a2) == 2 ? a2 : PM_ANY_CHILD), &reaped, &status);
        } else if (strcmp(cmd, "kill") == 0 && sscanf(line, "%*s %d", &a1) == 1) {
            pm_kill(a1);
        } else if (strcmp(cmd, "ps") == 0) {
            pm_ps(stdout);
        } else if (strcmp(cmd, "sleep") == 0 && sscanf(line, "%*s %d", &a1) == 1) {
            usleep(a1 * 1000);
        }
    }
    fclose(fp);
    return 0;
}

// ===== Utilities =====
const char *pm_state_to_string(pm_state_t state) {
    switch (state) {
        case PM_STATE_RUNNING: return "RUNNING";
        case PM_STATE_BLOCKED: return "BLOCKED";
        case PM_STATE_ZOMBIE: return "ZOMBIE";
        case PM_STATE_TERMINATED: return "TERMINATED";
        default: return "UNKNOWN";
    }
}