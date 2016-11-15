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
#include <ofono/netmon.h>

#include "common.h"
#include "netreg.h"
#include "ubloxmodem.h"
#include "drivers/atmodem/vendor.h"

static const char *cops_prefix[] = { "+COPS:", NULL };
static const char *cesq_prefix[] = { "+CESQ:", NULL };
static const char *ucged_prefix[] = { "+UCGED:", NULL };

struct netmon_driver_data {
	GAtChat *modem;
	GAtChat *aux;
};

struct req_cb_data {
	struct ofono_netmon *netmon;

	ofono_netmon_cb_t cb;
	void *data;

	struct ofono_network_operator op;

	/* Netmon fields */

	int rxlev;	/* CESQ: Received Signal Strength Indication */
	int ber;	/* CESQ: Bit Error Rate */
	int rscp;	/* CESQ: Received Signal Code Powe */
	int rsrp;	/* CESQ: Reference Signal Received Power */
	double ecn0;	/* CESQ: Received Energy Ratio */
	double rsrq;	/* CESQ: Reference Signal Received Quality */

	int earfcn;	/* UCGED: E-UTRAN Absolue Radio Frequency Channel */
	int lband;	/* UCGED: Lband */
	int ul_BW;	/* UCGED: Uplink Resource Blocks */
	int dl_BW;	/* UCGED: Downlink Resource Blocks */
	int tac;	/* UCGED: Tracking Area Code */
	int cellid;	/* UCGED: CI */
	double lsinr;	/* UCGED: E-UTRAN Signal to Interference and Noise Ratio */
	int cqi;	/* UCGED: Channel Quality Indicator */
	int avg_rsrp;	/* UCGED: Average last 10th RSRP */
};

/* Field from +UCGED response */
typedef struct ublox_ucged_value {
	/* Offset at response */
	int offset;

	/* fmt how to parse fields: 'd', 'x' or 'f' */
	const char fmt;
} ublox_ucged_value;

static int ublox_map_radio_access_technology(int tech)
{
	/* TODO: complete me */
	switch (tech) {
	case ACCESS_TECHNOLOGY_GSM:
	case ACCESS_TECHNOLOGY_GSM_COMPACT:
		return OFONO_NETMON_CELL_TYPE_GSM;
	case ACCESS_TECHNOLOGY_UTRAN:
		return OFONO_NETMON_CELL_TYPE_UMTS;
	case ACCESS_TECHNOLOGY_EUTRAN:
		return OFONO_NETMON_CELL_TYPE_LTE;
	}

	return 0;
}

static inline struct req_cb_data *req_cb_data_new0(void *cb, void *data, void *user)
{
	struct req_cb_data *ret = g_new0(struct req_cb_data, 1);
	if (ret == NULL)
		return ret;

	ret->cb = cb;
	ret->data = data;
	ret->netmon = user;

	/* Negatives are ignored in ofono by default */
	ret->rxlev = -1;
	ret->ber = -1;
	ret->rscp = -1;
	ret->rsrp = -1;
	ret->ecn0 = -1;
	ret->rsrq = -1;

	ret->earfcn = -1;
	ret->lband = -1;
	ret->ul_BW = -1;
	ret->dl_BW = -1;
	ret->tac = -1;
	ret->cellid = -1;
	ret->lsinr = -1;
	ret->cqi = -1;
	ret->avg_rsrp = -1;

	return ret;
}

static gboolean ublox_delayed_register(gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;

	ofono_netmon_register(netmon);

	return FALSE;
}

/* If conversion fails error is set to errno */
static double strtonum(const char *str, const char fmt, int *error)
{
	double number = -1;

	errno = 0;
	switch (fmt) {
	case 'd':
		/* If decimal parse it as a long int */
		number = strtol(str, NULL, 10);
		break;
	case 'x':
		/* If hex parse it as a long int in base 16 */
		number = strtol(str, NULL, 16);
		break;
	case 'f':
		/* if float or anything parse it as a double */
		number = strtod(str, NULL);
		break;
	}

	if (errno != 0 && number == 0)
		*error = -errno;

	return number;
}

