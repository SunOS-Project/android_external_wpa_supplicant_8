/*
 * wpa_supplicant - Event notifications
 * Copyright (c) 2009-2010, Jouni Malinen <j@w1.fi>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 *
 * Changes from Qualcomm Innovation Center are provided under the following license:
 *
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "utils/includes.h"

#include "utils/common.h"
#include "common/wpa_ctrl.h"
#include "common/nan_de.h"
#include "config.h"
#include "wpa_supplicant_i.h"
#include "wps_supplicant.h"
#include "dbus/dbus_common.h"
#include "dbus/dbus_new.h"
#include "rsn_supp/wpa.h"
#include "rsn_supp/pmksa_cache.h"
#include "fst/fst.h"
#include "crypto/tls.h"
#include "bss.h"
#include "driver_i.h"
#include "scan.h"
#include "p2p_supplicant.h"
#include "sme.h"
#include "notify.h"
#include "aidl/aidl.h"
#include "vendor_aidl/aidl_vendor.h"

int wpas_notify_supplicant_initialized(struct wpa_global *global)
{
#ifdef CONFIG_CTRL_IFACE_DBUS_NEW
	if (global->params.dbus_ctrl_interface) {
		global->dbus = wpas_dbus_init(global);
		if (global->dbus == NULL)
			return -1;
	}
#endif /* CONFIG_CTRL_IFACE_DBUS_NEW */

#ifdef CONFIG_AIDL
	// Initialize AIDL here if daemonize is disabled.
	// Otherwise initialize it later.
	if (!global->params.daemonize) {
		global->aidl = wpas_aidl_init(global);
		if (!global->aidl)
			return -1;
	}
#endif /* CONFIG_AIDL */

	return 0;
}


void wpas_notify_supplicant_deinitialized(struct wpa_global *global)
{
#ifdef CONFIG_CTRL_IFACE_DBUS_NEW
	if (global->dbus)
		wpas_dbus_deinit(global->dbus);
#endif /* CONFIG_CTRL_IFACE_DBUS_NEW */

#ifdef CONFIG_AIDL
	if (global->aidl)
		wpas_aidl_deinit(global->aidl);
#endif /* CONFIG_AIDL */

#ifdef CONFIG_SUPPLICANT_VENDOR_AIDL
	if (global->vendor_aidl)
		wpas_aidl_vendor_deinit(global->vendor_aidl);
#endif /* CONFIG_SUPPLICANT_VENDOR_AIDL */
}


int wpas_notify_iface_added(struct wpa_supplicant *wpa_s)
{
	if (!wpa_s->p2p_mgmt) {
		if (wpas_dbus_register_interface(wpa_s))
			return -1;
	}

#ifdef CONFIG_AIDL
	/*
	 * AIDL initialization may not be complete here if daemonize is enabled.
	 * Initialization is done after daemonizing in order to avoid
	 * issues with the file descriptor.
	 */
	if (!wpa_s->global->aidl)
		return 0;
	/* AIDL interface wants to keep track of the P2P mgmt iface. */
	if (wpas_aidl_register_interface(wpa_s))
		return -1;
#endif

#ifdef CONFIG_SUPPLICANT_VENDOR_AIDL
	if (!wpa_s || !wpa_s->global->vendor_aidl)
		return 0;
	if (wpas_aidl_vendor_register_interface(wpa_s))
		return -1;
#endif /* CONFIG_SUPPLICANT_VENDOR_AIDL */

	return 0;
}


void wpas_notify_iface_removed(struct wpa_supplicant *wpa_s)
{
	if (!wpa_s->p2p_mgmt) {
		/* unregister interface in new DBus ctrl iface */
		wpas_dbus_unregister_interface(wpa_s);
	}

	/* AIDL interface wants to keep track of the P2P mgmt iface. */
	wpas_aidl_unregister_interface(wpa_s);
#ifdef CONFIG_SUPPLICANT_VENDOR_AIDL
	wpas_aidl_vendor_unregister_interface(wpa_s);
#endif /* CONFIG_SUPPLICANT_VENDOR_AIDL */
}


void wpas_notify_state_changed(struct wpa_supplicant *wpa_s,
			       enum wpa_states new_state,
			       enum wpa_states old_state)
{
	struct wpa_ssid *ssid = wpa_s->current_ssid;

	if (wpa_s->p2p_mgmt)
		return;

	/* notify the new DBus API */
	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_STATE);

#ifdef CONFIG_FST
	if (wpa_s->fst && !is_zero_ether_addr(wpa_s->bssid)) {
		if (new_state == WPA_COMPLETED)
			fst_notify_peer_connected(wpa_s->fst, wpa_s->bssid);
		else if (old_state >= WPA_ASSOCIATED &&
			 new_state < WPA_ASSOCIATED)
			fst_notify_peer_disconnected(wpa_s->fst, wpa_s->bssid);
	}
#endif /* CONFIG_FST */

	if (new_state == WPA_COMPLETED) {
		wpas_p2p_notif_connected(wpa_s);
		if (ssid)
			wpa_drv_roaming(wpa_s, !ssid->bssid_set,
					ssid->bssid_set ? ssid->bssid : NULL);
	} else if (old_state >= WPA_ASSOCIATED && new_state < WPA_ASSOCIATED) {
		wpas_p2p_notif_disconnected(wpa_s);
	}

	sme_state_changed(wpa_s);

#ifdef ANDROID
	wpa_msg_ctrl(wpa_s, MSG_INFO, WPA_EVENT_STATE_CHANGE
		     "id=%d state=%d BSSID=" MACSTR " SSID=%s",
		     wpa_s->current_ssid ? wpa_s->current_ssid->id : -1,
		     new_state,
		     MAC2STR(wpa_s->bssid),
		     wpa_s->current_ssid && wpa_s->current_ssid->ssid ?
		     wpa_ssid_txt(wpa_s->current_ssid->ssid,
				  wpa_s->current_ssid->ssid_len) : "");
