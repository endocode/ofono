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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "gatchat.h"
#include "gatresult.h"

#include "ubloxmodem.h"

static const char *none_prefix[] = { NULL };
static const char *cgcontrdp_prefix[] = { "+CGCONTRDP:", NULL };

struct gprs_context_data {
	GAtChat *chat;
	unsigned int active_context;
	unsigned int gprs_cid;
	char apn[OFONO_GPRS_MAX_APN_LENGTH + 1];
	char username[OFONO_GPRS_MAX_USERNAME_LENGTH + 1];
	char password[OFONO_GPRS_MAX_PASSWORD_LENGTH + 1];
	enum ofono_gprs_auth_method auth_method;
	ofono_gprs_context_cb_t cb;
	void *cb_data;
};

#define UBLOX_MAX_DEF_CONTEXT 8

struct ublox_data {
	/* Stores default PDP context/default EPS bearer. */
	unsigned int default_context_id;
	struct gprs_context_data contexts[UBLOX_MAX_DEF_CONTEXT];
	unsigned int ncontexts;

	/* Save used cids on the modem. */
	bool active[UBLOX_MAX_DEF_CONTEXT];
	unsigned int nactive;
};

static struct ublox_data ublox_data;

static int get_context_id()
{
	int i;

	for (i = 0; i < UBLOX_MAX_DEF_CONTEXT; i++) {
		if (!ublox_data.active[i]) {
			ublox_data.active[i] = true;
			return i+1;
		}
	}

	return 0;
}

static void release_context_id(unsigned cid)
{
	if (!cid || cid > UBLOX_MAX_DEF_CONTEXT)
		return;

	ublox_data.active[cid-1] = false;
}

/*
 * CGCONTRDP returns addr + netmask in the same string in the form
 * of "a.b.c.d.m.m.m.m" for IPv4. IPv6 is not supported so we ignore it.
 */
static int read_addrnetmask(struct ofono_gprs_context *gc,
				const char *addrnetmask)
{
	char *dup = strdup(addrnetmask);
	char *s = dup;

	const char *addr = s;
	const char *netmask = NULL;

	int ret = -EINVAL;
	int i;

	/* Count 7 dots for ipv4, less or more means error. */
	for (i = 0; i < 8; i++, s++) {
		s = strchr(s, '.');
		if (!s)
			break;

		if (i == 3) {
			/* set netmask ptr and break the string */
			netmask = s+1;
			s[0] = 0;
		}
	}

	if (i == 7) {
		ofono_gprs_context_set_ipv4_address(gc, addr, 1);
		ofono_gprs_context_set_ipv4_netmask(gc, netmask);

		ret = 0;
	}

	free(dup);

	return ret;
}

static void cgcontrdp_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	int cid = -1;

	const char *laddrnetmask = NULL;
	const char *gw = NULL;
	const char *dns[2+1] = { NULL, NULL, NULL };
	const char *apn = NULL;

	struct ofono_modem *modem;
	const char *interface;

	DBG("ok %d", ok);

	if (!ok) {
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+CGCONTRDP:")) {
		/* tmp vals for ignored fields */
		int bearer_id;

		if (!g_at_result_iter_next_number(&iter, &cid))
			break;

		if (!g_at_result_iter_next_number(&iter, &bearer_id))
			break;

		if (!g_at_result_iter_next_string(&iter, &apn))
			break;

		if (!g_at_result_iter_next_string(&iter, &laddrnetmask))
			break;

		if (!g_at_result_iter_next_string(&iter, &gw))
			break;

		if (!g_at_result_iter_next_string(&iter, &dns[0]))
			break;

		if (!g_at_result_iter_next_string(&iter, &dns[1]))
			break;
	}

	/* read interface name read at detection time */
	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");
	ofono_gprs_context_set_interface(gc, interface);

	if (!laddrnetmask || read_addrnetmask(gc, laddrnetmask) < 0)
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);

	if (gw)
		ofono_gprs_context_set_ipv4_gateway(gc, gw);

	if (dns[0])
		ofono_gprs_context_set_ipv4_dns_servers(gc, dns);

	if (gcd->active_context == ublox_data.default_context_id) {
		/*
		 * Only for automatic default context: the APN set by the user
		 * might not be the correct one because the default context was
		 * used instead.
		 */
		ofono_gprs_context_set_apn(gc, apn);
		strcpy(gcd->apn, apn);
	}

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static int ublox_read_ip_config(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	/* read ip configuration info */
	snprintf(buf, sizeof(buf), "AT+CGCONTRDP=%u", gcd->active_context);
	return g_at_chat_send(gcd->chat, buf, cgcontrdp_prefix,
				cgcontrdp_cb, gc, NULL);

}

