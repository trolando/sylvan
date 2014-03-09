#include <stdio.h>  // for fprintf
#include <stdlib.h> // for memalign, malloc
#include <sys/mman.h> // for mprotect
#include <sys/time.h> // for gettimeofday
#include <pthread.h>

#include <barrier.h>
#include <lace.h>
#include <ticketlock.h>

#ifndef USE_NUMA
#define USE_NUMA 0 // by default, don't use special numa handling code
#endif

#if USE_NUMA
#include <numa.h>
#include <numa_tools.h>
#endif

static Worker **workers;
static int inited = 0;
static size_t dq_size = -1;

static int n_workers = 0;

static int more_work = 1;
static pthread_t *ts = NULL;

static pthread_attr_t worker_attr;
static pthread_key_t worker_key;
static barrier_t bar;

static pthread_cond_t wait_until_done = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t wait_until_done_mutex = PTHREAD_MUTEX_INITIALIZER;

#if LACE_PIE_TIMES
static hrtime_t count_at_start, count_at_end;
static long long unsigned us_elapsed_timer;

static void
us_elapsed_start(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    us_elapsed_timer = now.tv_sec * 1000000LL + now.tv_usec;
}

static long long unsigned
us_elapsed(void)
{
    struct timeval now;
    long long unsigned t;

    gettimeofday( &now, NULL );

    t = now.tv_sec * 1000000LL + now.tv_usec;

    return t - us_elapsed_timer;
}
#endif

#if USE_NUMA
ticketlock_t lock = {0};
#endif

static void
init_worker(int worker)
{
    Worker *w;

#if USE_NUMA
    size_t node;
    numa_worker_info(worker, &node, 0, 0, 0);
    ticketlock_lock(&lock);
    w = (Worker *)numa_alloc_onnode(sizeof(Worker), node);
    if (w == NULL) {
        fprintf(stderr, "Error: unable to allocate memory for worker!\n");
        exit(1);
    }
    w->dq = (Task*)numa_alloc_onnode(dq_size * sizeof(Task), node);
    if (w->dq == NULL) {
        fprintf(stderr, "Error: unable to allocate worker deques!\n");
        exit(1);
    }
    ticketlock_unlock(&lock);
#else
    if (posix_memalign((void**)&w, LINE_SIZE, sizeof(Worker)) != 0 ||
        posix_memalign((void**)&w->dq, LINE_SIZE, dq_size * sizeof(Task)) != 0) {
        fprintf(stderr, "Error: unable to allocate worker deques!\n");
        exit(1);
    }
#endif

    w->ts.v = 0;
    w->allstolen = 0;
    w->o_dq = w->dq;
    w->o_end = w->dq + dq_size;
    w->o_split = w->o_dq;
    w->o_allstolen = 0;
    w->movesplit = 0;
    w->worker = worker;

#if LACE_COUNT_EVENTS
    { int k; for (k=0; k<CTR_MAX; k++) w->ctr[k] = 0; }
#endif

#if LACE_PIE_TIMES
    w->time = gethrtime();
    w->level = 0;
#endif

    pthread_setspecific(worker_key, w);
    workers[worker] = w;
    barrier_wait(&bar);
}

static inline uint32_t
rng(uint32_t *seed, int max)
{
    uint32_t next = *seed;

    next *= 1103515245;
    next += 12345;

    *seed = next;

    return next % max;
}

static void*
lace_boot_wrapper(void *arg)
{
#if USE_NUMA
    numa_bind_me(0);
#endif

    init_worker(0); // init master

#if LACE_PIE_TIMES
    self->time = gethrtime();
#endif

    lace_time_event(workers[0], 1);

    ((void (*)())arg)();

    pthread_cond_broadcast(&wait_until_done);

    return NULL;
}

// By default, scan sequentially for 0..39 attempts
#define rand_interval 40

static void*
worker_thread( void *arg )
{
    long self_id = (long) arg;

#if USE_NUMA
    numa_bind_me(self_id);
#endif

    init_worker(self_id); // init slave

    Worker **self = &workers[self_id];
    Worker **victim = NULL;
    int worker_id = (*self)->worker;
    uint32_t seed = worker_id;
    unsigned int n = n_workers;
    int i=0;

#if LACE_PIE_TIMES
    (*self)->time = gethrtime();
#endif

    victim = self;

    do {
        // Computing a random number for every steal is too slow, so we do some amount of
        // sequential scanning of the workers and only randomize once in a while, just
        // to be sure.

        if( i>0 ) {
            i--;
            victim ++;
            // A couple of if's is faster than a %...
            if( victim == self ) victim++;
            if( victim >= workers + n ) victim = workers;
        } else {
            i = rng(&seed, rand_interval);
            victim = workers + (rng(&seed, n-1) + worker_id + 1) % n;
        }

        PR_COUNTSTEALS(*self, CTR_steal_tries);
        switch (lace_steal(*self, (*self)->o_dq, *victim)) {
        case LACE_NOWORK:
            lace_cb_stealing();
            break;
        case LACE_STOLEN:
            PR_COUNTSTEALS(*self, CTR_steals);
            break;
        case LACE_BUSY:
            PR_COUNTSTEALS(*self, CTR_steal_busy);
            break;
        default:
            break;
        }

        if (!more_work) break;

    } while(1);

    lace_time_event(*self, 9);

    return NULL;
}

