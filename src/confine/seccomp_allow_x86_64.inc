// SPDX-License-Identifier: Apache-2.0
// Provenance: recorded with tools/record-seccomp.py from a4l's own runs, then
// reviewed (ADR-0009, Follow-on). No GPL-3.0 sources.
//
// The system calls a confined --split app process may make, x86-64.
//
// Recorded 2026-11-14 on host ART (android-13.0.0_r84), Mesa radeonsi/RADV,
// glibc 2.43, from nine APKs, 45 to 60 s each: the five of tests/app-corpus
// (three present through Skia, two through libGDX and native GL) and four
// Flutter example apps (Impeller on GLES). No input was injected and no
// network was reachable, so the paths an app takes on a touch, a key or a
// completed connection are covered only as far as the second section goes.
//
// Deliberately absent, whatever a trace shows: execve and execveat, fork and
// vfork, ptrace, process_vm_*, mount and the new mount API, unshare, setns,
// bpf, perf_event_open, userfaultfd, io_uring_*, keyctl, the module and kexec
// calls, personality, seccomp, pidfd_*, name_to_handle_at, open_by_handle_at.
// clone3 answers ENOSYS without being listed (confine/seccomp_filter.cpp).
//
// To extend: record, review, add the line. Sorted within a section.

// --- recorded -------------------------------------------------------------
#ifdef SYS_access
ACL_SYSCALL(access)
#endif
#ifdef SYS_arch_prctl
ACL_SYSCALL(arch_prctl)
#endif
#ifdef SYS_bind
ACL_SYSCALL(bind)
#endif
#ifdef SYS_brk
ACL_SYSCALL(brk)
#endif
#ifdef SYS_chmod
ACL_SYSCALL(chmod)
#endif
#ifdef SYS_clock_gettime
ACL_SYSCALL(clock_gettime)
#endif
#ifdef SYS_clock_nanosleep
ACL_SYSCALL(clock_nanosleep)
#endif
#ifdef SYS_clone
ACL_SYSCALL(clone)
#endif
#ifdef SYS_close
ACL_SYSCALL(close)
#endif
#ifdef SYS_connect
ACL_SYSCALL(connect)
#endif
#ifdef SYS_dup
ACL_SYSCALL(dup)
#endif
#ifdef SYS_epoll_create1
ACL_SYSCALL(epoll_create1)
#endif
#ifdef SYS_epoll_ctl
ACL_SYSCALL(epoll_ctl)
#endif
#ifdef SYS_epoll_pwait
ACL_SYSCALL(epoll_pwait)
#endif
#ifdef SYS_epoll_wait
ACL_SYSCALL(epoll_wait)
#endif
#ifdef SYS_eventfd2
ACL_SYSCALL(eventfd2)
#endif
#ifdef SYS_exit
ACL_SYSCALL(exit)
#endif
#ifdef SYS_exit_group
ACL_SYSCALL(exit_group)
#endif
#ifdef SYS_faccessat
ACL_SYSCALL(faccessat)
#endif
#ifdef SYS_fallocate
ACL_SYSCALL(fallocate)
#endif
#ifdef SYS_fchown
ACL_SYSCALL(fchown)
#endif
#ifdef SYS_fcntl
ACL_SYSCALL(fcntl)
#endif
#ifdef SYS_flock
ACL_SYSCALL(flock)
#endif
#ifdef SYS_fstat
ACL_SYSCALL(fstat)
#endif
#ifdef SYS_fstatfs
ACL_SYSCALL(fstatfs)
#endif
#ifdef SYS_fsync
ACL_SYSCALL(fsync)
#endif
#ifdef SYS_ftruncate
ACL_SYSCALL(ftruncate)
#endif
#ifdef SYS_futex
ACL_SYSCALL(futex)
#endif
#ifdef SYS_getcwd
ACL_SYSCALL(getcwd)
#endif
#ifdef SYS_getdents64
ACL_SYSCALL(getdents64)
#endif
#ifdef SYS_getegid
ACL_SYSCALL(getegid)
#endif
#ifdef SYS_geteuid
ACL_SYSCALL(geteuid)
#endif
#ifdef SYS_getgid
ACL_SYSCALL(getgid)
#endif
#ifdef SYS_getpid
ACL_SYSCALL(getpid)
#endif
#ifdef SYS_getrandom
ACL_SYSCALL(getrandom)
#endif
#ifdef SYS_getresgid
ACL_SYSCALL(getresgid)
#endif
#ifdef SYS_getresuid
ACL_SYSCALL(getresuid)
#endif
#ifdef SYS_getrlimit
ACL_SYSCALL(getrlimit)
#endif
#ifdef SYS_getsockname
ACL_SYSCALL(getsockname)
#endif
#ifdef SYS_getsockopt
ACL_SYSCALL(getsockopt)
#endif
#ifdef SYS_gettid
ACL_SYSCALL(gettid)
#endif
#ifdef SYS_getuid
ACL_SYSCALL(getuid)
#endif
#ifdef SYS_ioctl
ACL_SYSCALL(ioctl)
#endif
#ifdef SYS_listen
ACL_SYSCALL(listen)
#endif
#ifdef SYS_lseek
ACL_SYSCALL(lseek)
#endif
#ifdef SYS_lstat
ACL_SYSCALL(lstat)
#endif
#ifdef SYS_madvise
ACL_SYSCALL(madvise)
#endif
#ifdef SYS_memfd_create
ACL_SYSCALL(memfd_create)
#endif
#ifdef SYS_mkdir
ACL_SYSCALL(mkdir)
#endif
#ifdef SYS_mkdirat
ACL_SYSCALL(mkdirat)
#endif
#ifdef SYS_mmap
ACL_SYSCALL(mmap)
#endif
#ifdef SYS_mprotect
ACL_SYSCALL(mprotect)
#endif
#ifdef SYS_mremap
ACL_SYSCALL(mremap)
#endif
#ifdef SYS_munmap
ACL_SYSCALL(munmap)
#endif
#ifdef SYS_newfstatat
ACL_SYSCALL(newfstatat)
#endif
#ifdef SYS_openat
ACL_SYSCALL(openat)
#endif
#ifdef SYS_pause
ACL_SYSCALL(pause)
#endif
#ifdef SYS_pipe2
ACL_SYSCALL(pipe2)
#endif
#ifdef SYS_ppoll
ACL_SYSCALL(ppoll)
#endif
#ifdef SYS_prctl
ACL_SYSCALL(prctl)
#endif
#ifdef SYS_pread64
ACL_SYSCALL(pread64)
#endif
#ifdef SYS_prlimit64
ACL_SYSCALL(prlimit64)
#endif
#ifdef SYS_read
ACL_SYSCALL(read)
#endif
#ifdef SYS_readlink
ACL_SYSCALL(readlink)
#endif
#ifdef SYS_readlinkat
ACL_SYSCALL(readlinkat)
#endif
#ifdef SYS_recvmsg
ACL_SYSCALL(recvmsg)
#endif
#ifdef SYS_rename
ACL_SYSCALL(rename)
#endif
#ifdef SYS_rseq
ACL_SYSCALL(rseq)
#endif
#ifdef SYS_rt_sigaction
ACL_SYSCALL(rt_sigaction)
#endif
#ifdef SYS_rt_sigprocmask
ACL_SYSCALL(rt_sigprocmask)
#endif
#ifdef SYS_rt_sigtimedwait
ACL_SYSCALL(rt_sigtimedwait)
#endif
#ifdef SYS_sched_get_priority_max
ACL_SYSCALL(sched_get_priority_max)
#endif
#ifdef SYS_sched_get_priority_min
ACL_SYSCALL(sched_get_priority_min)
#endif
#ifdef SYS_sched_getaffinity
ACL_SYSCALL(sched_getaffinity)
#endif
#ifdef SYS_sched_getparam
ACL_SYSCALL(sched_getparam)
#endif
#ifdef SYS_sched_getscheduler
ACL_SYSCALL(sched_getscheduler)
#endif
#ifdef SYS_sched_setaffinity
ACL_SYSCALL(sched_setaffinity)
#endif
#ifdef SYS_sched_setscheduler
ACL_SYSCALL(sched_setscheduler)
#endif
#ifdef SYS_sched_yield
ACL_SYSCALL(sched_yield)
#endif
#ifdef SYS_sendmsg
ACL_SYSCALL(sendmsg)
#endif
#ifdef SYS_sendto
ACL_SYSCALL(sendto)
#endif
#ifdef SYS_set_robust_list
ACL_SYSCALL(set_robust_list)
#endif
#ifdef SYS_set_tid_address
ACL_SYSCALL(set_tid_address)
#endif
#ifdef SYS_setpriority
ACL_SYSCALL(setpriority)
#endif
#ifdef SYS_setsockopt
ACL_SYSCALL(setsockopt)
#endif
#ifdef SYS_sigaltstack
ACL_SYSCALL(sigaltstack)
#endif
#ifdef SYS_socket
ACL_SYSCALL(socket)
#endif
#ifdef SYS_socketpair
ACL_SYSCALL(socketpair)
#endif
#ifdef SYS_stat
ACL_SYSCALL(stat)
#endif
#ifdef SYS_statfs
ACL_SYSCALL(statfs)
#endif
#ifdef SYS_statx
ACL_SYSCALL(statx)
#endif
#ifdef SYS_sysinfo
ACL_SYSCALL(sysinfo)
#endif
#ifdef SYS_timerfd_create
ACL_SYSCALL(timerfd_create)
#endif
#ifdef SYS_timerfd_settime
ACL_SYSCALL(timerfd_settime)
#endif
#ifdef SYS_uname
ACL_SYSCALL(uname)
#endif
#ifdef SYS_unlink
ACL_SYSCALL(unlink)
#endif
#ifdef SYS_write
ACL_SYSCALL(write)
#endif
#ifdef SYS_writev
ACL_SYSCALL(writev)
#endif

