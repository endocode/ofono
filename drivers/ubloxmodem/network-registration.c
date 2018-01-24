/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
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

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include "src/common.h"
#include "ubloxmodem.h"

static const char *none_prefix[] = { NULL };
static const char *creg_prefix[] = { "+CREG:", NULL };
static const char *cops_prefix[] = { "+COPS:", NULL };
static const char *csq_prefix[] = { "+CSQ:", NULL };
static const char *cind_prefix[] = { "+CIND:", NULL };
static const char *cmer_prefix[] = { "+CMER:", NULL };

struct netreg_data {
	GAtChat *chat;
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	int signal_index; /* If strength is reported via CIND */
	int signal_min; /* min strength reported via CIND */
	int signal_max; /* max strength reported via CIND */
	int signal_invalid; /* invalid strength reported via CIND */
	int tech;
	struct ofono_network_time time;
	guint nitz_timeout;
	unsigned int vendor;
};

struct tech_query {
	int status;
	int lac;
	int ci;
	struct ofono_netreg *netreg;
};

static void extract_mcc_mnc(const char *str, char *mcc, char *mnc)
{
	/* Three digit country code */
	strncpy(mcc, str, OFONO_MAX_MCC_LENGTH);
	mcc[OFONO_MAX_MCC_LENGTH] = '\0';

	/* Usually a 2 but sometimes 3 digit network code */
	strncpy(mnc, str + OFONO_MAX_MCC_LENGTH, OFONO_MAX_MNC_LENGTH);
	mnc[OFONO_MAX_MNC_LENGTH] = '\0';
}

static void at_creg_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_status_cb_t cb = cbd->cb;
	int status, lac, ci, tech;
	struct ofono_error error;
	struct netreg_data *nd = cbd->user;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, -1, -1, -1, cbd->data);
		return;
	}

	if (at_util_parse_reg(result, "+CREG:", NULL, &status,
				&lac, &ci, &tech, nd->vendor) == FALSE) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);
		return;
	}

	if ((status == 1 || status == 5) && (tech == -1))
		tech = nd->tech;

	cb(&error, status, lac, ci, tech, cbd->data);
}

static void ublox_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = nd;

	if (g_at_chat_send(nd->chat, "AT+CREG?", creg_prefix,
				at_creg_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, data);
}

static void cops_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(cbd->user);
	ofono_netreg_operator_cb_t cb = cbd->cb;
	struct ofono_network_operator op;
	GAtResultIter iter;
	int format, tech;
	const char *name;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+COPS:"))
		goto error;

	g_at_result_iter_skip_next(&iter);

	ok = g_at_result_iter_next_number(&iter, &format);

	if (ok == FALSE || format != 0)
		goto error;

	if (g_at_result_iter_next_string(&iter, &name) == FALSE)
		goto error;

	/* Default to GSM */
	if (g_at_result_iter_next_number(&iter, &tech) == FALSE)
		tech = ACCESS_TECHNOLOGY_GSM;

	strncpy(op.name, name, OFONO_MAX_OPERATOR_NAME_LENGTH);
	op.name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';

	strncpy(op.mcc, nd->mcc, OFONO_MAX_MCC_LENGTH);
	op.mcc[OFONO_MAX_MCC_LENGTH] = '\0';

	strncpy(op.mnc, nd->mnc, OFONO_MAX_MNC_LENGTH);
	op.mnc[OFONO_MAX_MNC_LENGTH] = '\0';

	/* Set to current */
	op.status = 2;
	op.tech = tech;

	DBG("cops_cb: %s, %s %s %d", name, nd->mcc, nd->mnc, tech);

	cb(&error, &op, cbd->data);
	g_free(cbd);

	return;

error:
	cb(&error, NULL, cbd->data);

	g_free(cbd);
}