#endif /* ANDROID */

	wpas_aidl_notify_state_changed(wpa_s);
}


void wpas_notify_disconnect_reason(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_DISCONNECT_REASON);

	wpas_aidl_notify_disconnect_reason(wpa_s);
}


void wpas_notify_mlo_info_change_reason(struct wpa_supplicant *wpa_s,
					enum mlo_info_change_reason reason)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_aidl_notify_mlo_info_change_reason(wpa_s, reason);
}


void wpas_notify_auth_status_code(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_AUTH_STATUS_CODE);
}


void wpas_notify_assoc_status_code(struct wpa_supplicant *wpa_s,
				   const u8 *bssid, u8 timed_out,
				   const u8 *assoc_resp_ie, size_t assoc_resp_ie_len)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_ASSOC_STATUS_CODE);

	wpas_aidl_notify_assoc_reject(wpa_s, bssid, timed_out, assoc_resp_ie, assoc_resp_ie_len);
}

void wpas_notify_auth_timeout(struct wpa_supplicant *wpa_s) {
	if (wpa_s->p2p_mgmt)
		return;

	wpas_aidl_notify_auth_timeout(wpa_s);
}

void wpas_notify_roam_time(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_ROAM_TIME);
}


void wpas_notify_roam_complete(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_ROAM_COMPLETE);
}


void wpas_notify_scan_in_progress_6ghz(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s,
				      WPAS_DBUS_PROP_SCAN_IN_PROGRESS_6GHZ);
}


void wpas_notify_session_length(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_SESSION_LENGTH);
}


void wpas_notify_bss_tm_status(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_BSS_TM_STATUS);

#ifdef CONFIG_WNM
	wpas_aidl_notify_bss_tm_status(wpa_s);
#endif
}


void wpas_notify_network_changed(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_CURRENT_NETWORK);
}


void wpas_notify_ap_scan_changed(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_AP_SCAN);
}


void wpas_notify_bssid_changed(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_CURRENT_BSS);

	wpas_aidl_notify_bssid_changed(wpa_s);
}


void wpas_notify_mac_address_changed(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_MAC_ADDRESS);
}


void wpas_notify_auth_changed(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_CURRENT_AUTH_MODE);
}


void wpas_notify_network_enabled_changed(struct wpa_supplicant *wpa_s,
					 struct wpa_ssid *ssid)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_network_enabled_changed(wpa_s, ssid);
}


void wpas_notify_network_selected(struct wpa_supplicant *wpa_s,
				  struct wpa_ssid *ssid)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_network_selected(wpa_s, ssid->id);
}


void wpas_notify_network_request(struct wpa_supplicant *wpa_s,
				 struct wpa_ssid *ssid,
				 enum wpa_ctrl_req_type rtype,
				 const char *default_txt)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_network_request(wpa_s, ssid, rtype, default_txt);

	wpas_aidl_notify_network_request(wpa_s, ssid, rtype, default_txt);
}


void wpas_notify_permanent_id_req_denied(struct wpa_supplicant *wpa_s)
{
	wpas_aidl_notify_permanent_id_req_denied(wpa_s);
}


void wpas_notify_scanning(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	/* notify the new DBus API */
	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_SCANNING);
}


void wpas_notify_scan_done(struct wpa_supplicant *wpa_s, int success)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_scan_done(wpa_s, success);
}


void wpas_notify_scan_results(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_wps_notify_scan_results(wpa_s);
}


void wpas_notify_wps_credential(struct wpa_supplicant *wpa_s,
				const struct wps_credential *cred)
{
	if (wpa_s->p2p_mgmt)
		return;

#ifdef CONFIG_WPS
	/* notify the new DBus API */
	wpas_dbus_signal_wps_cred(wpa_s, cred);
#endif /* CONFIG_WPS */
}


void wpas_notify_wps_event_m2d(struct wpa_supplicant *wpa_s,
			       struct wps_event_m2d *m2d)
{
	if (wpa_s->p2p_mgmt)
		return;

#ifdef CONFIG_WPS
	wpas_dbus_signal_wps_event_m2d(wpa_s, m2d);
#endif /* CONFIG_WPS */
}


void wpas_notify_wps_event_fail(struct wpa_supplicant *wpa_s,
				struct wps_event_fail *fail)
{
	if (wpa_s->p2p_mgmt)
		return;

#ifdef CONFIG_WPS
	wpas_dbus_signal_wps_event_fail(wpa_s, fail);

	wpas_aidl_notify_wps_event_fail(wpa_s, fail->peer_macaddr,
					fail->config_error,
					fail->error_indication);
#endif /* CONFIG_WPS */
}


void wpas_notify_wps_event_success(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

#ifdef CONFIG_WPS
	wpas_dbus_signal_wps_event_success(wpa_s);

	wpas_aidl_notify_wps_event_success(wpa_s);
#endif /* CONFIG_WPS */
}

void wpas_notify_wps_event_pbc_overlap(struct wpa_supplicant *wpa_s)
{
	if (wpa_s->p2p_mgmt)
		return;

#ifdef CONFIG_WPS
	wpas_dbus_signal_wps_event_pbc_overlap(wpa_s);

	wpas_aidl_notify_wps_event_pbc_overlap(wpa_s);
#endif /* CONFIG_WPS */
}


void wpas_notify_network_added(struct wpa_supplicant *wpa_s,
			       struct wpa_ssid *ssid)
{
	if (wpa_s->p2p_mgmt)
		return;

	/*
	 * Networks objects created during any P2P activities should not be
	 * exposed out. They might/will confuse certain non-P2P aware
	 * applications since these network objects won't behave like
	 * regular ones.
	 */
	if (!ssid->p2p_group && wpa_s->global->p2p_group_formation != wpa_s) {
		wpas_dbus_register_network(wpa_s, ssid);
		wpas_aidl_register_network(wpa_s, ssid);
		wpa_msg_ctrl(wpa_s, MSG_INFO, WPA_EVENT_NETWORK_ADDED "%d",
			     ssid->id);
	}
}


