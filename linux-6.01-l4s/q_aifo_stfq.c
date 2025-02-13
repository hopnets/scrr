/*
 * q_aifo_stfq.c	Parse/print AIFO-STFQ discipline module options.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Copyright 2022-2025 Hewlett Packard Enterprise Development LP.
 *	Author: Jean Tourrilhes <tourrilhes.hpl@gmail.com>
 *	Author: Zhuolong Yu <yuzhuolong1993@gmail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <math.h>

#include "utils.h"
#include "tc_util.h"

#define AIFO_SAMPLE_SIZE_MAX		(1024)

enum {
	TCA_AIFO_UNSPEC,
	TCA_AIFO_PLIMIT,	/* limit of total number of packets in queue */
	TCA_AIFO_BURST,		/* AIFO headroom before dropping packets */
	TCA_AIFO_BUCKETS_LOG,	/* log2(number of buckets) */
	TCA_AIFO_HASH_MASK,	/* mask applied to skb hashes */
	TCA_AIFO_FLOW_PLIMIT,	/* limit of packets per flow */
	TCA_AIFO_SAMPLE_SIZE,
	TCA_AIFO_SAMPLE_PERIOD,
	TCA_AIFO_FLAGS,		/* Options */
	__TCA_AIFO_MAX
};
#define TCA_AIFO_MAX (__TCA_AIFO_MAX - 1)

/* TCA_AIFO_FLAGS */
#define SCF_PEAK_NORESET	0x0020	/* Don't reset peak statistics */
#define AIFF_QUANT_FIXED	0x0000	/* Quantile: fixed computations */
#define AIFF_QUANT_ADD1		0x0100	/* Quantile: add current packet */
#define AIFF_QUANT_ORIG		0x0200	/* Quantile: original computations */

/* statistics exported to userspace */
struct tc_aifo_xstats {
	__u32	flows;		/* number of flows */
	__u64	flows_gc;	/* number of flows garbage collected */
	__u32	alloc_errors;	/* failed flow allocations */
	__u32	no_mark;	/* packet not dropped */
	__u32	drop_mark;	/* packet dropped */
	__u32	qlen_peak;	/* Maximum queue length */
	__u32	backlog_peak;	/* Maximum backlog */
	__u32	quant_avg_1k;	/* Average quantile * 1024 */
};


static void explain(void)
{
	fprintf(stderr, "Usage: ... aifo-stfq [ limit PACKETS ] [ burst PACKETS ] [ buckets NUMBER ] [ hash_mask MASK ] [ samples NUMBER ] [ speriod PACKETS ]\n");
}

static unsigned int ilog2(unsigned int val)
{
	unsigned int res = 0;

	val--;
	while (val) {
		res++;
		val >>= 1;
	}
	return res;
}

static int aifo_parse_opt(struct qdisc_util *qu, int argc, char **argv,
				struct nlmsghdr *n, const char *dev)
{
	uint32_t	plimit = 0xFFFFFFFF;
	uint32_t	burst = 0xFFFFFFFF;
	unsigned int	buckets = 0;
	uint32_t	hash_mask = 0x0;
	uint32_t	flow_plimit = 0xFFFFFFFF;
	uint16_t	sample_size = 0xFFFF;
	uint16_t	sample_period = 0xFFFF;
	unsigned int	flags = 0x0;
	bool		flags_upd = false;
	struct rtattr *tail;