static void cops_numeric_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(cbd->user);
	ofono_netreg_operator_cb_t cb = cbd->cb;
	GAtResultIter iter;
	const char *str;
	int format;
	int len;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+COPS:"))
		goto error;

	g_at_result_iter_skip_next(&iter);

	ok = g_at_result_iter_next_number(&iter, &format);

	if (ok == FALSE || format != 2)
		goto error;

	if (g_at_result_iter_next_string(&iter, &str) == FALSE)
		goto error;

	len = strspn(str, "0123456789");

	if (len != 5 && len != 6)
		goto error;

	extract_mcc_mnc(str, nd->mcc, nd->mnc);

	DBG("Cops numeric got mcc: %s, mnc: %s", nd->mcc, nd->mnc);

	ok = g_at_chat_send(nd->chat, "AT+COPS=3,0", none_prefix,
					NULL, NULL, NULL);

	if (ok)
		ok = g_at_chat_send(nd->chat, "AT+COPS?", cops_prefix,
					cops_cb, cbd, NULL);

	if (ok)
		return;

error:
	cb(&error, NULL, cbd->data);
	g_free(cbd);
}

static void ublox_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);
	gboolean ok;

	cbd->user = netreg;

	ok = g_at_chat_send(nd->chat, "AT+COPS=3,2", none_prefix,
						NULL, NULL, NULL);

	if (ok)
		ok = g_at_chat_send(nd->chat, "AT+COPS?", cops_prefix,
					cops_numeric_cb, cbd, NULL);

	if (ok)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, data);
}

static void cops_list_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_list_cb_t cb = cbd->cb;
	struct ofono_network_operator *list;
	GAtResultIter iter;
	int num = 0;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, 0, NULL, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+COPS:")) {
		while (g_at_result_iter_skip_next(&iter))
			num += 1;
	}

	DBG("Got %d elements", num);

	list = g_try_new0(struct ofono_network_operator, num);
	if (list == NULL) {
		CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
		return;
	}

	num = 0;
	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, "+COPS:")) {
		int status, tech, plmn;
		const char *l, *s, *n;
		gboolean have_long = FALSE;

		while (1) {
			if (!g_at_result_iter_open_list(&iter))
				break;

			if (!g_at_result_iter_next_number(&iter, &status))
				break;

			list[num].status = status;

			if (!g_at_result_iter_next_string(&iter, &l))
				break;

			if (strlen(l) > 0) {
				have_long = TRUE;
				strncpy(list[num].name, l,
					OFONO_MAX_OPERATOR_NAME_LENGTH);
			}

			if (!g_at_result_iter_next_string(&iter, &s))
				break;

			if (strlen(s) > 0 && !have_long)
				strncpy(list[num].name, s,
					OFONO_MAX_OPERATOR_NAME_LENGTH);

			list[num].name[OFONO_MAX_OPERATOR_NAME_LENGTH] = '\0';

			if (!g_at_result_iter_next_string(&iter, &n))
				break;

			extract_mcc_mnc(n, list[num].mcc, list[num].mnc);

			if (!g_at_result_iter_next_number(&iter, &tech))
				tech = ACCESS_TECHNOLOGY_GSM;

			list[num].tech = tech;

			if (!g_at_result_iter_next_number(&iter, &plmn))
				plmn = 0;

			if (!g_at_result_iter_close_list(&iter))
				break;

			num += 1;
		}
	}

	DBG("Got %d operators", num);

{
	int i = 0;

	for (; i < num; i++) {
		DBG("Operator: %s, %s, %s, status: %d, %d",
			list[i].name, list[i].mcc, list[i].mnc,
			list[i].status, list[i].tech);
	}
}

	cb(&error, num, list, cbd->data);

	g_free(list);
}

static void ublox_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(nd->chat, "AT+COPS=?", cops_prefix,
				cops_list_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, 0, NULL, data);
}

static void register_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_register_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void ublox_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(nd->chat, "AT+COPS=0", none_prefix,
				register_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ublox_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[128];

	snprintf(buf, sizeof(buf), "AT+COPS=1,2,\"%s%s\"", mcc, mnc);

	if (g_at_chat_send(nd->chat, buf, none_prefix,
				register_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void ciev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	int strength, ind;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIEV:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &ind))
		return;

	if (ind != nd->signal_index)
		return;

	if (!g_at_result_iter_next_number(&iter, &strength))
		return;

	if (strength == nd->signal_invalid)
		strength = -1;
	else
		strength = (strength * 100) / (nd->signal_max - nd->signal_min);

	ofono_netreg_strength_notify(netreg, strength);
}

