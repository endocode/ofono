/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016  EndoCode AG. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>
#include <ofono/netmon.h>

#include "gatchat.h"
#include "gatresult.h"

#include "common.h"
#include "ubloxmodem.h"
#include "drivers/atmodem/vendor.h"

static const char *cops_prefix[] = { "+COPS:", NULL };
static const char *cesq_prefix[] = { "+CESQ:", NULL };
static const char *ucged_prefix[] = { "+UCGED:", NULL };

/* Field from +UCGED response */
typedef struct ublox_ucged_value {
	int offset;	/* Offset at response */
	uint8_t base;	/* Base format how to parse */
} ublox_ucged_value;

struct netmon_driver_data {
	GAtChat *chat;
};

struct req_cb_data {
	gint ref_count; /* Ref count */

	struct ofono_netmon *netmon;

	ofono_netmon_cb_t cb;
	void *data;

	struct ofono_network_operator op;

	int rxlev;	/* CESQ: Received Signal Strength Indication */
	int ber;	/* CESQ: Bit Error Rate */
	int rscp;	/* CESQ: Received Signal Code Powe */
	int rsrp;	/* CESQ: Reference Signal Received Power */
	int ecn0;	/* CESQ: Received Energy Ratio */
	int rsrq;	/* CESQ: Reference Signal Received Quality */

	int earfcn;	/* UCGED: E-UTRA ARFCN */
	int eband;	/* UCGED: E-UTRA Band */
	int cqi;	/* UCGED: Channel Quality Indicator */
};

/*
 * Returns the appropriate radio access technology.
 *
 * If we can not resolve to a specific radio access technolgy
 * we return OFONO_NETMON_CELL_TYPE_GSM by default.
 */
static int ublox_map_radio_access_technology(int tech)
{
	switch (tech) {
	case ACCESS_TECHNOLOGY_GSM:
	case ACCESS_TECHNOLOGY_GSM_COMPACT:
		return OFONO_NETMON_CELL_TYPE_GSM;
	case ACCESS_TECHNOLOGY_UTRAN:
	case ACCESS_TECHNOLOGY_UTRAN_HSDPA:
	case ACCESS_TECHNOLOGY_UTRAN_HSUPA:
	case ACCESS_TECHNOLOGY_UTRAN_HSDPA_HSUPA:
		return OFONO_NETMON_CELL_TYPE_UMTS;
	case ACCESS_TECHNOLOGY_EUTRAN:
		return OFONO_NETMON_CELL_TYPE_LTE;
	}

	return OFONO_NETMON_CELL_TYPE_GSM;
}

static inline struct req_cb_data *req_cb_data_new0(void *cb, void *data,
							void *user)
{
	struct req_cb_data *ret = g_new0(struct req_cb_data, 1);

	ret->ref_count = 1;
	ret->cb = cb;
	ret->data = data;
	ret->netmon = user;

	ret->rxlev = -1;
	ret->ber = -1;
	ret->rscp = -1;
	ret->rsrp = -1;
	ret->ecn0 = -1;
	ret->rsrq = -1;

	ret->earfcn = -1;
	ret->eband = -1;
	ret->cqi = -1;

	return ret;
}

static inline struct req_cb_data *req_cb_data_ref(struct req_cb_data *cbd)
{
	if (cbd == NULL)
		return NULL;

	g_atomic_int_inc(&cbd->ref_count);

	return cbd;
}

static void req_cb_data_unref(gpointer user_data)
{
	gboolean is_zero;
	struct req_cb_data *cbd = user_data;

	if (cbd == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&cbd->ref_count);

	if (is_zero == TRUE)
		g_free(cbd);
}

static gboolean ublox_delayed_register(gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;

	ofono_netmon_register(netmon);

	return FALSE;
}

static void ublox_netmon_finish_success(struct req_cb_data *cbd)
{
	struct ofono_netmon *nm = cbd->netmon;

	ofono_netmon_serving_cell_notify(nm,
					cbd->op.tech,
					OFONO_NETMON_INFO_RXLEV, cbd->rxlev,
					OFONO_NETMON_INFO_BER, cbd->ber,
					OFONO_NETMON_INFO_RSCP, cbd->rscp,
					OFONO_NETMON_INFO_ECN0, cbd->ecn0,
					OFONO_NETMON_INFO_RSRQ, cbd->rsrq,
					OFONO_NETMON_INFO_RSRP, cbd->rsrp,
					OFONO_NETMON_INFO_EARFCN, cbd->earfcn,
					OFONO_NETMON_INFO_EBAND, cbd->eband,
					OFONO_NETMON_INFO_CQI, cbd->cqi,
					OFONO_NETMON_INFO_INVALID);

	CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
}

