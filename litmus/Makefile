#
# Makefile for LITMUS^RT
#

obj-y     = sched_plugin.o litmus.o \
	    preempt.o \
	    litmus_proc.o \
	    budget.o \
	    jobs.o \
	    sync.o \
	    rt_domain.o \
	    edf_common.o \
	    fdso.o \
	    srp.o \
	    fmlp.o \
	    bheap.o \
	    ctrldev.o \
	    sched_gsn_edf.o \
	    sched_psn_edf.o

obj-$(CONFIG_PLUGIN_CEDF) += sched_cedf.o
obj-$(CONFIG_PLUGIN_PFAIR) += sched_pfair.o

obj-$(CONFIG_FEATHER_TRACE) += ft_event.o ftdev.o
obj-$(CONFIG_SCHED_TASK_TRACE) += sched_task_trace.o
obj-$(CONFIG_SCHED_DEBUG_TRACE) += sched_trace.o
obj-$(CONFIG_SCHED_OVERHEAD_TRACE) += trace.o