static void cind_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_strength_cb_t cb = cbd->cb;
	struct netreg_data *nd = cbd->user;
	int index;
	int strength;
	GAtResultIter iter;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIND:")) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	for (index = 1; index < nd->signal_index; index++)
		g_at_result_iter_skip_next(&iter);

	g_at_result_iter_next_number(&iter, &strength);

	if (strength == nd->signal_invalid)
		strength = -1;
	else
		strength = (strength * 100) / (nd->signal_max - nd->signal_min);

	cb(&error, strength, cbd->data);
}

static void csq_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_strength_cb_t cb = cbd->cb;
	int strength;
	GAtResultIter iter;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSQ:")) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	g_at_result_iter_next_number(&iter, &strength);

	DBG("csq_cb: %d", strength);

	if (strength == 99)
		strength = -1;
	else
		strength = (strength * 100) / 31;

	cb(&error, strength, cbd->data);
}

static void ublox_signal_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb, void *data)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = nd;

	/*
	 * If we defaulted to using CIND, then keep using it,
	 * otherwise fall back to CSQ
	 */
	if (nd->signal_index > 0) {
		if (g_at_chat_send(nd->chat, "AT+CIND?", cind_prefix,
					cind_cb, cbd, g_free) > 0)
			return;
	} else {
		if (g_at_chat_send(nd->chat, "AT+CSQ", csq_prefix,
				csq_cb, cbd, g_free) > 0)
			return;
	}

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void creg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int status, lac, ci, tech;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct tech_query *tq;

	if (at_util_parse_reg_unsolicited(result, "+CREG:", &status,
				&lac, &ci, &tech, nd->vendor) == FALSE)
		return;

	if (status != 1 && status != 5)
		goto notify;

	tq = g_try_new0(struct tech_query, 1);
	if (tq == NULL)
		goto notify;

	tq->status = status;
	tq->lac = lac;
	tq->ci = ci;
	tq->netreg = netreg;

	g_free(tq);

	if ((status == 1 || status == 5) && tech == -1)
		tech = nd->tech;

notify:
	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

/* CGREG callback in netreg to fix Ublox staying in the wrong
 * network state.
 */
static void cgreg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int status, lac, ci, tech;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct tech_query *tq;

	if (at_util_parse_reg_unsolicited(result, "+CGREG:", &status,
				&lac, &ci, &tech, nd->vendor) == FALSE)
		return;

	if (status != 1 && status != 5)
		goto notify;

	tq = g_try_new0(struct tech_query, 1);
	if (tq == NULL)
		goto notify;

	tq->status = status;
	tq->lac = lac;
	tq->ci = ci;
	tq->netreg = netreg;

	g_free(tq);

	if ((status == 1 || status == 5) && tech == -1)
		tech = nd->tech;

notify:
	DBG("+CGREG notify from Ublox netreg.");
	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void cereg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int status, lac, ci, tech;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	struct tech_query *tq;

	if (at_util_parse_reg_unsolicited(result, "+CEREG:", &status,
				&lac, &ci, &tech, nd->vendor) == FALSE)
		return;

	if (status != 1 && status != 5)
		goto notify;

	tq = g_try_new0(struct tech_query, 1);
	if (tq == NULL)
		goto notify;

	tq->status = status;
	tq->lac = lac;
	tq->ci = ci;
	tq->netreg = netreg;

	g_free(tq);

	if ((status == 1 || status == 5) && tech == -1)
		tech = nd->tech;

notify:
	DBG("+CEREG notify from Ublox netreg.");
	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void at_cmer_not_supported(struct ofono_netreg *netreg)
{
	ofono_error("+CMER not supported by this modem.  If this is an error"
			" please submit patches to support this hardware");

	ofono_netreg_remove(netreg);
}

static void at_cmer_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (!ok) {
		at_cmer_not_supported(netreg);
		return;
	}

	g_at_chat_register(nd->chat, "+CIEV:",
			ciev_notify, FALSE, netreg, NULL);

	g_at_chat_register(nd->chat, "+CREG:",
				creg_notify, FALSE, netreg, NULL);
	
	g_at_chat_register(nd->chat, "+CGREG:",
				cgreg_notify, FALSE, netreg, NULL);

	g_at_chat_register(nd->chat, "+CEREG:",
				cereg_notify, FALSE, netreg, NULL);

	ofono_netreg_register(netreg);
}