/* Used to notify about all the data that we have collected so far */
static void ublox_netmon_finish_success(struct req_cb_data *cbd)
{
	struct ofono_netmon *nm = cbd->netmon;

	ofono_netmon_serving_cell_notify(nm,
					 cbd->op.tech,
					 OFONO_NETMON_INFO_OPERATOR, cbd->op.name,
					 OFONO_NETMON_INFO_RXLEV, cbd->rxlev,
					 OFONO_NETMON_INFO_BER, cbd->ber,
					 OFONO_NETMON_INFO_RSCP, cbd->rscp,
					 OFONO_NETMON_INFO_ECN0, cbd->ecn0,
					 OFONO_NETMON_INFO_RSRQ, cbd->rsrq,
					 OFONO_NETMON_INFO_RSRP, cbd->rsrp,
					 OFONO_NETMON_INFO_EARFCN, cbd->earfcn,
					 OFONO_NETMON_INFO_LBAND, cbd->lband,
					 OFONO_NETMON_INFO_UL_BW, cbd->ul_BW,
					 OFONO_NETMON_INFO_DL_BW, cbd->dl_BW,
					 OFONO_NETMON_INFO_TAC, cbd->tac,
					 OFONO_NETMON_INFO_CI, cbd->cellid,
					 OFONO_NETMON_INFO_LSINR, cbd->lsinr,
					 OFONO_NETMON_INFO_CQI, cbd->cqi,
					 OFONO_NETMON_INFO_AVG_RSRP, cbd->avg_rsrp,
					 OFONO_NETMON_INFO_INVALID);

	CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
	g_free(cbd);
}

static void ucged_parse_4g(struct req_cb_data *cbd, GAtResultIter *iter)
{
	enum ucged_mode2_rat4_offsets {
		UCGED_EARFCN		= 0,
		UCGED_LBAND		= 1,
		UCGED_UL_BW		= 2,
		UCGED_DL_BW		= 3,
		UCGED_TAC		= 4,
		UCGED_LCELLID		= 5,
		UCGED_LSINR		= 12,
		UCGED_CQI		= 15,
		UCGED_AVG_RSRP		= 16,
		UCGED_VOLTE_MODE	= 21,
		_MAX			= 22,	/* End */
	};

	ublox_ucged_value ucged_values[] = {
		{ UCGED_EARFCN,		'd'  },
		{ UCGED_LBAND,		'd'  },
		{ UCGED_UL_BW,		'd'  },
		{ UCGED_DL_BW,		'd'  },
		{ UCGED_TAC,		'x'  },
		{ UCGED_LCELLID,	'x'  },
		{ UCGED_LSINR,		'f'  },
		{ UCGED_CQI,		'd'  },
		{ UCGED_AVG_RSRP,	'd'  },
		{ -1,			'\0' },	/* End of Array */
	};

	gboolean ok;
	unsigned i;
	ublox_ucged_value *ptr;

	/* Skip first fields */
	g_at_result_iter_next(iter, NULL);

	for (i = 0, ptr = &ucged_values[0]; i < _MAX; i++) {
		const char *str = NULL;
		double number = -1;
		int err = 0;

		/* First read the data */
		ok = g_at_result_iter_next_unquoted_string(iter, &str);
		if (!ok) {
			DBG(" UCGED: error parsing at idx: %d ", i);
			return;
		}

		DBG(" UCGED: RAT = 4G idx: %d  -  reading field: %s ", i, str);

		/* Nothing to do we got all our values */
		if (ptr->offset < 0)
			break;

		/* Skip if we are not interested in this value */
		if (i != (unsigned) ptr->offset)
			continue;

		number = strtonum(str, ptr->fmt, &err);
		if (err < 0) {
			DBG(" UCGED: RAT = 4G  idx: %d  failed parsing '%s' ", i, str);
			continue;
		}

		switch (i) {
		case UCGED_EARFCN:
			cbd->earfcn = number != 65535 ? number : cbd->earfcn;
			break;
		case UCGED_LBAND:
			cbd->lband = number != 255 ? number : cbd->lband;
			break;
		case UCGED_UL_BW:
			cbd->ul_BW = number != 255 ? number : cbd->ul_BW;
			break;
		case UCGED_DL_BW:
			cbd->dl_BW = number != 255 ? number : cbd->dl_BW;
			break;
		case UCGED_TAC:
			cbd->tac = number != 0xFFFF ? number : cbd->tac;
			break;
		case UCGED_LCELLID:
			cbd->cellid = number ? number : cbd->cellid;
			break;
		case UCGED_LSINR:
			cbd->lsinr = number != 255 ? number : cbd->lsinr;
			break;
		case UCGED_CQI:
			cbd->cqi = number != 255 ? number : cbd->cqi;
			break;
		case UCGED_AVG_RSRP:
			cbd->avg_rsrp = number;
			break;
		}

		ptr++;
	}

	DBG(" UCGED: MODE 2 RAT = 4G	EARFCN = %d ", cbd->earfcn);
	DBG(" UCGED: MODE 2 RAT = 4G	LBAND = %d ", cbd->lband);
	DBG(" UCGED: MODE 2 RAT = 4G	UL_BW = %d ", cbd->ul_BW);
	DBG(" UCGED: MODE 2 RAT = 4G	DL_BW = %d ", cbd->dl_BW);
	DBG(" UCGED: MODE 2 RAT = 4G	TAC = %x ", cbd->tac);
	DBG(" UCGED: MODE 2 RAT = 4G	LCELLID = %x ", cbd->cellid);
	DBG(" UCGED: MODE 2 RAT = 4G	LSINR = %f ", cbd->lsinr);
	DBG(" UCGED: MODE 2 RAT = 4G	CQI = %d ", cbd->cqi);
	DBG(" UCGED: MODE 2 RAT = 4G	AVG_RSRP = %d ", cbd->avg_rsrp);
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

	/* We support only RAT 4 for now */
	if (rat == 4) {
		DBG(" UCGED: RAT	%d ", rat);
		ucged_parse_4g(cbd, &iter);
	} else {
		DBG(" UCGED: 'RAT' is %d, nothing to do. ", rat);
	}

	return;
}

