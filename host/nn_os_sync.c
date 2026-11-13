/* nn::os synchronization: mutexes, condition variables, events (light, normal, system, timer), semaphores,
   message queues, reader/writer locks, multi-wait.

   Every guest object is a host monitor (SRW lock + condition variable + state) found by the guest address of its
   nn::os struct; the guest struct itself is left alone. An object the game never initialized (nn::os has static
   initializers) is created on first use. Mutexes are always recursive: relocking a non-recursive nn::os mutex
   would be a game bug on hardware anyway. */
#include "hos.h"
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum { K_MUTEX = 1, K_COND, K_EVENT, K_SEMA, K_QUEUE, K_RWLOCK, K_MULTIWAIT, K_HOLDER, K_TIMER };

typedef struct Obj {
    uint64_t key;
    int kind;
    SRWLOCK lk;
    CONDITION_VARIABLE cv;
    /* mutex / rwlock */
    HosThread *owner;
    int count, readers;
    int mwait;   /* mutex: threads asleep on it (or about to be): unlock wakes only then */
    /* event / timer */
    int signaled, autoclear;
    int64_t due_ns, period_ns;
    /* semaphore */
    int sem, sem_max;
    /* queue */
    uint64_t *q;
    int qcap, qhead, qlen;
    /* condvar */
    uint64_t gen;
    /* (EvilOOT) Horizon's handoff: mutex: condition variable waiters a signal has woken that the mutex is owed to;
       condvar: its waiters, those a signal has given the mutex to and not yet out, the mutex they wait with
       (cv_mixed: waiters with more than one: no handoff) */
    int reserved;
    int cv_waiting, cv_in, cv_owed, cv_mixed;   /* (cv_waiting: counted before cv->lk, read by Signal without it; cv_in: under cv->lk) */
    struct Obj *cv_mutex, *cv_owed_mutex[8];
    /* multi-wait holder: its event; multi-wait: linked holders */
    uint64_t event;
    struct Obj **links;
    int nlinks, caplinks;
    struct Obj *mw;    /* holder -> the multi-wait it is linked to */
    int spin_misses, spin_waits;
    int64_t sig_ns;      /* (HOS_WAKE_LAT: when it was last signaled / sent to) */
    struct Obj *spare;   /* (on the spare list: see spares) */
} Obj;

static int wake_lat_on(void);   /* (HOS_WAKE_LAT, below) */

/* ---- the object table: open addressing on the guest address --------------------------------------------- */
static SRWLOCK table_lock = SRWLOCK_INIT;
static Obj **table;
static uint64_t *table_keys;   /* each slot's object's key, beside it: a probe compares keys without visiting objects */
static uint64_t cap, used;
typedef struct Table {   /* (the slots, then the keys: cap of each) */
    uint64_t cap;
    Obj *slot[];
} Table;
static uint64_t *table_keys_of(const Table *t) { return (uint64_t *)(void *)(t->slot + t->cap); }
static Table *live;

static uint64_t slot_of(uint64_t key, uint64_t c) { return (key * 0x9E3779B97F4A7C15ULL >> 20) & (c - 1); }

static Obj *find_locked(uint64_t key) {
    if (!table) return NULL;
    for (uint64_t i = slot_of(key, cap);; i = (i + 1) & (cap - 1)) {
        if (!table[i]) return NULL;
        if (table_keys[i] == key && table[i]->kind) return table[i];
    }
}

/* An object finalized (or initialized again) is a tombstone (kind 0). The same address initialized again reuses it in
   place (obj): the game initializes and finalizes ~25 short-lived mutexes and events a frame, and a new object each
   time leaked 13 MB a minute in the browser build. A tombstone whose slot another address takes (or that a rehash
   drops) is freed 30 s later, unless it still links or is linked by a multi-wait (links, mw): freeing one still
   linked (after 10 s, 2026-10-16) hung most boots. */
static Obj **graves;
static int64_t *grave_times;
static size_t grave_head, grave_count, grave_cap;

/* Objects are not given back to the heap: a freed one goes to this list (under table_lock) and a new object takes it,
   so a pointer obj()'s per-thread caches hold always points to an object, which they check by its key and kind. (A
   global count of frees that invalidated every cache missed nearly always: the game's ~25 short-lived objects a frame
   are freed 30 s later, each its own invalidation.) */
static Obj *spares;

static void bury_locked(Obj *o) {
    static int keep = -1;
    if (keep < 0) keep = env_on("HOS_KEEP_DEAD_OBJECTS");
    if (keep) return;
    const int64_t now = hos_now_ns();
    while (grave_count && now - grave_times[grave_head] > 30000000000LL) {
        Obj *d = graves[grave_head];
        grave_head = (grave_head + 1) % grave_cap;
        grave_count--;
        if (!d->kind && !d->mw && !d->nlinks) {
            free(d->q);
            free(d->links);
            d->q = NULL;
            d->links = NULL;
            d->spare = spares;
            spares = d;
        }
    }
    if (grave_count == grave_cap) {
        const size_t ncap = grave_cap ? grave_cap * 2 : 1024;
        Obj **ng = (Obj **)malloc(ncap * sizeof *ng);
        int64_t *nt = (int64_t *)malloc(ncap * sizeof *nt);
        if (!ng || !nt) {
            free(ng);
            free(nt);
            return;
        }
        for (size_t i = 0; i < grave_count; i++) {
            ng[i] = graves[(grave_head + i) % grave_cap];
            nt[i] = grave_times[(grave_head + i) % grave_cap];
        }
        free(graves);
        free(grave_times);
        graves = ng;
        grave_times = nt;
        grave_cap = ncap;
        grave_head = 0;
    }
    graves[(grave_head + grave_count) % grave_cap] = o;
    grave_times[(grave_head + grave_count) % grave_cap] = now;
    grave_count++;
}

/* the tombstone of this address, if it still has its slot */
static Obj *find_dead_locked(uint64_t key) {
    if (!table) return NULL;
    for (uint64_t i = slot_of(key, cap);; i = (i + 1) & (cap - 1)) {
        if (!table[i]) return NULL;
        if (table_keys[i] == key && !table[i]->kind) return table[i];
    }
}

static void insert_locked(Obj *o) {
    if (!table || used * 2 >= cap) {
        uint64_t ncap = cap ? cap * 2 : 1 << 16;
        Table *t = (Table *)calloc(1, sizeof(Table) + ncap * (sizeof(Obj *) + sizeof(uint64_t)));
        Obj **nt = t->slot;
        t->cap = ncap;
        uint64_t *nk = table_keys_of(t);
        for (uint64_t i = 0; i < cap; i++)
            if (table[i] && table[i]->kind) {
                uint64_t j = slot_of(table[i]->key, ncap);
                while (nt[j]) j = (j + 1) & (ncap - 1);
                nt[j] = table[i];
                nk[j] = table[i]->key;
            } else if (table[i]) {
                bury_locked(table[i]);
            }
        table = nt;
        table_keys = nk;
        cap = ncap;
        used = 0;
        for (uint64_t i = 0; i < cap; i++) used += table[i] != NULL;
        __atomic_store_n(&live, t, __ATOMIC_RELEASE);
    }
    uint64_t i = slot_of(o->key, cap);
    while (table[i] && table[i]->kind) i = (i + 1) & (cap - 1);
    if (!table[i]) used++;
    else bury_locked(table[i]);   /* (a tombstone of another address) */
    __atomic_store_n(&table_keys[i], o->key, __ATOMIC_RELAXED);   /* (before the object: see find_unlocked) */
    __atomic_store_n(&table[i], o, __ATOMIC_RELEASE);
}

static Obj *find_unlocked(uint64_t key, int kind) {
    const Table *t = __atomic_load_n(&live, __ATOMIC_ACQUIRE);
    if (!t) return NULL;
    const uint64_t *keys = table_keys_of(t);
    for (uint64_t i = slot_of(key, t->cap), n = 0; n < t->cap; i = (i + 1) & (t->cap - 1), n++) {
        Obj *o = __atomic_load_n(&t->slot[i], __ATOMIC_ACQUIRE);
        if (!o) return NULL;
        /* (the key beside the slot first; a slot changing meanwhile gives a miss, and the locked lookup answers) */
        if (__atomic_load_n(&keys[i], __ATOMIC_RELAXED) == key && o->key == key &&
            __atomic_load_n(&o->kind, __ATOMIC_ACQUIRE) == kind)
            return o;
    }
    return NULL;
}

static Obj *obj_lookup(uint64_t key, int kind);
/* Every nn::os call looks its object up by the guest address (~1.6% of the game threads' time at the castle: a probe
   of the shared table and a cache miss on a scattered object). Game threads use a few objects over and over: a small
   direct-mapped cache per thread answers those. An entry holds while its object is still that address's and of the
   kind asked for (objects are reused, never freed: spares). HOS_NO_OBJ_CACHE=1: off. */
static Obj *obj_nockpt(uint64_t key, int kind);
static Obj *obj(uint64_t key, int kind) {
    hos_vcore_checkpoint();   /* (virtual cores: every nn::os call, where a better waiter takes the core) */
    return obj_nockpt(key, kind);
}
/* obj without the checkpoint, for lookups under a host lock: a checkpoint there can give the core up and wait for one
   while the lock is held, and a thread that holds a core can then block on that lock (wait_any's scan under mw_lock vs
   poke_multiwait: a stall until the 250 ms vcore escape) */
static Obj *obj_nockpt(uint64_t key, int kind) {
    /* (hookshot: 128 still missed: obj_lookup was the busiest leaf in dense Kokiri, 3.2% of all samples, 4.3% of the instants when one
       thread alone ran; each actor has its own mutexes) */
    static _Thread_local struct { uint64_t key; Obj *o; } cache[1024];
    static int off = -1;
    if (off < 0) off = env_on("HOS_NO_OBJ_CACHE");
    if (!off) {
        const unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ULL) >> 54);
        Obj *o = cache[i].o;
        if (cache[i].key == key && o && __atomic_load_n(&o->key, __ATOMIC_RELAXED) == key &&
            __atomic_load_n(&o->kind, __ATOMIC_ACQUIRE) == kind)
            return o;
        o = obj_lookup(key, kind);
        cache[i].key = key, cache[i].o = o;
        return o;
    }
    return obj_lookup(key, kind);
}

