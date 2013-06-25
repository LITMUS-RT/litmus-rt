/*
 * included from arch/x86/include/asm/unistd_32.h
 *
 * LITMUS^RT syscalls with "relative" numbers
 */
#define __LSC(x) (__NR_LITMUS + x)

#define __NR_set_rt_task_param	__LSC(0)
#define __NR_get_rt_task_param	__LSC(1)
#define __NR_complete_job	__LSC(2)
#define __NR_od_open		__LSC(3)
#define __NR_od_close		__LSC(4)
#define __NR_litmus_lock       	__LSC(5)
#define __NR_litmus_unlock	__LSC(6)
#define __NR_query_job_no	__LSC(7)
#define __NR_wait_for_job_release __LSC(8)
#define __NR_wait_for_ts_release __LSC(9)
#define __NR_release_ts		__LSC(10)
#define __NR_null_call		__LSC(11)

#define NR_litmus_syscalls 12
