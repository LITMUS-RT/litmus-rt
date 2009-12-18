/*
 * included from arch/x86/include/asm/unistd_64.h
 *
 * LITMUS^RT syscalls with "relative" numbers
 */
#define __LSC(x) (__NR_LITMUS + x)

#define __NR_set_rt_task_param			__LSC(0)
__SYSCALL(__NR_set_rt_task_param, sys_set_rt_task_param)
#define __NR_get_rt_task_param			__LSC(1)
__SYSCALL(__NR_get_rt_task_param, sys_get_rt_task_param)
#define __NR_complete_job	  		__LSC(2)
__SYSCALL(__NR_complete_job, sys_complete_job)
#define __NR_od_open				__LSC(3)
__SYSCALL(__NR_od_open, sys_od_open)
#define __NR_od_close				__LSC(4)
__SYSCALL(__NR_od_close, sys_od_close)
#define __NR_fmlp_down				__LSC(5)
__SYSCALL(__NR_fmlp_down, sys_fmlp_down)
#define __NR_fmlp_up				__LSC(6)
__SYSCALL(__NR_fmlp_up, sys_fmlp_up)
#define __NR_srp_down				__LSC(7)
__SYSCALL(__NR_srp_down, sys_srp_down)
#define __NR_srp_up				__LSC(8)
__SYSCALL(__NR_srp_up, sys_srp_up)
#define __NR_query_job_no			__LSC(9)
__SYSCALL(__NR_query_job_no, sys_query_job_no)
#define __NR_wait_for_job_release		__LSC(10)
__SYSCALL(__NR_wait_for_job_release, sys_wait_for_job_release)
#define __NR_wait_for_ts_release		__LSC(11)
__SYSCALL(__NR_wait_for_ts_release, sys_wait_for_ts_release)
#define __NR_release_ts				__LSC(12)
__SYSCALL(__NR_release_ts, sys_release_ts)
#define __NR_null_call				__LSC(13)
__SYSCALL(__NR_null_call, sys_null_call)

#define NR_litmus_syscalls 14