	while (argc > 0) {
		if (strcmp(*argv, "limit") == 0) {
			NEXT_ARG();
			if (get_u32(&plimit, *argv, 0)) {
				fprintf(stderr, "Illegal \"limit\"\n");
				return -1;
			}
		} else if (strcmp(*argv, "buckets") == 0) {
			NEXT_ARG();
			if (get_unsigned(&buckets, *argv, 0)) {
				fprintf(stderr, "Illegal \"buckets\"\n");
				return -1;
			}
		} else if (strcmp(*argv, "burst") == 0) {
			NEXT_ARG();
			if (get_u32(&burst, *argv, 0)) {
				fprintf(stderr, "Illegal \"burst\"\n");
				return -1;
			}
		} else if (strcmp(*argv, "hash_mask") == 0) {
			NEXT_ARG();
			if (get_u32(&hash_mask, *argv, 0)) {
				fprintf(stderr, "Illegal \"hash_mask\"\n");
				return -1;
			}
		} else if (strcmp(*argv, "flow_limit") == 0) {
			NEXT_ARG();
			if (get_u32(&flow_plimit, *argv, 0)) {
				fprintf(stderr, "Illegal \"flow_limit\"\n");
				return -1;
			}
		} else if (strcmp(*argv, "samples") == 0) {
			NEXT_ARG();
			if (get_u16(&sample_size, *argv, 0)) {
				fprintf(stderr, "Illegal \"samples\"\n");
				return -1;
			}
			if (sample_size > AIFO_SAMPLE_SIZE_MAX) {
				fprintf(stderr, "Value for \"samples\" too big\n");
				return -1;
			}
		} else if (strcmp(*argv, "speriod") == 0) {
			NEXT_ARG();
			if (get_u16(&sample_period, *argv, 0)) {
				fprintf(stderr, "Illegal \"speriod\"\n");
				return -1;
			}
		} else if (strcasecmp(*argv, "flags") == 0) {
			NEXT_ARG();
			if (get_unsigned(&flags, *argv, 0)) {
				fprintf(stderr, "Illegal \"flags\"\n");
				return -1;
			}
			flags_upd = true;
		} else if (strcmp(*argv, "help") == 0) {
			explain();
			return -1;
		} else {
			fprintf(stderr, "%s: unknown parameter \"%s\"\n", qu->id, *argv);
			explain();
			return -1;
		}
		argc --;
		argv ++;
	}

	tail = addattr_nest(n, 1024, TCA_OPTIONS);
	// tail = NLMSG_TAIL(n);
	// addattr_l(n, 1024, TCA_OPTIONS, NULL, 0);

	if (plimit != 0xFFFFFFFF)
		addattr32(n, 1024, TCA_AIFO_PLIMIT, plimit);
	if (burst != 0xFFFFFFFF)
		addattr32(n, MAX_MSG, TCA_AIFO_BURST, burst);
	if (buckets != 0) {
		unsigned int hash_log = ilog2(buckets);
		addattr32(n, 1024, TCA_AIFO_BUCKETS_LOG, hash_log);
	}
	if (hash_mask != 0x0)
		addattr32(n, 1024, TCA_AIFO_HASH_MASK, hash_mask);
	if (flow_plimit != 0xFFFFFFFF)
		addattr32(n, 1024, TCA_AIFO_FLOW_PLIMIT, flow_plimit);
	if (sample_size != 0xFFFF)
		addattr16(n, MAX_MSG, TCA_AIFO_SAMPLE_SIZE, sample_size);
	if (sample_period != 0xFFFF)
		addattr16(n, MAX_MSG, TCA_AIFO_SAMPLE_PERIOD, sample_period);
	if (flags_upd)
		addattr32(n, MAX_MSG, TCA_AIFO_FLAGS, flags);
	
	addattr_nest_end(n, tail);
	// tail->rta_len = (void *)NLMSG_TAIL(n) - (void *)tail;

	return 0;
}

static int aifo_print_opt(struct qdisc_util *qu, FILE *f,
				struct rtattr *opt)
{
	struct rtattr *tb[TCA_AIFO_MAX + 1];

	if (opt == NULL)
		return 0;

	parse_rtattr_nested(tb, TCA_AIFO_MAX, opt);

	if (tb[TCA_AIFO_PLIMIT] &&
	    RTA_PAYLOAD(tb[TCA_AIFO_PLIMIT]) >= sizeof(__u32)) {
		unsigned int plimit;
		plimit = rta_getattr_u32(tb[TCA_AIFO_PLIMIT]);
		print_uint(PRINT_ANY, "limit", "limit %up ", plimit);
	}