// --- added in review ------------------------------------------------------
// Not seen in the corpus. Each is the fallback, the sibling or the
// counterpart of a recorded call, and reaches nothing the recorded ones do
// not: clone is what the C library falls back to when clone3 answers ENOSYS,
// and its flags are checked.

// threads and exit
#ifdef SYS_kill
ACL_SYSCALL(kill)
#endif
#ifdef SYS_membarrier
ACL_SYSCALL(membarrier)
#endif
#ifdef SYS_restart_syscall
ACL_SYSCALL(restart_syscall)
#endif
#ifdef SYS_rt_sigreturn
ACL_SYSCALL(rt_sigreturn)
#endif
#ifdef SYS_tgkill
ACL_SYSCALL(tgkill)
#endif

// memory
#ifdef SYS_mincore
ACL_SYSCALL(mincore)
#endif
#ifdef SYS_mlock
ACL_SYSCALL(mlock)
#endif
#ifdef SYS_msync
ACL_SYSCALL(msync)
#endif
#ifdef SYS_munlock
ACL_SYSCALL(munlock)
#endif

// files, the variants of recorded calls
#ifdef SYS_chdir
ACL_SYSCALL(chdir)
#endif
#ifdef SYS_close_range
ACL_SYSCALL(close_range)
#endif
#ifdef SYS_copy_file_range
ACL_SYSCALL(copy_file_range)
#endif
#ifdef SYS_dup2
ACL_SYSCALL(dup2)
#endif
#ifdef SYS_dup3
ACL_SYSCALL(dup3)
#endif
#ifdef SYS_faccessat2
ACL_SYSCALL(faccessat2)
#endif
#ifdef SYS_fchdir
ACL_SYSCALL(fchdir)
#endif
#ifdef SYS_fchmodat
ACL_SYSCALL(fchmodat)
#endif
#ifdef SYS_fdatasync
ACL_SYSCALL(fdatasync)
#endif
#ifdef SYS_link
ACL_SYSCALL(link)
#endif
#ifdef SYS_linkat
ACL_SYSCALL(linkat)
#endif
#ifdef SYS_open
ACL_SYSCALL(open)
#endif
#ifdef SYS_pipe
ACL_SYSCALL(pipe)
#endif
#ifdef SYS_preadv
ACL_SYSCALL(preadv)
#endif
#ifdef SYS_pwrite64
ACL_SYSCALL(pwrite64)
#endif
#ifdef SYS_pwritev
ACL_SYSCALL(pwritev)
#endif
#ifdef SYS_readv
ACL_SYSCALL(readv)
#endif
#ifdef SYS_renameat
ACL_SYSCALL(renameat)
#endif
#ifdef SYS_renameat2
ACL_SYSCALL(renameat2)
#endif
#ifdef SYS_rmdir
ACL_SYSCALL(rmdir)
#endif
#ifdef SYS_sendfile
ACL_SYSCALL(sendfile)
#endif
#ifdef SYS_symlink
ACL_SYSCALL(symlink)
#endif
#ifdef SYS_symlinkat
ACL_SYSCALL(symlinkat)
#endif
#ifdef SYS_truncate
ACL_SYSCALL(truncate)
#endif
#ifdef SYS_umask
ACL_SYSCALL(umask)
#endif
#ifdef SYS_unlinkat
ACL_SYSCALL(unlinkat)
#endif
#ifdef SYS_utimensat
ACL_SYSCALL(utimensat)
#endif

