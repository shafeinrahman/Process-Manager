#include "process_manager.h"
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Global manager instance
static pm_manager_t g_pm;
static _Thread_local int g_worker_thread_id = -1;

// ===== Internal Helpers =====
static pm_pcb_t *find_pcb(int pid) {
    if (pid <= 0) return NULL;
    for (int i = 0; i < PM_MAX_PROCESSES; i++) {
        pm_pcb_t *p = g_pm.table[i];
        if (p && p->pid == pid) {
            return p;
        }
    }
    return NULL;
}

static int find_slot_by_pid(int pid) {
    for (int i = 0; i < PM_MAX_PROCESSES; i++) {
        pm_pcb_t *p = g_pm.table[i];
        if (p && p->pid == pid) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < PM_MAX_PROCESSES; i++) {
        if (!g_pm.table[i]) {
            return i;
        }
    }
    return -1;
}

static int child_index_in_parent(const pm_pcb_t *parent, int child_pid) {
    for (int i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child_pid) {
            return i;
        }
    }
    return -1;
}

static pm_pcb_t *find_zombie_child(pm_pcb_t *parent, int child_pid) {
    if (child_pid == PM_ANY_CHILD) {
        for (int i = 0; i < parent->child_count; i++) {
            pm_pcb_t *c = find_pcb(parent->children[i]);
            if (c && c->state == PM_STATE_ZOMBIE) {
                return c;
            }
        }
        return NULL;
    }

    if (child_index_in_parent(parent, child_pid) < 0) {
        return NULL;
    }
    pm_pcb_t *child = find_pcb(child_pid);
    if (child && child->state == PM_STATE_ZOMBIE) {
        return child;
    }
    return NULL;
}

static void sleep_millis(int ms) {
    if (ms <= 0) {
        return;
    }

    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) != 0) {
        // Retry with remaining time if interrupted by a signal.
    }
}

static void notify_snapshot(void) {
    g_pm.snapshot_version++;
    pthread_cond_broadcast(&g_pm.snapshot_cv);
}

static void notify_snapshot_with_action_locked(int thread_id, const char *fmt, ...) {
    va_list args;
    unsigned long version = ++g_pm.snapshot_version;
    unsigned long index = g_pm.action_write % PM_ACTION_LOG_CAP;

    va_start(args, fmt);
    g_pm.action_actor[index] = thread_id;
    g_pm.action_version[index] = version;
    vsnprintf(g_pm.action_text[index], sizeof(g_pm.action_text[index]), fmt, args);
    va_end(args);

    g_pm.action_write++;
    if (g_pm.action_write - g_pm.action_read > PM_ACTION_LOG_CAP) {
        g_pm.action_read = g_pm.action_write - PM_ACTION_LOG_CAP;
    }

    pthread_cond_broadcast(&g_pm.snapshot_cv);
}

static void pop_action_for_version_locked(unsigned long version, int *actor, char *action, size_t action_size) {
    *actor = -1;
    action[0] = '\0';

    while (g_pm.action_read < g_pm.action_write) {
        unsigned long idx = g_pm.action_read % PM_ACTION_LOG_CAP;
        unsigned long v = g_pm.action_version[idx];

        if (v < version) {
            g_pm.action_read++;
            continue;
        }
        if (v > version) {
            return;
        }

        *actor = g_pm.action_actor[idx];
        snprintf(action, action_size, "%s", g_pm.action_text[idx]);
        g_pm.action_read++;
        return;
    }
}

// ===== Lifecycle =====
int pm_init(const char *snapshot_path) {
    memset(&g_pm, 0, sizeof(g_pm));
    pthread_mutex_init(&g_pm.table_lock, NULL);
    pthread_cond_init(&g_pm.snapshot_cv, NULL);
    g_pm.next_pid = PM_INIT_PID + 1;
    g_pm.process_count = 0;
    g_pm.shutting_down = false;
    g_pm.monitor_started = false;
    g_pm.monitor_exited = false;
    g_pm.action_read = 0;
    g_pm.action_write = 0;

    // open snapshot file
    g_pm.snapshot_file = fopen(snapshot_path, "w");
    if (!g_pm.snapshot_file) return -1;

    // create init process
    pm_pcb_t *init = (pm_pcb_t *)calloc(1, sizeof(pm_pcb_t));
    if (!init) {
        fclose(g_pm.snapshot_file);
        g_pm.snapshot_file = NULL;
        pthread_cond_destroy(&g_pm.snapshot_cv);
        pthread_mutex_destroy(&g_pm.table_lock);
        return -1;
    }
    init->pid = PM_INIT_PID;
    init->ppid = PM_INIT_PPID;
    init->state = PM_STATE_RUNNING;
    pthread_cond_init(&init->child_exit_cv, NULL);
    g_pm.table[0] = init;
    g_pm.process_count = 1;
    return 0;
}

