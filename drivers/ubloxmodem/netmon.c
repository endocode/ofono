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

#include "ubloxmodem.h"
#include "drivers/atmodem/vendor.h"

struct netmon_driver_data {
	GAtChat *modem;
	GAtChat *aux;
};

struct req_cb_data {
	struct ofono_netmon *netmon;

	ofono_netmon_cb_t cb;
	void *data;
};

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

static void ublox_netmon_request_update(struct ofono_netmon *netmon,
					ofono_netmon_cb_t cb, void *data)
{
	struct req_cb_data *cbd;

	DBG("ublox netmon request update");

	cbd = req_cb_data_new0(cb, data, netmon);
	if (!cbd) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, data);
	g_free(cbd);
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