static void ucged_collect_mode3_data(struct req_cb_data *cbd, GAtResult *result)
{
	GAtResultIter iter;
	gboolean ok;
	unsigned i;
	ublox_ucged_value *ptr;

	/*
	 * Offset at the line that we are interested in, the last line of
	 * +UCGED. Please update what is appropriate to read other fields
	 */
	enum ucged_mode3_offsets {
		UCGED_EARFCN		= 0,
		UCGED_LBAND		= 1,
		UCGED_UL_BW		= 2,
		UCGED_DL_BW		= 3,
		UCGED_TAC		= 6,
		UCGED_LCELLID		= 7,
		UCGED_LSINR		= 11,
		UCGED_LRRC		= 12,
		_MAX			= 13,	/* End */
	};

	ublox_ucged_value ucged_values[] = {
		{ UCGED_EARFCN,		'd'  },
		{ UCGED_LBAND,		'd'  },
		{ UCGED_UL_BW,		'd'  },
		{ UCGED_DL_BW,		'd'  },
		{ UCGED_TAC,		'x'  },
		{ UCGED_LCELLID,	'x'  },
		{ UCGED_LSINR,		'f'  },
		{ -1,			'\0' }, /* End of Array */
	};

	g_at_result_iter_init(&iter, result);

	g_at_result_iter_next(&iter, NULL);
	g_at_result_iter_next(&iter, NULL);
	g_at_result_iter_next(&iter, NULL);
	g_at_result_iter_next(&iter, NULL);

	for (i = 0, ptr = &ucged_values[0]; i < _MAX; i++) {
		const char *str = NULL;
		double number = -1;
		int err = 0;

		/* First read the data */
		ok = g_at_result_iter_next_unquoted_string(&iter, &str);
		if (!ok) {
			DBG(" UCGED: error parsing at idx: %d ", i);
			return;
		}

		DBG(" UCGED: mode 3 idx: %d  -  reading field: %s ", i, str);

		/* Nothing to do we got all our values */
		if (ptr->offset < 0)
			break;

		/* Skip if we are not interested in this value */
		if (i != (unsigned) ptr->offset)
			continue;

		number = strtonum(str, ptr->fmt, &err);
		if (err < 0) {
			DBG(" UCGED: mode 3  idx: %d  failed parsing '%s' ", i, str);
			continue;
		}

		switch (i) {
		case UCGED_EARFCN:
			cbd->earfcn = number != 65535 ? number : cbd->earfcn;
			break;
		case UCGED_LBAND:
			cbd->lband = number != 255 ? number : cbd->lband;
			break;
		case UCGED_UL_BW:
			cbd->ul_BW = number != 255 ? number : cbd->ul_BW;
			break;
		case UCGED_DL_BW:
			cbd->dl_BW = number != 255 ? number : cbd->dl_BW;
			break;
		case UCGED_TAC:
			cbd->tac = number != 0xFFFF ? number : cbd->tac;
			break;
		case UCGED_LCELLID:
			cbd->cellid = number ? number : cbd->cellid;
			break;
		case UCGED_LSINR:
			cbd->lsinr = number != 255 ? number : cbd->lsinr;
			break;
		}

		ptr++;
	}

	DBG(" UCGED: MODE 3	EARFCN = %d ", cbd->earfcn);
	DBG(" UCGED: MODE 3	LBAND = %d ", cbd->lband);
	DBG(" UCGED: MODE 3	UL_BW = %d ", cbd->ul_BW);
	DBG(" UCGED: MODE 3	DL_BW = %d ", cbd->dl_BW);
	DBG(" UCGED: MODE 3	TAC = %x ", cbd->tac);
	DBG(" UCGED: MODE 3	LCELLID = %x ", cbd->cellid);
	DBG(" UCGED: MODE 3	LSINR = %f ", cbd->lsinr);
}