void wpas_notify_persistent_group_added(struct wpa_supplicant *wpa_s,
					struct wpa_ssid *ssid)
{
#ifdef CONFIG_P2P
	wpas_dbus_register_persistent_group(wpa_s, ssid);
	wpas_aidl_register_network(wpa_s, ssid);
#endif /* CONFIG_P2P */
}


void wpas_notify_persistent_group_removed(struct wpa_supplicant *wpa_s,
					  struct wpa_ssid *ssid)
{
#ifdef CONFIG_P2P
	wpas_dbus_unregister_persistent_group(wpa_s, ssid->id);
	wpas_aidl_unregister_network(wpa_s, ssid);
#endif /* CONFIG_P2P */
}


void wpas_notify_network_removed(struct wpa_supplicant *wpa_s,
				 struct wpa_ssid *ssid)
{
	if (wpa_s->next_ssid == ssid)
		wpa_s->next_ssid = NULL;
	if (wpa_s->last_ssid == ssid)
		wpa_s->last_ssid = NULL;
	if (wpa_s->current_ssid == ssid)
		wpa_s->current_ssid = NULL;
	if (wpa_s->ml_connect_probe_ssid == ssid) {
		wpa_s->ml_connect_probe_ssid = NULL;
		wpa_s->ml_connect_probe_bss = NULL;
	}
	if (wpa_s->connect_without_scan == ssid)
		wpa_s->connect_without_scan = NULL;
#if defined(CONFIG_SME) && defined(CONFIG_SAE)
	if (wpa_s->sme.ext_auth_wpa_ssid == ssid)
		wpa_s->sme.ext_auth_wpa_ssid = NULL;
#endif /* CONFIG_SME && CONFIG_SAE */
	if (wpa_s->wpa) {
		if ((wpa_key_mgmt_sae(ssid->key_mgmt) &&
		     (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_SAE_OFFLOAD_STA)) ||
		    ((ssid->key_mgmt & WPA_KEY_MGMT_OWE) &&
		     (wpa_s->drv_flags2 & WPA_DRIVER_FLAGS2_OWE_OFFLOAD_STA))) {
			/* For cases when PMK is generated at the driver */
			struct wpa_pmkid_params params;

			os_memset(&params, 0, sizeof(params));
			params.ssid = ssid->ssid;
			params.ssid_len = ssid->ssid_len;
			wpa_drv_remove_pmkid(wpa_s, &params);
		}
		wpa_sm_pmksa_cache_flush(wpa_s->wpa, ssid);
	}
	if (!ssid->p2p_group && wpa_s->global->p2p_group_formation != wpa_s &&
	    !wpa_s->p2p_mgmt) {
		wpas_dbus_unregister_network(wpa_s, ssid->id);
		wpas_aidl_unregister_network(wpa_s, ssid);
		wpa_msg_ctrl(wpa_s, MSG_INFO, WPA_EVENT_NETWORK_REMOVED "%d",
			     ssid->id);
	}
	if (network_is_persistent_group(ssid))
		wpas_notify_persistent_group_removed(wpa_s, ssid);

	wpas_p2p_network_removed(wpa_s, ssid);
}


void wpas_notify_bss_added(struct wpa_supplicant *wpa_s,
			   u8 bssid[], unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_register_bss(wpa_s, bssid, id);
	wpa_msg_ctrl(wpa_s, MSG_INFO, WPA_EVENT_BSS_ADDED "%u " MACSTR,
		     id, MAC2STR(bssid));
}


void wpas_notify_bss_removed(struct wpa_supplicant *wpa_s,
			     u8 bssid[], unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_unregister_bss(wpa_s, bssid, id);
	wpa_msg_ctrl(wpa_s, MSG_INFO, WPA_EVENT_BSS_REMOVED "%u " MACSTR,
		     id, MAC2STR(bssid));
}


void wpas_notify_bss_freq_changed(struct wpa_supplicant *wpa_s,
				  unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_FREQ, id);
}


void wpas_notify_bss_signal_changed(struct wpa_supplicant *wpa_s,
				    unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_SIGNAL,
					  id);
}


void wpas_notify_bss_privacy_changed(struct wpa_supplicant *wpa_s,
				     unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_PRIVACY,
					  id);
}


void wpas_notify_bss_mode_changed(struct wpa_supplicant *wpa_s,
				  unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_MODE, id);
}


void wpas_notify_bss_wpaie_changed(struct wpa_supplicant *wpa_s,
				   unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_WPA, id);
}


void wpas_notify_bss_rsnie_changed(struct wpa_supplicant *wpa_s,
				   unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_RSN, id);
}


void wpas_notify_bss_wps_changed(struct wpa_supplicant *wpa_s,
				 unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

#ifdef CONFIG_WPS
	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_WPS, id);
#endif /* CONFIG_WPS */
}


void wpas_notify_bss_ies_changed(struct wpa_supplicant *wpa_s,
				   unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_IES, id);
}


void wpas_notify_bss_rates_changed(struct wpa_supplicant *wpa_s,
				   unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_RATES, id);
}


void wpas_notify_bss_seen(struct wpa_supplicant *wpa_s, unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_AGE, id);
}


void wpas_notify_bss_anqp_changed(struct wpa_supplicant *wpa_s, unsigned int id)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_bss_signal_prop_changed(wpa_s, WPAS_DBUS_BSS_PROP_ANQP, id);
}


void wpas_notify_blob_added(struct wpa_supplicant *wpa_s, const char *name)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_blob_added(wpa_s, name);
}


void wpas_notify_blob_removed(struct wpa_supplicant *wpa_s, const char *name)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_blob_removed(wpa_s, name);
}


void wpas_notify_debug_level_changed(struct wpa_global *global)
{
	wpas_dbus_signal_debug_level_changed(global);
}


void wpas_notify_debug_timestamp_changed(struct wpa_global *global)
{
	wpas_dbus_signal_debug_timestamp_changed(global);
}


