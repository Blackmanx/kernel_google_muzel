// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2024 Google LLC */

#include <linux/kernel.h>
#include <linux/units.h>

#include <dvfs-helper/google_dvfs_helper.h>

#include "cpm_tracepoint_decoder.h"
#include "perf_tracepoints.h"

#define TRACE_OP_LVL_BITFIELD GENMASK(15, 0)
#define EXTRACT_DOMAIN_ID(payload) (payload >> 16)
#define EXTRACT_DOMAIN_OP_LVL(payload) (payload & TRACE_OP_LVL_BITFIELD)

#define DOMAIN_TRACE_STR_LEN (MAX_DVFS_NAME_LEN + sizeof("_freq"))

enum tracepoint_handle domain_freq_handler(const char *tp_string, u32 payload,
					   u64 timestamp)
{
	u16 domain_id;
	const char *domain_name;
	char trace_name[DOMAIN_TRACE_STR_LEN];
	s64 freq;

	domain_id = EXTRACT_DOMAIN_ID(payload);
	domain_name = dvfs_helper_domain_id_to_name(domain_id);
	if (!domain_name)
		return CLIENT_TP_HANDLING_ERROR;

	freq = dvfs_helper_get_domain_opp_frequency_mhz(
		domain_id, EXTRACT_DOMAIN_OP_LVL(payload));
	if (freq < 0)
		return CLIENT_TP_HANDLING_ERROR;

	scnprintf(trace_name, sizeof(trace_name), "%s_freq", domain_name);
	add_cpm_param_trace(trace_name, KHZ_PER_MHZ * freq, timestamp);

	return CLIENT_TP_HANDLING_COMPLETE;
}

struct client_tracepoint domain_freq_tp = { .enabled = true,
					    .tp_string = "DvfsTar %d",
					    .init = NULL,
					    .handler = domain_freq_handler,
					    .exit = NULL };

/*
 * Payload format:
 * 27 - 31 bits: last vote
 * 19 - 26 bits: domain id (gmc, memss, etc.)
 * 16 - 18 bits: voter id (thermal, debug, devfreq, ...)
 * 15 bit: vote type (min or max) (0: min, 1: max)
 * 10 - 14 bits: aggregated high opp from votes (i.e. before clamp)
 * 5 - 9 bits: aggregated low opp from votes
 * 0 - 4 bits: new overall high opp (i.e. after clamp)
 */

#define EXTRACT_LAST_VOTE(payload) ((payload & GENMASK(31, 27)) >> 27)
#define EXTRACT_AGG_UPDATE_DOMAIN_ID(payload) \
	((payload & GENMASK(26, 19)) >> 19)
#define EXTRACT_AGG_UPDATE_VOTER_ID(payload) ((payload & GENMASK(18, 16)) >> 16)
#define EXTRACT_AGG_UPDATE_VOTE_TYPE(payload) ((payload & BIT(15)) >> 15)
#define EXTRACT_AGG_UPDATE_AGG_HI_OPP(payload) \
	((payload & GENMASK(14, 10)) >> 10)
#define EXTRACT_AGG_UPDATE_NEW_LO_OPP(payload) ((payload & GENMASK(9, 5)) >> 5)
#define EXTRACT_AGG_UPDATE_NEW_HI_OPP(payload) (payload & GENMASK(4, 0))

enum dvfs_voter_id {
	DVFS_VOTER_DEVFREQ,
	DVFS_VOTER_GOVERNOR,
	DVFS_VOTER_THERMAL,
	DVFS_VOTER_DEBUG,
	DVFS_VOTER_NUM,
} dvfs_voter_id_t;

const char *voter_id_str[] = {
	[DVFS_VOTER_DEVFREQ] = "devfreq",
	[DVFS_VOTER_GOVERNOR] = "governor",
	[DVFS_VOTER_THERMAL] = "thermal",
	[DVFS_VOTER_DEBUG] = "debug",
};

#define MAX_VOTER_ID_STR_LEN 9

#define AGG_UPDATE_TRACE_STR_LEN \
	(MAX_DVFS_NAME_LEN + sizeof("_agg_") + sizeof("max"))

enum tracepoint_handle freq_agg_update_handler(const char *tp_string,
					       u32 payload, u64 timestamp)
{
	char trace_name_min_max[AGG_UPDATE_TRACE_STR_LEN];
	char trace_name_agg_update[AGG_UPDATE_TRACE_STR_LEN];
	char trace_voter_id[MAX_DVFS_NAME_LEN + MAX_VOTER_ID_STR_LEN +
			    sizeof("_vote")];
	u16 domain_id;
	const char *domain_name;
	u32 voter_id;
	u32 last_vote;
	bool is_max;
	u32 min_of_high_votes;
	u32 max_of_low_votes;
	u32 final_high_vote;

	domain_id = EXTRACT_AGG_UPDATE_DOMAIN_ID(payload);
	domain_name = dvfs_helper_domain_id_to_name(domain_id);
	if (!domain_name)
		return CLIENT_TP_HANDLING_ERROR;

	voter_id = EXTRACT_AGG_UPDATE_VOTER_ID(payload);
	if (voter_id >= DVFS_VOTER_NUM)
		return CLIENT_TP_HANDLING_ERROR;

	last_vote = EXTRACT_LAST_VOTE(payload);
	is_max = EXTRACT_AGG_UPDATE_VOTE_TYPE(payload);
	min_of_high_votes = EXTRACT_AGG_UPDATE_AGG_HI_OPP(payload);
	max_of_low_votes = EXTRACT_AGG_UPDATE_NEW_LO_OPP(payload);
	final_high_vote = EXTRACT_AGG_UPDATE_NEW_HI_OPP(payload);

	scnprintf(trace_name_min_max, sizeof(trace_name_min_max), "%s_agg_%s",
		  domain_name, is_max ? "max" : "min");

	scnprintf(trace_name_agg_update, sizeof(trace_name_agg_update),
		  "%s_agg_hi", domain_name);

	scnprintf(trace_voter_id, sizeof(trace_voter_id), "%s_%s_vote",
		  domain_name, voter_id_str[voter_id]);

	add_cpm_param_trace(trace_name_min_max,
			    is_max ? final_high_vote : max_of_low_votes,
			    timestamp);

	add_cpm_param_trace(trace_name_agg_update, min_of_high_votes,
			    timestamp);

	add_cpm_param_trace(trace_voter_id, last_vote, timestamp);

	return CLIENT_TP_HANDLING_COMPLETE;
}

struct client_tracepoint freq_agg_update_tp = {
	.enabled = true,
	.tp_string = "freq_agg_update %d",
	.init = NULL,
	.handler = freq_agg_update_handler,
	.exit = NULL
};