static void ucged_query_mode3_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	struct ofono_error error;
	GAtResultIter iter;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		DBG(" UCGED: set mode 3 failed ");
		goto out;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+UCGED:")) {
		DBG(" UCGED: mode 3 no result ");
		goto out;
	}

	ucged_collect_mode3_data(cbd, result);

	/* We never fail at this point we always send what we collected so far */
out:
	ublox_netmon_finish_success(cbd);
	return;
}

static void ucged_query_mode2_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	struct ofono_netmon *nm = cbd->netmon;
	struct netmon_driver_data *nmd = ofono_netmon_get_data(nm);
	struct ofono_error error;
	GAtResultIter iter;
	int mode;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		DBG(" UCGED: query mode 2 failed ");
		/*
		 * Here +UCGED mode should be set to 2, however on some
		 * ublox modems mode 2 does not work, so try mode 3.
		 * Note that to be in mode 3 you have to be already in
		 * mode 2.
		 */
		if (g_at_chat_send(nmd->aux, "AT+UCGED=3", NULL,
				   ucged_query_mode3_cb, cbd, NULL)) {
			return;
		}
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

	/* We never fail at this point we always send what we collected so far */
out:
	ublox_netmon_finish_success(cbd);
	return;
}

static void cesq_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	enum ublox_cesq_ofono_netmon_info {
		RXLEV, BER, RSCP, ECN0, RSRQ, RSRP, COUNT
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
		goto error;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CESQ:"))
		return;

	for (idx = 0; idx < COUNT; idx++) {

		ok = g_at_result_iter_next_number(&iter, &number);
		if (!ok) {
			DBG(" error at idx: %d", idx);
			goto error;
		}

		switch (idx) {
		case RXLEV:
			cbd->rxlev = number != 99 ? number:cbd->rxlev;
			break;
		case BER:
			cbd->ber = number != 99 ? number:cbd->ber;
			break;
		case RSCP:
			cbd->rscp= number != 255 ? number:cbd->rscp;
			break;
		case ECN0:
			cbd->ecn0= number != 255 ? number:cbd->ecn0;
			break;
		case RSRQ:
			cbd->rsrq= number != 255 ? number:cbd->rsrq;
			break;
		case RSRP:
			cbd->rsrp= number != 255 ? number:cbd->rsrp;
			break;
		default:
			break;
		}
	}

	DBG(" RXLEV	%d ", cbd->rxlev);
	DBG(" BER	%d ", cbd->ber);
	DBG(" RSCP	%d ", cbd->rscp);
	DBG(" ECN0	%f ", cbd->ecn0);
	DBG(" RSRQ	%f ", cbd->rsrq);
	DBG(" RSRP	%d ", cbd->rsrp);

	if (g_at_chat_send(nmd->aux, "AT+UCGED?", NULL,
			   ucged_query_mode2_cb, cbd, NULL)) {
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
	g_free(cbd);
}

static void cops_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	struct ofono_netmon *nm = cbd->netmon;
	struct netmon_driver_data *nmd = ofono_netmon_get_data(nm);
	struct ofono_error error;
	GAtResultIter iter;
	int tech;
	const char *name;

	DBG("ok %d", ok);

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		goto error;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+COPS:"))
		return;

	g_at_result_iter_skip_next(&iter);
	g_at_result_iter_skip_next(&iter);

	if (g_at_result_iter_next_string(&iter, &name) == FALSE)
		goto error;

	strncpy(cbd->op.name, name, OFONO_MAX_OPERATOR_NAME_LENGTH);
	cbd->op.name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';

	/* Ignored for now. TODO: maybe read format but value
	 * wouldn't be forwarder anywhere
	 */
	cbd->op.mcc[0] = '\0';
	cbd->op.mnc[0] = '\0';

	/* Default to GSM */
	if (g_at_result_iter_next_number(&iter, &tech) == FALSE)
		cbd->op.tech = ublox_map_radio_access_technology(ACCESS_TECHNOLOGY_GSM);
	else
		cbd->op.tech = ublox_map_radio_access_technology(tech);

	if (g_at_chat_send(nmd->aux, "AT+CESQ", cesq_prefix,
			   cesq_cb, cbd, NULL)) {
		return;
	}

error:
	CALLBACK_WITH_FAILURE(cbd->cb, cbd->data);
	g_free(cbd);
}