static Obj *obj_lookup(uint64_t key, int kind) {
    static int locked_reads = -1;
    if (locked_reads < 0) locked_reads = env_on("HOS_SYNC_TABLE_LOCK");
    Obj *o;
    if (!locked_reads) {
        if ((o = find_unlocked(key, kind))) return o;
    } else {
        AcquireSRWLockShared(&table_lock);
        o = find_locked(key);
        ReleaseSRWLockShared(&table_lock);
        if (o && o->kind == kind) return o;
    }
    AcquireSRWLockExclusive(&table_lock);
    o = find_locked(key);
    if (o && o->kind != kind) { __atomic_store_n(&o->kind, 0, __ATOMIC_RELEASE); o = NULL; }   /* memory reused for another kind of object */
    if (!o && (o = find_dead_locked(key))) {
        /* the same address initialized again (an object on a stack, every frame): its own tombstone, as new (the
           lock and condition variable stay as they are: nothing holds them now) */
        free(o->q);
        free(o->links);
        memset(&o->owner, 0, sizeof *o - offsetof(Obj, owner));
        __atomic_store_n(&o->kind, kind, __ATOMIC_RELEASE);
    }
    if (!o) {
        if ((o = spares)) {   /* (its old address's caches see the new key: see spares) */
            spares = o->spare;
            memset(&o->owner, 0, sizeof *o - offsetof(Obj, owner));
            __atomic_store_n(&o->kind, 0, __ATOMIC_RELAXED);
        } else {
            o = (Obj *)calloc(1, sizeof *o);
        }
        InitializeSRWLock(&o->lk);
        InitializeConditionVariable(&o->cv);
        __atomic_store_n(&o->key, key, __ATOMIC_RELAXED);
        __atomic_store_n(&o->kind, kind, __ATOMIC_RELEASE);
        insert_locked(o);
    }
    ReleaseSRWLockExclusive(&table_lock);
    return o;
}

static Obj *fresh(uint64_t key, int kind) {
    AcquireSRWLockExclusive(&table_lock);
    Obj *o = find_locked(key);
    if (o) __atomic_store_n(&o->kind, 0, __ATOMIC_RELEASE);   /* re-initialized: forget the old one */
    ReleaseSRWLockExclusive(&table_lock);
    return obj(key, kind);
}

static void drop(uint64_t key) {
    AcquireSRWLockExclusive(&table_lock);
    Obj *o = find_locked(key);
    if (o) __atomic_store_n(&o->kind, 0, __ATOMIC_RELEASE);   /* tombstone (see bury_locked) */
    ReleaseSRWLockExclusive(&table_lock);
}

/* Wait on o->cv until `deadline` (ns, 0 = forever). Returns 0 on timeout. */
static int wait_until(Obj *o, int64_t deadline) {
    if (!deadline) return hos_cv_sleep(&o->cv, &o->lk, INFINITE), 1;
    int64_t left = deadline - hos_now_ns();
    if (left <= 0) return 0;
    /* (in steps of at most ~24 days: 0xFFFFFFFF ms is INFINITE; callers wait again until the deadline) */
    const int64_t ms = (left + 999999) / 1000000;
    hos_cv_sleep(&o->cv, &o->lk, (DWORD)(ms < 0x7FFFFFFF ? ms : 0x7FFFFFFF));
    return 1;
}

/* ---- multi-wait wake-ups: any event change pokes waiting WaitAny calls --------------------------------- */
static SRWLOCK mw_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE mw_cv = CONDITION_VARIABLE_INIT;
static volatile LONG mw_waiting;
/* The guest addresses linked holders watch (events, timers, semaphores, queues), each with how many holders watch it.
   Kept by address under mw_lock, not on the Obj: objects are re-initialized and their memory reused by address (fresh,
   the tombstone path in obj_lookup) while holders still link them, and Link/Unlink never look the target up. */
typedef struct { uint64_t key; int n; } MwWatched;
static MwWatched *mw_watched;
static int mw_nwatched, mw_capwatched;
static int mw_watch_incomplete;   /* failed registration: keep old global wakes for this process */
static int mw_watched_at_locked(uint64_t key) {
    for (int i = 0; i < mw_nwatched; i++)
        if (mw_watched[i].key == key) return i;
    return -1;
}
static void mw_watch_add_locked(uint64_t key) {
    const int i = mw_watched_at_locked(key);
    if (i >= 0) { mw_watched[i].n++; return; }
    if (mw_nwatched == mw_capwatched) {
        MwWatched *w = (MwWatched *)realloc(mw_watched, (mw_capwatched ? mw_capwatched * 2 : 16) * sizeof *w);
        if (!w) { mw_watch_incomplete = 1; return; }
        mw_watched = w;
        mw_capwatched = mw_capwatched ? mw_capwatched * 2 : 16;
    }
    mw_watched[mw_nwatched].key = key, mw_watched[mw_nwatched].n = 1;
    mw_nwatched++;
}
static void mw_watch_remove_locked(uint64_t key) {
    const int i = mw_watched_at_locked(key);
    if (i >= 0 && --mw_watched[i].n <= 0) mw_watched[i] = mw_watched[--mw_nwatched];
}
/* o: the object that changed. Only an object some holder links into a multi-wait can make a WaitAny ready, so the
   others no longer wake every WaitAny: each event, semaphore or queue change anywhere woke them all, and OoT's three
   nn.npln threads (offline: their own events never set) woke ~8,500 times a second each to sleep again, 57% of the game's
   context switches. The check is under mw_lock, as Link and WaitAny's scan are: a link before it is seen here, one after
   it is followed by a scan that sees the stored state. HOS_MW_WAKE=all: every change, as before */
static void poke_multiwait(Obj *o) {
    static int all = -1;
    if (all < 0) all = getenv("HOS_MW_WAKE") && !strcmp(getenv("HOS_MW_WAKE"), "all");
    if (!InterlockedCompareExchange(&mw_waiting, 0, 0)) return;
    AcquireSRWLockExclusive(&mw_lock);
    const int wake = all || mw_watch_incomplete || !o || mw_watched_at_locked(o->key) >= 0;
    ReleaseSRWLockExclusive(&mw_lock);
    if (wake) WakeAllConditionVariable(&mw_cv);
}

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__EMSCRIPTEN__)
#include <emmintrin.h>
static int spin_begin(Obj *o, int *misses) {
    if (hos_spin_ns() <= 0) return 0;
    static int adaptive = -1;
    if (adaptive < 0) adaptive = !env_on("HOS_SPIN_ALWAYS");
    *misses = __atomic_load_n(&o->spin_misses, __ATOMIC_RELAXED);
    return !(adaptive && *misses >= 4 && (__atomic_add_fetch(&o->spin_waits, 1, __ATOMIC_RELAXED) & 31));
}
static void spin_end(Obj *o, int misses, int ok) {
    if (ok) __atomic_store_n(&o->spin_misses, 0, __ATOMIC_RELAXED);
    else if (misses < 4) __atomic_store_n(&o->spin_misses, misses + 1, __ATOMIC_RELAXED);
}
static void spin_until_set(Obj *o, volatile int *flag) {
    int misses;
    if (*flag > 0 || !spin_begin(o, &misses)) return;
    const int64_t end = hos_now_ns() + hos_spin_ns();
    for (;;) {
        for (int i = 0; i < 16; i++) {
            if (*flag > 0) {
                spin_end(o, misses, 1);
                return;
            }
            _mm_pause();
        }
        if (hos_now_ns() >= end) {
            spin_end(o, misses, 0);
            return;
        }
    }
}
static void spin_until_free(Obj *o) {
    int misses;
    if (!__atomic_load_n(&o->owner, __ATOMIC_RELAXED) || !spin_begin(o, &misses)) return;
    const int64_t end = hos_now_ns() + hos_spin_ns();
    for (;;) {
        for (int i = 0; i < 16; i++) {
            if (!__atomic_load_n(&o->owner, __ATOMIC_RELAXED)) {
                spin_end(o, misses, 1);
                return;
            }
            _mm_pause();
        }
        if (hos_now_ns() >= end) {
            spin_end(o, misses, 0);
            return;
        }
    }
}
#else
static void spin_until_set(Obj *o, volatile int *flag) { (void)o; (void)flag; }
static void spin_until_free(Obj *o) { (void)o; }
#endif

/* ---- mutex ----------------------------------------------------------------------------------------------- */
/* Mutexes: the owner is taken with one compare-and-swap and given back with one exchange; the SRW lock and condition
   variable are only for threads that must sleep (mwait counts them, and unlock wakes only then). The engine locks
   mutexes all the time (its job queues lock one per item); an uncontended lock and unlock were two SRW lock pairs and
   a wake. No wake-up is lost: a sleeper raises mwait, then tries the CAS, under m->lk; an unlocker exchanges the owner,
   then reads mwait (both sequentially consistent): either it sees the sleeper and takes m->lk (held until the sleeper
   sleeps) to wake it, or the sleeper's CAS sees the mutex free. */
/* (EvilOOT, 11-07) Horizon hands a mutex to its waiters: a condition variable's signal makes the woken thread one of
   the mutex's waiters in the kernel, and the unlock gives the mutex to it. Here any thread could take it first: the
   movie player's thread signalled its decoder to stop, unlocked, locked again before the decoder had the mutex, and
   joined it holding the mutex: a freeze after Load about one start in four. A signal now reserves the mutex for the
   thread it wakes (reserved): others cannot take it until that thread has. */