Task*
lace_get_head()
{
    Worker *self = (Worker*)pthread_getspecific(worker_key);

    Task *low = self->o_dq;
    Task *high = self->o_end;

    if (low->f == 0) return low;

    while (low < high) {
        Task *mid = low + (high-low)/2;
        if (mid->f == 0) high = mid;
        else low = mid + 1;
    }

    return low;
}

Worker*
lace_get_worker()
{
    return (Worker*)pthread_getspecific(worker_key);
}

static void
lace_default_cb()
{
}

int
lace_inited()
{
    return inited;
}

size_t
lace_workers()
{
    return n_workers;
}

static pthread_t
_lace_create_thread(int worker, size_t stacksize, void* (*f)(void*), void *arg)
{
#if USE_NUMA
    if (stacksize != 0) {
        size_t pagesize = numa_pagesize();
        stacksize = (stacksize + pagesize - 1) & ~(pagesize - 1); // ceil(stacksize, pagesize)

        // Allocate memory for the program stack on the NUMA nodes
        size_t node;
        numa_worker_info(worker, &node, 0, 0, 0);
        ticketlock_lock(&lock);
        void *stack_location = numa_alloc_onnode(stacksize + pagesize, node);
        if (stack_location == 0) {
            fprintf(stderr, "Error: Unable to allocate memory for the pthread stack!\n");
            exit(1);
        }
        if (0 != mprotect(stack_location, pagesize, PROT_NONE)) {
            fprintf(stderr, "Error: Unable to protect the allocated stack memory with a guard page!\n");
            exit(1);
        }
        stack_location = (uint8_t *)stack_location + pagesize; // skip protected page.
        if (0 != pthread_attr_setstack(&worker_attr, stack_location, stacksize)) {
            fprintf(stderr, "Error: Unable to set the pthread stack in Lace!\n");
            exit(1);
        }
        ticketlock_unlock(&lock);
    }
#endif

    pthread_t res;
    pthread_create(&res, &worker_attr, f, arg);
    return res;
#if ! USE_NUMA
    (void)worker;
    (void)stacksize;
#endif
}

static void
_lace_init(int n)
{
    n_workers = n;

    more_work = 1;
    inited = 1;
    lace_cb_stealing = &lace_default_cb;

    barrier_init(&bar, lace_workers());
    if (posix_memalign((void**)&workers, LINE_SIZE, n*sizeof(Worker*)) != 0) {
        fprintf(stderr, "Error: unable to allocate memory for Lace workers\n");
        exit(1);
    }

    pthread_key_create(&worker_key, NULL);

#if USE_NUMA
    if (numa_available() != 0) {
        fprintf(stderr, "Error: NUMA not available!\n");
        exit(1);
    }
    else fprintf(stderr, "Initializing Lace with NUMA support.\n");

    if (numa_distribute(n) != 0) {
        fprintf(stderr, "Error: no suitable NUMA configuration found!\n");
        exit(1);
    }
#endif

#if LACE_PIE_TIMES
    us_elapsed_start();
    count_at_start = gethrtime();
#endif
}

static void
_lace_spawn_workers(int n, size_t stacksize, void (*f)(void))
{
    pthread_attr_init(&worker_attr);
    pthread_attr_setscope(&worker_attr, PTHREAD_SCOPE_SYSTEM);

    if (stacksize == 0) {
        pthread_attr_getstacksize(&worker_attr, &stacksize);
    } else {
        if (0 != pthread_attr_setstacksize(&worker_attr, stacksize)) {
            fprintf(stderr, "Error: Cannot set stacksize for new pthreads in Lace!\n");
            exit(1);
            pthread_attr_getstacksize(&worker_attr, &stacksize);
        }
    }

    if (stacksize == 0) {
        fprintf(stderr, "Error: Unable to get stacksize for new pthreads in Lace!\n");
        exit(1);
    }

    long i;
    ts = (pthread_t *)malloc((n-1) * sizeof(pthread_t));
    for (i=1; i<n; i++) {
        *(ts+i-1) = _lace_create_thread(i, stacksize, &worker_thread, (void*)i);
    }

    if (f != NULL) {
        _lace_create_thread(0, stacksize, &lace_boot_wrapper, (void*)f);

        // Wait on condition until done
        pthread_mutex_lock(&wait_until_done_mutex);
        pthread_cond_wait(&wait_until_done, &wait_until_done_mutex);
        pthread_mutex_unlock(&wait_until_done_mutex);
    } else {
#if USE_NUMA
        // Pin ourself
        numa_bind_me(0);
#endif

        init_worker(0); // init master
        lace_time_event(workers[0], 1);
    }
}