static void ucged_parse_4g(struct req_cb_data *cbd, GAtResultIter *iter)
{
	/* Offset of values in returned response */
	enum ucged_mode2_rat4_offsets {
		UCGED_EARFCN		= 0,
		UCGED_EBAND		= 1,
		UCGED_CQI		= 15,
		UCGED_VOLTE_MODE	= 21,
		_MAX		/* End */
	};

	ublox_ucged_value ucged_values[] = {
		{ UCGED_EARFCN,	10 },
		{ UCGED_EBAND,	10 },
		{ UCGED_CQI,	10 },
		{ -1,		0  }, /* End of Array */
	};

	gboolean ok;
	unsigned i;
	ublox_ucged_value *ptr;

	/* Skip first fields */
	g_at_result_iter_next(iter, NULL);

	for (i = 0, ptr = &ucged_values[0]; i < _MAX; i++) {
		const char *str = NULL;
		int number;

		/* First read the data */
		ok = g_at_result_iter_next_unquoted_string(iter, &str);
		if (!ok) {
			DBG(" UCGED: error parsing at idx: %d ", i);
			return;
		}

		DBG(" UCGED: RAT = 4G idx: %d  -  reading field: %s ", i, str);

		/* End, nothing to do we got all our values */
		if (ptr->offset < 0)
			break;

		/* Skip to next if we are not interested in this value */
		if (i != (unsigned) ptr->offset)
			continue;

		errno = 0;
		number = strtol(str, NULL, ptr->base);
		if (errno != 0 && number == 0) {
			DBG(" UCGED: RAT = 4G  idx: %u  failed parsing '%s' ",
				i, str);
			continue;
		}

		switch (i) {
		case UCGED_EARFCN:
			cbd->earfcn = number != 65535 ? number : cbd->earfcn;
			break;
		case UCGED_EBAND:
			cbd->eband = number != 255 ? number : cbd->eband;
			break;
		case UCGED_CQI:
			cbd->cqi = number != 255 ? number : cbd->cqi;
			break;
		}

		ptr++;
	}

	DBG(" UCGED: MODE 2 RAT = 4G	EARFCN = %d ", cbd->earfcn);
	DBG(" UCGED: MODE 2 RAT = 4G	EBAND = %d ", cbd->eband);
	DBG(" UCGED: MODE 2 RAT = 4G	CQI = %d ", cbd->cqi);
}

static void ucged_collect_mode2_data(struct req_cb_data *cbd, GAtResult *result)
{
	int rat;
	GAtResultIter iter;
	gboolean ok;

	g_at_result_iter_init(&iter, result);

	g_at_result_iter_next(&iter, NULL);
	g_at_result_iter_next(&iter, NULL);

	ok = g_at_result_iter_next_number(&iter, &rat);
	if (!ok) {
		DBG(" UCGED: error parsing 'RAT' ");
		return;
	}

	/* For now we support only RAT 4 */
	if (rat != 4) {
		DBG(" UCGED: 'RAT' is %d, nothing to do. ", rat);
		return;
	}

	ucged_parse_4g(cbd, &iter);
}

static void ucged_query_mode2_cb(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	struct ofono_error error;
	GAtResultIter iter;
	int mode;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		DBG(" UCGED: failed ");
		goto out;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+UCGED:")) {
		DBG(" UCGED: no result ");
		goto out;
	}

	ok = g_at_result_iter_next_number(&iter, &mode);
	if (!ok) {
		DBG(" UCGED: error parsing 'mode' ");
		goto out;
	}

	DBG(" UCGED:  report mode is %d ", mode);

	/* mode 2 collect +UCGED data */
	if (mode == 2)
		ucged_collect_mode2_data(cbd, result);

	/*
	 * We never fail at this point we always send what we collected so
	 * far
	 */
out:
	ublox_netmon_finish_success(cbd);
}