static void cgact_enable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);
	if (!ok) {
		if (ublox_data.default_context_id &&
			!ublox_data.nactive) {
			/*
			 * Proceed with an already active context known from a
			 * CGEV ME PDN ACT. That one should be good enough.
			 */
			gcd->active_context = ublox_data.default_context_id;
		} else {

			gcd->active_context = 0;
			goto fail;
		}
	}

	ublox_data.nactive++;

	if (ublox_read_ip_config(gc) > 0)
		return;

fail:
	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("ok %d", ok);

	if (!ok) {
		struct ofono_error error;

		gcd->active_context = 0;

		decode_at_error(&error, g_at_result_final_response(result));
		gcd->cb(&error, gcd->cb_data);
		return;
	}

	snprintf(buf, sizeof(buf), "AT+CGACT=1,%u", gcd->active_context);
	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				cgact_enable_cb, gc, NULL))
		return;

	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void ublox_activate_ctx(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[OFONO_GPRS_MAX_APN_LENGTH + 128];
	int len;

	len = snprintf(buf, sizeof(buf), "AT+CGDCONT=%u,\"IP\"",
				gcd->active_context);

	if (gcd->apn)
		snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"",
					gcd->apn);

	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				cgdcont_cb, gc, NULL) > 0)
		return;

	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void uauthreq_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);

	if (!ok) {
		ofono_error("can't authenticate");
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
		return;
	}

	ublox_activate_ctx(gc);
}

#define UBLOX_MAX_USER_LEN 50
#define UBLOX_MAX_PASS_LEN 50

static void ublox_authenticate(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[UBLOX_MAX_USER_LEN + UBLOX_MAX_PASS_LEN + 32];
	unsigned auth_method;

	switch (gcd->auth_method) {
	case OFONO_GPRS_AUTH_METHOD_PAP:
		auth_method = 1;
		break;
	case OFONO_GPRS_AUTH_METHOD_CHAP:
		auth_method = 2;
		break;
	default:
		ofono_error("Unsupported auth type %u", gcd->auth_method);
		goto error;
	}

	snprintf(buf, sizeof(buf), "AT+UAUTHREQ=%u,%u,\"%s\",\"%s\"",
			gcd->active_context, auth_method,
			gcd->username, gcd->password);

	/* If this failed, we will see it during context activation. */
	if (g_at_chat_send(gcd->chat, buf, none_prefix,
				uauthreq_cb, gc, NULL) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
}

static void ublox_gprs_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	/* IPv6 support not implemented */
	if (ctx->proto != OFONO_GPRS_PROTO_IP) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	DBG("cid %u", ctx->cid);

	gcd->active_context = get_context_id();
	if (!gcd->active_context) {
		ofono_error("can't activate more contexts");
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	gcd->cb = cb;
	gcd->cb_data = data;
	gcd->auth_method = ctx->auth_method;
	gcd->gprs_cid = ctx->cid;
	memcpy(gcd->apn, ctx->apn, sizeof(ctx->apn));

	if (strlen(ctx->username) && strlen(ctx->password)) {
		memcpy(gcd->username, ctx->username, sizeof(ctx->username));
		memcpy(gcd->password, ctx->password, sizeof(ctx->password));
		ublox_authenticate(gc);
	} else
		ublox_activate_ctx(gc);
}

static void cgact_disable_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("ok %d", ok);
	if (!ok) {
		if (gcd->active_context && ublox_data.default_context_id)
			ofono_warn("Disabling the default context is not "
					"allowed on LTE.");
		CALLBACK_WITH_FAILURE(gcd->cb, gcd->cb_data);
		return;
	}

	release_context_id(gcd->active_context);
	gcd->active_context = 0;
	gcd->gprs_cid = 0;

	ublox_data.nactive--;

	CALLBACK_WITH_SUCCESS(gcd->cb, gcd->cb_data);
}