// waiting
#ifdef SYS_epoll_create
ACL_SYSCALL(epoll_create)
#endif
#ifdef SYS_epoll_pwait2
ACL_SYSCALL(epoll_pwait2)
#endif
#ifdef SYS_eventfd
ACL_SYSCALL(eventfd)
#endif
#ifdef SYS_inotify_add_watch
ACL_SYSCALL(inotify_add_watch)
#endif
#ifdef SYS_inotify_init1
ACL_SYSCALL(inotify_init1)
#endif
#ifdef SYS_inotify_rm_watch
ACL_SYSCALL(inotify_rm_watch)
#endif
#ifdef SYS_poll
ACL_SYSCALL(poll)
#endif
#ifdef SYS_pselect6
ACL_SYSCALL(pselect6)
#endif
#ifdef SYS_select
ACL_SYSCALL(select)
#endif
#ifdef SYS_timerfd_gettime
ACL_SYSCALL(timerfd_gettime)
#endif

// time
#ifdef SYS_clock_getres
ACL_SYSCALL(clock_getres)
#endif
#ifdef SYS_getitimer
ACL_SYSCALL(getitimer)
#endif
#ifdef SYS_getrusage
ACL_SYSCALL(getrusage)
#endif
#ifdef SYS_gettimeofday
ACL_SYSCALL(gettimeofday)
#endif
#ifdef SYS_nanosleep
ACL_SYSCALL(nanosleep)
#endif
#ifdef SYS_setitimer
ACL_SYSCALL(setitimer)
#endif
#ifdef SYS_time
ACL_SYSCALL(time)
#endif
#ifdef SYS_timer_create
ACL_SYSCALL(timer_create)
#endif
#ifdef SYS_timer_delete
ACL_SYSCALL(timer_delete)
#endif
#ifdef SYS_timer_getoverrun
ACL_SYSCALL(timer_getoverrun)
#endif
#ifdef SYS_timer_gettime
ACL_SYSCALL(timer_gettime)
#endif
#ifdef SYS_timer_settime
ACL_SYSCALL(timer_settime)
#endif
#ifdef SYS_times
ACL_SYSCALL(times)
#endif

