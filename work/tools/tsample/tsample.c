// tsample <tid> <seconds> <freq>: user-space IP (+callchain) samples of one thread via perf_event_open.
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
int main(int argc, char** argv) {
    int tid = atoi(argv[1]); double secs = atof(argv[2]); int freq = argc > 3 ? atoi(argv[3]) : 1000;
    struct perf_event_attr a; memset(&a, 0, sizeof a);
    a.size = sizeof a; a.type = PERF_TYPE_SOFTWARE; a.config = PERF_COUNT_SW_TASK_CLOCK;
    a.sample_freq = freq; a.freq = 1; a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN;
    a.exclude_kernel = 1; a.exclude_hv = 1; a.disabled = 0; a.sample_max_stack = 16;
    int fd = syscall(SYS_perf_event_open, &a, tid, -1, -1, 0);
    if (fd < 0) { perror("perf_event_open"); return 1; }
    size_t pg = sysconf(_SC_PAGESIZE), n = 1 + 512;
    uint8_t* m = mmap(NULL, n * pg, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    struct perf_event_mmap_page* h = (void*)m; uint8_t* data = m + pg; size_t dsz = (n - 1) * pg;
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint64_t head = __atomic_load_n(&h->data_head, __ATOMIC_ACQUIRE), tail = h->data_tail;
        while (tail < head) {
            struct perf_event_header eh; 
            for (size_t i = 0; i < sizeof eh; i++) ((uint8_t*)&eh)[i] = data[(tail + i) % dsz];
            uint8_t buf[4096]; size_t len = eh.size < sizeof buf ? eh.size : sizeof buf;
            for (size_t i = 0; i < len; i++) buf[i] = data[(tail + i) % dsz];
            if (eh.type == PERF_RECORD_SAMPLE) {
                uint64_t* p = (uint64_t*)(buf + sizeof eh);
                uint64_t ip = p[0], nr = p[1];
                printf("%llx", (unsigned long long)ip);
                for (uint64_t k = 0; k < nr && k < 16; k++) { uint64_t c = p[2 + k]; if (c < 0xfffffffffffff000ull) printf(" %llx", (unsigned long long)c); }
                printf("\n");
            }
            tail += eh.size;
        }
        __atomic_store_n(&h->data_tail, tail, __ATOMIC_RELEASE);
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 > secs) break;
        usleep(20000);
    }
    return 0;
}