void wpas_notify_debug_show_keys_changed(struct wpa_global *global)
{
	wpas_dbus_signal_debug_show_keys_changed(global);
}


void wpas_notify_suspend(struct wpa_global *global)
{
	struct wpa_supplicant *wpa_s;

	os_get_time(&global->suspend_time);
	wpa_printf(MSG_DEBUG, "System suspend notification");
	for (wpa_s = global->ifaces; wpa_s; wpa_s = wpa_s->next)
		wpa_drv_suspend(wpa_s);
}


void wpas_notify_resume(struct wpa_global *global)
{
	struct os_time now;
	int slept;
	struct wpa_supplicant *wpa_s;

	if (global->suspend_time.sec == 0)
		slept = -1;
	else {
		os_get_time(&now);
		slept = now.sec - global->suspend_time.sec;
	}
	wpa_printf(MSG_DEBUG, "System resume notification (slept %d seconds)",
		   slept);

	for (wpa_s = global->ifaces; wpa_s; wpa_s = wpa_s->next) {
		wpa_drv_resume(wpa_s);
		if (wpa_s->wpa_state == WPA_DISCONNECTED)
			wpa_supplicant_req_scan(wpa_s, 0, 100000);
	}
}


#ifdef CONFIG_P2P

void wpas_notify_p2p_find_stopped(struct wpa_supplicant *wpa_s)
{
	/* Notify P2P find has stopped */
	wpas_dbus_signal_p2p_find_stopped(wpa_s);

	wpas_aidl_notify_p2p_find_stopped(wpa_s);
}


void wpas_notify_p2p_device_found(struct wpa_supplicant *wpa_s,
				  const u8 *addr, const struct p2p_peer_info *info,
				  const u8* peer_wfd_device_info, u8 peer_wfd_device_info_len,
				  const u8* peer_wfd_r2_device_info, u8 peer_wfd_r2_device_info_len,
				  int new_device)
{
	if (new_device) {
		/* Create the new peer object */
		wpas_dbus_register_peer(wpa_s, info->p2p_device_addr);
	}

	/* Notify a new peer has been detected*/
	wpas_dbus_signal_peer_device_found(wpa_s, info->p2p_device_addr);

	wpas_aidl_notify_p2p_device_found(wpa_s, addr, info,
					  peer_wfd_device_info,
					  peer_wfd_device_info_len,
					  peer_wfd_r2_device_info,
					  peer_wfd_r2_device_info_len);
}


void wpas_notify_p2p_device_lost(struct wpa_supplicant *wpa_s,
				 const u8 *dev_addr)
{
	wpas_dbus_unregister_peer(wpa_s, dev_addr);

	/* Create signal on interface object*/
	wpas_dbus_signal_peer_device_lost(wpa_s, dev_addr);

	wpas_aidl_notify_p2p_device_lost(wpa_s, dev_addr);
}


void wpas_notify_p2p_group_removed(struct wpa_supplicant *wpa_s,
				   const struct wpa_ssid *ssid,
				   const char *role)
{
	wpas_dbus_signal_p2p_group_removed(wpa_s, role);

	wpas_dbus_unregister_p2p_group(wpa_s, ssid);

	wpas_aidl_notify_p2p_group_removed(wpa_s, ssid, role);
}


void wpas_notify_p2p_go_neg_req(struct wpa_supplicant *wpa_s,
				const u8 *src, u16 dev_passwd_id, u8 go_intent)
{
	wpas_dbus_signal_p2p_go_neg_req(wpa_s, src, dev_passwd_id, go_intent);

	wpas_aidl_notify_p2p_go_neg_req(wpa_s, src, dev_passwd_id, go_intent);
}


void wpas_notify_p2p_go_neg_completed(struct wpa_supplicant *wpa_s,
				      struct p2p_go_neg_results *res)
{
	wpas_dbus_signal_p2p_go_neg_resp(wpa_s, res);

	wpas_aidl_notify_p2p_go_neg_completed(wpa_s, res);
}


void wpas_notify_p2p_invitation_result(struct wpa_supplicant *wpa_s,
				       int status, const u8 *bssid)
{
	wpas_dbus_signal_p2p_invitation_result(wpa_s, status, bssid);

	wpas_aidl_notify_p2p_invitation_result(wpa_s, status, bssid);
}


void wpas_notify_p2p_sd_request(struct wpa_supplicant *wpa_s,
				int freq, const u8 *sa, u8 dialog_token,
				u16 update_indic, const u8 *tlvs,
				size_t tlvs_len)
{
	wpas_dbus_signal_p2p_sd_request(wpa_s, freq, sa, dialog_token,
					update_indic, tlvs, tlvs_len);
}


void wpas_notify_p2p_sd_response(struct wpa_supplicant *wpa_s,
				 const u8 *sa, u16 update_indic,
				 const u8 *tlvs, size_t tlvs_len)
{
	wpas_dbus_signal_p2p_sd_response(wpa_s, sa, update_indic,
					 tlvs, tlvs_len);

	wpas_aidl_notify_p2p_sd_response(wpa_s, sa, update_indic,
					 tlvs, tlvs_len);
}


/**
 * wpas_notify_p2p_provision_discovery - Notification of provision discovery
 * @dev_addr: Who sent the request or responded to our request.
 * @request: Will be 1 if request, 0 for response.
 * @status: Valid only in case of response (0 in case of success)
 * @config_methods: WPS config methods
 * @generated_pin: PIN to be displayed in case of WPS_CONFIG_DISPLAY method
 * @group_ifname: Group interface name of the group owner in case the provision
 *                discovery request is received with P2P Group ID attribute.
 *                i.e., valid only when the peer device is joining an
 *                operating P2P group.
 *
 * This can be used to notify:
 * - Requests or responses
 * - Various config methods
 * - Failure condition in case of response
 */