void
lace_init_static(int workers, size_t dqsize)
{
    dq_size = dqsize;
    _lace_init(workers);
}

void
lace_init_worker(int idx)
{
    long i = idx;
    if (idx != 0) {
        worker_thread((void *)i);
    } else {
        init_worker(idx);
    }
}

void
lace_init(int workers, size_t dqsize, size_t stacksize)
{
    dq_size = dqsize;
    _lace_init(workers);
    _lace_spawn_workers(workers, stacksize, NULL);
}

#if LACE_COUNT_EVENTS
static uint64_t ctr_all[CTR_MAX];
#endif

void
lace_count_reset()
{
#if LACE_COUNT_EVENTS
    size_t i, j;

    for (i=0;i<n_workers;i++) {
        for (j=0;j<CTR_MAX;j++) {
            workers[i]->ctr[j] = 0;
        }
    }

#if LACE_PIE_TIMES
    for (i=0;i<n_workers;i++) {
        workers[i]->time = gethrtime();
        if (i != 0) workers[i]->level = 0;
    }

    us_elapsed_start();
    count_at_start = gethrtime();
#endif
#endif
}

void
lace_count_report_file(FILE *file)
{
#if LACE_COUNT_EVENTS
    size_t i, j;

    for (j=0;j<CTR_MAX;j++) ctr_all[j] = 0;
    for (i=0;i<n_workers;i++) {
        uint64_t *wctr = workers[i]->ctr;
        for (j=0;j<CTR_MAX;j++) {
            ctr_all[j] += wctr[j];
        }
    }

#if LACE_COUNT_TASKS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Tasks (%zu): %zu\n", i, workers[i]->ctr[CTR_tasks]);
    }
    fprintf(file, "Tasks (sum): %zu\n", ctr_all[CTR_tasks]);
    fprintf(file, "\n");
#endif

#if LACE_COUNT_STEALS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Steals (%zu): %zu good/%zu busy of %zu tries; leaps: %zu good/%zu busy of %zu tries\n", i,
            workers[i]->ctr[CTR_steals], workers[i]->ctr[CTR_steal_busy],
            workers[i]->ctr[CTR_steal_tries], workers[i]->ctr[CTR_leaps], 
            workers[i]->ctr[CTR_leap_busy], workers[i]->ctr[CTR_leap_tries]);
    }
    fprintf(file, "Steals (sum): %zu good/%zu busy of %zu tries; leaps: %zu good/%zu busy of %zu tries\n", 
        ctr_all[CTR_steals], ctr_all[CTR_steal_busy],
        ctr_all[CTR_steal_tries], ctr_all[CTR_leaps], 
        ctr_all[CTR_leap_busy], ctr_all[CTR_leap_tries]);
    fprintf(file, "\n");
#endif

#if LACE_COUNT_STEALS && LACE_COUNT_TASKS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Tasks per steal (%zu): %zu\n", i, 
            workers[i]->ctr[CTR_tasks]/(workers[i]->ctr[CTR_steals]+workers[i]->ctr[CTR_leaps]));
    }
    fprintf(file, "Tasks per steal (sum): %zu\n", ctr_all[CTR_tasks]/(ctr_all[CTR_steals]+ctr_all[CTR_leaps]));
    fprintf(file, "\n");
#endif

#if LACE_COUNT_SPLITS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Splits (%zu): %zu shrinks, %zu grows, %zu outgoing requests\n", i,
            workers[i]->ctr[CTR_split_shrink], workers[i]->ctr[CTR_split_grow], workers[i]->ctr[CTR_split_req]);
    }
    fprintf(file, "Splits (sum): %zu shrinks, %zu grows, %zu outgoing requests\n",
        ctr_all[CTR_split_shrink], ctr_all[CTR_split_grow], ctr_all[CTR_split_req]);
    fprintf(file, "\n");
#endif