static int mutex_take(Obj *m, HosThread *self) {
    if (__atomic_load_n(&m->reserved, __ATOMIC_SEQ_CST) > 0) return 0;   /* (owed to a woken waiter) */
    HosThread *none = NULL;
    return __atomic_compare_exchange_n(&m->owner, &none, self, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}
static int mutex_take_owed(Obj *m, HosThread *self) {   /* (the woken waiter the mutex is reserved for) */
    HosThread *none = NULL;
    if (!__atomic_compare_exchange_n(&m->owner, &none, self, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return 0;
    __atomic_sub_fetch(&m->reserved, 1, __ATOMIC_SEQ_CST);
    return 1;
}
static void mutex_give(Obj *m) {   /* (the owner's count already 0) */
    __atomic_exchange_n(&m->owner, (HosThread *)NULL, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m->mwait, __ATOMIC_SEQ_CST)) {
        HOS_CHAIN(HOS_CHAIN_SIGNAL, m->key);
        AcquireSRWLockExclusive(&m->lk);
        ReleaseSRWLockExclusive(&m->lk);
        /* (all of them while one is owed it: woken alone, another sleeper would fail its take and sleep again) */
        if (__atomic_load_n(&m->reserved, __ATOMIC_SEQ_CST) > 0) WakeAllConditionVariable(&m->cv);
        else WakeConditionVariable(&m->cv);
    }
}
static void mutex_lock(Obj *m, uint64_t guest, Ctx *c);
/* the lock at the end of a condition variable wait: owed = the signal that woke it reserved the mutex for it */
static void mutex_lock_owed(Obj *m, uint64_t guest, int owed) {
    if (!owed) { mutex_lock(m, guest, NULL); return; }
    HosThread *self = hos_self();
    if (mutex_take_owed(m, self)) { m->count = 1; return; }
    HOS_CHAIN(HOS_CHAIN_WAIT, m->key);
    AcquireSRWLockExclusive(&m->lk);
    __atomic_add_fetch(&m->mwait, 1, __ATOMIC_SEQ_CST);
    while (!mutex_take_owed(m, self)) hos_cv_sleep(&m->cv, &m->lk, 2000);
    __atomic_sub_fetch(&m->mwait, 1, __ATOMIC_SEQ_CST);
    ReleaseSRWLockExclusive(&m->lk);
    HOS_CHAIN(HOS_CHAIN_WOKE, m->key);
    m->count = 1;
}
static int handoff_on(void) {   /* HOS_NO_HANDOFF=1: the old way (any thread may take a mutex a signal's waiter is owed) */
    static int v = -1;
    if (v < 0) v = env_on("HOS_NO_HANDOFF") ? 0 : 1;
    return v;
}
/* a waiter in: (cv->lk held) */
/* a waiter counted in for SignalConditionVariable's check, before it takes cv->lk: a signal that reads 0 then came
   before this waiter started to wait. Counted after cv->lk instead, a signal made without the guest mutex could read 0
   while the waiter held cv->lk between its generation read and cv_enter, and return without the wake the old Signal
   (which waited for cv->lk) gave it. cv_leave counts it out. */
static void cv_count_in(Obj *cv) {
    __atomic_add_fetch(&cv->cv_waiting, 1, __ATOMIC_SEQ_CST);
}
static void cv_enter(Obj *cv, Obj *m) {
    if (cv->cv_in && cv->cv_mutex != m) cv->cv_mixed = 1;
    if (!cv->cv_in) cv->cv_mixed = 0;
    cv->cv_mutex = m;
    cv->cv_in++;
}
/* a waiter out (cv->lk held): whether a signal reserved its mutex m for it. A reservation made on another mutex (a
   waiter with a different one came in after the signal) is given back: never left owed to nobody */
static int cv_leave(Obj *cv, Obj *m) {
    cv->cv_in--;
    __atomic_sub_fetch(&cv->cv_waiting, 1, __ATOMIC_SEQ_CST);   /* (counted in by cv_count_in) */
    if (cv->cv_owed <= 0) return 0;
    Obj *r = cv->cv_owed_mutex[--cv->cv_owed];
    if (r == m) return 1;
    __atomic_sub_fetch(&r->reserved, 1, __ATOMIC_SEQ_CST);
    return 0;
}
/* a signal (cv->lk held): the mutex reserved for the waiter it wakes */
static void cv_reserve(Obj *cv) {
    if (!handoff_on() || cv->cv_mixed || !cv->cv_mutex || cv->cv_in <= cv->cv_owed || cv->cv_owed >= 8) return;
    cv->cv_owed_mutex[cv->cv_owed++] = cv->cv_mutex;
    __atomic_add_fetch(&cv->cv_mutex->reserved, 1, __ATOMIC_SEQ_CST);
}
static void mutex_lock(Obj *m, uint64_t guest, Ctx *c) {
    HosThread *self = hos_self();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == self) { m->count++; return; }
    if (mutex_take(m, self)) { m->count = 1; return; }
    spin_until_free(m);
    if (mutex_take(m, self)) { m->count = 1; return; }
    /* HOS_MUTEX_TRACE=<ms> (diagnostic): waits longer than that are logged with the thread that held the mutex */
    static int trace_ms = -1;
    if (trace_ms < 0) { const char *e = getenv("HOS_MUTEX_TRACE"); trace_ms = e ? atoi(e) : 0; }
    HosThread *holder = __atomic_load_n(&m->owner, __ATOMIC_RELAXED);
    const int64_t t0 = trace_ms && holder ? hos_now_ns() : 0;
    const int chain = hos_chain_on;
    if (chain) hos_chain_note(HOS_CHAIN_WAIT, m->key, c ? c->x[30] : 0);
    AcquireSRWLockExclusive(&m->lk);
    __atomic_add_fetch(&m->mwait, 1, __ATOMIC_SEQ_CST);
    {   /* (EvilOOT, 11-07: a wait of over 10 s says who holds the mutex, once per wait: the freeze after Load) */
        int64_t told_at = hos_now_ns() + 10000000000ll;
        while (!mutex_take(m, self)) {
            hos_cv_sleep(&m->cv, &m->lk, 2000);
            if (told_at && hos_now_ns() > told_at) {
                HosThread *h = __atomic_load_n(&m->owner, __ATOMIC_RELAXED);
                hos_log("mutex %#llx (obj %p): %s waiting 10 s; held by %s (count %d, %d waiting)", (unsigned long long)guest, (void *)m,
                        hos_thread_name(self), h ? hos_thread_name(h) : "nobody", m->count, (int)m->mwait);
                told_at = 0;
            }
        }
    }
    __atomic_sub_fetch(&m->mwait, 1, __ATOMIC_SEQ_CST);
    ReleaseSRWLockExclusive(&m->lk);
    if (chain) hos_chain_note(HOS_CHAIN_WOKE, m->key, c ? c->x[30] : 0);
    m->count = 1;
    if (t0) {
        const int64_t waited = hos_now_ns() - t0;
        static volatile LONG logged;
        if (waited > (int64_t)trace_ms * 1000000 && InterlockedIncrement(&logged) <= 60)
            hos_log("mutex %#llx: waited %.1f ms (lr %#llx); held by %s", (unsigned long long)guest, waited / 1e6,
                    (unsigned long long)(c ? c->x[30] : 0), hos_thread_name(holder));
    }
}

static int mutex_unlock(Obj *m) {   /* returns the count it had */
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != hos_self()) return m->count;
    const int had = m->count;
    if (--m->count == 0) mutex_give(m);
    return had;
}
/* a condition variable's wait: the mutex given back whatever its count; the count it had */
static int mutex_release_all(Obj *m) {
    const int count = m->count;
    m->count = 0;
    mutex_give(m);
    return count;
}

/* (EvilOOT, 11-07) HOS_MUTEX_WATCH=<guest address>: every operation on that mutex, and the signals of the condition
   variables waited on with it, logged with the thread and the caller (lr). The freeze after Load: MovieOperateThread
   holds 0x106a387d58 and joins VideoDecoderThread, which waits for it on its way out of a condition variable wait. */
static uint64_t watch_mutex(void) {
    static int looked;
    static uint64_t addr;
    if (!looked) { const char *e = getenv("HOS_MUTEX_WATCH"); addr = e ? strtoull(e, NULL, 16) : 0; looked = 1; }
    return addr;
}
static volatile uint64_t watch_cvs[8];
static void watch_log(const char *what, uint64_t m, uint64_t cv, Ctx *c, Obj *o) {
    HosThread *self = hos_self(), *own = o ? __atomic_load_n(&o->owner, __ATOMIC_RELAXED) : NULL;
    hos_log("watch %s: mutex %#llx cv %#llx by %s (lr %#llx); owner %s count %d", what, (unsigned long long)m,
            (unsigned long long)cv, self ? hos_thread_name(self) : "?", (unsigned long long)(c ? c->x[30] : 0),
            own ? hos_thread_name(own) : "none", o ? o->count : -1);
}
static int watched(uint64_t m) { return m && m == watch_mutex(); }
static int watched_cv(uint64_t cv) {
    if (!watch_mutex()) return 0;
    for (int i = 0; i < 8; i++) if (watch_cvs[i] == cv) return 1;
    return 0;
}

HLE(_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi) {
    fresh(ARG(0), K_MUTEX);
    if (watched(ARG(0))) watch_log("init", ARG(0), 0, c, NULL);
}
HLE(_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE) {
    if (watched(ARG(0))) watch_log("finalize", ARG(0), 0, c, NULL);
    drop(ARG(0));
}
HLE(_ZN2nn2os9LockMutexEPNS0_9MutexTypeE) {
    Obj *m = obj(ARG(0), K_MUTEX);
    if (watched(ARG(0))) watch_log("lock", ARG(0), 0, c, m);
    mutex_lock(m, ARG(0), c);
    if (watched(ARG(0))) watch_log("locked", ARG(0), 0, c, m);
}
HLE(_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE) {
    Obj *m = obj(ARG(0), K_MUTEX);
    if (watched(ARG(0))) watch_log("unlock", ARG(0), 0, c, m);
    mutex_unlock(m);
}
HLE(_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE) {
    Obj *m = obj(ARG(0), K_MUTEX);
    HosThread *self = hos_self();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == self) { m->count++; RET(1); return; }
    const int ok = mutex_take(m, self);
    if (ok) m->count = 1;
    if (watched(ARG(0))) watch_log(ok ? "trylock ok" : "trylock failed", ARG(0), 0, c, m);
    RET(ok);
}
HLE(_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE) {
    RET(__atomic_load_n(&obj(ARG(0), K_MUTEX)->owner, __ATOMIC_RELAXED) == hos_self());
}