void wpas_notify_p2p_provision_discovery(struct wpa_supplicant *wpa_s,
					 const u8 *dev_addr, int request,
					 enum p2p_prov_disc_status status,
					 u16 config_methods,
					 unsigned int generated_pin,
					 const char *group_ifname)
{
	wpas_dbus_signal_p2p_provision_discovery(wpa_s, dev_addr, request,
						 status, config_methods,
						 generated_pin);

	wpas_aidl_notify_p2p_provision_discovery(wpa_s, dev_addr, request,
						 status, config_methods,
						 generated_pin, group_ifname);

}


void wpas_notify_p2p_group_started(struct wpa_supplicant *wpa_s,
				   struct wpa_ssid *ssid, int persistent,
				   int client, const u8 *ip)
{
	/* Notify a group has been started */
	wpas_dbus_register_p2p_group(wpa_s, ssid);

	wpas_dbus_signal_p2p_group_started(wpa_s, client, persistent, ip);

	wpas_aidl_notify_p2p_group_started(wpa_s, ssid, persistent, client, ip);
}


void wpas_notify_p2p_group_formation_failure(struct wpa_supplicant *wpa_s,
					     const char *reason)
{
	/* Notify a group formation failed */
	wpas_dbus_signal_p2p_group_formation_failure(wpa_s, reason);

	wpas_aidl_notify_p2p_group_formation_failure(wpa_s, reason);
}


void wpas_notify_p2p_wps_failed(struct wpa_supplicant *wpa_s,
				struct wps_event_fail *fail)
{
	wpas_dbus_signal_p2p_wps_failed(wpa_s, fail);
}


void wpas_notify_p2p_invitation_received(struct wpa_supplicant *wpa_s,
					 const u8 *sa, const u8 *go_dev_addr,
					 const u8 *bssid, int id, int op_freq)
{
	/* Notify a P2P Invitation Request */
	wpas_dbus_signal_p2p_invitation_received(wpa_s, sa, go_dev_addr, bssid,
						 id, op_freq);

	wpas_aidl_notify_p2p_invitation_received(wpa_s, sa, go_dev_addr, bssid,
						 id, op_freq);
}

void wpas_notify_p2p_bootstrap_req(struct wpa_supplicant *wpa_s,
				   const u8 *src, u16 bootstrap_method)
{
	wpas_dbus_signal_p2p_bootstrap_req(wpa_s, src, bootstrap_method);
}

void wpas_notify_p2p_bootstrap_completed(struct wpa_supplicant *wpa_s,
					 const u8 *src, int status)
{
	wpas_dbus_signal_p2p_bootstrap_completed(wpa_s, src, status);
}

#endif /* CONFIG_P2P */


static void wpas_notify_ap_sta_authorized(struct wpa_supplicant *wpa_s,
					  const u8 *sta,
					  const u8 *p2p_dev_addr, const u8 *ip)
{
#ifdef CONFIG_P2P
	wpas_p2p_notify_ap_sta_authorized(wpa_s, p2p_dev_addr);

	/*
	 * Create 'peer-joined' signal on group object -- will also
	 * check P2P itself.
	 */
	if (p2p_dev_addr)
		wpas_dbus_signal_p2p_peer_joined(wpa_s, p2p_dev_addr);
#endif /* CONFIG_P2P */

	/* Register the station */
	wpas_dbus_register_sta(wpa_s, sta);

	/* Notify listeners a new station has been authorized */
	wpas_dbus_signal_sta_authorized(wpa_s, sta);

	wpas_aidl_notify_ap_sta_authorized(wpa_s, sta, p2p_dev_addr, ip);
}


static void wpas_notify_ap_sta_deauthorized(struct wpa_supplicant *wpa_s,
					    const u8 *sta,
					    const u8 *p2p_dev_addr)
{
#ifdef CONFIG_P2P
	/*
	 * Create 'peer-disconnected' signal on group object if this
	 * is a P2P group.
	 */
	if (p2p_dev_addr)
		wpas_dbus_signal_p2p_peer_disconnected(wpa_s, p2p_dev_addr);
#endif /* CONFIG_P2P */

	/* Notify listeners a station has been deauthorized */
	wpas_dbus_signal_sta_deauthorized(wpa_s, sta);

	wpas_aidl_notify_ap_sta_deauthorized(wpa_s, sta, p2p_dev_addr);
	/* Unregister the station */
	wpas_dbus_unregister_sta(wpa_s, sta);
}


void wpas_notify_sta_authorized(struct wpa_supplicant *wpa_s,
				const u8 *mac_addr, int authorized,
				const u8 *p2p_dev_addr, const u8 *ip)
{
	if (authorized)
		wpas_notify_ap_sta_authorized(wpa_s, mac_addr, p2p_dev_addr,
					      ip);
	else
		wpas_notify_ap_sta_deauthorized(wpa_s, mac_addr, p2p_dev_addr);
}


void wpas_notify_certification(struct wpa_supplicant *wpa_s,
			       struct tls_cert_data *cert,
			       const char *cert_hash)
{
	int i;

	wpa_msg(wpa_s, MSG_INFO, WPA_EVENT_EAP_PEER_CERT
		"depth=%d subject='%s'%s%s%s%s",
		cert->depth, cert->subject, cert_hash ? " hash=" : "",
		cert_hash ? cert_hash : "",
		cert->tod == 2 ? " tod=2" : "",
		cert->tod == 1 ? " tod=1" : "");

	if (cert->cert) {
		char *cert_hex;
		size_t len = wpabuf_len(cert->cert) * 2 + 1;
		cert_hex = os_malloc(len);
		if (cert_hex) {
			wpa_snprintf_hex(cert_hex, len, wpabuf_head(cert->cert),
					 wpabuf_len(cert->cert));
			wpa_msg_ctrl(wpa_s, MSG_INFO,
				     WPA_EVENT_EAP_PEER_CERT
				     "depth=%d subject='%s' cert=%s",
				     cert->depth, cert->subject, cert_hex);
			os_free(cert_hex);
		}
	}

	for (i = 0; i < cert->num_altsubject; i++)
		wpa_msg(wpa_s, MSG_INFO, WPA_EVENT_EAP_PEER_ALT
			"depth=%d %s", cert->depth, cert->altsubject[i]);

	wpas_aidl_notify_ceritification(wpa_s, cert->depth, cert->subject,
				       cert->altsubject, cert->num_altsubject,
				       cert_hash, cert->cert);

	/* notify the new DBus API */
	wpas_dbus_signal_certification(wpa_s, cert->depth, cert->subject,
				       cert->altsubject, cert->num_altsubject,
				       cert_hash, cert->cert);
}


