#
# Makefile for LITMUS^RT
#

obj-y     = sched_plugin.o litmus.o sched_trace.o \
	    edf_common.o jobs.o\
            sched_gsn_edf.o sched_psn_edf.o \
	    rt_domain.o fdso.o sync.o \
	    fmlp.o srp.o norqlock.o

obj-$(CONFIG_FEATHER_TRACE) += trace.o ft_event.o
