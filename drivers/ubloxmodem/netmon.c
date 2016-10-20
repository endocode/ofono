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

struct netmon_driver_data {
	GAtChat *modem;
	GAtChat *aux;
};

struct req_cb_data {
	struct ofono_netmon *netmon;

	ofono_netmon_cb_t cb;
	void *data;

	struct ofono_network_operator op;
	int rssi;

	int rxlev;	/* CESQ: Received Signal Strength Indication */
	int ber;	/* CESQ: Bit Error Rate */
	int rscp;	/* CESQ: Received Signal Code Powe */
	int rsrp;	/* CESQ: Reference Signal Received Power */
	double ecn0;	/* CESQ: Received Energy Ratio */
	double rsrq;	/* CESQ: Reference Signal Received Quality */
};

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
	default:
		break;
	}

	return 0;
}

static inline struct req_cb_data *req_cb_data_new0(void *cb, void *data, void *user)
{
	struct req_cb_data *ret = g_new0(struct req_cb_data, 1);
	if (ret) {
		ret->cb = cb;
		ret->data = data;
		ret->netmon = user;
	}

	return ret;
}

static gboolean ublox_delayed_register(gpointer user_data)
{
	struct ofono_netmon *netmon = user_data;

	ofono_netmon_register(netmon);

	return FALSE;
}

/* num in [il, ir] -> [ol, or] or -1 if equual to exception e return e */
static int clamp_int(int num, int il, int ir, int ol, int or, int step, int e)
{
	if (num == e)
		return e;
	else if (num > ir)
		return or;
	else
		return ol + step * num;
}

static float clamp_fl(int num, int il, int ir, double ol, double or, float step, int e)
{
	if (num == e)
		return e;
	else if (num > ir)
		return or;
	else
		return ol + step * num;
}

static void cesq_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	enum ublox_cesq_ofono_netmon_info {
		RXLEV, BER, RSCP, ECN0, RSRQ, RSRP, COUNT
	};

	struct req_cb_data *cbd = user_data;
	struct ofono_netmon *nm = cbd->netmon;
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
			cbd->rxlev = clamp_int(number, 0, 63, -110, -48, 1, 99);
			break;
		case BER:
			cbd->ber = number;
			break;
		case RSCP:
			cbd->rscp = clamp_int(number, 0, 96, -121, -25, 1, 255);
			break;
		case ECN0:
			cbd->ecn0 = clamp_fl(number, 0, 49, -24.5, 0.0, 0.5, 255);
			break;
		case RSRQ:
			cbd->rsrq = clamp_fl(number, 0, 34, -19.0, -3.0, 0.5, 255);
			break;
		case RSRP:
			cbd->rsrp = clamp_int(number, 0, 97, -141, -44, 1, 255);
			break;
		default:
			break;
		}
	}

	switch (cbd->op.tech) {
	case OFONO_NETMON_CELL_TYPE_GSM:
		cbd->rssi = cbd->rxlev;
		break;
	case OFONO_NETMON_CELL_TYPE_UMTS:
		cbd->rssi = cbd->rscp/cbd->ecn0;
		break;
	case OFONO_NETMON_CELL_TYPE_LTE:
		cbd->rssi = cbd->rsrp/cbd->rsrq;
		break;
	default:
		break;
	}

	DBG(" RXLEV	%d ", cbd->rxlev);
	DBG(" BER	%d ", cbd->ber);
	DBG(" RSCP	%d ", cbd->rscp);
	DBG(" ECN0	%f ", cbd->ecn0);
	DBG(" RSRQ	%f ", cbd->rsrq);
	DBG(" RSRP	%d ", cbd->rsrp);
	DBG(" RSSI	%d ", cbd->rssi);

	ofono_netmon_serving_cell_notify(nm,
					 cbd->op.tech,
					 OFONO_NETMON_INFO_OPERATOR, cbd->op.name,
					 OFONO_NETMON_INFO_RSSI, cbd->rssi,
					 OFONO_NETMON_INFO_RXLEV, cbd->rxlev,
					 OFONO_NETMON_INFO_BER, cbd->ber,
					 OFONO_NETMON_INFO_RSCP, cbd->rscp,
					 OFONO_NETMON_INFO_ECN0, cbd->ecn0,
					 OFONO_NETMON_INFO_RSRQ, cbd->rsrq,
					 OFONO_NETMON_INFO_RSRP, cbd->rsrp,
					 OFONO_NETMON_INFO_INVALID);

	CALLBACK_WITH_SUCCESS(cbd->cb, cbd->data);
	g_free(cbd);
	return;

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