static inline char wanted_cmer(int supported, const char *pref)
{
	while (*pref) {
		if (supported & (1 << (*pref - '0')))
			return *pref;

		pref++;
	}

	return '\0';
}

static inline ofono_bool_t append_cmer_element(char *buf, int *len, int cap,
						const char *wanted,
						ofono_bool_t last)
{
	char setting = wanted_cmer(cap, wanted);

	if (!setting)
		return FALSE;

	buf[*len] = setting;

	if (last)
		buf[*len + 1] = '\0';
	else
		buf[*len + 1] = ',';

	*len += 2;

	return TRUE;
}

static ofono_bool_t build_cmer_string(char *buf, int *cmer_opts,
					struct netreg_data *nd)
{
	const char *ind;
	int len = sprintf(buf, "AT+CMER=");
	const char *mode;

	DBG("");

	/* UBX-13002752 R33: TOBY L2 doesn't support mode 2 and 3 */
	mode = "1";


	/*
	 * Forward unsolicited result codes directly to the TE;
	 * TA‑TE link specific inband technique used to embed result codes and
	 * data when TA is in on‑line data mode
	 */
	if (!append_cmer_element(buf, &len, cmer_opts[0], mode, FALSE))
		return FALSE;

	/* No keypad event reporting */
	if (!append_cmer_element(buf, &len, cmer_opts[1], "0", FALSE))
		return FALSE;

	/* No display event reporting */
	if (!append_cmer_element(buf, &len, cmer_opts[2], "0", FALSE))
		return FALSE;

	/*
	 * Only those indicator events, which are not caused by +CIND
	 * shall be indicated by the TA to the TE.
	 */
	ind = "1";


	/*
	 * Indicator event reporting using URC +CIEV: <ind>,<value>.
	 * <ind> indicates the indicator order number (as specified for +CIND)
	 * and <value> is the new value of indicator.
	 */
	if (!append_cmer_element(buf, &len, cmer_opts[3], ind, TRUE))
		return FALSE;

	return TRUE;
}

static void at_cmer_query_cb(ofono_bool_t ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	GAtResultIter iter;
	int cmer_opts_cnt = 5; /* See 27.007 Section 8.10 */
	int cmer_opts[cmer_opts_cnt];
	int opt;
	int mode;
	char buf[128];

	if (!ok)
		goto error;

	memset(cmer_opts, 0, sizeof(cmer_opts));

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CMER:"))
		goto error;

	for (opt = 0; opt < cmer_opts_cnt; opt++) {
		int min, max;

		if (!g_at_result_iter_open_list(&iter))
			goto error;

		while (g_at_result_iter_next_range(&iter, &min, &max)) {
			for (mode = min; mode <= max; mode++)
				cmer_opts[opt] |= 1 << mode;
		}

		if (!g_at_result_iter_close_list(&iter))
			goto error;
	}

	if (build_cmer_string(buf, cmer_opts, nd) == FALSE)
		goto error;

	g_at_chat_send(nd->chat, buf, cmer_prefix,
			at_cmer_set_cb, netreg, NULL);

	return;

error:
	at_cmer_not_supported(netreg);
}

static void cind_support_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	GAtResultIter iter;
	const char *str;
	char *signal_identifier = "signal";
	int index;
	int min = 0;
	int max = 0;
	int tmp_min, tmp_max, invalid;
	int i, len;
	char buf[256];

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CIND:"))
		goto error;

	index = 1;

	while (g_at_result_iter_open_list(&iter)) {
		/* Reset invalid default value for every token */
		invalid = 99;

		if (!g_at_result_iter_next_string(&iter, &str))
			goto error;

		if (!g_at_result_iter_open_list(&iter))
			goto error;

		while (g_at_result_iter_next_range(&iter, &tmp_min, &tmp_max)) {
			if (tmp_min != tmp_max) {
				min = tmp_min;
				max = tmp_max;
			} else
				invalid = tmp_min;
		}

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (g_str_equal(signal_identifier, str) == TRUE) {
			nd->signal_index = index;
			nd->signal_min = min;
			nd->signal_max = max;
			nd->signal_invalid = invalid;
		}

		index += 1;
	}

	if (nd->signal_index == 0)
		goto error;

	/* Turn off all CIEV indicators except the signal indicator */
	len = sprintf(buf, "AT+CIND=");

	for (i = 1; i < index - 1; i++)
		len += sprintf(buf + len, i == nd->signal_index ? "1," : "0,");

	len += sprintf(buf + len, i == nd->signal_index ? "1" : "0");
	g_at_chat_send(nd->chat, buf, NULL, NULL, NULL, NULL);

	g_at_chat_send(nd->chat, "AT+CMER=?", cmer_prefix,
			at_cmer_query_cb, netreg, NULL);

	return;