/* ---- condition variable ---------------------------------------------------------------------------------- */
HLE(_ZN2nn2os27InitializeConditionVariableEPNS0_21ConditionVariableTypeE) { fresh(ARG(0), K_COND); }
HLE(_ZN2nn2os25FinalizeConditionVariableEPNS0_21ConditionVariableTypeE) { drop(ARG(0)); }
/* A signal with no waiter returns at once (~2,500 signals a frame in Hyrule: each took cv->lk and made a wake call, a
   waiter there or not), as glibc's pthread_cond_signal does, and as Horizon's user mode does when the key reads 0. No
   waiter that can still miss its predicate's change is missed: a waiter counts itself in (cv_enter) under cv->lk before
   mutex_release_all gives the guest mutex back (sequentially consistent), so a signaller that holds the mutex, having
   taken it after that, reads the count with the waiter in; a waiter woken and not yet out (cv_leave) still counts. A
   signaller that does not hold the mutex can miss a waiter between its gen snapshot and its sleep, as on Horizon: with
   the predicate changed under the mutex that waiter has not yet looked at it. HOS_CV_SIGNAL_ALWAYS=1: every signal takes
   the lock, as before (tests/test_cv_signal_early.py) */
static int cv_signal_always(void) {
    static int v = -1;   /* (atomic: the first signals of several threads read it at once) */
    int on = __atomic_load_n(&v, __ATOMIC_RELAXED);
    if (on < 0) __atomic_store_n(&v, on = env_on("HOS_CV_SIGNAL_ALWAYS"), __ATOMIC_RELAXED);
    return on;
}
HLE(_ZN2nn2os23SignalConditionVariableEPNS0_21ConditionVariableTypeE) {
    Obj *cv = obj(ARG(0), K_COND);
    if (watched_cv(ARG(0))) watch_log("signal", watch_mutex(), ARG(0), c, NULL);
    HOS_CHAIN(HOS_CHAIN_SIGNAL, cv->key);
    if (!__atomic_load_n(&cv->cv_waiting, __ATOMIC_SEQ_CST) && !cv_signal_always()) return;
    AcquireSRWLockExclusive(&cv->lk);
    cv_reserve(cv);
    cv->gen++;
    ReleaseSRWLockExclusive(&cv->lk);
    WakeConditionVariable(&cv->cv);
}
/* (hookshot) Broadcast: every waiter wakes. Left to the guest SDK, it signals the kernel key at cv+4, which no HLE waiter
   sleeps on, so its waiters slept on until some Signal (the movie framework's reply and buffer queues, NVIDIA's OS layer:
   untimed waits). Horizon moves all the waiters onto the mutex, so each is owed it, as a Signal's one is (cv_reserve: one mutex for all of them, at most 8 owed). */
HLE(_ZN2nn2os26BroadcastConditionVariableEPNS0_21ConditionVariableTypeE) {
    Obj *cv = obj(ARG(0), K_COND);
    if (watched_cv(ARG(0))) watch_log("broadcast", watch_mutex(), ARG(0), c, NULL);
    HOS_CHAIN(HOS_CHAIN_SIGNAL, cv->key);
    AcquireSRWLockExclusive(&cv->lk);
    for (int n = cv->cv_waiting - cv->cv_owed; n > 0; n--) cv_reserve(cv);
    cv->gen++;
    ReleaseSRWLockExclusive(&cv->lk);
    WakeAllConditionVariable(&cv->cv);
}
HLE(_ZN2nn2os21WaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeE) {
    Obj *cv = obj(ARG(0), K_COND), *m = obj(ARG(1), K_MUTEX);
    const int w = watched(ARG(1));
    if (w) {
        int i = 0;
        while (i < 8 && watch_cvs[i] && watch_cvs[i] != ARG(0)) i++;
        if (i < 8) watch_cvs[i] = ARG(0);
        watch_log("cv wait", ARG(1), ARG(0), c, m);
    }
    cv_count_in(cv);
    AcquireSRWLockExclusive(&cv->lk);
    uint64_t gen = cv->gen;
    cv_enter(cv, m);
    /* release the guest mutex fully while holding the condvar's lock: a signal cannot slip in between */
    const int count = mutex_release_all(m);
    HOS_CHAIN(HOS_CHAIN_WAIT, cv->key);
    while (cv->gen == gen) hos_cv_sleep(&cv->cv, &cv->lk, INFINITE);
    const int owed = cv_leave(cv, m);
    ReleaseSRWLockExclusive(&cv->lk);
    HOS_CHAIN(HOS_CHAIN_WOKE, cv->key);
    if (w) watch_log(owed ? "cv woken, owed the mutex" : "cv woken", ARG(1), ARG(0), c, m);
    mutex_lock_owed(m, ARG(1), owed);
    m->count = count;
    if (w) watch_log("cv relocked", ARG(1), ARG(0), c, m);
}

/* ---- events (EventType, LightEventType, SystemEventType share this) ------------------------------------------ */
static void event_init(uint64_t key, int signaled, int clear_mode) {
    Obj *e = fresh(key, K_EVENT);
    e->signaled = signaled;
    e->autoclear = clear_mode == 1;   /* EventClearMode_AutoClear = 1, ManualClear = 0 */
}
static void event_signal(Obj *e) {
    HOS_CHAIN(HOS_CHAIN_SIGNAL, e->key);
    AcquireSRWLockExclusive(&e->lk);
    e->signaled = 1;
    if (wake_lat_on()) e->sig_ns = hos_now_ns();
    ReleaseSRWLockExclusive(&e->lk);
    WakeAllConditionVariable(&e->cv);
    poke_multiwait(e);
}
static void event_clear(Obj *e) {
    AcquireSRWLockExclusive(&e->lk);
    e->signaled = 0;
    ReleaseSRWLockExclusive(&e->lk);
}

/* HOS_WAKE_LAT=1 (diagnostic): for event and message queue waits by thread, every 5 s: how many found it ready at once,
   after spinning, or after sleeping, and for the sleepers the time from the signal to running again (a hand-off's
   cost on the frame's chain of threads) */
static int wake_lat_on(void) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_WAKE_LAT");
    return on;
}
static void wake_lat_note(int how, int64_t lat_ns) {   /* how: 0 ready, 1 spun, 2 slept */
    static SRWLOCK lk = SRWLOCK_INIT;
    static struct { const char *thread; int64_t n[3], lat, max; } tab[32];
    static int64_t last;
    const char *thread = hos_thread_name(hos_self());
    AcquireSRWLockExclusive(&lk);
    int i = 0;
    while (i < 32 && tab[i].thread && tab[i].thread != thread) i++;
    if (i < 32) {
        tab[i].thread = thread;
        tab[i].n[how]++;
        if (how == 2) {
            tab[i].lat += lat_ns;
            if (lat_ns > tab[i].max) tab[i].max = lat_ns;
        }
    }
    const int64_t now = hos_now_ns();
    if (now - last > 5000000000LL) {
        last = now;
        for (int k = 0; k < 32 && tab[k].thread; k++)
            if (tab[k].n[2] > 100)
                hos_log("wake: %s: %lld ready, %lld after spinning, %lld after sleeping (signal to running %.1f us average, "
                        "%.0f us longest)", tab[k].thread, (long long)tab[k].n[0], (long long)tab[k].n[1],
                        (long long)tab[k].n[2], tab[k].lat / 1e3 / (double)tab[k].n[2], tab[k].max / 1e3);
        memset(tab, 0, sizeof tab);
    }
    ReleaseSRWLockExclusive(&lk);
}

static int event_wait(Obj *e, int64_t timeout_ns, int try_only) {
    int64_t deadline = timeout_ns > 0 ? hos_now_ns() + timeout_ns : 0;
    const int was_set = e->signaled > 0;
    const int chain = !try_only && !was_set && hos_chain_on;
    if (chain) hos_chain_note(HOS_CHAIN_WAIT, e->key, 0);
    if (!try_only) spin_until_set(e, &e->signaled);
    int slept = 0;
    AcquireSRWLockExclusive(&e->lk);
    const int spun = !was_set && e->signaled;
    while (!e->signaled) {
        if (try_only || (timeout_ns > 0 && !wait_until(e, deadline))) break;
        if (timeout_ns <= 0) hos_cv_sleep(&e->cv, &e->lk, INFINITE);
        slept = 1;
    }
    int got = e->signaled;
    if (got && !try_only && wake_lat_on()) wake_lat_note(slept ? 2 : spun ? 1 : 0, slept ? hos_now_ns() - e->sig_ns : 0);
    if (got && e->autoclear) e->signaled = 0;
    ReleaseSRWLockExclusive(&e->lk);
    if (chain) hos_chain_note(HOS_CHAIN_WOKE, e->key, 0);
    return got;
}

/* event_wait(e, 0, 1) without its spin, sleep and wake-latency note: for WaitAny's scan, which holds mw_lock and
   may make no scheduling point (vcore checkpoint, yield, sleep) there. Set: taken (cleared when auto-clear) */
static int event_peek(Obj *e) {
    AcquireSRWLockExclusive(&e->lk);
    const int got = e->signaled;
    if (got && e->autoclear) e->signaled = 0;
    ReleaseSRWLockExclusive(&e->lk);
    return got;
}

/* SignalEvent from host code (any host thread): devices the OS layer emulates, the video decoder for one. */
void hos_event_signal(uint64_t guest_event) { event_signal(obj(guest_event, K_EVENT)); }

HLE(_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE) { event_init(ARG(0), (int)(ARG(1) & 1), (int)ARG(2)); }
HLE(_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE) { drop(ARG(0)); }
HLE(_ZN2nn2os11SignalEventEPNS0_9EventTypeE) { event_signal(obj(ARG(0), K_EVENT)); }
HLE(_ZN2nn2os10ClearEventEPNS0_9EventTypeE) { event_clear(obj(ARG(0), K_EVENT)); }

/* HOS_WAIT_LOG=1 (diagnostic): event waits by guest caller and thread every 5 s (count, total, longest), the
   longest first: what a thread blocks on */
