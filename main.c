#include "process_manager.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) { //program name & 1 script atleast
        fprintf(stderr, "Usage: %s <script1> <script2> ...\n", argv[0]);
        return 1;
    }

    if (pm_init("snapshots.txt") != 0) { //initialize pm with snapshots
        fprintf(stderr, "Failed to initialize process manager\n");
        return 1;
    }

    pthread_t monitor_thread; //declare monitor thread 
    if (pthread_create(&monitor_thread, NULL, pm_monitor_thread, NULL) != 0) { //create new thead for monitor with default attributes
        fprintf(stderr, "Failed to start monitor thread\n");
        pm_shutdown();
        return 1;
    }

    int worker_count = argc - 1;
    pthread_t *worker_threads = calloc((size_t)worker_count, sizeof(*worker_threads)); //allocate memory for worker threads
    pm_worker_args_t *worker_args = calloc((size_t)worker_count, sizeof(*worker_args)); //allocate memory for worker thread arguments
    if (!worker_threads || !worker_args) { //check if memory allocation succeeded
        fprintf(stderr, "Out of memory\n");
        free(worker_threads);
        free(worker_args);
        pm_shutdown();
        return 1;
    }
    //loop through all scipts till worker count -1 
    for (int i = 0; i < worker_count; i++) {
        worker_args[i].thread_id = i;
        worker_args[i].script_path = argv[i + 1]; 
        if (pthread_create(&worker_threads[i], NULL, pm_worker_thread, &worker_args[i]) != 0) { //a new worker thread that runs pm_worker_thread function
            fprintf(stderr, "Failed to start worker thread %d\n", i);
            worker_count = i;
            break;
        }
    }

    for (int i = 0; i < worker_count; i++) { //join all threads to main thread
        pthread_join(worker_threads[i], NULL);
    }

    pm_shutdown();
    pthread_join(monitor_thread, NULL);

    free(worker_threads);
    free(worker_args);
    return 0;
}
