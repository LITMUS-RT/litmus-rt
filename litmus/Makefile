#
# Makefile for LITMUS^RT
#

obj-y     = sched_plugin.o litmus.o \
	    edf_common.o jobs.o \
	    rt_domain.o fdso.o sync.o \
	    fmlp.o srp.o norqlock.o \
	    cheap.o \
            sched_gsn_edf.o \
	    sched_psn_edf.o \
            sched_cedf.o \
            sched_pfair.o \
	    sched_gq_edf.o \
	    sched_gedf.o

obj-$(CONFIG_FEATHER_TRACE) += ft_event.o ftdev.o
obj-$(CONFIG_SCHED_TASK_TRACE) += sched_task_trace.o
obj-$(CONFIG_SCHED_DEBUG_TRACE) += sched_trace.o
obj-$(CONFIG_SCHED_OVERHEAD_TRACE) += trace.o