static int logged_wait(Ctx *c, Obj *e, int64_t timeout_ns, int try_only, const char *what) {
    static int on = -1, long_waits = -1;
    if (on < 0) on = env_on("HOS_WAIT_LOG");
    /* HOS_LONG_WAITS=1 (diagnostic): an untimed wait still blocked after 5 s is logged (thread, caller), then goes on:
       what a stuck game waits for */
    if (long_waits < 0) long_waits = env_on("HOS_LONG_WAITS");
    if (long_waits && !try_only && timeout_ns == 0) {
        static int logged;
        if (event_wait(e, 5000000000LL, 0)) return 1;
        if (logged++ < 64) hos_log("wait: %s blocked 5 s on a %s event %#llx (lr %#llx)", hos_thread_name(hos_self()), what,
                (unsigned long long)e->key, (unsigned long long)c->x[30]);
        const int r = event_wait(e, 0, 0);
        if (logged < 64) hos_log("wait: %s's %s event %#llx came (lr %#llx)", hos_thread_name(hos_self()), what,
                (unsigned long long)e->key, (unsigned long long)c->x[30]);
        return r;
    }
    if (!on || try_only) return event_wait(e, timeout_ns, try_only);
    const int64_t t0 = hos_now_ns();
    const int r = event_wait(e, timeout_ns, try_only);
    const int64_t took = hos_now_ns() - t0;
    static SRWLOCK lk = SRWLOCK_INIT;
    static struct { uint64_t lr; const char *thread, *what; int64_t n, total, max; } tab[128];
    static int64_t last;
    const char *thread = hos_thread_name(hos_self());
    AcquireSRWLockExclusive(&lk);
    int i = 0;
    while (i < 128 && tab[i].lr && !(tab[i].lr == c->x[30] && tab[i].thread == thread)) i++;
    if (i < 128) {
        tab[i].lr = c->x[30];
        tab[i].thread = thread;
        tab[i].what = what;
        tab[i].n++;
        tab[i].total += took;
        if (took > tab[i].max) tab[i].max = took;
    }
    const int64_t now = hos_now_ns();
    if (now - last > 5000000000LL) {
        last = now;
        for (int k = 0; k < 12; k++) {   /* the 12 longest totals */
            int best = -1;
            for (int j = 0; j < 128 && tab[j].lr; j++)
                if (tab[j].n && (best < 0 || tab[j].total > tab[best].total)) best = j;
            if (best < 0) break;
            hos_log("wait: %s %s lr %#llx: %lld waits, %.1f ms in all, longest %.2f ms", tab[best].thread ? tab[best].thread : "?",
                    tab[best].what, (unsigned long long)tab[best].lr, (long long)tab[best].n, tab[best].total / 1e6,
                    tab[best].max / 1e6);
            tab[best].n = 0;
        }
        memset(tab, 0, sizeof tab);
    }
    ReleaseSRWLockExclusive(&lk);
    return r;
}

HLE(_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE) { event_init(ARG(0), (int)(ARG(1) & 1), (int)ARG(2)); }
HLE(_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE) { drop(ARG(0)); }
/* HOS_WAIT_WHO=<thread name> (diagnostic): which threads (and guest callers) signal the light events that thread
   waits on, every 5 s: the other end of its waits */
static const char *who_thread;
static uint64_t who_keys[64];
static int nwho_keys;
static void who_note_wait(Ctx *c, uint64_t key) {   /* HOS_WAIT_WHO_LR=<hex>: only waits from that caller */
    static int on = -1;
    static uint64_t only_lr;
    if (on < 0) {
        who_thread = getenv("HOS_WAIT_WHO");
        on = who_thread != NULL;
        const char *l = getenv("HOS_WAIT_WHO_LR");
        only_lr = l ? strtoull(l, NULL, 16) : 0;
    }
    if (!on || strcmp(hos_thread_name(hos_self()), who_thread) || (only_lr && c->x[30] != only_lr)) return;
    for (int i = 0; i < nwho_keys; i++)
        if (who_keys[i] == key) return;
    static int next;
    if (nwho_keys < 64) who_keys[nwho_keys++] = key;
    else who_keys[next++ & 63] = key;
}
static void who_note_signal(Ctx *c, uint64_t key) {
    if (!who_thread) return;
    int hit = 0;
    for (int i = 0; i < nwho_keys && !hit; i++) hit = who_keys[i] == key;
    if (!hit) return;
    static SRWLOCK lk = SRWLOCK_INIT;
    static struct { uint64_t key, lr; const char *thread; int64_t n; } tab[64];
    static int64_t last;
    const char *thread = hos_thread_name(hos_self());
    AcquireSRWLockExclusive(&lk);
    int i = 0;
    while (i < 64 && tab[i].key && !(tab[i].key == key && tab[i].lr == c->x[30] && tab[i].thread == thread)) i++;
    if (i < 64) {
        tab[i].key = key, tab[i].lr = c->x[30], tab[i].thread = thread;
        tab[i].n++;
    }
    const int64_t now = hos_now_ns();
    if (now - last > 5000000000LL) {
        last = now;
        for (int k = 0; k < 64 && tab[k].key; k++)
            hos_log("who: %s's event %#llx signaled by %s (lr %#llx) %lld times", who_thread,
                    (unsigned long long)tab[k].key, tab[k].thread, (unsigned long long)tab[k].lr, (long long)tab[k].n);
        memset(tab, 0, sizeof tab);
    }
    ReleaseSRWLockExclusive(&lk);
}
HLE(_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE) {
    who_note_signal(c, ARG(0));
    event_signal(obj(ARG(0), K_EVENT));
}
HLE(_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE) { event_clear(obj(ARG(0), K_EVENT)); }
/* HOS_PROFILE_GATE=<thread name part>:<lr hex> (diagnostic, with HOS_PROFILE): the profiler samples only while that
   thread waits on a light event from that caller (hos_prof.c): what the other threads run while it waits, the frame's
   critical path when it is the one the frame waits for */
volatile long hos_prof_gate;
static int prof_gate_match(Ctx *c) {
    static int on = -1;
    static char thread[64];
    static uint64_t lr;
    if (on < 0) {
        const char *e = getenv("HOS_PROFILE_GATE"), *colon = e ? strchr(e, ':') : NULL;
        on = colon && colon - e < (long)sizeof thread;
        if (on) {
            memcpy(thread, e, (size_t)(colon - e));
            thread[colon - e] = 0;
            lr = strtoull(colon + 1, NULL, 16);
        }
    }
    return on && c->x[30] == lr && strstr(hos_thread_name(hos_self()), thread) != NULL;
}
HLE(_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE) {
    who_note_wait(c, ARG(0));
    const int gate = prof_gate_match(c);
    if (gate) __atomic_add_fetch(&hos_prof_gate, 1, __ATOMIC_SEQ_CST);
    logged_wait(c, obj(ARG(0), K_EVENT), 0, 0, "light");
    if (gate) __atomic_sub_fetch(&hos_prof_gate, 1, __ATOMIC_SEQ_CST);
}
HLE(_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE) {
    RET(event_wait(obj(ARG(0), K_EVENT), (int64_t)ARG(1) > 0 ? (int64_t)ARG(1) : 1, (int64_t)ARG(1) <= 0));
}

/* System events are created by other HLE APIs (vsync, audio device changes ...) with hos_system_event_init. */
void hos_system_event_init(uint64_t key, int autoclear) { event_init(key, 0, autoclear); }
void hos_system_event_signal(uint64_t key) { event_signal(obj(key, K_EVENT)); }
HLE(_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE) { drop(ARG(0)); }
HLE(_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE) { event_clear(obj(ARG(0), K_EVENT)); }
HLE(_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE) { logged_wait(c, obj(ARG(0), K_EVENT), 0, 0, "system"); }
HLE(_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE) { RET(event_wait(obj(ARG(0), K_EVENT), 0, 1)); }

/* ---- timer events ------------------------------------------------------------------------------------------ */
HLE(_ZN2nn2os20InitializeTimerEventEPNS0_14TimerEventTypeENS0_14EventClearModeE) {
    Obj *t = fresh(ARG(0), K_TIMER);
    t->autoclear = (int)ARG(1) == 1;
}
HLE(_ZN2nn2os18FinalizeTimerEventEPNS0_14TimerEventTypeE) { drop(ARG(0)); }
/* StartPeriodicTimerEvent(TimerEventType*, TimeSpan first, TimeSpan interval) */
HLE(_ZN2nn2os23StartPeriodicTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanES3_) {
    Obj *t = obj(ARG(0), K_TIMER);
    AcquireSRWLockExclusive(&t->lk);
    t->due_ns = hos_now_ns() + (int64_t)ARG(1);
    t->period_ns = (int64_t)ARG(2);
    t->signaled = 0;
    ReleaseSRWLockExclusive(&t->lk);
    WakeAllConditionVariable(&t->cv);   /* (a first wait that beat this start sleeps on the cv) */
    poke_multiwait(t);
}
HLE(_ZN2nn2os14WaitTimerEventEPNS0_14TimerEventTypeE) {
    Obj *t = obj(ARG(0), K_TIMER);
    AcquireSRWLockExclusive(&t->lk);
    while (!t->due_ns) hos_cv_sleep(&t->cv, &t->lk, INFINITE);   /* (the rumble thread can wait before the start, a pad connected at boot) */
    int64_t due = t->due_ns;
    ReleaseSRWLockExclusive(&t->lk);
    /* (chrispository, PR 55: a first wait before the start blocks until it, as on Horizon) */
    hos_sleep_ns(due - hos_now_ns());
    AcquireSRWLockExclusive(&t->lk);
    if (t->period_ns > 0) {
        int64_t now = hos_now_ns();
        while (t->due_ns <= now) t->due_ns += t->period_ns;   /* skip missed periods, as a hardware timer would */
    }
    ReleaseSRWLockExclusive(&t->lk);
}