void wpas_notify_preq(struct wpa_supplicant *wpa_s,
		      const u8 *addr, const u8 *dst, const u8 *bssid,
		      const u8 *ie, size_t ie_len, u32 ssi_signal)
{
#ifdef CONFIG_AP
	wpas_dbus_signal_preq(wpa_s, addr, dst, bssid, ie, ie_len, ssi_signal);
#endif /* CONFIG_AP */
}


void wpas_notify_eap_status(struct wpa_supplicant *wpa_s, const char *status,
			    const char *parameter)
{
	wpas_dbus_signal_eap_status(wpa_s, status, parameter);
	wpa_msg_ctrl(wpa_s, MSG_INFO, WPA_EVENT_EAP_STATUS
		     "status='%s' parameter='%s'",
		     status, parameter);
}


void wpas_notify_eap_error(struct wpa_supplicant *wpa_s, int error_code)
{
	wpa_dbg(wpa_s, MSG_ERROR,
		"EAP Error code = %d", error_code);
	wpas_aidl_notify_eap_error(wpa_s, error_code);
}


void wpas_notify_psk_mismatch(struct wpa_supplicant *wpa_s)
{
	wpas_dbus_signal_psk_mismatch(wpa_s);
}


void wpas_notify_network_bssid_set_changed(struct wpa_supplicant *wpa_s,
					   struct wpa_ssid *ssid)
{
	if (wpa_s->current_ssid != ssid)
		return;

	wpa_dbg(wpa_s, MSG_DEBUG,
		"Network bssid config changed for the current network - within-ESS roaming %s",
		ssid->bssid_set ? "disabled" : "enabled");

	wpa_drv_roaming(wpa_s, !ssid->bssid_set,
			ssid->bssid_set ? ssid->bssid : NULL);
}


void wpas_notify_network_type_changed(struct wpa_supplicant *wpa_s,
				      struct wpa_ssid *ssid)
{
#ifdef CONFIG_P2P
	if (ssid->disabled == 2) {
		/* Changed from normal network profile to persistent group */
		ssid->disabled = 0;
		wpas_dbus_unregister_network(wpa_s, ssid->id);
		ssid->disabled = 2;
		ssid->p2p_persistent_group = 1;
		wpas_dbus_register_persistent_group(wpa_s, ssid);
	} else {
		/* Changed from persistent group to normal network profile */
		wpas_dbus_unregister_persistent_group(wpa_s, ssid->id);
		ssid->p2p_persistent_group = 0;
		wpas_dbus_register_network(wpa_s, ssid);
	}
#endif /* CONFIG_P2P */
}

void wpas_notify_anqp_query_done(struct wpa_supplicant *wpa_s, const u8* bssid,
				 const char *result,
				 const struct wpa_bss_anqp *anqp)
{
	wpa_msg(wpa_s, MSG_INFO, ANQP_QUERY_DONE "addr=" MACSTR " result=%s",
		MAC2STR(bssid), result);
#ifdef CONFIG_INTERWORKING
	if (!wpa_s || !bssid || !anqp)
		return;

	wpas_aidl_notify_anqp_query_done(wpa_s, bssid, result, anqp);
	wpas_dbus_signal_anqp_query_done(wpa_s, bssid, result);
#endif /* CONFIG_INTERWORKING */
}

void wpas_notify_hs20_icon_query_done(struct wpa_supplicant *wpa_s, const u8* bssid,
				      const char* file_name, const u8* image,
				      u32 image_length)
{
#ifdef CONFIG_HS20
	if (!wpa_s || !bssid || !file_name || !image)
		return;

	wpas_aidl_notify_hs20_icon_query_done(wpa_s, bssid, file_name, image,
					      image_length);
#endif /* CONFIG_HS20 */
}

void wpas_notify_hs20_rx_subscription_remediation(struct wpa_supplicant *wpa_s,
						  const char* url,
						  u8 osu_method)
{
#ifdef CONFIG_HS20
	if (!wpa_s || !url)
		return;

	wpas_aidl_notify_hs20_rx_subscription_remediation(wpa_s, url, osu_method);
#endif /* CONFIG_HS20 */
}

void wpas_notify_hs20_rx_deauth_imminent_notice(struct wpa_supplicant *wpa_s,
						u8 code, u16 reauth_delay,
						const char *url)
{
#ifdef CONFIG_HS20
	if (!wpa_s)
		return;

	wpas_aidl_notify_hs20_rx_deauth_imminent_notice(wpa_s, code, reauth_delay,
			url);
#endif /* CONFIG_HS20 */
}



#ifdef CONFIG_NAN_USD

void wpas_notify_nan_discovery_result(struct wpa_supplicant *wpa_s,
				      enum nan_service_protocol_type
				      srv_proto_type,
				      int subscribe_id, int peer_publish_id,
				      const u8 *peer_addr,
				      bool fsd, bool fsd_gas,
				      const u8 *ssi, size_t ssi_len)
{
	char *ssi_hex;

	ssi_hex = os_zalloc(2 * ssi_len + 1);
	if (!ssi_hex)
		return;
	if (ssi)
		wpa_snprintf_hex(ssi_hex, 2 * ssi_len + 1, ssi, ssi_len);
	wpa_msg(wpa_s, MSG_INFO, NAN_DISCOVERY_RESULT
		"subscribe_id=%d publish_id=%d address=" MACSTR
		" fsd=%d fsd_gas=%d srv_proto_type=%u ssi=%s",
		subscribe_id, peer_publish_id, MAC2STR(peer_addr),
		fsd, fsd_gas, srv_proto_type, ssi_hex);
	os_free(ssi_hex);
}