#if LACE_PIE_TIMES
    count_at_end = gethrtime();

    uint64_t count_per_ms = (count_at_end - count_at_start) / (us_elapsed() / 1000);
    double dcpm = (double)count_per_ms;

    uint64_t sum_count;
    sum_count = ctr_all[CTR_init] + ctr_all[CTR_wapp] + ctr_all[CTR_lapp] + ctr_all[CTR_wsteal] + ctr_all[CTR_lsteal]
              + ctr_all[CTR_close] + ctr_all[CTR_wstealsucc] + ctr_all[CTR_lstealsucc] + ctr_all[CTR_wsignal]
              + ctr_all[CTR_lsignal];

    fprintf(file, "Measured clock (tick) frequency: %.2f GHz\n", count_per_ms / 1000000.0);
    fprintf(file, "Aggregated time per pie slice, total time: %.2f CPU seconds\n\n", sum_count / (1000*dcpm));

    for (i=0;i<n_workers;i++) {
        fprintf(file, "Startup time (%zu):    %10.2f ms\n", i, workers[i]->ctr[CTR_init] / dcpm);
        fprintf(file, "Steal work (%zu):      %10.2f ms\n", i, workers[i]->ctr[CTR_wapp] / dcpm);
        fprintf(file, "Leap work (%zu):       %10.2f ms\n", i, workers[i]->ctr[CTR_lapp] / dcpm);
        fprintf(file, "Steal overhead (%zu):  %10.2f ms\n", i, (workers[i]->ctr[CTR_wstealsucc]+workers[i]->ctr[CTR_wsignal]) / dcpm);
        fprintf(file, "Leap overhead (%zu):   %10.2f ms\n", i, (workers[i]->ctr[CTR_lstealsucc]+workers[i]->ctr[CTR_lsignal]) / dcpm);
        fprintf(file, "Steal search (%zu):    %10.2f ms\n", i, (workers[i]->ctr[CTR_wsteal]-workers[i]->ctr[CTR_wstealsucc]-workers[i]->ctr[CTR_wsignal]) / dcpm);
        fprintf(file, "Leap search (%zu):     %10.2f ms\n", i, (workers[i]->ctr[CTR_lsteal]-workers[i]->ctr[CTR_lstealsucc]-workers[i]->ctr[CTR_lsignal]) / dcpm);
        fprintf(file, "Exit time (%zu):       %10.2f ms\n", i, workers[i]->ctr[CTR_close] / dcpm);
        fprintf(file, "\n");
    }

    fprintf(file, "Startup time (sum):    %10.2f ms\n", ctr_all[CTR_init] / dcpm);
    fprintf(file, "Steal work (sum):      %10.2f ms\n", ctr_all[CTR_wapp] / dcpm);
    fprintf(file, "Leap work (sum):       %10.2f ms\n", ctr_all[CTR_lapp] / dcpm);
    fprintf(file, "Steal overhead (sum):  %10.2f ms\n", (ctr_all[CTR_wstealsucc]+ctr_all[CTR_wsignal]) / dcpm);
    fprintf(file, "Leap overhead (sum):   %10.2f ms\n", (ctr_all[CTR_lstealsucc]+ctr_all[CTR_lsignal]) / dcpm);
    fprintf(file, "Steal search (sum):    %10.2f ms\n", (ctr_all[CTR_wsteal]-ctr_all[CTR_wstealsucc]-ctr_all[CTR_wsignal]) / dcpm);
    fprintf(file, "Leap search (sum):     %10.2f ms\n", (ctr_all[CTR_lsteal]-ctr_all[CTR_lstealsucc]-ctr_all[CTR_lsignal]) / dcpm);
    fprintf(file, "Exit time (sum):       %10.2f ms\n", ctr_all[CTR_close] / dcpm);
    fprintf(file, "\n" );
#endif
#endif
    return;
    (void)file;
}

void lace_exit()
{
    lace_time_event(workers[0], 2);

    more_work = 0;
    inited = 0;

    if (ts != NULL) {
        int i;
        for(i=0; i<n_workers-1; i++) pthread_join(ts[i], NULL);
    }

    barrier_destroy(&bar);

#if LACE_COUNT_EVENTS
    lace_count_report_file(stderr);
#endif
}

void
lace_boot(int workers, size_t dqsize, size_t stack_size, void (*f)(void))
{
    dq_size = dqsize;
    _lace_init(workers);
    _lace_spawn_workers(workers, stack_size, f);
    lace_exit();
}

void (*lace_cb_stealing)(void);

void lace_set_callback(void (*cb)(void))
{
    lace_cb_stealing = cb;
}