/* ---- semaphores (normal and light) ------------------------------------------------------------------------ */
static void sema_init(uint64_t key, int initial, int max) {
    Obj *s = fresh(key, K_SEMA);
    s->sem = initial;
    s->sem_max = max;
}
static void sema_acquire(Obj *s) {
    const int chain = s->sem <= 0 && hos_chain_on;
    if (chain) hos_chain_note(HOS_CHAIN_WAIT, s->key, 0);
    spin_until_set(s, &s->sem);
    AcquireSRWLockExclusive(&s->lk);
    while (s->sem <= 0) hos_cv_sleep(&s->cv, &s->lk, INFINITE);
    s->sem--;
    ReleaseSRWLockExclusive(&s->lk);
    if (chain) hos_chain_note(HOS_CHAIN_WOKE, s->key, 0);
}
static void sema_release(Obj *s, int n) {
    HOS_CHAIN(HOS_CHAIN_SIGNAL, s->key);
    AcquireSRWLockExclusive(&s->lk);
    s->sem += n;
    if (s->sem_max && s->sem > s->sem_max) s->sem = s->sem_max;
    ReleaseSRWLockExclusive(&s->lk);
    WakeAllConditionVariable(&s->cv);
    poke_multiwait(s);
}
/* an acquire that waits at most ns (<= 0: does not wait); whether it got one */
static int sema_acquire_for(Obj *s, int64_t ns) {
    const int64_t now = hos_now_ns();
    const int64_t deadline = ns <= 0 ? now : ns > INT64_MAX - now ? INT64_MAX : now + ns;   /* (a huge TimeSpan: no wrap) */
    const int chain = ns > 0 && s->sem <= 0 && hos_chain_on;
    if (chain) hos_chain_note(HOS_CHAIN_WAIT, s->key, 0);
    AcquireSRWLockExclusive(&s->lk);
    while (s->sem <= 0 && wait_until(s, deadline) && hos_now_ns() < deadline) {}
    const int ok = s->sem > 0;
    if (ok) s->sem--;
    ReleaseSRWLockExclusive(&s->lk);
    if (chain) hos_chain_note(HOS_CHAIN_WOKE, s->key, 0);
    return ok;
}
HLE(_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii) { sema_init(ARG(0), (int)ARG(1), (int)ARG(2)); }
HLE(_ZN2nn2os17FinalizeSemaphoreEPNS0_13SemaphoreTypeE) { drop(ARG(0)); }
HLE(_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE) { sema_acquire(obj(ARG(0), K_SEMA)); }
HLE(_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeEi) { sema_release(obj(ARG(0), K_SEMA), (int)ARG(1)); }
HLE(_ZN2nn2os24InitializeLightSemaphoreEPNS0_18LightSemaphoreTypeEii) { sema_init(ARG(0), (int)ARG(1), (int)ARG(2)); }
HLE(_ZN2nn2os22FinalizeLightSemaphoreEPNS0_18LightSemaphoreTypeE) { drop(ARG(0)); }
HLE(_ZN2nn2os21AcquireLightSemaphoreEPNS0_18LightSemaphoreTypeE) { sema_acquire(obj(ARG(0), K_SEMA)); }
HLE(_ZN2nn2os21ReleaseLightSemaphoreEPNS0_18LightSemaphoreTypeE) { sema_release(obj(ARG(0), K_SEMA), 1); }
/* (hookshot) the count overload, which OoT imports: left to the guest SDK it would bypass the host's state */
HLE(_ZN2nn2os21ReleaseLightSemaphoreEPNS0_18LightSemaphoreTypeEi) { sema_release(obj(ARG(0), K_SEMA), (int)ARG(1)); }
/* (hookshot) the timed and try acquires: left to the guest SDK they read its count words, which the HLE never writes, and
   wait on a kernel key no HLE release signals (the C library's timed semaphore wait and NVIDIA's OS layer call
   TimedAcquireSemaphore). */
HLE(_ZN2nn2os21TimedAcquireSemaphoreEPNS0_13SemaphoreTypeENS_8TimeSpanE) { RET(sema_acquire_for(obj(ARG(0), K_SEMA), (int64_t)ARG(1))); }
HLE(_ZN2nn2os26TimedAcquireLightSemaphoreEPNS0_18LightSemaphoreTypeENS_8TimeSpanE) { RET(sema_acquire_for(obj(ARG(0), K_SEMA), (int64_t)ARG(1))); }
HLE(_ZN2nn2os24TryAcquireLightSemaphoreEPNS0_18LightSemaphoreTypeE) { RET(sema_acquire_for(obj(ARG(0), K_SEMA), 0)); }

/* ---- message queues (normal and light): the guest's buffer size is the capacity ------------------------------ */
static void queue_init(uint64_t key, int capacity) {
    Obj *q = fresh(key, K_QUEUE);
    q->qcap = capacity > 0 ? capacity : 1;
    q->q = (uint64_t *)calloc(q->qcap, 8);
    if (!q->q) hos_fatal(NULL, "os: message queue of %d entries", q->qcap);   /* (hookshot: was written through NULL) */
}
/* (mkw-recomp) HOS_QUEUE_TRACE=1: the first 400 puts (thread, queue, value) */
static void queue_trace(Obj *q, uint64_t v, const char *what) {
    static int on = -1;
    static volatile LONG n;
    if (on < 0) on = env_on("HOS_QUEUE_TRACE");
    if (on && InterlockedIncrement(&n) <= 400)
        hos_log("queue: %s %s %#llx value %#llx (len %d)", hos_thread_name(hos_self()), what, (unsigned long long)q->key,
                (unsigned long long)v, q->qlen);
}
/* mode: 0 send (back), 1 jam (front). try: do not block. Returns 1 if done. */
static int queue_put(Obj *q, uint64_t v, int jam, int try_only) {
    queue_trace(q, v, jam ? "jam" : "send");
    HOS_CHAIN(HOS_CHAIN_SIGNAL, q->key);
    AcquireSRWLockExclusive(&q->lk);
    while (q->qlen == q->qcap) {
        if (try_only) { ReleaseSRWLockExclusive(&q->lk); return 0; }
        hos_cv_sleep(&q->cv, &q->lk, INFINITE);
    }
    if (jam) {
        q->qhead = (q->qhead + q->qcap - 1) % q->qcap;
        q->q[q->qhead] = v;
    } else {
        q->q[(q->qhead + q->qlen) % q->qcap] = v;
    }
    q->qlen++;
    if (wake_lat_on()) q->sig_ns = hos_now_ns();
    ReleaseSRWLockExclusive(&q->lk);
    WakeAllConditionVariable(&q->cv);
    poke_multiwait(q);
    return 1;
}
/* take: remove it; timeout_ns: < 0 forever, 0 try, > 0 timed. Returns 1 if a value was read. */
static int queue_get(Obj *q, uint64_t *out, int take, int64_t timeout_ns) {
    int64_t deadline = timeout_ns > 0 ? hos_now_ns() + timeout_ns : 0;
    const int was_set = q->qlen > 0;
    const int chain = timeout_ns != 0 && !was_set && hos_chain_on;
    if (chain) hos_chain_note(HOS_CHAIN_WAIT, q->key, 0);
    if (timeout_ns != 0) spin_until_set(q, &q->qlen);
    int slept = 0;
    AcquireSRWLockExclusive(&q->lk);
    const int spun = !was_set && q->qlen;
    while (!q->qlen) {
        if (timeout_ns == 0 || (timeout_ns > 0 && !wait_until(q, deadline))) {
            ReleaseSRWLockExclusive(&q->lk);
            if (chain) hos_chain_note(HOS_CHAIN_WOKE, q->key, 0);
            return 0;
        }
        if (timeout_ns < 0) hos_cv_sleep(&q->cv, &q->lk, INFINITE);
        slept = 1;
    }
    if (timeout_ns != 0 && wake_lat_on()) wake_lat_note(slept ? 2 : spun ? 1 : 0, slept ? hos_now_ns() - q->sig_ns : 0);
    *out = q->q[q->qhead];
    if (take) {
        q->qhead = (q->qhead + 1) % q->qcap;
        q->qlen--;
    }
    ReleaseSRWLockExclusive(&q->lk);
    if (chain) hos_chain_note(HOS_CHAIN_WOKE, q->key, 0);
    if (take) {
        WakeAllConditionVariable(&q->cv);
        poke_multiwait(q);
    }
    return 1;
}

HLE(_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm) { queue_init(ARG(0), (int)ARG(2)); }
HLE(_ZN2nn2os20FinalizeMessageQueueEPNS0_16MessageQueueTypeE) { drop(ARG(0)); }
HLE(_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm) { queue_put(obj(ARG(0), K_QUEUE), ARG(1), 0, 0); }
HLE(_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE) {
    uint64_t v;
    queue_get(obj(ARG(1), K_QUEUE), &v, 1, -1);
    G64(ARG(0)) = v;
}

HLE(_ZN2nn2os27InitializeLightMessageQueueEPNS0_21LightMessageQueueTypeEPmm) { queue_init(ARG(0), (int)ARG(2)); }
HLE(_ZN2nn2os25FinalizeLightMessageQueueEPNS0_21LightMessageQueueTypeE) { drop(ARG(0)); }
HLE(_ZN2nn2os21SendLightMessageQueueEPNS0_21LightMessageQueueTypeEm) { queue_put(obj(ARG(0), K_QUEUE), ARG(1), 0, 0); }
HLE(_ZN2nn2os24TrySendLightMessageQueueEPNS0_21LightMessageQueueTypeEm) { RET(queue_put(obj(ARG(0), K_QUEUE), ARG(1), 0, 1)); }
HLE(_ZN2nn2os20JamLightMessageQueueEPNS0_21LightMessageQueueTypeEm) { queue_put(obj(ARG(0), K_QUEUE), ARG(1), 1, 0); }
HLE(_ZN2nn2os23TryJamLightMessageQueueEPNS0_21LightMessageQueueTypeEm) { RET(queue_put(obj(ARG(0), K_QUEUE), ARG(1), 1, 1)); }
HLE(_ZN2nn2os24ReceiveLightMessageQueueEPmPNS0_21LightMessageQueueTypeE) {
    uint64_t v;
    queue_get(obj(ARG(1), K_QUEUE), &v, 1, -1);
    G64(ARG(0)) = v;
}
HLE(_ZN2nn2os27TryReceiveLightMessageQueueEPmPNS0_21LightMessageQueueTypeE) {
    uint64_t v;
    int ok = queue_get(obj(ARG(1), K_QUEUE), &v, 1, 0);
    if (ok) G64(ARG(0)) = v;
    RET(ok);
}
HLE(_ZN2nn2os24TryPeekLightMessageQueueEPmPKNS0_21LightMessageQueueTypeE) {
    uint64_t v;
    int ok = queue_get(obj(ARG(1), K_QUEUE), &v, 0, 0);
    if (ok) G64(ARG(0)) = v;
    RET(ok);
}
HLE(_ZN2nn2os26TimedPeekLightMessageQueueEPmPKNS0_21LightMessageQueueTypeENS_8TimeSpanE) {
    uint64_t v;
    int64_t t = (int64_t)ARG(2);
    int ok = queue_get(obj(ARG(1), K_QUEUE), &v, 0, t > 0 ? t : 0);
    if (ok) G64(ARG(0)) = v;
    RET(ok);
}