	if (tb[TCA_AIFO_BURST] &&
	    RTA_PAYLOAD(tb[TCA_AIFO_BURST]) >= sizeof(__u16)) {
		unsigned int burst;
		burst = rta_getattr_u32(tb[TCA_AIFO_BURST]);
		print_uint(PRINT_ANY, "burst", "burst %u ", burst);
	}
	if (tb[TCA_AIFO_BUCKETS_LOG] &&
	    RTA_PAYLOAD(tb[TCA_AIFO_BUCKETS_LOG]) >= sizeof(__u32)) {
		unsigned int buckets_log;
		buckets_log = rta_getattr_u32(tb[TCA_AIFO_BUCKETS_LOG]);
		print_uint(PRINT_ANY, "buckets", "buckets %u ",
			   1U << buckets_log);
	}

	if (tb[TCA_AIFO_HASH_MASK] &&
	    RTA_PAYLOAD(tb[TCA_AIFO_HASH_MASK]) >= sizeof(__u32)) {
		unsigned int hash_mask;
		hash_mask = rta_getattr_u32(tb[TCA_AIFO_HASH_MASK]);
		print_uint(PRINT_ANY, "hash_mask", "hash_mask %u ",
			   hash_mask);
	}
	if (tb[TCA_AIFO_FLOW_PLIMIT] &&
	    RTA_PAYLOAD(tb[TCA_AIFO_FLOW_PLIMIT]) >= sizeof(__u32)) {
		unsigned int flow_plimit;
		flow_plimit = rta_getattr_u32(tb[TCA_AIFO_FLOW_PLIMIT]);
		print_uint(PRINT_ANY, "flow_limit", "flow_limit %up ",
			   flow_plimit);
	}
	if (tb[TCA_AIFO_SAMPLE_SIZE] &&
	    RTA_PAYLOAD(tb[TCA_AIFO_SAMPLE_SIZE]) >= sizeof(__u16)) {
		unsigned int sample_size;
		sample_size = rta_getattr_u16(tb[TCA_AIFO_SAMPLE_SIZE]);
		print_uint(PRINT_ANY, "samples", "samples %u ", sample_size);
	}
	if (tb[TCA_AIFO_SAMPLE_PERIOD] &&
	    RTA_PAYLOAD(tb[TCA_AIFO_SAMPLE_PERIOD]) >= sizeof(__u16)) {
		unsigned int sample_period;
		sample_period = rta_getattr_u16(tb[TCA_AIFO_SAMPLE_PERIOD]);
		print_uint(PRINT_ANY, "speriod", "speriod %u ", sample_period);
	}
	if (tb[TCA_AIFO_FLAGS] &&
	    RTA_PAYLOAD(tb[TCA_AIFO_FLAGS]) >= sizeof(__u32)) {
		unsigned int flags;
		flags = rta_getattr_u32(tb[TCA_AIFO_FLAGS]);
		print_uint(PRINT_ANY, "flags", "flags 0x%X ", flags);
	}
	return 0;
}

static int aifo_print_xstats(struct qdisc_util *qu, FILE *f,
				   struct rtattr *xstats)
{
	struct tc_aifo_xstats *st;

	if (xstats == NULL)
		return 0;
	
	if (RTA_PAYLOAD(xstats) < sizeof(*st))
		return -1;

	st = RTA_DATA(xstats);
	print_uint(PRINT_ANY, "flows", "  flows %u", st->flows);
	print_uint(PRINT_ANY, "flows_gc", " gc %u", st->flows_gc);
	print_uint(PRINT_ANY, "alloc_errors", " alloc_errors %u",
		   st->alloc_errors);
	print_uint(PRINT_ANY, "no_mark", " \n  no_mark %u", st->no_mark);
	print_uint(PRINT_ANY, "drop_mark", " drop_mark %u", st->drop_mark);
	print_float(PRINT_ANY, "quant_avg", " quant_avg %.3f",
		    (float) st->quant_avg_1k / 1024.0);
	if (st->backlog_peak != 0 || st->qlen_peak != 0) {
		print_uint(PRINT_ANY, "backlog_peak", "  backlog_peak %ub",
			   st->backlog_peak);
		print_uint(PRINT_ANY, "qlen_peak", " %up", st->qlen_peak);
	}

	return 0;
}

struct qdisc_util aifo_stfq_qdisc_util = {
	.id = "aifo_stfq",
	.parse_qopt = aifo_parse_opt,
	.print_qopt = aifo_print_opt,
	.print_xstats = aifo_print_xstats,
};