static void ublox_gprs_deactivate_primary(struct ofono_gprs_context *gc,
					unsigned int cid,
					ofono_gprs_context_cb_t cb, void *data)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	char buf[64];

	DBG("cid %u", cid);

	gcd->cb = cb;
	gcd->cb_data = data;

	snprintf(buf, sizeof(buf), "AT+CGACT=0,%u", gcd->active_context);
	g_at_chat_send(gcd->chat, buf, none_prefix,
			cgact_disable_cb, gc, NULL);
}

static void cgev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);
	GAtResultIter iter;
	const char *event;
	unsigned int cid;
	char tmp[5] = {0};

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGEV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &event))
		return;

	if (g_str_has_prefix(event, "ME PDN ACT") == TRUE) {
		/*
		 * We only care about the first even coming in before any user
		 * activations happen. This is for detecting the default
		 * context in LTE networks which doesn't require user
		 * activation.
		 */
		sscanf(event, "%s %s %s %u", tmp, tmp, tmp, &cid);
		if (!ublox_data.default_context_id && !ublox_data.nactive)
			ublox_data.default_context_id = cid;
		DBG("cid=%d activated", cid);

		return;
	} else if (g_str_has_prefix(event, "NW PDN DEACT") == TRUE) {
		sscanf(event, "%s %s %s %u", tmp, tmp, tmp, &cid);
		DBG("cid=%d deactivated", cid);
	} else
		return;

	/* Can happen if non-default context is deactivated. Ignore. */
	if (gcd->active_context != cid)
		return;

	if (ublox_data.default_context_id == cid)
		ublox_data.default_context_id = 0;

	ofono_gprs_context_deactivated(gc, gcd->gprs_cid);
	gcd->active_context = 0;
	gcd->gprs_cid = 0;
}


static int ublox_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_context_data *gcd;

	DBG("");

	if (ublox_data.ncontexts >= UBLOX_MAX_DEF_CONTEXT) {
		ofono_error("modem supports defining max 8 contexts");
		return -E2BIG;
	}

	gcd = &ublox_data.contexts[ublox_data.ncontexts];
	memset(gcd, 0, sizeof(*gcd));
	ublox_data.ncontexts++;

	gcd->chat = g_at_chat_clone(chat);

	ofono_gprs_context_set_data(gc, gcd);

	/* Not sure if ok to register all contexts with the callback. */
	g_at_chat_register(chat, "+CGEV:", cgev_notify, FALSE, gc, NULL);

	return 0;
}

static void ublox_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *gcd = ofono_gprs_context_get_data(gc);

	DBG("");

	ofono_gprs_context_set_data(gc, NULL);

	g_at_chat_unref(gcd->chat);

	memset(gcd, 0, sizeof(*gcd));
	ublox_data.ncontexts--;
}

static struct ofono_gprs_context_driver driver = {
	.name			= "ubloxmodem",
	.probe			= ublox_gprs_context_probe,
	.remove			= ublox_gprs_context_remove,
	.activate_primary	= ublox_gprs_activate_primary,
	.deactivate_primary	= ublox_gprs_deactivate_primary,
};


void ublox_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void ublox_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