void pm_shutdown(void) {
    pthread_mutex_lock(&g_pm.table_lock);
    g_pm.shutting_down = true;
    pthread_cond_broadcast(&g_pm.snapshot_cv);

    while (g_pm.monitor_started && !g_pm.monitor_exited) {
        pthread_cond_wait(&g_pm.snapshot_cv, &g_pm.table_lock);
    }

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
    int slot = find_free_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }
    if (parent->child_count >= PM_MAX_PROCESSES) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }

    pm_pcb_t *child = (pm_pcb_t *)calloc(1, sizeof(pm_pcb_t));
    if (!child) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }
    child->pid = pid;
    child->ppid = parent_pid;
    child->state = PM_STATE_RUNNING;
    pthread_cond_init(&child->child_exit_cv, NULL);
    g_pm.table[slot] = child;
    g_pm.process_count++;

    parent->children[parent->child_count++] = pid;

    notify_snapshot_with_action_locked(g_worker_thread_id, "pm_fork %d", parent_pid);
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
    pm_pcb_t *parent = find_pcb(p->ppid);
    if (parent) {
        pthread_cond_broadcast(&parent->child_exit_cv);
    }
    notify_snapshot_with_action_locked(g_worker_thread_id, "pm_exit %d %d", pid, status);
    pthread_mutex_unlock(&g_pm.table_lock);
    return 0;
}

int pm_wait(int parent_pid, int child_pid, int *reaped_pid, int *exit_status) {
    pthread_mutex_lock(&g_pm.table_lock);
    pm_pcb_t *parent = find_pcb(parent_pid);
    if (!parent || !reaped_pid || !exit_status) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }

    if (child_pid != PM_ANY_CHILD && child_index_in_parent(parent, child_pid) < 0) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }
    if (parent->child_count == 0) {
        *reaped_pid = -1;
        *exit_status = 0;
        pthread_mutex_unlock(&g_pm.table_lock);
        return 0;
    }

    pm_pcb_t *child = find_zombie_child(parent, child_pid);
    if (!child) {
        parent->state = PM_STATE_BLOCKED;
        notify_snapshot();
        while (!g_pm.shutting_down && !child) {
            child = find_zombie_child(parent, child_pid);
            if (!child) {
                pthread_cond_wait(&parent->child_exit_cv, &g_pm.table_lock);
            }
        }
        parent->state = PM_STATE_RUNNING;
        notify_snapshot();
    }

    if (!child) {
        pthread_mutex_unlock(&g_pm.table_lock);
        return -1;
    }

    *reaped_pid = child->pid;
    *exit_status = child->exit_status;

    int child_idx = child_index_in_parent(parent, child->pid);
    if (child_idx >= 0) {
        for (int i = child_idx; i + 1 < parent->child_count; i++) {
            parent->children[i] = parent->children[i + 1];
        }
        parent->child_count--;
    }

    child->state = PM_STATE_TERMINATED;
    int slot = find_slot_by_pid(child->pid);
    if (slot >= 0) {
        g_pm.table[slot] = NULL;
    }
    g_pm.process_count--;
    pthread_cond_destroy(&child->child_exit_cv);
    free(child);

    notify_snapshot_with_action_locked(g_worker_thread_id, "pm_wait %d %d", parent_pid, child_pid);
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
    pm_pcb_t *parent = find_pcb(p->ppid);
    if (parent) {
        pthread_cond_broadcast(&parent->child_exit_cv);
    }
    notify_snapshot_with_action_locked(g_worker_thread_id, "pm_kill %d", pid);
    pthread_mutex_unlock(&g_pm.table_lock);
    return 0;
}

