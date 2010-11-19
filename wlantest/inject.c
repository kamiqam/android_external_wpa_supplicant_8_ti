/*
 * wlantest frame injection
 * Copyright (c) 2010, Jouni Malinen <j@w1.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include "utils/includes.h"

#include "utils/common.h"
#include "common/defs.h"
#include "common/ieee802_11_defs.h"
#include "wlantest.h"


static int inject_frame(int s, const void *data, size_t len)
{
#define	IEEE80211_RADIOTAP_F_FRAG	0x08
	unsigned char rtap_hdr[] = {
		0x00, 0x00, /* radiotap version */
		0x0e, 0x00, /* radiotap length */
		0x02, 0xc0, 0x00, 0x00, /* bmap: flags, tx and rx flags */
		IEEE80211_RADIOTAP_F_FRAG, /* F_FRAG (fragment if required) */
		0x00,       /* padding */
		0x00, 0x00, /* RX and TX flags to indicate that */
		0x00, 0x00, /* this is the injected frame directly */
	};
	struct iovec iov[2] = {
		{
			.iov_base = &rtap_hdr,
			.iov_len = sizeof(rtap_hdr),
		},
		{
			.iov_base = (void *) data,
			.iov_len = len,
		}
	};
	struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = iov,
		.msg_iovlen = 2,
		.msg_control = NULL,
		.msg_controllen = 0,
		.msg_flags = 0,
	};
	int ret;

	ret = sendmsg(s, &msg, 0);
	if (ret < 0)
		perror("sendmsg");
	return ret;
}


static int is_robust_mgmt(u8 *frame, size_t len)
{
	struct ieee80211_mgmt *mgmt;
	u16 fc, stype;
	if (len < 24)
		return 0;
	mgmt = (struct ieee80211_mgmt *) frame;
	fc = le_to_host16(mgmt->frame_control);
	if (WLAN_FC_GET_TYPE(fc) != WLAN_FC_TYPE_MGMT)
		return 0;
	stype = WLAN_FC_GET_STYPE(fc);
	if (stype == WLAN_FC_STYPE_DEAUTH || stype == WLAN_FC_STYPE_DISASSOC)
		return 1;
	if (stype == WLAN_FC_STYPE_ACTION) {
		if (len < 25)
			return 0;
		if (mgmt->u.action.category != WLAN_ACTION_PUBLIC)
			return 1;
	}
	return 0;
}


static int wlantest_inject_prot(struct wlantest *wt, struct wlantest_bss *bss,
				struct wlantest_sta *sta, u8 *frame,
				size_t len, int incorrect_key)
{
	u8 *crypt;
	size_t crypt_len;
	int ret;
	u8 dummy[64];
	u8 *pn;
	struct ieee80211_hdr *hdr;
	u16 fc;
	int tid = 0;
	u8 *qos = NULL;
	int hdrlen;

	if (sta == NULL)
		return -1; /* TODO: add support for group Data and BIP */

	if (!sta->ptk_set)
		return -1;

	hdr = (struct ieee80211_hdr *) frame;
	hdrlen = 24;
	fc = le_to_host16(hdr->frame_control);
	if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT)
		tid = 16;
	else if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_DATA) {
		if ((fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) ==
		    (WLAN_FC_TODS | WLAN_FC_FROMDS))
			hdrlen += ETH_ALEN;
		if (WLAN_FC_GET_STYPE(fc) & 0x08) {
			qos = frame + hdrlen;
			hdrlen += 2;
			tid = qos[0] & 0x0f;
		}
	}
	if (os_memcmp(hdr->addr2, bss->bssid, ETH_ALEN) == 0)
		pn = sta->rsc_fromds[tid];
	else
		pn = sta->rsc_tods[tid];
	inc_byte_array(pn, 6);

	os_memset(dummy, 0x11, sizeof(dummy));
	if (sta->pairwise_cipher == WPA_CIPHER_TKIP)
		crypt = tkip_encrypt(incorrect_key ? dummy : sta->ptk.tk1,
				     frame, len, hdrlen, qos, pn, 0,
				     &crypt_len);
	else
		crypt = ccmp_encrypt(incorrect_key ? dummy : sta->ptk.tk1,
				     frame, len, hdrlen, qos, pn, 0,
				     &crypt_len);

	if (crypt == NULL)
		return -1;

	ret = inject_frame(wt->monitor_sock, crypt, crypt_len);
	os_free(crypt);

	return (ret < 0) ? -1 : 0;
}


int wlantest_inject(struct wlantest *wt, struct wlantest_bss *bss,
		    struct wlantest_sta *sta, u8 *frame, size_t len,
		    enum wlantest_inject_protection prot)
{
	int ret;
	struct ieee80211_hdr *hdr;
	u16 fc;
	int protectable, protect = 0;

	wpa_hexdump(MSG_DEBUG, "Inject frame", frame, len);
	if (wt->monitor_sock < 0) {
		wpa_printf(MSG_INFO, "Cannot inject frames when monitor "
			   "interface is not in use");
		return -1;
	}

	hdr = (struct ieee80211_hdr *) frame;
	fc = le_to_host16(hdr->frame_control);
	protectable = WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_DATA ||
		is_robust_mgmt(frame, len);

	if (prot == WLANTEST_INJECT_PROTECTED ||
	    prot == WLANTEST_INJECT_INCORRECT_KEY) {
		if (!sta) {
			wpa_printf(MSG_INFO, "Broadcast protection not yet "
				   "implemented");
			return -1;
		}
		if (sta && !sta->ptk_set) {
			wpa_printf(MSG_INFO, "No PTK known for the STA " MACSTR
				   " to encrypt the injected frame",
				   MAC2STR(sta->addr));
			return -1;
		}
		protect = 1;
	} else if (protectable && prot != WLANTEST_INJECT_UNPROTECTED) {
		if (sta && sta->ptk_set)
			protect = 1;
		else if (!sta) {
			if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_DATA &&
			    (bss->gtk_len[1] || bss->gtk_len[2]))
				protect = 1;
			if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT &&
			    (bss->igtk_set[4] || bss->igtk_set[5]))
				protect = 1;
		}
	}

	if ((prot == WLANTEST_INJECT_PROTECTED ||
	     prot == WLANTEST_INJECT_INCORRECT_KEY) && !protect) {
		wpa_printf(MSG_INFO, "Cannot protect injected frame");
		return -1;
	}

	if (protect)
		return wlantest_inject_prot(
			wt, bss, sta, frame, len,
			prot == WLANTEST_INJECT_INCORRECT_KEY);

	ret = inject_frame(wt->monitor_sock, frame, len);
	return (ret < 0) ? -1 : 0;
}