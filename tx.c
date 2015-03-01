/*
 * Copyright (C) 2014 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2015 Jakub Kicinski <kubakici@wp.pl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "mt7601u.h"
#include "dma.h" /* TODO: take the dma code out of here! */
#include "trace.h"

enum mt76_txq_id { /* TODO: is this mapping correct? */
	MT_TXQ_VO = IEEE80211_AC_VO,
	MT_TXQ_VI = IEEE80211_AC_VI,
	MT_TXQ_BE = IEEE80211_AC_BE,
	MT_TXQ_BK = IEEE80211_AC_BK,
	MT_TXQ_PSD,
	MT_TXQ_MCU,
	__MT_TXQ_MAX
};

static u8 mt7601u_tx_pktid_enc(struct mt7601u_dev *dev, u8 rate, bool is_probe)
{
	u8 encoded = (rate + 1) + is_probe *  8;

	/* Because PKT_ID 0 disables status reporting only 15 values are
	 * available but 16 are needed (8 MCS * 2 for encoding is_probe)
	 * - we need to cram together two rates. MCS0 and MCS7 with is_probe
	 * share PKT_ID 9.
	 */
	if (is_probe && rate == 7)
		return encoded - 7;

	return encoded;
}

static void mt7601u_tx_skb_remove_dma_overhead(struct sk_buff *skb,
					       struct ieee80211_tx_info *info)
{
	int pkt_len = (unsigned long)info->status.status_driver_data[0];

	skb_pull(skb, sizeof(struct mt7601u_txwi) + 4);
	if (ieee80211_get_hdrlen_from_skb(skb) % 4)
		mt76_remove_hdr_pad(skb);

	skb_trim(skb, pkt_len);
}

void mt7601u_tx_status(struct mt7601u_dev *dev, struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);

	mt7601u_tx_skb_remove_dma_overhead(skb, info);

	ieee80211_tx_info_clear_status(info);
	info->status.rates[0].idx = -1;
	info->flags |= IEEE80211_TX_STAT_ACK;
	ieee80211_tx_status(dev->hw, skb);
}

static int mt7601u_skb_rooms(struct mt7601u_dev *dev, struct sk_buff *skb)
{
	int hdr_len = ieee80211_get_hdrlen_from_skb(skb);
	u32 need_head, need_tail;
	int ret;

	need_head = sizeof(struct mt7601u_txwi) + 4;
	need_tail = 3 + 4 + 4; /* TODO: replace 3 with round-up. */

	if (hdr_len % 4)
		need_head += 2;

	if (skb_headroom(skb) < need_head && dev->n_cows++ > 100)
		printk("Warning: TX skb needs more head - will COW!\n");
	if (skb_tailroom(skb) < need_tail) {
		printk("Error: TX skb needs more tail - fail!!\n");
		return -ENOMEM;
	}

	ret = skb_cow(skb, need_head);
	if (ret) {
		printk("Failed to get the headroom\n");
		return ret;
	}

	return 0;
}

static u8 q2hwq(u8 q)
{
	return q ^ 0x3;
}

/* mac80211 qid to hardware queue idx */
static u8 skb2q(struct sk_buff *skb)
{
	int qid = skb_get_queue_mapping(skb);

	if (WARN_ON(qid >= MT_TXQ_PSD)) {
		qid = MT_TXQ_BE;
		skb_set_queue_mapping(skb, qid);
	}

	return q2hwq(qid);
}

static u8 q2ep(u8 qid)
{
	/* TODO: we will not get mgmt in separate q... */
	return qid + 1;
}

static enum mt76_qsel ep2dmaq(u8 ep)
{
	if (ep == 5)
		return MT_QSEL_MGMT;
	return MT_QSEL_EDCA;
}

