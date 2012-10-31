/* main dyninst header file
 * Copyright (C) 2012 Red Hat Inc.
 *
 * This file is part of systemtap, and is free software.  You can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License (GPL); either version 2, or (at your option) any
 * later version.
 */

#ifndef _STAPDYN_RUNTIME_H_
#define _STAPDYN_RUNTIME_H_

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/ptrace.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>


#include "loc2c-runtime.h"
#include "stapdyn.h"

#if __WORDSIZE == 64
#define CONFIG_64BIT 1
#endif

#define BITS_PER_LONG __BITS_PER_LONG

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#include "linux_types.h"


#ifndef NSEC_PER_SEC
#define NSEC_PER_SEC 1000000000L
#endif

#define _stp_timespec_sub(a, b, result)					      \
  do {									      \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;			      \
    (result)->tv_nsec = (a)->tv_nsec - (b)->tv_nsec;			      \
    if ((result)->tv_nsec < 0) {					      \
      --(result)->tv_sec;						      \
      (result)->tv_nsec += NSEC_PER_SEC;				      \
    }									      \
  } while (0)

#define simple_strtol strtol


// segments don't matter in dyninst...
#define USER_DS (1)
#define KERNEL_DS (-1)
typedef int mm_segment_t;
static inline mm_segment_t get_fs(void) { return 0; }
static inline void set_fs(mm_segment_t seg) { (void)seg; }


static inline int atomic_add_return(int i, atomic_t *v)
{
	return __sync_add_and_fetch(&(v->counter), i);
}

static inline int atomic_sub_return(int i, atomic_t *v)
{
	return __sync_sub_and_fetch(&(v->counter), i);
}

static inline int pseudo_atomic_cmpxchg(atomic_t *v, int oldval, int newval)
{
	return __sync_val_compare_and_swap(&(v->counter), oldval, newval);
}

#include "linux_defs.h"

#define MODULE_DESCRIPTION(str)
#define MODULE_LICENSE(str)
#define MODULE_INFO(tag,info)

/* Semi-forward declaration from runtime_context.h, needed by stat.c. */
static int _stp_runtime_num_contexts;

#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < _stp_runtime_num_contexts; (cpu)++)

#define yield() sched_yield()

#define access_ok(type, addr, size) 1

#define preempt_disable() 0
#define preempt_enable_no_resched() 0

/*
 * By definition, we can only debug our own processes with dyninst, so
 * assert_is_myproc() will never assert.
 *
 * FIXME: We might want to add the check back, to get a better error
 * message.
 */
#define assert_is_myproc() do {} while (0)

#include "debug.h"

#include "io.c"
#include "alloc.c"
#include "print.h"
#include "stp_string.c"
#include "arith.c"
#include "copy.c"
#include "regs.c"
#include "regs-ia64.c"
#include "task_finder.c"
#include "sym.c"
#include "perf.c"
#include "addr-map.c"
#include "stat.c"
#include "unwind.c"

static int systemtap_module_init(void);
static void systemtap_module_exit(void);

static unsigned long stap_hash_seed; /* Init during module startup */


/*
 * For stapdyn to work in a multiprocess environment, the module must be
 * prepared to be loaded multiple times in different processes.  Thus, we have
 * to distinguish between process-level resource initialization and
 * "session"-level (like probe begin/end/error).
 *
 * So stp_dyninst_ctor/dtor are the process-level functions, using gcc
 * attributes to get called at the right time.
 *
 * The session-level resources have to be started by the stapdyn mutator, since
 * only it knows which process is the "master" mutatee, so it can call
 * stp_dyninst_session_init only in the right one.  That process will run the
 * session exit in the dtor, since dyninst doesn't have a suitable exit hook.
 * It may still be invoked manually from stapdyn for detaching though.
 *
 * The stp_dyninst_master is set as a PID, so it can be checked and made not to
 * inherit across forks.
 *
 * XXX Once we have a shared-memory area (which will be necessary anyway for a
 * multiprocess session to share globals), then a process refcount may be
 * better for begin/end than this "master" designation.
 *
 * XXX Functions like _exit() which bypass destructors are a problem...
 */

static pid_t stp_dyninst_master = 0;

__attribute__((constructor))
static void stp_dyninst_ctor(void)
{
    stap_hash_seed = _stp_random_u ((unsigned long)-1);

    _stp_mem_fd = open("/proc/self/mem", O_RDWR /*| O_LARGEFILE*/);
    if (_stp_mem_fd != -1) {
        fcntl(_stp_mem_fd, F_SETFD, FD_CLOEXEC);
    }

    /* XXX We don't really want to be using the target's stdio. (PR14491)
     * But while we are, clone our own FILE handles so we're not affected by
     * the target's actions, liking closing stdout early.
     */
    _stp_out = _stp_clone_file(stdout);
    _stp_err = _stp_clone_file(stderr);
}

static int _stp_runtime_contexts_init(void);

int stp_dyninst_session_init(void)
{
    /* We don't have a chance to indicate errors in the ctor, so do it here. */
    if (_stp_mem_fd < 0) {
	return -errno;
    }
    
    int rc = _stp_runtime_contexts_init();
    if (rc != 0)
	return rc;

    rc = _stp_print_init();
    if (rc != 0)
	return rc;

    rc = systemtap_module_init();
    if (rc == 0) {
	stp_dyninst_master = getpid();
    }
    return rc;
}

void stp_dyninst_session_exit(void)
{
    if (stp_dyninst_master == getpid()) {
	systemtap_module_exit();
	_stp_print_cleanup();
	stp_dyninst_master = 0;
    }
}

__attribute__((destructor))
static void stp_dyninst_dtor(void)
{
    stp_dyninst_session_exit();

    if (_stp_mem_fd != -1) {
	close (_stp_mem_fd);
    }

    if (_stp_out && _stp_out != stdout) {
	fclose(_stp_out);
	_stp_out = stdout;
    }

    if (_stp_err && _stp_err != stderr) {
	fclose(_stp_err);
	_stp_err = stderr;
    }
}

#endif /* _STAPDYN_RUNTIME_H_ */