/* ---- reader/writer locks: the write owner may also take read locks and relock for write --------------------- */
static void rw_read(Obj *l) {
    HosThread *self = hos_self();
    AcquireSRWLockExclusive(&l->lk);
    while (l->owner && l->owner != self) hos_cv_sleep(&l->cv, &l->lk, INFINITE);
    l->readers++;
    ReleaseSRWLockExclusive(&l->lk);
}
static int rw_try_read(Obj *l) {
    HosThread *self = hos_self();
    AcquireSRWLockExclusive(&l->lk);
    int ok = !l->owner || l->owner == self;
    if (ok) l->readers++;
    ReleaseSRWLockExclusive(&l->lk);
    return ok;
}
static void rw_unread(Obj *l) {
    AcquireSRWLockExclusive(&l->lk);
    if (l->readers > 0) l->readers--;
    ReleaseSRWLockExclusive(&l->lk);
    WakeAllConditionVariable(&l->cv);
}
static int rw_write(Obj *l, int try_only) {
    HosThread *self = hos_self();
    AcquireSRWLockExclusive(&l->lk);
    if (l->owner == self) { l->count++; ReleaseSRWLockExclusive(&l->lk); return 1; }
    while (l->owner || l->readers) {
        if (try_only) { ReleaseSRWLockExclusive(&l->lk); return 0; }
        hos_cv_sleep(&l->cv, &l->lk, INFINITE);
    }
    l->owner = self;
    l->count = 1;
    ReleaseSRWLockExclusive(&l->lk);
    return 1;
}
static void rw_unwrite(Obj *l) {
    AcquireSRWLockExclusive(&l->lk);
    if (l->owner == hos_self() && --l->count == 0) l->owner = NULL;
    ReleaseSRWLockExclusive(&l->lk);
    WakeAllConditionVariable(&l->cv);
}
HLE(_ZN2nn2os26InitializeReaderWriterLockEPNS0_20ReaderWriterLockTypeE) { fresh(ARG(0), K_RWLOCK); }
HLE(_ZN2nn2os24FinalizeReaderWriterLockEPNS0_20ReaderWriterLockTypeE) { drop(ARG(0)); }
HLE(_ZN2nn2os15AcquireReadLockEPNS0_20ReaderWriterLockTypeE) { rw_read(obj(ARG(0), K_RWLOCK)); }
HLE(_ZN2nn2os18TryAcquireReadLockEPNS0_20ReaderWriterLockTypeE) { RET(rw_try_read(obj(ARG(0), K_RWLOCK))); }
HLE(_ZN2nn2os15ReleaseReadLockEPNS0_20ReaderWriterLockTypeE) { rw_unread(obj(ARG(0), K_RWLOCK)); }
HLE(_ZN2nn2os16AcquireWriteLockEPNS0_20ReaderWriterLockTypeE) { rw_write(obj(ARG(0), K_RWLOCK), 0); }
HLE(_ZN2nn2os19TryAcquireWriteLockEPNS0_20ReaderWriterLockTypeE) { RET(rw_write(obj(ARG(0), K_RWLOCK), 1)); }
HLE(_ZN2nn2os16ReleaseWriteLockEPNS0_20ReaderWriterLockTypeE) { rw_unwrite(obj(ARG(0), K_RWLOCK)); }
HLE(_ZN2nn2os30IsWriteLockHeldByCurrentThreadEPKNS0_20ReaderWriterLockTypeE) { RET(obj(ARG(0), K_RWLOCK)->owner == hos_self()); }

/* ---- multi-wait: holders over events, timers, threads, semaphores, queues; WaitAny returns the first ready one -- */
enum { H_EVENT, H_TIMER, H_THREAD, H_SEMA, H_QUEUE_NOT_EMPTY, H_QUEUE_NOT_FULL, H_NATIVE };
static void holder(Ctx *c, int what) {
    Obj *h = fresh(ARG(0), K_HOLDER);
    h->event = ARG(1);
    h->sem_max = what;   /* the kind of object watched (Obj fields reused: a holder has no semaphore) */
}
/* Ready now? Consumes an auto-clear event's signal, as the kernel does when a wait is satisfied by it. */
/* (under mw_lock: lookups without the vcore checkpoint, obj_nockpt) */
static int holder_ready(Obj *h) {
    switch (h->sem_max) {
    case H_EVENT: return event_peek(obj_nockpt(h->event, K_EVENT));
    case H_TIMER: { Obj *t = obj_nockpt(h->event, K_TIMER); return t->due_ns && hos_now_ns() >= t->due_ns; }
    case H_THREAD: return 0;   /* thread exit is not tracked as an object yet */
    case H_SEMA: { Obj *s = obj_nockpt(h->event, K_SEMA); return s->sem > 0; }
    case H_QUEUE_NOT_EMPTY: { Obj *q = obj_nockpt(h->event, K_QUEUE); return q->qlen > 0; }
    case H_QUEUE_NOT_FULL: { Obj *q = obj_nockpt(h->event, K_QUEUE); return q->qlen < q->qcap; }
    default: return 0;         /* native handles: never, without a kernel behind them */
    }
}
/* The guest address a holder watches, whose changes poke WaitAny (poke_multiwait); 0: threads and native handles, never
   ready. The address alone: no object lookup (Unlink runs at teardown, often after the object was finalized) */
static uint64_t holder_watch(const Obj *h) {
    return h->sem_max == H_THREAD || h->sem_max == H_NATIVE ? 0 : h->event;
}
HLE(_ZN2nn2os19InitializeMultiWaitEPNS0_13MultiWaitTypeE) { fresh(ARG(0), K_MULTIWAIT); }
HLE(_ZN2nn2os17FinalizeMultiWaitEPNS0_13MultiWaitTypeE) { drop(ARG(0)); }
HLE(_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE) { holder(c, H_EVENT); }
HLE(_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE) { holder(c, H_EVENT); }
HLE(_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_14TimerEventTypeE) { holder(c, H_TIMER); }
HLE(_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_10ThreadTypeE) { holder(c, H_THREAD); }
HLE(_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_13SemaphoreTypeE) { holder(c, H_SEMA); }
HLE(_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_16MessageQueueTypeENS0_20MessageQueueWaitTypeE) {
    holder(c, (int)ARG(2) == 0 ? H_QUEUE_NOT_FULL : H_QUEUE_NOT_EMPTY);   /* MessageQueueWaitType: ForNotFull = 0 (assumed) */
}
HLE(_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEj) { holder(c, H_NATIVE); }
HLE(_ZN2nn2os26SetMultiWaitHolderUserDataEPNS0_19MultiWaitHolderTypeEm) { obj(ARG(0), K_HOLDER)->period_ns = (int64_t)ARG(1); }
HLE(_ZN2nn2os26GetMultiWaitHolderUserDataEPKNS0_19MultiWaitHolderTypeE) { RET(obj(ARG(0), K_HOLDER)->period_ns); }
HLE(_ZN2nn2os23FinalizeMultiWaitHolderEPNS0_19MultiWaitHolderTypeE) { drop(ARG(0)); }
HLE(_ZN2nn2os19LinkMultiWaitHolderEPNS0_13MultiWaitTypeEPNS0_19MultiWaitHolderTypeE) {
    Obj *mw = obj(ARG(0), K_MULTIWAIT), *h = obj(ARG(1), K_HOLDER);
    AcquireSRWLockExclusive(&mw_lock);
    if (mw->nlinks == mw->caplinks) {
        mw->caplinks = mw->caplinks ? mw->caplinks * 2 : 8;
        mw->links = (Obj **)realloc(mw->links, mw->caplinks * sizeof *mw->links);
    }
    mw->links[mw->nlinks++] = h;
    h->mw = mw;
    if (holder_watch(h)) mw_watch_add_locked(holder_watch(h));
    ReleaseSRWLockExclusive(&mw_lock);
}
HLE(_ZN2nn2os21UnlinkMultiWaitHolderEPNS0_19MultiWaitHolderTypeE) {
    Obj *h = obj(ARG(0), K_HOLDER), *mw = h->mw;
    AcquireSRWLockExclusive(&mw_lock);
    if (mw) {
        for (int i = 0; i < mw->nlinks; i++)
            if (mw->links[i] == h) { mw->links[i] = mw->links[--mw->nlinks]; break; }
        if (holder_watch(h)) mw_watch_remove_locked(holder_watch(h));
    }
    h->mw = NULL;
    ReleaseSRWLockExclusive(&mw_lock);
}
/* WaitAny / TimedWaitAny / TryWaitAny -> the first ready holder, or 0. timeout_ns: < 0 forever, 0 try.
   Events signaled through event_signal poke mw_cv; everything else is re-checked every 5 ms. */