void wpas_notify_nan_replied(struct wpa_supplicant *wpa_s,
			     enum nan_service_protocol_type srv_proto_type,
			     int publish_id, int peer_subscribe_id,
			     const u8 *peer_addr,
			     const u8 *ssi, size_t ssi_len)
{
	char *ssi_hex;

	ssi_hex = os_zalloc(2 * ssi_len + 1);
	if (!ssi_hex)
		return;
	if (ssi)
		wpa_snprintf_hex(ssi_hex, 2 * ssi_len + 1, ssi, ssi_len);
	wpa_msg(wpa_s, MSG_INFO, NAN_REPLIED
		"publish_id=%d address=" MACSTR
		" subscribe_id=%d srv_proto_type=%u ssi=%s",
		publish_id, MAC2STR(peer_addr), peer_subscribe_id,
		srv_proto_type, ssi_hex);
	os_free(ssi_hex);
}


void wpas_notify_nan_receive(struct wpa_supplicant *wpa_s, int id,
			     int peer_instance_id, const u8 *peer_addr,
			     const u8 *ssi, size_t ssi_len)
{
	char *ssi_hex;

	ssi_hex = os_zalloc(2 * ssi_len + 1);
	if (!ssi_hex)
		return;
	if (ssi)
		wpa_snprintf_hex(ssi_hex, 2 * ssi_len + 1, ssi, ssi_len);
	wpa_msg(wpa_s, MSG_INFO, NAN_RECEIVE
		"id=%d peer_instance_id=%d address=" MACSTR " ssi=%s",
		id, peer_instance_id, MAC2STR(peer_addr), ssi_hex);
	os_free(ssi_hex);
}


static const char * nan_reason_txt(enum nan_de_reason reason)
{
	switch (reason) {
	case NAN_DE_REASON_TIMEOUT:
		return "timeout";
	case NAN_DE_REASON_USER_REQUEST:
		return "user-request";
	case NAN_DE_REASON_FAILURE:
		return "failure";
	}

	return "unknown";
}


void wpas_notify_nan_publish_terminated(struct wpa_supplicant *wpa_s,
					int publish_id,
					enum nan_de_reason reason)
{
	wpa_msg(wpa_s, MSG_INFO, NAN_PUBLISH_TERMINATED
		"publish_id=%d reason=%s",
		publish_id, nan_reason_txt(reason));
}


void wpas_notify_nan_subscribe_terminated(struct wpa_supplicant *wpa_s,
					  int subscribe_id,
					  enum nan_de_reason reason)
{
	wpa_msg(wpa_s, MSG_INFO, NAN_SUBSCRIBE_TERMINATED
		"subscribe_id=%d reason=%s",
		subscribe_id, nan_reason_txt(reason));
}

#endif /* CONFIG_NAN_USD */

#ifdef CONFIG_MESH

void wpas_notify_mesh_group_started(struct wpa_supplicant *wpa_s,
				    struct wpa_ssid *ssid)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_mesh_group_started(wpa_s, ssid);
}


void wpas_notify_mesh_group_removed(struct wpa_supplicant *wpa_s,
				    const u8 *meshid, u8 meshid_len,
				    u16 reason_code)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpas_dbus_signal_mesh_group_removed(wpa_s, meshid, meshid_len,
					    reason_code);
}


void wpas_notify_mesh_peer_connected(struct wpa_supplicant *wpa_s,
				     const u8 *peer_addr)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpa_msg(wpa_s, MSG_INFO, MESH_PEER_CONNECTED MACSTR,
		MAC2STR(peer_addr));
	wpas_dbus_signal_mesh_peer_connected(wpa_s, peer_addr);
}


void wpas_notify_mesh_peer_disconnected(struct wpa_supplicant *wpa_s,
					const u8 *peer_addr, u16 reason_code)
{
	if (wpa_s->p2p_mgmt)
		return;

	wpa_msg(wpa_s, MSG_INFO, MESH_PEER_DISCONNECTED MACSTR,
		MAC2STR(peer_addr));
	wpas_dbus_signal_mesh_peer_disconnected(wpa_s, peer_addr, reason_code);
}

#endif /* CONFIG_MESH */

/*
 * DPP Notifications
 */

/* DPP Success notifications */

void wpas_notify_dpp_config_received(struct wpa_supplicant *wpa_s,
	    struct wpa_ssid *ssid, bool conn_status_requested)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_config_received(wpa_s, ssid, conn_status_requested);
#endif /* CONFIG_DPP */
}

void wpas_notify_dpp_config_sent(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_config_sent(wpa_s);
#endif /* CONFIG_DPP */
}

void wpas_notify_dpp_connection_status_sent(struct wpa_supplicant *wpa_s,
	    enum dpp_status_error result)
{
#ifdef CONFIG_DPP2
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_connection_status_sent(wpa_s, result);
#endif /* CONFIG_DPP2 */
}

/* DPP Progress notifications */
void wpas_notify_dpp_auth_success(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_auth_success(wpa_s);
#endif /* CONFIG_DPP */
}

void wpas_notify_dpp_resp_pending(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_resp_pending(wpa_s);
#endif /* CONFIG_DPP */
}

/* DPP Failure notifications */
void wpas_notify_dpp_not_compatible(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_not_compatible(wpa_s);
#endif /* CONFIG_DPP */
}

void wpas_notify_dpp_missing_auth(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_missing_auth(wpa_s);
#endif /* CONFIG_DPP */
}

void wpas_notify_dpp_configuration_failure(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_configuration_failure(wpa_s);
#endif /* CONFIG_DPP */
}

void wpas_notify_dpp_timeout(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_timeout(wpa_s);
#endif /* CONFIG_DPP */
}

void wpas_notify_dpp_auth_failure(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_auth_failure(wpa_s);
#endif /* CONFIG_DPP */
}