error:
	ofono_error("This driver is not setup with Signal Strength reporting"
			" via CIND indications, please write proper netreg"
			" handling for this device");

	ofono_netreg_remove(netreg);
}

static void at_creg_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (!ok) {
		ofono_error("Unable to initialize Network Registration");
		ofono_netreg_remove(netreg);
		return;
	}

	g_at_chat_send(nd->chat, "AT+CIND=?", cind_prefix,
			cind_support_cb, netreg, NULL);

}

// CEREG set callback for Ublox staying in searching issue
static void at_cereg_set_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;

	if (!ok) {
		ofono_error("Unable to initialize EPS Network Registration");
		ofono_netreg_remove(netreg);
		return;
	}
}

static void at_creg_test_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
	gint range[2];
	GAtResultIter iter;
	int creg1 = 0;
	int creg2 = 0;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

retry:
	if (!g_at_result_iter_next(&iter, "+CREG:"))
		goto error;

	if (!g_at_result_iter_open_list(&iter))
		goto retry;

	while (g_at_result_iter_next_range(&iter, &range[0], &range[1])) {
		if (1 >= range[0] && 1 <= range[1])
			creg1 = 1;
		if (2 >= range[0] && 2 <= range[1])
			creg2 = 1;
	}

	g_at_result_iter_close_list(&iter);

	if (creg2) {
		g_at_chat_send(nd->chat, "AT+CREG=2", none_prefix,
				at_creg_set_cb, netreg, NULL);
		// Enable +CEREG URC
		g_at_chat_send(nd->chat, "AT+CEREG=2", none_prefix,
				at_cereg_set_cb, netreg, NULL);
		return;
	}

	if (creg1) {
		g_at_chat_send(nd->chat, "AT+CREG=1", none_prefix,
				at_creg_set_cb, netreg, NULL);
		// Enable +CEREG URC
		g_at_chat_send(nd->chat, "AT+CEREG=2", none_prefix,
				at_cereg_set_cb, netreg, NULL);
		return;
	}

error:
	ofono_error("Unable to initialize Network Registration");
	ofono_netreg_remove(netreg);
}

static int ublox_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;
	struct netreg_data *nd;

	nd = g_new0(struct netreg_data, 1);

	nd->chat = g_at_chat_clone(chat);
	nd->vendor = vendor;
	nd->tech = -1;
	nd->time.sec = -1;
	nd->time.min = -1;
	nd->time.hour = -1;
	nd->time.mday = -1;
	nd->time.mon = -1;
	nd->time.year = -1;
	nd->time.dst = 0;
	nd->time.utcoff = 0;
	ofono_netreg_set_data(netreg, nd);

	g_at_chat_send(nd->chat, "AT+CREG=?", creg_prefix,
			at_creg_test_cb, netreg, NULL);

	return 0;
}

static void ublox_netreg_remove(struct ofono_netreg *netreg)
{
	struct netreg_data *nd = ofono_netreg_get_data(netreg);

	if (nd->nitz_timeout)
		g_source_remove(nd->nitz_timeout);

	ofono_netreg_set_data(netreg, NULL);

	g_at_chat_unref(nd->chat);
	g_free(nd);
}

static struct ofono_netreg_driver driver = {
	.name				= UBLOXMODEM,
	.probe				= ublox_netreg_probe,
	.remove				= ublox_netreg_remove,
	.registration_status		= ublox_registration_status,
	.current_operator		= ublox_current_operator,
	.list_operators			= ublox_list_operators,
	.register_auto			= ublox_register_auto,
	.register_manual		= ublox_register_manual,
	.strength			= ublox_signal_strength,
};

void ublox_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void ublox_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