static void ublox_netmon_request_update(struct ofono_netmon *netmon,
					ofono_netmon_cb_t cb, void *data)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);
	struct req_cb_data *cbd;

	DBG("ublox netmon request update");

	cbd = req_cb_data_new0(cb, data, netmon);
	if (!cbd) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	if (g_at_chat_send(nmd->aux, "AT+COPS?", cops_prefix,
			   cops_cb, cbd, NULL) == 0) {
		CALLBACK_WITH_FAILURE(cb, data);
		g_free(cbd);
	}
}

static int ublox_netmon_probe(struct ofono_netmon *netmon,
			      unsigned int vendor, void *user)
{
	struct netmon_driver_data *n = user;
	struct netmon_driver_data *nmd;

	DBG("ublox netmon probe");

	nmd = g_try_new0(struct netmon_driver_data, 1);
	if (!nmd) {
		return -ENOMEM;
	}

	nmd->modem = g_at_chat_clone(n->modem);
	nmd->aux = g_at_chat_clone(n->aux);

	ofono_netmon_set_data(netmon, nmd);

	/* Set +UCGED=2 mode as early as possible */
	g_at_chat_send(nmd->aux, "AT+UCGED=2", ucged_prefix,
		       NULL, NULL, NULL);

	g_idle_add(ublox_delayed_register, netmon);

	return 0;
}

static void ublox_netmon_remove(struct ofono_netmon *netmon)
{
	struct netmon_driver_data *nmd = ofono_netmon_get_data(netmon);

	DBG("ublox netmon remove");

	ofono_netmon_set_data(netmon, NULL);

	g_at_chat_unref(nmd->modem);
	g_at_chat_unref(nmd->aux);

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