static uint64_t wait_any(uint64_t multiwait, int64_t timeout_ns) {
    Obj *mw = obj(multiwait, K_MULTIWAIT);
    int64_t deadline = timeout_ns > 0 ? hos_now_ns() + timeout_ns : 0;
    const int chain = timeout_ns != 0 && hos_chain_on;   /* (outside mw_lock: the scan's region calls nothing else) */
    if (chain) hos_chain_note(HOS_CHAIN_WAIT, multiwait, 0);
    AcquireSRWLockExclusive(&mw_lock);
    InterlockedIncrement(&mw_waiting);
    uint64_t found = 0, found_watch = 0;
    for (;;) {
        int64_t wake = deadline ? deadline : INT64_MAX;
        for (int i = 0; i < mw->nlinks && !found; i++) {
            if (holder_ready(mw->links[i])) found = mw->links[i]->key, found_watch = holder_watch(mw->links[i]);
            else if (mw->links[i]->sem_max == H_TIMER) {
                const int64_t due = obj_nockpt(mw->links[i]->event, K_TIMER)->due_ns;
                if (due && due < wake) wake = due;
            }
        }
        if (found) break;
        const int64_t now = hos_now_ns();
        if (timeout_ns == 0 || (timeout_ns > 0 && now >= deadline)) break;
        const int64_t left = wake - now;
        hos_cv_sleep(&mw_cv, &mw_lock, left < 1000000 ? 1 : left < 5000000 ? (DWORD)((left + 999999) / 1000000) : 5);
    }
    InterlockedDecrement(&mw_waiting);
    ReleaseSRWLockExclusive(&mw_lock);
    if (chain) hos_chain_note(HOS_CHAIN_WOKE, found_watch ? found_watch : multiwait, 0);   /* (the object that ended it) */
    return found;
}
HLE(_ZN2nn2os7WaitAnyEPNS0_13MultiWaitTypeE) { RET(wait_any(ARG(0), -1)); }
HLE(_ZN2nn2os12TimedWaitAnyEPNS0_13MultiWaitTypeENS_8TimeSpanE) { RET(wait_any(ARG(0), (int64_t)ARG(1) > 0 ? (int64_t)ARG(1) : 0)); }
HLE(_ZN2nn2os10TryWaitAnyEPNS0_13MultiWaitTypeE) { RET(wait_any(ARG(0), 0)); }
HLE(_ZN2nn2os24UnlinkAllMultiWaitHolderEPNS0_13MultiWaitTypeE) {
    Obj *mw = obj(ARG(0), K_MULTIWAIT);
    AcquireSRWLockExclusive(&mw_lock);
    for (int i = 0; i < mw->nlinks; i++) {
        if (holder_watch(mw->links[i])) mw_watch_remove_locked(holder_watch(mw->links[i]));
        mw->links[i]->mw = NULL;
    }
    mw->nlinks = 0;
    ReleaseSRWLockExclusive(&mw_lock);
}
HLE(_ZN2nn2os22MoveAllMultiWaitHolderEPNS0_13MultiWaitTypeES2_) {
    Obj *to = obj(ARG(0), K_MULTIWAIT), *from = obj(ARG(1), K_MULTIWAIT);
    AcquireSRWLockExclusive(&mw_lock);
    for (int i = 0; i < from->nlinks; i++) {
        if (to->nlinks == to->caplinks) {
            to->caplinks = to->caplinks ? to->caplinks * 2 : 8;
            to->links = (Obj **)realloc(to->links, to->caplinks * sizeof *to->links);
        }
        to->links[to->nlinks++] = from->links[i];
        from->links[i]->mw = to;
    }
    from->nlinks = 0;
    ReleaseSRWLockExclusive(&mw_lock);
}

/* ---- system events made by the SDK itself (the game's come from other HLE APIs) ------------------------------- */
/* CreateSystemEvent(SystemEventType*, EventClearMode, bool interProcess) -> Result */
HLE(_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb) { event_init(ARG(0), 0, (int)ARG(1)); RET(0); }
HLE(_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE) { event_signal(obj(ARG(0), K_EVENT)); }
HLE(_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE) {
    RET(logged_wait(c, obj(ARG(0), K_EVENT), (int64_t)ARG(1) > 0 ? (int64_t)ARG(1) : 1, (int64_t)ARG(1) <= 0, "system timed"));
}
/* Attaching kernel handles: without a kernel nothing ever signals them, which is what an idle device would do. */
HLE(_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE) { event_init(ARG(0), 0, (int)ARG(3)); }
HLE(_ZN2nn2os33AttachWritableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE) { event_init(ARG(0), 0, (int)ARG(3)); }
HLE(_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE) { event_init(ARG(0), 0, (int)ARG(5)); }
HLE(_ZN2nn2os33DetachReadableHandleOfSystemEventEPNS0_15SystemEventTypeE) { RET(0xFFFF0001u); }
HLE(_ZN2nn2os33DetachWritableHandleOfSystemEventEPNS0_15SystemEventTypeE) { RET(0xFFFF0002u); }
HLE(_ZN2nn2os30GetReadableHandleOfSystemEventEPKNS0_15SystemEventTypeE) { RET(0xFFFF0001u); }
HLE(_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE) { RET(0xFFFF0002u); }
/* nn::os::SystemEvent: the C++ wrapper holds a SystemEventType at offset 0 */
HLE(_ZN2nn2os11SystemEventC2ENS0_14EventClearModeEb) { event_init(ARG(0), 0, (int)ARG(1)); }
HLE(_ZN2nn2os11SystemEventD2Ev) { drop(ARG(0)); }

/* ---- the remaining waits on the HLE's objects ----------------------------------------------------------------- */
HLE(_ZN2nn2os9WaitEventEPNS0_9EventTypeE) { event_wait(obj(ARG(0), K_EVENT), 0, 0); }
HLE(_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE) { RET(event_wait(obj(ARG(0), K_EVENT), 0, 1)); }
HLE(_ZN2nn2os14TimedWaitEventEPNS0_9EventTypeENS_8TimeSpanE) {
    RET(event_wait(obj(ARG(0), K_EVENT), (int64_t)ARG(1) > 0 ? (int64_t)ARG(1) : 1, (int64_t)ARG(1) <= 0));
}
HLE(_ZN2nn2os17TryWaitLightEventEPNS0_14LightEventTypeE) { RET(event_wait(obj(ARG(0), K_EVENT), 0, 1)); }
HLE(_ZN2nn2os17TryWaitTimerEventEPNS0_14TimerEventTypeE) {
    Obj *t = obj(ARG(0), K_TIMER);
    int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&t->lk);
    int ready = t->due_ns && now >= t->due_ns;
    if (ready && t->period_ns > 0) while (t->due_ns <= now) t->due_ns += t->period_ns;
    ReleaseSRWLockExclusive(&t->lk);
    RET(ready);
}
/* TimedWaitConditionVariable(cv, mutex, TimeSpan) -> ConditionVariableStatus (0 timeout, 1 no timeout) */
HLE(_ZN2nn2os26TimedWaitConditionVariableEPNS0_21ConditionVariableTypeEPNS0_9MutexTypeENS_8TimeSpanE) {
    Obj *cv = obj(ARG(0), K_COND), *m = obj(ARG(1), K_MUTEX);
    int64_t deadline = hos_now_ns() + ((int64_t)ARG(2) > 0 ? (int64_t)ARG(2) : 0);
    cv_count_in(cv);
    AcquireSRWLockExclusive(&cv->lk);
    uint64_t gen = cv->gen;
    cv_enter(cv, m);
    const int count = mutex_release_all(m);
    int signaled = 1;
    HOS_CHAIN(HOS_CHAIN_WAIT, cv->key);
    while (cv->gen == gen) {
        if (!wait_until(cv, deadline) || hos_now_ns() >= deadline) { signaled = cv->gen != gen; break; }
    }
    const int owed = cv_leave(cv, m);
    ReleaseSRWLockExclusive(&cv->lk);
    HOS_CHAIN(HOS_CHAIN_WOKE, cv->key);
    mutex_lock_owed(m, ARG(1), owed);
    m->count = count;
    RET(signaled);
}

/* ---- (mkw-recomp) MKW's SDK: the variants TOTK never called. Every call on an object the HLE made must reach the
   HLE: one left to the lifted SDK writes the SDK's own structure and never wakes an HLE waiter (the loader threads
   slept forever on ReceiveMessageQueue while the game posted their work with TrySendMessageQueue). ---------------- */
HLE(_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm) { RET(queue_put(obj(ARG(0), K_QUEUE), ARG(1), 0, 1)); }
HLE(_ZN2nn2os15JamMessageQueueEPNS0_16MessageQueueTypeEm) { queue_put(obj(ARG(0), K_QUEUE), ARG(1), 1, 0); }
HLE(_ZN2nn2os18TryJamMessageQueueEPNS0_16MessageQueueTypeEm) { RET(queue_put(obj(ARG(0), K_QUEUE), ARG(1), 1, 1)); }
HLE(_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE) {
    uint64_t v;
    const int ok = queue_get(obj(ARG(1), K_QUEUE), &v, 1, 0);
    if (ok) G64(ARG(0)) = v;
    RET(ok);
}
/* ReleaseSemaphore(SemaphoreType*): the Switch 2 SDK's form without a count */
HLE(_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE) { sema_release(obj(ARG(0), K_SEMA), 1); }
HLE(_ZN2nn2os19TryAcquireSemaphoreEPNS0_13SemaphoreTypeE) {
    Obj *s = obj(ARG(0), K_SEMA);
    AcquireSRWLockExclusive(&s->lk);
    const int ok = s->sem > 0;
    if (ok) s->sem--;
    ReleaseSRWLockExclusive(&s->lk);
    RET(ok);
}
/* StartOneShotTimerEvent(TimerEventType*, TimeSpan): a periodic timer without a period */
HLE(_ZN2nn2os22StartOneShotTimerEventEPNS0_14TimerEventTypeENS_8TimeSpanE) {
    Obj *t = obj(ARG(0), K_TIMER);
    AcquireSRWLockExclusive(&t->lk);
    t->due_ns = hos_now_ns() + (int64_t)ARG(1);
    t->period_ns = 0;
    t->signaled = 0;
    ReleaseSRWLockExclusive(&t->lk);
    WakeAllConditionVariable(&t->cv);
    poke_multiwait(t);
}
/* GenerateRandomBytes(void*, size_t) */
HLE(_ZN2nn2os19GenerateRandomBytesEPvm) {
    uint8_t *p = (uint8_t *)GP(ARG(0));
    static volatile LONG64 state = 0x9E3779B97F4A7C15LL;
    for (uint64_t i = 0; i < ARG(1); i++) {
        const uint64_t x = (uint64_t)InterlockedAdd64(&state, 0x9E3779B97F4A7C15LL);
        p[i] = (uint8_t)((x ^ (x >> 29) ^ (x >> 47)) * 0xBF58476D1CE4E5B9ULL >> 56);
    }
}
