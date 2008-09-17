#
# Makefile for LITMUS^RT
#

obj-y     = sched_plugin.o litmus.o \
	    edf_common.o jobs.o \
	    rt_domain.o fdso.o sync.o \
	    fmlp.o srp.o norqlock.o \
            sched_gsn_edf.o \
	    sched_psn_edf.o \
            sched_cedf.o \
            sched_pfair.o

obj-$(CONFIG_FEATHER_TRACE) += trace.o ft_event.o ftdev.o
obj-$(CONFIG_SCHED_DEBUG_TRACE) +=  sched_trace.o