static void cesq_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	enum cesq_ofono_netmon_info {
		CESQ_RXLEV,
		CESQ_BER,
		CESQ_RSCP,
		CESQ_ECN0,
		CESQ_RSRQ,
		CESQ_RSRP,
		_MAX,
	};

	struct req_cb_data *cbd = user_data;
	struct ofono_netmon *nm = cbd->netmon;
	struct netmon_driver_data *nmd = ofono_netmon_get_data(nm);
	struct ofono_error error;
	GAtResultIter iter;
	int idx, number;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CESQ:")) {
		DBG(" CESQ: no result ");
		goto out;
	}

	for (idx = 0; idx < _MAX; idx++) {
		ok = g_at_result_iter_next_number(&iter, &number);

		if (!ok) {
			/* Ignore and do not fail */
			DBG(" CESQ: error parsing idx: %d ", idx);
			goto out;
		}

		switch (idx) {
		case CESQ_RXLEV:
			cbd->rxlev = number != 99 ? number:cbd->rxlev;
			break;
		case CESQ_BER:
			cbd->ber = number != 99 ? number:cbd->ber;
			break;
		case CESQ_RSCP:
			cbd->rscp = number != 255 ? number:cbd->rscp;
			break;
		case CESQ_ECN0:
			cbd->ecn0 = number != 255 ? number:cbd->ecn0;
			break;
		case CESQ_RSRQ:
			cbd->rsrq = number != 255 ? number:cbd->rsrq;
			break;
		case CESQ_RSRP:
			cbd->rsrp = number != 255 ? number:cbd->rsrp;
			break;
		}
	}

	DBG(" RXLEV	%d ", cbd->rxlev);
	DBG(" BER	%d ", cbd->ber);
	DBG(" RSCP	%d ", cbd->rscp);
	DBG(" ECN0	%d ", cbd->ecn0);
	DBG(" RSRQ	%d ", cbd->rsrq);
	DBG(" RSRP	%d ", cbd->rsrp);

	cbd = req_cb_data_ref(cbd);
	if (g_at_chat_send(nmd->chat, "AT+UCGED?", NULL,
			ucged_query_mode2_cb, cbd, req_cb_data_unref) == 0) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		req_cb_data_unref(cbd);
	}

	return;

	/* We never fail, we always send what we collected so far */
out:
	ublox_netmon_finish_success(cbd);
}

static void cops_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	struct ofono_netmon *nm = cbd->netmon;
	struct netmon_driver_data *nmd = ofono_netmon_get_data(nm);
	struct ofono_error error;
	GAtResultIter iter;
	int tech;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	/* Do not fail */
	if (!g_at_result_iter_next(&iter, "+COPS:")) {
		CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
		return;
	}

	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);

	/* Default to GSM */
	if (g_at_result_iter_next_number(&iter, &tech) == FALSE)
		cbd->op.tech = OFONO_NETMON_CELL_TYPE_GSM;
	else
		cbd->op.tech = ublox_map_radio_access_technology(tech);

	cbd = req_cb_data_ref(cbd);
	if (g_at_chat_send(nmd->chat, "AT+CESQ", cesq_prefix,
				cesq_cb, cbd, req_cb_data_unref) == 0) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		req_cb_data_unref(cbd);
	}
}

static void ublox_netmon_request_update(struct ofono_netmon *netmon,
					ofono_netmon_cb_t cb, void *data)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);
	struct req_cb_data *cbd;

	DBG("ublox netmon request update");

	cbd = req_cb_data_new0(cb, data, netmon);

	if (g_at_chat_send(nmd->chat, "AT+COPS?", cops_prefix,
				cops_cb, cbd, req_cb_data_unref) == 0) {
		CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
		req_cb_data_unref(cbd);
	}
}

static int ublox_netmon_probe(struct ofono_netmon *netmon,
					unsigned int vendor, void *user)
{
	GAtChat *chat = user;
	struct netmon_driver_data *nmd;

	DBG("ublox netmon probe");

	nmd = g_try_new0(struct netmon_driver_data, 1);
	if (nmd == NULL)
		return -ENOMEM;

	nmd->chat = g_at_chat_clone(chat);

	ofono_netmon_set_data(netmon, nmd);

	g_idle_add(ublox_delayed_register, netmon);

	/*
	 * We set +UCGED=2 mode as early as possible so we are able to handle
	 * the following:
	 * 1) In case the modem supports only +UCGED mode 2 then we have
	 *    already set the appropriate mode and we can query the information
	 *    later.
	 * 2) In case the modem supports only +UCGED mode 3, then to set mode 3
	 *    the modem has to be first in +UCGED mode 2. Setting the mode here
	 *    allows later to query the information and on errors we fallback
	 *    to mode 3.
	 *
	 * This should handle current firmware versions of ublox modems.
	 */
	g_at_chat_send(nmd->chat, "AT+UCGED=2", ucged_prefix,
			NULL, NULL, NULL);

	return 0;
}

static void ublox_netmon_remove(struct ofono_netmon *netmon)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);

	DBG("ublox netmon remove");

	g_at_chat_unref(nmd->chat);

	ofono_netmon_set_data(netmon, NULL);

	g_free(nmd);
}

static struct ofono_netmon_driver driver = {
	.name			= UBLOXMODEM,
	.probe			= ublox_netmon_probe,
	.remove			= ublox_netmon_remove,
	.request_update		= ublox_netmon_request_update,
};

void ublox_netmon_init(void)
{
	ofono_netmon_driver_register(&driver);
}

void ublox_netmon_exit(void)
{
	ofono_netmon_driver_unregister(&driver);
}