void pm_ps(FILE *out) {
    pthread_mutex_lock(&g_pm.table_lock);
    fprintf(out, "PID\tPPID\tSTATE\tEXIT_STATUS\n");
    fprintf(out, "----------------------------------------------\n");
    for (int i = 0; i < PM_MAX_PROCESSES; i++) {
        pm_pcb_t *p = g_pm.table[i];
        if (p && p->state != PM_STATE_TERMINATED) {
            if (p->state == PM_STATE_ZOMBIE) {
                fprintf(out, "%d\t%d\t%s\t%d\n",
                        p->pid, p->ppid,
                        pm_state_to_string(p->state),
                        p->exit_status);
            } else {
                fprintf(out, "%d\t%d\t%s\t-\n",
                        p->pid, p->ppid,
                        pm_state_to_string(p->state));
            }
        }
    }
    pthread_mutex_unlock(&g_pm.table_lock);
}

// ===== Threads =====
void *pm_worker_thread(void *arg) {
    pm_worker_args_t *args = (pm_worker_args_t *)arg;
    g_worker_thread_id = args->thread_id;
    pm_run_script(args->thread_id, args->script_path);
    g_worker_thread_id = -1;
    return NULL;
}

void *pm_monitor_thread(void *arg) {
    (void)arg;
    unsigned long seen_version = 0;

    pthread_mutex_lock(&g_pm.table_lock);
    g_pm.monitor_started = true;
    pthread_cond_broadcast(&g_pm.snapshot_cv);
    pthread_mutex_unlock(&g_pm.table_lock);

    if (g_pm.snapshot_file) {
        fprintf(g_pm.snapshot_file, "Initial Process Table\n");
        pm_ps(g_pm.snapshot_file);
        fflush(g_pm.snapshot_file);
    }

    for (;;) {
        pthread_mutex_lock(&g_pm.table_lock);
        while (seen_version == g_pm.snapshot_version && !g_pm.shutting_down) {
            pthread_cond_wait(&g_pm.snapshot_cv, &g_pm.table_lock);
        }

        if (g_pm.shutting_down) {
            g_pm.monitor_exited = true;
            pthread_cond_broadcast(&g_pm.snapshot_cv);
            pthread_mutex_unlock(&g_pm.table_lock);
            break;
        }

        while (seen_version < g_pm.snapshot_version && !g_pm.shutting_down) {
            int actor = -1;
            char action[128];

            seen_version++;
            pop_action_for_version_locked(seen_version, &actor, action, sizeof(action));
            pthread_mutex_unlock(&g_pm.table_lock);

            if (g_pm.snapshot_file) {
                if (action[0] != '\0' && actor >= 0) {
                    fprintf(g_pm.snapshot_file, "\nThread %d calls %s\n", actor, action);
                }
                pm_ps(g_pm.snapshot_file);
                fflush(g_pm.snapshot_file);
            }

            pthread_mutex_lock(&g_pm.table_lock);
        }

        pthread_mutex_unlock(&g_pm.table_lock);
    }
    return NULL;
}

// ===== Script Runner =====
int pm_run_script(int thread_id, const char *script_path) {
    int prev_thread_id = g_worker_thread_id;
    g_worker_thread_id = thread_id;

    FILE *fp = fopen(script_path, "r");
    if (!fp) {
        g_worker_thread_id = prev_thread_id;
        return -1;
    }

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
            int parsed = sscanf(line, "%*s %d %d", &a1, &a2);
            int wait_child = (parsed == 2) ? a2 : PM_ANY_CHILD;
            pm_wait(a1, wait_child, &reaped, &status);
        } else if (strcmp(cmd, "kill") == 0 && sscanf(line, "%*s %d", &a1) == 1) {
            pm_kill(a1);
        } else if (strcmp(cmd, "ps") == 0) {
            pm_ps(stdout);
        } else if (strcmp(cmd, "sleep") == 0 && sscanf(line, "%*s %d", &a1) == 1) {
            sleep_millis(a1);
        }
    }
    fclose(fp);
    g_worker_thread_id = prev_thread_id;
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