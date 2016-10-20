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

static void cops_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct req_cb_data *cbd = user_data;
	ofono_netmon_cb_t cb = cbd->cb;
	struct ofono_netmon *nm = cbd->netmon;
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


	ofono_netmon_serving_cell_notify(nm,
					 cbd->op.tech,
					 OFONO_NETMON_INFO_INVALID);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	g_free(cbd);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
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