void mt7601u_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
		struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_tx_rate *rate;
	struct mt7601u_dev *dev = hw->priv;
	struct ieee80211_vif *vif = info->control.vif;
	struct ieee80211_sta *sta = control->sta;
	struct mt76_sta *msta = NULL;
	struct mt76_wcid *wcid = dev->mon_wcid;
	struct mt7601u_txwi *txwi;
	int pkt_len = skb->len;
	int hw_q = skb2q(skb);
	u32 dma_flags, pkt_id;
	u16 rate_ctl;
	u8 ep = q2ep(hw_q);
	unsigned long flags;
	bool is_probe;

	BUILD_BUG_ON(ARRAY_SIZE(info->status.status_driver_data) < 1);
	info->status.status_driver_data[0] = (void *)(unsigned long)pkt_len;

	/* TODO: should pkt_len include hdr_pad? */
	if (mt7601u_skb_rooms(dev, skb) || mt76_insert_hdr_pad(skb)) {
		ieee80211_free_txskb(dev->hw, skb);
		return;
	}

	if (sta) {
		msta = (struct mt76_sta *) sta->drv_priv;
		wcid = &msta->wcid;
	} else if (vif) {
		struct mt76_vif *mvif = (struct mt76_vif *)vif->drv_priv;
		wcid = &mvif->group_wcid;
	}

	if (!wcid->tx_rate_set)
		ieee80211_get_tx_rates(info->control.vif, control->sta, skb,
				       info->control.rates, 1);
	rate = &info->control.rates[0];

	/* TODO: this is unaligned */
	txwi = (struct mt7601u_txwi *)skb_push(skb, sizeof(struct mt7601u_txwi));
	memset(txwi, 0, sizeof(*txwi));

	spin_lock_irqsave(&dev->lock, flags);
	if (rate->idx < 0 || !rate->count)
		rate_ctl = wcid->tx_rate;
	else
		rate_ctl = mt76_mac_tx_rate_val(dev, rate);
	spin_unlock_irqrestore(&dev->lock, flags);
	txwi->rate_ctl = cpu_to_le16(rate_ctl);

	if (!(info->flags & IEEE80211_TX_CTL_NO_ACK))
		txwi->ack_ctl |= MT_TXWI_ACK_CTL_REQ;
	if (info->flags & IEEE80211_TX_CTL_ASSIGN_SEQ)
		txwi->ack_ctl |= MT_TXWI_ACK_CTL_NSEQ;
	txwi->ack_ctl |= MT76_SET(MT_TXWI_ACK_CTL_BA_WINDOW, 7);
	if ((info->flags & IEEE80211_TX_CTL_AMPDU) && sta) {
		u8 ba_size = IEEE80211_MIN_AMPDU_BUF;
		ba_size <<= sta->ht_cap.ampdu_factor;
		ba_size = min_t(int, 63, ba_size);
		if (info->flags & IEEE80211_TX_CTL_RATE_CTRL_PROBE)
			ba_size = 0;
		txwi->ack_ctl |= MT76_SET(MT_TXWI_ACK_CTL_BA_WINDOW, ba_size);

		txwi->flags = cpu_to_le16(MT_TXWI_FLAGS_AMPDU |
					  MT76_SET(MT_TXWI_FLAGS_MPDU_DENSITY,
						   sta->ht_cap.ampdu_density));
		if (info->flags & IEEE80211_TX_CTL_RATE_CTRL_PROBE)
			txwi->flags = 0;
	}
	txwi->wcid = wcid->idx;

	/* Note: TX retry reporting is a bit broken.
	 *	 Retries are reported only once per AMPDU and often come
	 *	 a frame early i.e. they are reported in the last status
	 *	 preceding the AMPDU. Apart from the fact that it's hard
	 *	 to know length of the AMPDU (to how many consecutive frames
	 *	 retries should be applied), if status comes early on full
	 *	 fifo it gets lost and retries of the whole AMPDU become
	 *	 invisible.
	 *	 As a work-around encode the desired rate in PKT_ID and based
	 *	 on that guess the retries (every rate is tried once).
	 *	 Only downside here is that for MCS0 we have to rely solely on
	 *	 transmission failures as no retries can ever be reported.
	 *	 Not having to read EXT_FIFO has a nice effect of doubling
	 *	 the number of reports which can be fetched.
	 *	 Also the vendor driver never uses the EXT_FIFO register
	 *	 so it may be untested.
	 */
	is_probe = !!(info->flags & IEEE80211_TX_CTL_RATE_CTRL_PROBE);
	pkt_id = mt7601u_tx_pktid_enc(dev, rate_ctl & 0x7, is_probe);
	pkt_len |= MT76_SET(MT_TXWI_LEN_PKTID, pkt_id);
	txwi->len_ctl = cpu_to_le16(pkt_len);

	dma_flags = MT_TXD_PKT_INFO_80211;
	if (wcid->hw_key_idx == 0xff)
		dma_flags |= MT_TXD_PKT_INFO_WIV;
	mt7601u_dma_skb_wrap_pkt(skb, ep2dmaq(ep), dma_flags);

	if (mt7601u_dma_submit_tx(dev, skb, ep)) {
		ieee80211_free_txskb(dev->hw, skb);
		return;
	}

	trace_mt_tx(skb, msta, txwi);
}

static void mt7601u_tx_pktid_dec(struct mt7601u_dev *dev,
				 struct mt76_tx_status *stat)
{
	u8 req_rate = stat->pktid;
	u8 eff_rate = stat->rate & 0x7;

	req_rate -= 1;

	if (req_rate > 7) {
		stat->is_probe = true;
		req_rate -= 8;

		/* Decide between MCS0 and MCS7 which share pktid 9 */
		if (!req_rate && eff_rate)
			req_rate = 7;
	}

	stat->retry = req_rate - eff_rate;
}

