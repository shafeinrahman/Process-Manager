#define _POSIX_C_SOURCE 200809L

#include "pm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/* ========== CONSTANTS ========== */
#define MAX_COMMAND_LENGTH 256

/* ========== GLOBAL VARIABLES ========== */
static volatile int g_monitor_running = 1;
static pthread_t g_monitor_thread;
static FILE *g_snapshot_file = NULL;

static void sleep_ms(int milliseconds) {
    if (milliseconds <= 0) {
        return;
    }

    struct timespec req;
    req.tv_sec = milliseconds / 1000;
    req.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

/* ========== MONITOR THREAD ========== */

/*
 * monitor_thread_func: Monitor thread that writes a snapshot on each table change
 * 
 * This thread:
 * 1. Sleeps on table_changed (no polling)
 * 2. Writes snapshots.txt whenever a table update is signaled
 */
void* monitor_thread_func(void *arg) {
    (void)arg;  /* Suppress unused parameter warning */
    int observed_changes = 0;

    printf("[MONITOR] Monitor thread started\n");
    fflush(stdout);

    /* Write initial snapshot */
    char snapshot_buffer[8192];
    pm_ps(snapshot_buffer, sizeof(snapshot_buffer));
    if (g_snapshot_file) {
        fprintf(g_snapshot_file, "Initial Process Table\n");
        fprintf(g_snapshot_file, "%s", snapshot_buffer);
        fprintf(g_snapshot_file, "\n");
        fflush(g_snapshot_file);
        printf("[MONITOR] Initial snapshot written\n");
    }

    pthread_mutex_lock(&g_process_table.table_lock);
    observed_changes = g_process_table.last_change_time;
    while (g_monitor_running) {
        while (g_monitor_running && observed_changes == g_process_table.last_change_time) {
            pthread_cond_wait(&g_process_table.table_changed, &g_process_table.table_lock);
        }

        if (!g_monitor_running) {
            break;
        }

        while (g_monitor_running && observed_changes < g_process_table.last_change_time) {
            observed_changes++;
            pthread_mutex_unlock(&g_process_table.table_lock);

            pm_ps(snapshot_buffer, sizeof(snapshot_buffer));

            if (g_snapshot_file) {
                fprintf(g_snapshot_file, "Process Table Updated\n");
                fprintf(g_snapshot_file, "%s", snapshot_buffer);
                fprintf(g_snapshot_file, "\n");
                fflush(g_snapshot_file);
            }

            pthread_mutex_lock(&g_process_table.table_lock);
        }
    }
    pthread_mutex_unlock(&g_process_table.table_lock);

    printf("[MONITOR] Monitor thread stopping\n");
    return NULL;
}

/* ========== COMMAND PARSER ========== */

/*
 * parse_and_execute_command: Parse a command string and execute it
 * 
 * Supported commands:
 * - fork <parent_pid>
 * - exit <pid> <status>
 * - wait <parent_pid> [child_pid]    (child_pid defaults to -1)
 * - kill <pid>
 * - sleep <milliseconds>
 * - ps
 */
void parse_and_execute_command(const char *line, int thread_id) {
    char command[MAX_COMMAND_LENGTH];
    int arg1, arg2;
    
    /* Skip empty lines and comments */
    if (!line || line[0] == '\0' || line[0] == '#') {
        return;
    }
    
    /* Make a copy for parsing */
    strncpy(command, line, sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';
    
    /* Remove trailing newline */
    size_t len = strlen(command);
    if (len > 0 && command[len-1] == '\n') {
        command[len-1] = '\0';
    }
    
    /* Parse command */
    char cmd_name[32];
    int parsed = sscanf(command, "%31s", cmd_name);
    if (parsed != 1) {
        return;
    }
    
    printf("[WORKER %d] Executing: %s\n", thread_id, command);
    
    if (strcmp(cmd_name, "fork") == 0) {
        if (sscanf(command, "%31s %d", cmd_name, &arg1) == 2) {
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
        arg2 = -1;  /* Default to wait for any child */
        if (sscanf(command, "%31s %d %d", cmd_name, &arg1, &arg2) >= 2) {
            pid_t reaped = pm_wait(arg1, arg2);
            if (reaped > 0) {
                printf("[WORKER %d] Wait successful: reaped PID %d\n", thread_id, reaped);
            } else {
                printf("[WORKER %d] Wait failed\n", thread_id);
            }
        } else {
            printf("[WORKER %d] Invalid wait command\n", thread_id);
        }
    }
    else if (strcmp(cmd_name, "kill") == 0) {
        if (sscanf(command, "%31s %d", cmd_name, &arg1) == 2) {
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

/*
 * worker_thread_func: Execute commands from a file
 */
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
    
    /* Initialize the process manager */
    pm_init();
    
    /* Open snapshot file */
    g_snapshot_file = fopen("snapshots.txt", "w");
    if (!g_snapshot_file) {
        printf("ERROR: Cannot open snapshots.txt for writing\n");
        return 1;
    }
    fprintf(g_snapshot_file, "Process Manager Snapshot Log\n");
    fprintf(g_snapshot_file, "============================\n");
    fflush(g_snapshot_file);
    
    /* Create monitor thread */
    printf("[MAIN] Creating monitor thread...\n");
    if (pthread_create(&g_monitor_thread, NULL, monitor_thread_func, NULL) != 0) {
        printf("ERROR: Cannot create monitor thread\n");
        fclose(g_snapshot_file);
        return 1;
    }
    
    /* Create worker threads for each input file */
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
    
    /* Wait for all worker threads to complete */
    printf("[MAIN] Waiting for worker threads to complete...\n");
    for (int i = 0; i < num_workers; i++) {
        pthread_join(worker_threads[i], NULL);
    }
    printf("[MAIN] All worker threads completed\n");
    
    /* Stop monitor thread */
    printf("[MAIN] Stopping monitor thread...\n");
    pthread_mutex_lock(&g_process_table.table_lock);
    g_monitor_running = 0;
    pthread_cond_broadcast(&g_process_table.table_changed);
    pthread_mutex_unlock(&g_process_table.table_lock);
    pthread_join(g_monitor_thread, NULL);
    
    /* Print final process table */
    printf("\n[MAIN] Final Process Table:\n");
    pm_print_process_table();
    
    /* Cleanup */
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