// signals
#ifdef SYS_rt_sigpending
ACL_SYSCALL(rt_sigpending)
#endif
#ifdef SYS_rt_sigqueueinfo
ACL_SYSCALL(rt_sigqueueinfo)
#endif
#ifdef SYS_rt_sigsuspend
ACL_SYSCALL(rt_sigsuspend)
#endif

// scheduling and identity
#ifdef SYS_getcpu
ACL_SYSCALL(getcpu)
#endif
#ifdef SYS_getgroups
ACL_SYSCALL(getgroups)
#endif
#ifdef SYS_getpgid
ACL_SYSCALL(getpgid)
#endif
#ifdef SYS_getpgrp
ACL_SYSCALL(getpgrp)
#endif
#ifdef SYS_getppid
ACL_SYSCALL(getppid)
#endif
#ifdef SYS_getpriority
ACL_SYSCALL(getpriority)
#endif
#ifdef SYS_getsid
ACL_SYSCALL(getsid)
#endif
#ifdef SYS_sched_setparam
ACL_SYSCALL(sched_setparam)
#endif

// sockets, the rest of a client's and a server's calls
#ifdef SYS_accept
ACL_SYSCALL(accept)
#endif
#ifdef SYS_accept4
ACL_SYSCALL(accept4)
#endif
#ifdef SYS_getpeername
ACL_SYSCALL(getpeername)
#endif
#ifdef SYS_recvfrom
ACL_SYSCALL(recvfrom)
#endif
#ifdef SYS_recvmmsg
ACL_SYSCALL(recvmmsg)
#endif
#ifdef SYS_sendmmsg
ACL_SYSCALL(sendmmsg)
#endif
#ifdef SYS_shutdown
ACL_SYSCALL(shutdown)
#endif