void wpas_notify_dpp_failure(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP
	if (!wpa_s)
		return;

	wpas_aidl_notify_dpp_fail(wpa_s);
#endif /* CONFIG_DPP */
}

void wpas_notify_dpp_config_sent_wait_response(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP2
	wpas_aidl_notify_dpp_config_sent_wait_response(wpa_s);
#endif /* CONFIG_DPP2 */
}

void wpas_notify_dpp_config_accepted(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP2
	wpas_aidl_notify_dpp_config_accepted(wpa_s);
#endif /* CONFIG_DPP2 */
}

void wpas_notify_dpp_conn_status(struct wpa_supplicant *wpa_s,
		enum dpp_status_error status, const char *ssid,
		const char *channel_list, unsigned short band_list[], int size)
{
#ifdef CONFIG_DPP2
	wpas_aidl_notify_dpp_conn_status(wpa_s, status, ssid, channel_list, band_list, size);
#endif /* CONFIG_DPP2 */
}

void wpas_notify_dpp_config_rejected(struct wpa_supplicant *wpa_s)
{
#ifdef CONFIG_DPP2
	wpas_aidl_notify_dpp_config_rejected(wpa_s);
#endif /* CONFIG_DPP2 */
}

void wpas_notify_pmk_cache_added(struct wpa_supplicant *wpa_s,
				 struct rsn_pmksa_cache_entry *entry)
{
	if (!wpa_s)
		return;

	wpas_aidl_notify_pmk_cache_added(wpa_s, entry);
}

void wpas_notify_transition_disable(struct wpa_supplicant *wpa_s,
				    struct wpa_ssid *ssid,
				    u8 bitmap)
{
	if (!wpa_s)
		return;

	if (!ssid)
		return;

	wpas_aidl_notify_transition_disable(wpa_s, ssid, bitmap);
}

void wpas_notify_network_not_found(struct wpa_supplicant *wpa_s)
{
	if (!wpa_s)
		return;

	wpas_aidl_notify_network_not_found(wpa_s);
}

#ifdef CONFIG_INTERWORKING

void wpas_notify_interworking_ap_added(struct wpa_supplicant *wpa_s,
				       struct wpa_bss *bss,
				       struct wpa_cred *cred, int excluded,
				       const char *type, int bh, int bss_load,
				       int conn_capab)
{
	wpa_msg(wpa_s, MSG_INFO, "%s" MACSTR " type=%s%s%s%s id=%d priority=%d sp_priority=%d",
		excluded ? INTERWORKING_EXCLUDED : INTERWORKING_AP,
		MAC2STR(bss->bssid), type,
		bh ? " below_min_backhaul=1" : "",
		bss_load ? " over_max_bss_load=1" : "",
		conn_capab ? " conn_capab_missing=1" : "",
		cred->id, cred->priority, cred->sp_priority);

	wpas_dbus_signal_interworking_ap_added(wpa_s, bss, cred, type, excluded,
					       bh, bss_load, conn_capab);
}


void wpas_notify_interworking_select_done(struct wpa_supplicant *wpa_s)
{
	wpas_dbus_signal_interworking_select_done(wpa_s);
}


#endif /* CONFIG_INTERWORKING */

void wpas_notify_eap_method_selected(struct wpa_supplicant *wpa_s,
			const char* reason_string)
{
	wpas_aidl_notify_eap_method_selected(wpa_s, reason_string);
}

void wpas_notify_ssid_temp_disabled(struct wpa_supplicant *wpa_s,
			const char *reason_string)
{
	wpas_aidl_notify_ssid_temp_disabled(wpa_s, reason_string);
}

void wpas_notify_open_ssl_failure(struct wpa_supplicant *wpa_s,
			const char *reason_string)
{
	wpas_aidl_notify_open_ssl_failure(wpa_s, reason_string);
}

void wpas_notify_qos_policy_reset(struct wpa_supplicant *wpa_s)
{
	if (!wpa_s)
		return;

	wpas_aidl_notify_qos_policy_reset(wpa_s);
}

void wpas_notify_qos_policy_request(struct wpa_supplicant *wpa_s,
	struct dscp_policy_data *policies, int num_policies)
{
	if (!wpa_s || !policies)
		return;

	wpas_aidl_notify_qos_policy_request(wpa_s, policies, num_policies);
}

void wpas_notify_frequency_changed(struct wpa_supplicant *wpa_s, int frequency)
{
	if (!wpa_s)
		return;

	wpas_aidl_notify_frequency_changed(wpa_s, frequency);
}

ssize_t wpas_get_certificate(const char *alias, uint8_t** value)
{
	wpa_printf(MSG_INFO, "wpas_get_certificate");
	return wpas_aidl_get_certificate(alias, value);
}

ssize_t wpas_list_aliases(const char *prefix, char ***aliases)
{
	return wpas_aidl_list_aliases(prefix, aliases);
}

void wpas_notify_signal_change(struct wpa_supplicant *wpa_s)
{
	wpas_dbus_signal_prop_changed(wpa_s, WPAS_DBUS_PROP_SIGNAL_CHANGE);
}

void wpas_notify_qos_policy_scs_response(struct wpa_supplicant *wpa_s,
		unsigned int num_scs_resp, int **scs_resp)
{
	if (!wpa_s || !num_scs_resp || !scs_resp)
		return;

	wpas_aidl_notify_qos_policy_scs_response(wpa_s, num_scs_resp, scs_resp);
}

void wpas_notify_hs20_t_c_acceptance(struct wpa_supplicant *wpa_s,
				     const char *url)
{
#ifdef CONFIG_HS20
	if (!wpa_s || !url)
		return;

	wpa_msg(wpa_s, MSG_INFO, HS20_T_C_ACCEPTANCE "%s", url);
	wpas_aidl_notify_hs20_rx_terms_and_conditions_acceptance(wpa_s, url);
	wpas_dbus_signal_hs20_t_c_acceptance(wpa_s, url);
#endif /* CONFIG_HS20 */
}