void mt7601u_tx_stat(struct work_struct *work)
{
	struct mt7601u_dev *dev = container_of(work, struct mt7601u_dev,
					       stat_work.work);

	struct mt76_tx_status stat;
	struct ieee80211_tx_info info = {};
	struct ieee80211_sta *sta = NULL;
	struct mt76_wcid *wcid = NULL;
	void *msta;
	int cleaned = 0;
	unsigned long flags;

	while (!test_bit(MT7601U_STATE_REMOVED, &dev->state)) {
		stat = mt7601u_mac_fetch_tx_status(dev);
		if (!stat.valid)
			break;

		mt7601u_tx_pktid_dec(dev, &stat);

		rcu_read_lock();
		if (stat.wcid < ARRAY_SIZE(dev->wcid))
			wcid = rcu_dereference(dev->wcid[stat.wcid]);

		if (wcid) {
			msta = container_of(wcid, struct mt76_sta, wcid);
			sta = container_of(msta, struct ieee80211_sta,
					   drv_priv);
		}

		mt76_mac_fill_tx_status(dev, &info, &stat);
		ieee80211_tx_status_noskb(dev->hw, sta, &info);
		rcu_read_unlock();

		cleaned++;
	}
	trace_mt_tx_status_cleaned(dev, cleaned);

	spin_lock_irqsave(&dev->tx_lock, flags);
	if (cleaned)
		queue_delayed_work(dev->stat_wq, &dev->stat_work,
				   msecs_to_jiffies(10));
	else if (__test_and_clear_bit(MT7601U_STATE_MORE_STATS, &dev->state))
		queue_delayed_work(dev->stat_wq, &dev->stat_work,
				   msecs_to_jiffies(20));
	else
		__clear_bit(MT7601U_STATE_READING_STATS, &dev->state);
	spin_unlock_irqrestore(&dev->tx_lock, flags);
}

int mt7601u_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		    u16 queue, const struct ieee80211_tx_queue_params *params)
{
	struct mt7601u_dev *dev = hw->priv;
	u8 cw_min = 5, cw_max = 10, hw_q = q2hwq(queue);
	u32 val;

	printk("%s %02hhx <- %04hx\n", __func__, hw_q, queue);

	if (params->cw_min)
		cw_min = fls(params->cw_min);
	if (params->cw_max)
		cw_max = fls(params->cw_max);

#define check_param(_p, max) ({						\
			if (_p > max)					\
				printk("%s: too big " #_p ": %d > %d\n", \
				       __func__, _p, max);		\
		})
	check_param(params->txop, 0xff);
	check_param(params->aifs, 0xf);
	check_param(cw_min, 0xf);
	check_param(cw_max, 0xf);
#undef check_param

	val = MT76_SET(MT_EDCA_CFG_AIFSN, params->aifs) |
	      MT76_SET(MT_EDCA_CFG_CWMIN, cw_min) |
	      MT76_SET(MT_EDCA_CFG_CWMAX, cw_max);
	/* TODO: based on user-controlled EnableTxBurst var vendor drv sets
	 *	 a really long txop on AC0 (see connect.c:2009) but only on
	 *	 connect? When not connected should be 0.
	 */
	if (!hw_q)
		val |= 0x60;
	else
		val |= MT76_SET(MT_EDCA_CFG_TXOP, params->txop);
	mt76_wr(dev, MT_EDCA_CFG_AC(hw_q), val);

	val = mt76_rr(dev, MT_WMM_TXOP(hw_q));
	val &= ~(MT_WMM_TXOP_MASK << MT_WMM_TXOP_SHIFT(hw_q));
	val |= params->txop << MT_WMM_TXOP_SHIFT(hw_q);
	mt76_wr(dev, MT_WMM_TXOP(hw_q), val);

	val = mt76_rr(dev, MT_WMM_AIFSN);
	val &= ~(MT_WMM_AIFSN_MASK << MT_WMM_AIFSN_SHIFT(hw_q));
	val |= params->aifs << MT_WMM_AIFSN_SHIFT(hw_q);
	mt76_wr(dev, MT_WMM_AIFSN, val);

	val = mt76_rr(dev, MT_WMM_CWMIN);
	val &= ~(MT_WMM_CWMIN_MASK << MT_WMM_CWMIN_SHIFT(hw_q));
	val |= cw_min << MT_WMM_CWMIN_SHIFT(hw_q);
	mt76_wr(dev, MT_WMM_CWMIN, val);

	val = mt76_rr(dev, MT_WMM_CWMAX);
	val &= ~(MT_WMM_CWMAX_MASK << MT_WMM_CWMAX_SHIFT(hw_q));
	val |= cw_max << MT_WMM_CWMAX_SHIFT(hw_q);
	mt76_wr(dev, MT_WMM_CWMAX, val);

	return 0;
}
