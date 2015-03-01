/*
 * (c) Copyright 2002-2010, Ralink Technology, Inc.
 * Copyright (C) 2014 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2015 Jakub Kicinski <kubakici@wp.pl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mt7601u.h"
#include "mcu.h"
#include "eeprom.h"
#include "trace.h"
#include <linux/etherdevice.h>

/* TODO: I set the "CCK CH14 OBW" here and they overwrite was was done during
 *       channel switching. Seems stupid at best.
 */
static const struct mt76_reg_pair mt7601u_high_temp[] = {
	{  75, 0x60 },
	{  92, 0x02 },
	{ 178, 0xff },		// For CCK CH14 OBW
	{ 195, 0x88 }, { 196, 0x60 },
}, mt7601u_high_temp_bw20[] = {
	{  69, 0x12 },
	{  91, 0x07 },
	{ 195, 0x23 }, { 196, 0x17 },
	{ 195, 0x24 }, { 196, 0x06 },
	{ 195, 0x81 }, { 196, 0x12 },
	{ 195, 0x83 }, { 196, 0x17 },
}, mt7601u_high_temp_bw40[] = {
	{  69, 0x15 },
	{  91, 0x04 },
	{ 195, 0x23 }, { 196, 0x12 },
	{ 195, 0x24 }, { 196, 0x08 },
	{ 195, 0x81 }, { 196, 0x15 },
	{ 195, 0x83 }, { 196, 0x16 },
}, mt7601u_low_temp[] = {
	{ 178, 0xff },		// For CCK CH14 OBW
}, mt7601u_low_temp_bw20[] = {
	{  69, 0x12 },
	{  75, 0x5e },
	{  91, 0x07 },
	{  92, 0x02 },
	{ 195, 0x23 }, { 196, 0x17 },
	{ 195, 0x24 }, { 196, 0x06 },
	{ 195, 0x81 }, { 196, 0x12 },
	{ 195, 0x83 }, { 196, 0x17 },
	{ 195, 0x88 }, { 196, 0x5e },
}, mt7601u_low_temp_bw40[] = {
	{  69, 0x15 },
	{  75, 0x5c },
	{  91, 0x04 },
	{  92, 0x03 },
	{ 195, 0x23 }, { 196, 0x10 },
	{ 195, 0x24 }, { 196, 0x08 },
	{ 195, 0x81 }, { 196, 0x15 },
	{ 195, 0x83 }, { 196, 0x16 },
	{ 195, 0x88 }, { 196, 0x5b },
}, mt7601u_normal_temp[] = {
	{  75, 0x60 },
	{  92, 0x02 },
	{ 178, 0xff },		// For CCK CH14 OBW
	{ 195, 0x88 }, { 196, 0x60 },
}, mt7601u_normal_temp_bw20[] = {
	{  69, 0x12 },
	{  91, 0x07 },
	{ 195, 0x23 }, { 196, 0x17 },
	{ 195, 0x24 }, { 196, 0x06 },
	{ 195, 0x81 }, { 196, 0x12 },
	{ 195, 0x83 }, { 196, 0x17 },
}, mt7601u_normal_temp_bw40[] = {
	{  69, 0x15 },
	{  91, 0x04 },
	{ 195, 0x23 }, { 196, 0x12 },
	{ 195, 0x24 }, { 196, 0x08 },
	{ 195, 0x81 }, { 196, 0x15 },
	{ 195, 0x83 }, { 196, 0x16 },
};


static void mt7601u_agc_reset(struct mt7601u_dev *dev);

static int
mt7601u_rf_wr(struct mt7601u_dev *dev, u8 bank, u8 offset, u8 value)
{
	int ret = 0;

	WARN_ON(!(dev->wlan_ctrl & MT_WLAN_FUN_CTRL_WLAN_EN));
	WARN_ON(offset >= 63);

	if (test_bit(MT7601U_STATE_REMOVED, &dev->state))
		return 0;

	mutex_lock(&dev->reg_atomic_mutex);

	if (!mt76_poll(dev, MT_RF_CSR_CFG, MT_RF_CSR_CFG_KICK, 0, 100)) {
		ret = -ETIMEDOUT;
		goto out;
	}

	mt7601u_wr(dev, MT_RF_CSR_CFG, MT76_SET(MT_RF_CSR_CFG_DATA, value) |
				       MT76_SET(MT_RF_CSR_CFG_REG_BANK, bank) |
				       MT76_SET(MT_RF_CSR_CFG_REG_ID, offset) |
				       MT_RF_CSR_CFG_WR |
				       MT_RF_CSR_CFG_KICK);
out:
	mutex_unlock(&dev->reg_atomic_mutex);

	trace_rf_write(bank, offset, value);
	return ret;
}

static int
mt7601u_rf_rr(struct mt7601u_dev *dev, u8 bank, u8 offset)
{
	int ret = -ETIMEDOUT;
	u32 val;

	WARN_ON(!(dev->wlan_ctrl & MT_WLAN_FUN_CTRL_WLAN_EN));
	WARN_ON(offset >= 63);

	if (test_bit(MT7601U_STATE_REMOVED, &dev->state))
		return 0xff;

	mutex_lock(&dev->reg_atomic_mutex);

	if (!mt76_poll(dev, MT_RF_CSR_CFG, MT_RF_CSR_CFG_KICK, 0, 100))
		goto out;

	mt7601u_wr(dev, MT_RF_CSR_CFG, MT76_SET(MT_RF_CSR_CFG_REG_BANK, bank) |
				       MT76_SET(MT_RF_CSR_CFG_REG_ID, offset) |
				       MT_RF_CSR_CFG_KICK);

	if (!mt76_poll(dev, MT_RF_CSR_CFG, MT_RF_CSR_CFG_KICK, 0, 100))
		goto out;

	val = mt7601u_rr(dev, MT_RF_CSR_CFG);
	if (MT76_GET(MT_RF_CSR_CFG_REG_ID, val) == offset &&
	    MT76_GET(MT_RF_CSR_CFG_REG_BANK, val) == bank)
		ret = MT76_GET(MT_RF_CSR_CFG_DATA, val);
out:
	mutex_unlock(&dev->reg_atomic_mutex);

	if (ret < 0)
		printk("Error: the reg rf read failed %d!!\n", ret);

	trace_rf_read(bank, offset, ret);
	return ret;
}
/* TODO: dunno about the ret val. */
static int
mt7601u_rf_rmw(struct mt7601u_dev *dev, u8 bank, u8 offset, u8 mask, u8 val)
{
	int ret;
	ret = mt7601u_rf_rr(dev, bank, offset);
	if (ret < 0)
		return ret;
	val |= ret & ~mask;
	mt7601u_rf_wr(dev, bank, offset, val);
	return val;
}

static int mt7601u_rf_set(struct mt7601u_dev *dev, u8 bank, u8 offset, u8 val)
{
	return mt7601u_rf_rmw(dev, bank, offset, 0, val);
}

static int
mt7601u_rf_clear(struct mt7601u_dev *dev, u8 bank, u8 offset, u8 mask)
{
	return mt7601u_rf_rmw(dev, bank, offset, mask, 0);
}

int mt7601u_phy_get_rssi(struct mt7601u_dev *dev,
			 struct mt7601u_rxwi *rxwi, u16 rate)
{
	static const s8 lna[2][2][3] = {
		/* main LNA */ {
			/* bw20 */ { -2, 15, 33 },
			/* bw40 */ {  0, 16, 34 }
		},
		/*  aux LNA */ {
			/* bw20 */ { -2, 15, 33 },
			/* bw40 */ { -2, 16, 34 }
		}
	};
	int bw = MT76_GET(MT_XWI_RATE_BW, rate);
	int aux_lna = MT76_GET(MT_RXWI_ANT_AUX_LNA, rxwi->ant);
	int lna_id = MT76_GET(MT_RXWI_GAIN_RSSI_LNA_ID, rxwi->gain);
	int val;

	if (lna_id) /* LNA id can be 0, 2, 3. */
		lna_id--;

	val = 8;
	val -= lna[aux_lna][bw][lna_id];
	val -= MT76_GET(MT_RXWI_GAIN_RSSI_VAL, rxwi->gain);
	val -= dev->ee->lna_gain;
	val -= dev->ee->rssi_offset[0];

	return val;
}

static void mt7601u_vco_cal(struct mt7601u_dev *dev)
{
	/* TODO: these two can be combined in an andes write */
	mt7601u_rf_wr(dev, 0, 4, 0x0a);
	mt7601u_rf_wr(dev, 0, 5, 0x20);
	mt7601u_rf_set(dev, 0, 4, BIT(7));
	msleep(2);
}

static int mt7601u_set_bw_filter(struct mt7601u_dev *dev, bool cal)
{
	u32 filter = 0;
	int ret;

	if (!cal)
		filter |= 0x10000;
	if (dev->bw != MT_BW_20)
		filter |= 0x00100;

	/* TX */
	ret = mt7601u_mcu_calibrate(dev, MCU_CAL_BW, filter | 1);
	if (ret)
		return ret;
	/* RX */
	return mt7601u_mcu_calibrate(dev, MCU_CAL_BW, filter);
}

static int mt7601u_update_bbp_temp_table_after_set_bw(struct mt7601u_dev *dev)
{
	const struct mt76_reg_pair *t;
	int n;

	/* TODO: only do this when bw really changed */
	/* TODO: these tables are a huge mess, clean this up */
	switch (dev->temp_mode) {
	case MT_TEMP_MODE_LOW:
		if (dev->bw == MT_BW_20) {
			t = mt7601u_low_temp_bw20;
			n = ARRAY_SIZE(mt7601u_low_temp_bw20);
		} else {
			t = mt7601u_low_temp_bw40;
			n = ARRAY_SIZE(mt7601u_low_temp_bw40);
		}
		break;

	case MT_TEMP_MODE_NORMAL:
		if (dev->bw == MT_BW_20) {
			t = mt7601u_normal_temp_bw20;
			n = ARRAY_SIZE(mt7601u_normal_temp_bw20);
		} else {
			t = mt7601u_normal_temp_bw40;
			n = ARRAY_SIZE(mt7601u_normal_temp_bw40);
		}
		break;

	case MT_TEMP_MODE_HIGH:
		if (dev->bw == MT_BW_20) {
			t = mt7601u_high_temp_bw20;
			n = ARRAY_SIZE(mt7601u_high_temp_bw20);
		} else {
			t = mt7601u_high_temp_bw40;
			n = ARRAY_SIZE(mt7601u_high_temp_bw40);
		}
		break;

	default:
		/* TODO: turn TEMP_MODE into enum and drop this */
		printk("Error: %s detected invalid state\n", __func__);
		return -EINVAL;
	}

	return mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_BBP, t, n);
}

static int __mt7601u_phy_set_channel(struct mt7601u_dev *dev,
				     struct cfg80211_chan_def *chandef)
{
	struct ieee80211_channel *chan = chandef->chan;
	enum nl80211_channel_type chan_type =
		cfg80211_get_chandef_type(chandef);
	struct mt7601u_rate_power *t = &dev->ee->power_rate_table;
	int chan_idx;
	bool chan_ext_below;
	u8 bw;
	int i, ret;

#define FREQ_PLAN_REGS	4
	static const u8 freq_plan[14][FREQ_PLAN_REGS] = {
		{ 0x99,	0x99,	0x09,	0x50 },
		{ 0x46,	0x44,	0x0a,	0x50 },
		{ 0xec,	0xee,	0x0a,	0x50 },
		{ 0x99,	0x99,	0x0b,	0x50 },
		{ 0x46,	0x44,	0x08,	0x51 },
		{ 0xec,	0xee,	0x08,	0x51 },
		{ 0x99,	0x99,	0x09,	0x51 },
		{ 0x46,	0x44,	0x0a,	0x51 },
		{ 0xec,	0xee,	0x0a,	0x51 },
		{ 0x99,	0x99,	0x0b,	0x51 },
		{ 0x46,	0x44,	0x08,	0x52 },
		{ 0xec,	0xee,	0x08,	0x52 },
		{ 0x99,	0x99,	0x09,	0x52 },
		{ 0x33,	0x33,	0x0b,	0x52 },
	};
	struct mt76_reg_pair channel_freq_plan[FREQ_PLAN_REGS] = {
		{ 17, 0 }, { 18, 0 }, { 19, 0 }, { 20, 0 },
	};
	struct mt76_reg_pair bbp_settings[3] = {
		{ 62, 0x37 - dev->ee->lna_gain },
		{ 63, 0x37 - dev->ee->lna_gain },
		{ 64, 0x37 - dev->ee->lna_gain },
	};

	bw = MT_BW_20;
	chan_ext_below = chan_type == NL80211_CHAN_HT40MINUS;
	chan_idx = chan->hw_value - 1;
	if (chandef->width == NL80211_CHAN_WIDTH_40) {
		bw = MT_BW_40;

		if (chan_idx > 1 && chan_type == NL80211_CHAN_HT40MINUS)
			chan_idx -= 2;
		else if (chan_idx < 12 && chan_type == NL80211_CHAN_HT40PLUS)
			chan_idx += 2;
		else
			printk("Error: invalid 40MHz channel!!\n");
	}

	if (bw != dev->bw || chan_ext_below != dev->chan_ext_below) {
		printk("Info: switching HT mode bw:%d below:%d\n",
		       bw, chan_ext_below);
		mt7601u_bbp_set_bw(dev, bw);
		mt7601u_bbp_set_ctrlch(dev, chan_ext_below);
		mt7601u_mac_set_ctrlch(dev, chan_ext_below);

		dev->chan_ext_below = chan_ext_below;
	}

	for (i = 0; i < FREQ_PLAN_REGS; i++)
		channel_freq_plan[i].value = freq_plan[chan_idx][i];

	ret = mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_RF,
				      channel_freq_plan, FREQ_PLAN_REGS);
	if (ret)
		return ret;

	mt7601u_rmw(dev, MT_TX_ALC_CFG_0, 0x3f3f,
		    dev->ee->chan_pwr[chan_idx] & 0x3f);

	ret = mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_BBP,
				      bbp_settings, ARRAY_SIZE(bbp_settings));
	if (ret)
		return ret;

	mt7601u_vco_cal(dev);

	/* TODO: already did this above */
	mt7601u_bbp_set_bw(dev, bw);

	/* TODO: move this to set BW, no? */
	ret = mt7601u_update_bbp_temp_table_after_set_bw(dev);
	if (ret)
		return ret;

	ret = mt7601u_set_bw_filter(dev, false);
	if (ret)
		return ret;

	/* TODO: perhaps move this mess out of here? */
	if (chan->hw_value != 14 || bw != MT_BW_20) {
		mt7601u_bbp_rmw(dev, 4, 0x20, 0);
		mt7601u_bbp_wr(dev, 178, 0xff);

		t->cck[0].bw20 = dev->ee->real_cck_bw20[0];
		t->cck[1].bw20 = dev->ee->real_cck_bw20[1];
	} else { /* Apply CH14 OBW fixup */
		mt7601u_bbp_wr(dev, 4, 0x60);
		mt7601u_bbp_wr(dev, 178, 0);

		/* Note: vendor code is buggy here for negative values */
		t->cck[0].bw20 = dev->ee->real_cck_bw20[0] - 2;
		t->cck[1].bw20 = dev->ee->real_cck_bw20[1] - 2;
	}

	mt7601u_wr(dev, MT_TX_PWR_CFG_0, int_to_s6(t->ofdm[1].bw20) << 24 |
					 int_to_s6(t->ofdm[0].bw20) << 16 |
					 int_to_s6(t->cck[1].bw20) << 8 |
					 int_to_s6(t->cck[0].bw20));

	/* TODO: perhaps set ctrl channel (below/above)? */

	if (test_bit(MT7601U_STATE_SCANNING, &dev->state))
		mt7601u_agc_reset(dev);

	dev->chandef = *chandef;

	return 0;
}

int mt7601u_phy_set_channel(struct mt7601u_dev *dev,
			    struct cfg80211_chan_def *chandef)
{
	int ret;

	cancel_delayed_work_sync(&dev->cal_work);
	cancel_delayed_work_sync(&dev->freq_cal.work);

	mutex_lock(&dev->hw_atomic_mutex);
	ret = __mt7601u_phy_set_channel(dev, chandef);
	mutex_unlock(&dev->hw_atomic_mutex);
	if (ret)
		return ret;

	if (test_bit(MT7601U_STATE_SCANNING, &dev->state))
		return 0;

	ieee80211_queue_delayed_work(dev->hw, &dev->cal_work,
				     MT_CALIBRATE_INTERVAL);
	if (dev->freq_cal.enabled)
		ieee80211_queue_delayed_work(dev->hw, &dev->freq_cal.work,
					     MT_FREQ_CAL_INIT_DELAY);

	return 0;

}

#define BBP_R47_FLAG		GENMASK(2, 0)
#define BBP_R47_F_TSSI		0
#define BBP_R47_F_PKT_T		1
#define BBP_R47_F_TX_RATE	2
#define BBP_R47_F_TEMP		4
/**
 * mt7601u_bbp_r47_get - read value through BBP R47/R49 pair
 * @dev: adapter structure
 * @reg: value of BBP R47 before the operation
 * @flag: one of the BBP_R47_F_* flags
 *
 * Convenience helper for reading values through BBP R47/R49 pair.
 * Takes old value of BBP R47 as @reg, because callers usually have it
 * cached already.
 *
 * Return: value of BBP R49.
 */
static u8 mt7601u_bbp_r47_get(struct mt7601u_dev *dev, u8 reg, u8 flag)
{
	flag |= reg & ~BBP_R47_FLAG;
	mt7601u_bbp_wr(dev, 47, flag);
	usleep_range(500, 700);
	return mt7601u_bbp_rr(dev, 49);
}

static s8 mt7601u_read_bootup_temp(struct mt7601u_dev *dev)
{
	u8 bbp_val, temp;
	u32 rf_bp, rf_set;
	int i;

	rf_set = mt7601u_rr(dev, MT_RF_SETTING_0);
	rf_bp = mt7601u_rr(dev, MT_RF_BYPASS_0);

	mt7601u_wr(dev, MT_RF_BYPASS_0, 0);
	mt7601u_wr(dev, MT_RF_SETTING_0, 0x10);
	mt7601u_wr(dev, MT_RF_BYPASS_0, 0x10);

	bbp_val = mt7601u_bbp_rmw(dev, 47, 0, 0x10);

	mt7601u_bbp_wr(dev, 22, 0x40);

	for (i = 100; i && (bbp_val & 0x10); i--)
		bbp_val = mt7601u_bbp_rr(dev, 47);

	temp = mt7601u_bbp_r47_get(dev, bbp_val, BBP_R47_F_TEMP);
	trace_printk("I recon boot up temp is %02hhx\n", temp);

	mt7601u_bbp_wr(dev, 22, 0);

	bbp_val = mt7601u_bbp_rr(dev, 21);
	bbp_val |= 0x02;
	mt7601u_bbp_wr(dev, 21, bbp_val);
	bbp_val &= ~0x02;
	mt7601u_bbp_wr(dev, 21, bbp_val);

	mt7601u_wr(dev, MT_RF_BYPASS_0, 0);
	mt7601u_wr(dev, MT_RF_SETTING_0, rf_set);
	mt7601u_wr(dev, MT_RF_BYPASS_0, rf_bp);

	return temp;
}

static s8 mt7601u_read_temp(struct mt7601u_dev *dev)
{
	int i;
	u8 val;
	s8 temp;

	val = mt7601u_bbp_rmw(dev, 47, 0x7f, 0x10);

	/* TODO: this will never succeed. We can try to kick it off and try
	 *       to read the value in the later iterations like tssi_cal does.
	 */
	for (i = 100; i && (val & 0x10); i--)
		val = mt7601u_bbp_rr(dev, 47);

	temp = mt7601u_bbp_r47_get(dev, val, BBP_R47_F_TEMP);

	return temp;
}

static void mt7601u_rxdc_cal(struct mt7601u_dev *dev)
{
	static const struct mt76_reg_pair intro[] = {
		{ 158, 0x8d }, { 159, 0xfc },
		{ 158, 0x8c }, { 159, 0x4c },
	}, outro[] = {
		{ 158, 0x8d }, { 159, 0xe0 },
	};
	u32 mac_ctrl;
	int i, ret;

	mac_ctrl = mt7601u_rr(dev, MT_MAC_SYS_CTRL);
	mt7601u_wr(dev, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_RX);

	ret = mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_BBP,
				      intro, ARRAY_SIZE(intro));
	if (ret)
		printk("%s intro failed\n", __func__);

	for (i = 20; i; i--) {
		usleep_range(300, 500);

		mt7601u_bbp_wr(dev, 158, 0x8c);
		if (mt7601u_bbp_rr(dev, 159) == 0x0c)
			break;
	}
	if (!i)
		printk("%s timed out\n", __func__);

	mt7601u_wr(dev, MT_MAC_SYS_CTRL, 0);

	ret = mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_BBP,
				      outro, ARRAY_SIZE(outro));
	if (ret)
		printk("%s outro failed\n", __func__);

	mt7601u_wr(dev, MT_MAC_SYS_CTRL, mac_ctrl);
}

void mt7601u_phy_recalibrate_after_assoc(struct mt7601u_dev *dev)
{
	mt7601u_mcu_calibrate(dev, MCU_CAL_DPD, dev->curr_temp);

	mt7601u_rxdc_cal(dev);
}

/* TODO: rewrite this - it's copied */
static s16 lin2dBd(u16 linear)
{
	short exp = 0;
	unsigned int mantisa;
	int app, dBd;

	if (WARN_ON(!linear))
		return -10000;

	mantisa = linear;

	exp = fls(mantisa) - 16;
	if (exp > 0)
		mantisa >>= exp;
	else
		mantisa <<= abs(exp);

	//S(15,0)
	if (mantisa <= 0xb800)
		app = (mantisa + (mantisa >> 3) + (mantisa >> 4) - 0x9600);
	else
		app = (mantisa - (mantisa >> 3) - (mantisa >> 6) - 0x5a00);
	if (app < 0)
		app = 0;

	dBd = ((15 + exp) << 15) + app; //since 2^15=1 here
	dBd = (dBd << 2) + (dBd << 1) + (dBd >> 6) + (dBd >> 7);
	dBd = (dBd >> 10); //S10.5

	return dBd;
}

static void
mt7601u_set_initial_tssi(struct mt7601u_dev *dev, s16 tssi_db, s16 tssi_hvga_db)
{
	struct tssi_data *d = &dev->ee->tssi_data;
	int init_offset;

	init_offset = -((tssi_db * d->slope + d->offset[1]) / 4096) + 10;

	mt76_rmw(dev, MT_TX_ALC_CFG_1, MT_TX_ALC_CFG_1_TEMP_COMP,
		 int_to_s6(init_offset) & MT_TX_ALC_CFG_1_TEMP_COMP);
}

static void mt7601u_tssi_dc_gain_cal(struct mt7601u_dev *dev)
{
	u8 rf_vga, rf_mixer, bbp_r47;
	int i, j;
	s8 res[4];
	s16 tssi_init_db, tssi_init_hvga_db;

	mt7601u_wr(dev, MT_RF_SETTING_0, 0x00000030);
	mt7601u_wr(dev, MT_RF_BYPASS_0, 0x000c0030);
	mt7601u_wr(dev, MT_MAC_SYS_CTRL, 0);

	mt7601u_bbp_wr(dev, 58, 0);
	mt7601u_bbp_wr(dev, 241, 0x2);
	mt7601u_bbp_wr(dev, 23, 0x8);
	bbp_r47 = mt7601u_bbp_rr(dev, 47);

	/* Set VGA gain */
	rf_vga = mt7601u_rf_rr(dev, 5, 3);
	mt7601u_rf_wr(dev, 5, 3, 8);

	/* Mixer disable */
	rf_mixer = mt7601u_rf_rr(dev, 4, 39);
	mt7601u_rf_wr(dev, 4, 39, 0);

	for (i = 0; i < 4; i++) {
		mt7601u_rf_wr(dev, 4, 39, (i & 1) ? rf_mixer : 0);

		mt7601u_bbp_wr(dev, 23, (i < 2) ? 0x08 : 0x02);
		mt7601u_rf_wr(dev, 5, 3, (i < 2) ? 0x08 : 0x11);

		/* BBP TSSI initial and soft reset */
		mt7601u_bbp_wr(dev, 22, 0);
		mt7601u_bbp_wr(dev, 244, 0);

		mt7601u_bbp_wr(dev, 21, 1);
		udelay(1);
		mt7601u_bbp_wr(dev, 21, 0);

		/* TSSI measurement */
		mt7601u_bbp_wr(dev, 47, 0x50);
		mt7601u_bbp_wr(dev, (i & 1) ? 244 : 22, (i & 1) ? 0x31 : 0x40);

		for (j = 20; j; j--)
			if (!(mt7601u_bbp_rr(dev, 47) & 0x10))
				break;
		if (!j)
			printk("%s timed out\n", __func__);

		/* TSSI read */
		mt7601u_bbp_wr(dev, 47, 0x40);
		res[i] = mt7601u_bbp_rr(dev, 49);
	}

	tssi_init_db = lin2dBd((short)res[1] - res[0]);
	tssi_init_hvga_db = lin2dBd(((short)res[3] - res[2]) * 4);
	dev->tssi_init = res[0];
	dev->tssi_init_hvga = res[2];
	dev->tssi_init_hvga_offset_db = tssi_init_hvga_db - tssi_init_db;

	trace_printk("TSSI_init:%hhx db:%hx hvga:%hhx hvga_db:%hx off_db:%hx\n",
		     dev->tssi_init, tssi_init_db, dev->tssi_init_hvga,
		     tssi_init_hvga_db, dev->tssi_init_hvga_offset_db);

	mt7601u_bbp_wr(dev, 22, 0);
	mt7601u_bbp_wr(dev, 244, 0);

	mt7601u_bbp_wr(dev, 21, 1);
	udelay(1);
	mt7601u_bbp_wr(dev, 21, 0);

	mt7601u_wr(dev, MT_RF_BYPASS_0, 0);
	mt7601u_wr(dev, MT_RF_SETTING_0, 0);

	mt7601u_rf_wr(dev, 5, 3, rf_vga);
	mt7601u_rf_wr(dev, 4, 39, rf_mixer);
	mt7601u_bbp_wr(dev, 47, bbp_r47);

	mt7601u_set_initial_tssi(dev, tssi_init_db, tssi_init_hvga_db);
}

static int mt7601u_bbp_temp(struct mt7601u_dev *dev,
			    int mode, const char *name,
			    const struct mt76_reg_pair *common, int n_common,
			    const struct mt76_reg_pair *bw20, int n_bw20,
			    const struct mt76_reg_pair *bw40, int n_bw40)
{
	int ret;

	if (dev->temp_mode == mode)
		return 0;

	dev->temp_mode = mode;
	trace_printk("Switching to %s temp\n", name);

	ret = mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_BBP,
				      common, n_common);
	if (ret)
		return ret;

	if (dev->bw == MT_BW_20)
		return mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_BBP,
					       bw20, n_bw20);
	else
		return mt7601u_write_reg_pairs(dev, MT_MCU_MEMMAP_BBP,
					       bw40, n_bw40);
}

static int mt7601u_temp_comp(struct mt7601u_dev *dev, bool on)
{
	int ret, temp, hi_temp = 400, lo_temp = -200;

	temp = (dev->b49_temp - dev->ee->ref_temp) *
		MT7601_E2_TEMPERATURE_SLOPE;
	dev->curr_temp = temp;

	/* DPD Calibration */
	if (temp - dev->dpd_temp > 450 || temp - dev->dpd_temp < -450) {
		dev->dpd_temp = temp;

		ret = mt7601u_mcu_calibrate(dev, MCU_CAL_DPD, dev->dpd_temp);
		if (ret)
			return ret;

		mt7601u_vco_cal(dev);

		trace_printk("Recalibrate DPD\n");
	}

	/* PLL Lock Protect */
	if (temp < -50 && !dev->pll_lock_protect) { /* < 20C */
		dev->pll_lock_protect =  true;

		mt7601u_rf_wr(dev, 4, 4, 6);
		mt7601u_rf_clear(dev, 4, 10, 0x30);

		trace_printk("PLL lock protect on - too cold\n");
	} else if (temp > 50 && dev->pll_lock_protect) { /* > 30C */
		dev->pll_lock_protect = false;

		mt7601u_rf_wr(dev, 4, 4, 0);
		mt7601u_rf_rmw(dev, 4, 10, 0x30, 0x10);

		trace_printk("PLL lock protect off\n");
	}

	if (on) {
		hi_temp -= 50;
		lo_temp -= 50;
	}

	if (dev->bw != MT_BW_20 && dev->bw != MT_BW_40) {
		printk("Error: unknown bw:%d\n", dev->bw);
		return -EINVAL;
	}

	/* BBP CR for H, L, N temperature */
	if (temp > hi_temp)
		return mt7601u_bbp_temp(dev, MT_TEMP_MODE_HIGH, "high",
					mt7601u_high_temp,
					ARRAY_SIZE(mt7601u_high_temp),
					mt7601u_high_temp_bw20,
					ARRAY_SIZE(mt7601u_high_temp_bw20),
					mt7601u_high_temp_bw40,
					ARRAY_SIZE(mt7601u_high_temp_bw40));
	else if (temp > lo_temp)
		return mt7601u_bbp_temp(dev, MT_TEMP_MODE_NORMAL, "normal",
					mt7601u_normal_temp,
					ARRAY_SIZE(mt7601u_normal_temp),
					mt7601u_normal_temp_bw20,
					ARRAY_SIZE(mt7601u_normal_temp_bw20),
					mt7601u_normal_temp_bw40,
					ARRAY_SIZE(mt7601u_normal_temp_bw40));
	else
		return mt7601u_bbp_temp(dev, MT_TEMP_MODE_LOW, "low",
					mt7601u_low_temp,
					ARRAY_SIZE(mt7601u_low_temp),
					mt7601u_low_temp_bw20,
					ARRAY_SIZE(mt7601u_low_temp_bw20),
					mt7601u_low_temp_bw40,
					ARRAY_SIZE(mt7601u_low_temp_bw40));
}

/* TODO: If this is used only with HVGA we can just use trgt_pwr directly. */
static int mt7601u_current_tx_power(struct mt7601u_dev *dev)
{
	if (!dev->ee->tssi_enabled)
		printk("Warning: %s used for non-TSSI mode!\n", __func__);
	return dev->ee->chan_pwr[dev->chandef.chan->hw_value - 1];
}

static bool mt7601u_use_hvga(struct mt7601u_dev *dev)
{
	return !(mt7601u_current_tx_power(dev) > 20);
}

static s16
mt7601u_phy_rf_pa_mode_val(struct mt7601u_dev *dev, int phy_mode, int tx_rate)
{
	static const s16 decode_tb[] = { 0, 8847, -5734, -5734 };
	u32 reg;

	switch (phy_mode) {
	case MT_PHY_TYPE_OFDM:
		tx_rate += 4;
	case MT_PHY_TYPE_CCK:
		reg = dev->rf_pa_mode[0];
		break;
	default:
		reg = dev->rf_pa_mode[1];
		break;
	}

	return decode_tb[(reg >> (tx_rate * 2)) & 0x3];
}

static struct mt7601u_tssi_params
mt7601u_tssi_params_get(struct mt7601u_dev *dev)
{
	static const u8 ofdm_pkt2rate[8] = { 6, 4, 2, 0, 7, 5, 3, 1 };
	static const int static_power[4] = { 0, -49152, -98304, 49152 };
	struct mt7601u_tssi_params p;
	u8 bbp_r47, pkt_type, tx_rate;
	struct power_per_rate *rate_table;

	bbp_r47 = mt7601u_bbp_rr(dev, 47);

	p.tssi0 = mt7601u_bbp_r47_get(dev, bbp_r47, BBP_R47_F_TSSI);
	dev->b49_temp = mt7601u_bbp_r47_get(dev, bbp_r47, BBP_R47_F_TEMP);
	pkt_type = mt7601u_bbp_r47_get(dev, bbp_r47, BBP_R47_F_PKT_T);

	p.trgt_power = mt7601u_current_tx_power(dev);

	switch (pkt_type & 0x03) {
	case MT_PHY_TYPE_CCK:
		tx_rate = (pkt_type >> 4) & 0x03;
		rate_table = dev->ee->power_rate_table.cck;
		break;

	case MT_PHY_TYPE_OFDM:
		tx_rate = ofdm_pkt2rate[(pkt_type >> 4) & 0x07];
		rate_table = dev->ee->power_rate_table.ofdm;
		break;

	default:
		tx_rate = mt7601u_bbp_r47_get(dev, bbp_r47, BBP_R47_F_TX_RATE);
		tx_rate &= 0x7f;
		rate_table = dev->ee->power_rate_table.ht;
		break;
	}

	if (dev->bw == MT_BW_20)
		p.trgt_power += rate_table[tx_rate / 2].bw20;
	else
		p.trgt_power += rate_table[tx_rate / 2].bw40;

	p.trgt_power <<= 12;

	trace_printk("tx_rate:%02hhx pwr:%08x\n", tx_rate, p.trgt_power);

	p.trgt_power += mt7601u_phy_rf_pa_mode_val(dev, pkt_type & 0x03,
						   tx_rate);

	/* Channel 14, cck, bw20 */
	if ((pkt_type & 0x03) == MT_PHY_TYPE_CCK) {
		if (mt7601u_bbp_rr(dev, 4) & 0x20)
			p.trgt_power += mt7601u_bbp_rr(dev, 178) ? 18022 : 9830;
		else
			p.trgt_power += mt7601u_bbp_rr(dev, 178) ? 819 : 24576;
	}

	p.trgt_power += static_power[mt7601u_bbp_rr(dev, 1) & 0x03];

	p.trgt_power += dev->ee->tssi_data.tx0_delta_offset;

	trace_printk("tssi:%02hhx t_power:%08x temp:%02hhx pkt_type:%02hhx\n",
		     p.tssi0, p.trgt_power, dev->b49_temp, pkt_type);

	return p;
}

static bool mt7601u_tssi_read_ready(struct mt7601u_dev *dev)
{
	return !(mt7601u_bbp_rr(dev, 47) & 0x10);
}

static int mt7601u_tssi_cal(struct mt7601u_dev *dev)
{
	struct mt7601u_tssi_params params;
	int curr_pwr, diff_pwr;
	char tssi_offset;
	s8 tssi_init;
	s16 tssi_m_dc, tssi_db;
	bool hvga;
	u32 val;

	if (!dev->ee->tssi_enabled)
		return 0;

	hvga = mt7601u_use_hvga(dev);
	if (!dev->tssi_read_trig)
		return mt7601u_mcu_tssi_read_kick(dev, hvga);

	if (!mt7601u_tssi_read_ready(dev))
		return 0;

	params = mt7601u_tssi_params_get(dev);

	tssi_init = (hvga ? dev->tssi_init_hvga : dev->tssi_init);
	tssi_m_dc = params.tssi0 - tssi_init;
	tssi_db = lin2dBd(tssi_m_dc);
	trace_printk("tssi dc:%04hx db:%04hx hvga:%d\n",
		     tssi_m_dc, tssi_db, hvga);

	if (dev->chandef.chan->hw_value < 5)
		tssi_offset = dev->ee->tssi_data.offset[0];
	else if (dev->chandef.chan->hw_value < 9)
		tssi_offset = dev->ee->tssi_data.offset[1];
	else
		tssi_offset = dev->ee->tssi_data.offset[2];

	if (hvga)
		tssi_db -= dev->tssi_init_hvga_offset_db;

	curr_pwr = tssi_db * dev->ee->tssi_data.slope + (tssi_offset << 9);
	diff_pwr = params.trgt_power - curr_pwr;
	trace_printk("Power curr:%08x diff:%08x\n", curr_pwr, diff_pwr);

	if (params.tssi0 > 126 && diff_pwr > 0) {
		printk("Error: TSSI upper saturation\n");
		diff_pwr = 0;
	}
	if (params.tssi0 - tssi_init < 1 && diff_pwr < 0) {
		printk("Error: TSSI lower saturation\n");
		diff_pwr = 0;
	}

	if ((dev->prev_pwr_diff ^ diff_pwr) < 0 && abs(diff_pwr) < 4096 &&
	    (abs(diff_pwr) > abs(dev->prev_pwr_diff) ||
	     (diff_pwr > 0 && diff_pwr == -dev->prev_pwr_diff)))
		diff_pwr = 0;
	else
		dev->prev_pwr_diff = diff_pwr;

	diff_pwr += (diff_pwr > 0) ? 2048 : -2048;
	diff_pwr /= 4096;

	trace_printk("final diff: %08x\n", diff_pwr);

	val = mt7601u_rr(dev, MT_TX_ALC_CFG_1);
	curr_pwr = s6_to_int(MT76_GET(MT_TX_ALC_CFG_1_TEMP_COMP, val));
	diff_pwr += curr_pwr;
	val = (val & ~MT_TX_ALC_CFG_1_TEMP_COMP) | int_to_s6(diff_pwr);
	mt7601u_wr(dev, MT_TX_ALC_CFG_1, val);

	return mt7601u_mcu_tssi_read_kick(dev, hvga);
}

static u8 mt7601u_agc_default(struct mt7601u_dev *dev)
{
	return (dev->ee->lna_gain - 8) * 2 + 0x34;
}

static void mt7601u_agc_reset(struct mt7601u_dev *dev)
{
	u8 agc = mt7601u_agc_default(dev);

	mt7601u_bbp_wr(dev, 66,	agc);
}

void mt7601u_agc_save(struct mt7601u_dev *dev)
{
	dev->agc_save = mt7601u_bbp_rr(dev, 66);
}

void mt7601u_agc_restore(struct mt7601u_dev *dev)
{
	mt7601u_bbp_wr(dev, 66, dev->agc_save);
}

static void mt7601u_agc_tune(struct mt7601u_dev *dev)
{
	u8 val = mt7601u_agc_default(dev);

	/* TODO: only in STA mode and not dozing; perhaps do this only if
	 *       there is enough rssi updates since last run?
	 *       Rssi updates are only on beacons and U2M so should work...
	 */
	if (dev->avg_rssi <= -70)
		val -= 0x20;
	else if (dev->avg_rssi <= -60)
		val -= 0x10;

	if (val != mt7601u_bbp_rr(dev, 66))
		mt7601u_bbp_wr(dev, 66, val);

	/* TODO: also if lost a lot of beacons try resetting
	 *       (see RTMPSetAGCInitValue() call in mlme.c).
	 */
}

static void mt7601u_phy_calibrate(struct work_struct *work)
{
	struct mt7601u_dev *dev = container_of(work, struct mt7601u_dev,
					    cal_work.work);

	mt7601u_agc_tune(dev);
	mt7601u_tssi_cal(dev);
	/* If TSSI calibration was run it already updated temperature. */
	if (!dev->ee->tssi_enabled)
		dev->b49_temp = mt7601u_read_temp(dev);
	mt7601u_temp_comp(dev, true); /* TODO: find right value for @on */

	ieee80211_queue_delayed_work(dev->hw, &dev->cal_work,
				     MT_CALIBRATE_INTERVAL);
}

static unsigned long
__mt7601u_phy_freq_cal(struct mt7601u_dev *dev, s8 last_offset, u8 phy_mode)
{
	u8 activate_threshold, deactivate_threshold;

	trace_freq_cal_offset(phy_mode, last_offset);

	/* No beacons received - reschedule soon */
	if (last_offset == MT7601U_FREQ_OFFSET_INVALID)
		return MT_FREQ_CAL_ADJ_INTERVAL;

	switch (phy_mode) {
	case MT_PHY_TYPE_CCK:
		activate_threshold = 19;
		deactivate_threshold = 5;
		break;
	case MT_PHY_TYPE_OFDM:
		activate_threshold = 102;
		deactivate_threshold = 32;
		break;
	case MT_PHY_TYPE_HT:
	case MT_PHY_TYPE_HT_GF:
		activate_threshold = 82;
		deactivate_threshold = 20;
		break;
	default:
		WARN_ON(1);
		return MT_FREQ_CAL_CHECK_INTERVAL;
	}

	if (abs(last_offset) >= activate_threshold)
		dev->freq_cal.adjusting = true;
	else if (abs(last_offset) <= deactivate_threshold)
		dev->freq_cal.adjusting = false;

	if (!dev->freq_cal.adjusting)
		return MT_FREQ_CAL_CHECK_INTERVAL;

	if (last_offset > deactivate_threshold) {
		if (dev->freq_cal.freq > 0)
			dev->freq_cal.freq--;
		else
			dev->freq_cal.adjusting = false;
	} else if (last_offset < -deactivate_threshold) {
		if (dev->freq_cal.freq < 0xbf)
			dev->freq_cal.freq++;
		else
			dev->freq_cal.adjusting = false;
	}

	trace_freq_cal_adjust(dev->freq_cal.freq);
	mt7601u_rf_wr(dev, 0, 12, dev->freq_cal.freq);
	mt7601u_vco_cal(dev);

	return dev->freq_cal.adjusting ? MT_FREQ_CAL_ADJ_INTERVAL :
					 MT_FREQ_CAL_CHECK_INTERVAL;
}

static void mt7601u_phy_freq_cal(struct work_struct *work)
{
	struct mt7601u_dev *dev = container_of(work, struct mt7601u_dev,
					       freq_cal.work.work);
	s8 last_offset;
	u8 phy_mode;
	unsigned long delay;

	spin_lock_bh(&dev->last_beacon.lock);
	last_offset = dev->last_beacon.freq_off;
	phy_mode = dev->last_beacon.phy_mode;
	spin_unlock_bh(&dev->last_beacon.lock);

	delay = __mt7601u_phy_freq_cal(dev, last_offset, phy_mode);
	ieee80211_queue_delayed_work(dev->hw, &dev->freq_cal.work, delay);

	spin_lock_bh(&dev->last_beacon.lock);
	dev->last_beacon.freq_off = MT7601U_FREQ_OFFSET_INVALID;
	spin_unlock_bh(&dev->last_beacon.lock);
}

void mt7601u_phy_freq_cal_onoff(struct mt7601u_dev *dev,
				struct ieee80211_bss_conf *info)
{
	/* TODO: support multi-bssid? */
	if (!info->assoc)
		cancel_delayed_work_sync(&dev->freq_cal.work);

	/* Start/stop collecting beacon data */
	ether_addr_copy(dev->bssid, info->bssid);

	spin_lock_bh(&dev->last_beacon.lock);
	dev->last_beacon.freq_off = MT7601U_FREQ_OFFSET_INVALID;
	spin_unlock_bh(&dev->last_beacon.lock);

	dev->freq_cal.freq = dev->ee->rf_freq_off;
	dev->freq_cal.enabled = info->assoc;
	dev->freq_cal.adjusting = false;

	if (info->assoc)
		ieee80211_queue_delayed_work(dev->hw, &dev->freq_cal.work,
					     MT_FREQ_CAL_INIT_DELAY);
}

static int mt7601u_init_cal(struct mt7601u_dev *dev)
{
	u32 mac_ctrl;
	int ret;

	dev->b49_temp = mt7601u_read_bootup_temp(dev);
	dev->curr_temp = (dev->b49_temp - dev->ee->ref_temp) *
		MT7601_E2_TEMPERATURE_SLOPE;
	dev->dpd_temp = dev->curr_temp;

	mac_ctrl = mt7601u_rr(dev, MT_MAC_SYS_CTRL);

	ret = mt7601u_mcu_calibrate(dev, MCU_CAL_R, 0);
	if (ret)
		return ret;

	ret = mt7601u_rf_rr(dev, 0, 4);
	if (ret < 0)
		return ret;
	ret |= 0x80;
	ret = mt7601u_rf_wr(dev, 0, 4, ret);
	if (ret)
		return ret;
	msleep(2);

	ret = mt7601u_mcu_calibrate(dev, MCU_CAL_TXDCOC, 0);
	if (ret)
		return ret;

	mt7601u_rxdc_cal(dev);

	ret = mt7601u_set_bw_filter(dev, true);
	if (ret)
		return ret;
	ret = mt7601u_mcu_calibrate(dev, MCU_CAL_LOFT, 0);
	if (ret)
		return ret;
	ret = mt7601u_mcu_calibrate(dev, MCU_CAL_TXIQ, 0);
	if (ret)
		return ret;
	ret = mt7601u_mcu_calibrate(dev, MCU_CAL_RXIQ, 0);
	if (ret)
		return ret;
	ret = mt7601u_mcu_calibrate(dev, MCU_CAL_DPD, dev->dpd_temp);
	if (ret)
		return ret;

	mt7601u_rxdc_cal(dev);

	mt7601u_tssi_dc_gain_cal(dev);

	mt7601u_wr(dev, MT_MAC_SYS_CTRL, mac_ctrl);

	mt7601u_temp_comp(dev, true);

	return 0;
}

int mt7601u_bbp_set_bw(struct mt7601u_dev *dev, int bw)
{
	/* TODO: save-and-restore MAC_SYS_CTRL rather than blind disable enable
	 */
	if (bw != dev->bw) {
		mt76_clear(dev, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_TX |
			   MT_MAC_SYS_CTRL_ENABLE_RX);
		mt76_poll(dev, MT_MAC_STATUS,
			  MT_MAC_STATUS_TX | MT_MAC_STATUS_RX, 0, 500000);
	}

	switch (bw) {
	case MT_BW_20:
		mt7601u_bbp_rmc(dev, 4, 0x18, 0);
		break;
	case MT_BW_40:
		mt7601u_bbp_rmc(dev, 4, 0x18, 0x10);
		break;
	default:
		printk("Error: Wrong BW!\n");
	}

	if (bw != dev->bw)
		mt76_set(dev, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_TX |
			 MT_MAC_SYS_CTRL_ENABLE_RX);
	dev->bw = bw;

	return 0;
}

/**
 * mt7601u_set_rx_path - set rx path in BBP
 * @dev: pointer to device
 * @path: rx path to set values are 0-based
 */
void mt7601u_set_rx_path(struct mt7601u_dev *dev, u8 path)
{
	mt7601u_bbp_rmw(dev, 3, 0x18, path << 3);
}

/**
 * mt7601u_set_tx_dac - set which tx DAC to use
 * @dev: pointer to device
 * @path: DAC index, values are 0-based
 */
void mt7601u_set_tx_dac(struct mt7601u_dev *dev, u8 dac)
{
	mt7601u_bbp_rmc(dev, 1, 0x18, dac << 3);
}

#define RF_REG_PAIR(bank, reg, value)				\
	{ MT_MCU_MEMMAP_RF | (bank) << 16 | (reg), value }

int mt7601u_phy_init(struct mt7601u_dev *dev)
{
	int ret;
	static const struct mt76_reg_pair rf_central[] = {
		/* Bank 0 - for central blocks: BG, PLL, XTAL, LO, ADC/DAC */
		RF_REG_PAIR(0,	 0, 0x02),
		RF_REG_PAIR(0,	 1, 0x01),
		RF_REG_PAIR(0,	 2, 0x11),
		RF_REG_PAIR(0,	 3, 0xff),
		RF_REG_PAIR(0,	 4, 0x0a),
		RF_REG_PAIR(0,	 5, 0x20),
		RF_REG_PAIR(0,	 6, 0x00),
		/* B/G */
		RF_REG_PAIR(0,	 7, 0x00),
		RF_REG_PAIR(0,	 8, 0x00),
		RF_REG_PAIR(0,	 9, 0x00),
		RF_REG_PAIR(0,	10, 0x00),
		RF_REG_PAIR(0,	11, 0x21),
		/* XO */
		RF_REG_PAIR(0,	13, 0x00),		// 40mhz xtal
		//RF_REG_PAIR(0,	13, 0x13),	// 20mhz xtal
		RF_REG_PAIR(0,	14, 0x7c),
		RF_REG_PAIR(0,	15, 0x22),
		RF_REG_PAIR(0,	16, 0x80),
		/* PLL */
		RF_REG_PAIR(0,	17, 0x99),
		RF_REG_PAIR(0,	18, 0x99),
		RF_REG_PAIR(0,	19, 0x09),
		RF_REG_PAIR(0,	20, 0x50),
		RF_REG_PAIR(0,	21, 0xb0),
		RF_REG_PAIR(0,	22, 0x00),
		RF_REG_PAIR(0,	23, 0xc5),
		RF_REG_PAIR(0,	24, 0xfc),
		RF_REG_PAIR(0,	25, 0x40),
		RF_REG_PAIR(0,	26, 0x4d),
		RF_REG_PAIR(0,	27, 0x02),
		RF_REG_PAIR(0,	28, 0x72),
		RF_REG_PAIR(0,	29, 0x01),
		RF_REG_PAIR(0,	30, 0x00),
		RF_REG_PAIR(0,	31, 0x00),
		/* test ports */
		RF_REG_PAIR(0,	32, 0x00),
		RF_REG_PAIR(0,	33, 0x00),
		RF_REG_PAIR(0,	34, 0x23),
		RF_REG_PAIR(0,	35, 0x01), /* change setting to reduce spurs */
		RF_REG_PAIR(0,	36, 0x00),
		RF_REG_PAIR(0,	37, 0x00),
		/* ADC/DAC */
		RF_REG_PAIR(0,	38, 0x00),
		RF_REG_PAIR(0,	39, 0x20),
		RF_REG_PAIR(0,	40, 0x00),
		RF_REG_PAIR(0,	41, 0xd0),
		RF_REG_PAIR(0,	42, 0x1b),
		RF_REG_PAIR(0,	43, 0x02),
		RF_REG_PAIR(0,	44, 0x00),
	};
	static const struct mt76_reg_pair rf_channel[] = {
		RF_REG_PAIR(4,	 0, 0x01),
		RF_REG_PAIR(4,	 1, 0x00),
		RF_REG_PAIR(4,	 2, 0x00),
		RF_REG_PAIR(4,	 3, 0x00),
		/* LDO */
		RF_REG_PAIR(4,	 4, 0x00),
		RF_REG_PAIR(4,	 5, 0x08),
		RF_REG_PAIR(4,	 6, 0x00),
		/* RX */
		RF_REG_PAIR(4,	 7, 0x5b),
		RF_REG_PAIR(4,	 8, 0x52),
		RF_REG_PAIR(4,	 9, 0xb6),
		RF_REG_PAIR(4,	10, 0x57),
		RF_REG_PAIR(4,	11, 0x33),
		RF_REG_PAIR(4,	12, 0x22),
		RF_REG_PAIR(4,	13, 0x3d),
		RF_REG_PAIR(4,	14, 0x3e),
		RF_REG_PAIR(4,	15, 0x13),
		RF_REG_PAIR(4,	16, 0x22),
		RF_REG_PAIR(4,	17, 0x23),
		RF_REG_PAIR(4,	18, 0x02),
		RF_REG_PAIR(4,	19, 0xa4),
		RF_REG_PAIR(4,	20, 0x01),
		RF_REG_PAIR(4,	21, 0x12),
		RF_REG_PAIR(4,	22, 0x80),
		RF_REG_PAIR(4,	23, 0xb3),
		RF_REG_PAIR(4,	24, 0x00), /* reserved */
		RF_REG_PAIR(4,	25, 0x00), /* reserved */
		RF_REG_PAIR(4,	26, 0x00), /* reserved */
		RF_REG_PAIR(4,	27, 0x00), /* reserved */
		/* LOGEN */
		RF_REG_PAIR(4,	28, 0x18),
		RF_REG_PAIR(4,	29, 0xee),
		RF_REG_PAIR(4,	30, 0x6b),
		RF_REG_PAIR(4,	31, 0x31),
		RF_REG_PAIR(4,	32, 0x5d),
		RF_REG_PAIR(4,	33, 0x00), /* reserved */
		/* TX */
		RF_REG_PAIR(4,	34, 0x96),
		RF_REG_PAIR(4,	35, 0x55),
		RF_REG_PAIR(4,	36, 0x08),
		RF_REG_PAIR(4,	37, 0xbb),
		RF_REG_PAIR(4,	38, 0xb3),
		RF_REG_PAIR(4,	39, 0xb3),
		RF_REG_PAIR(4,	40, 0x03),
		RF_REG_PAIR(4,	41, 0x00), /* reserved */
		RF_REG_PAIR(4,	42, 0x00), /* reserved */
		RF_REG_PAIR(4,	43, 0xc5),
		RF_REG_PAIR(4,	44, 0xc5),
		RF_REG_PAIR(4,	45, 0xc5),
		RF_REG_PAIR(4,	46, 0x07),
		RF_REG_PAIR(4,	47, 0xa8),
		RF_REG_PAIR(4,	48, 0xef),
		RF_REG_PAIR(4,	49, 0x1a),
		/* PA */
		RF_REG_PAIR(4,	54, 0x07),
		RF_REG_PAIR(4,	55, 0xa7),
		RF_REG_PAIR(4,	56, 0xcc),
		RF_REG_PAIR(4,	57, 0x14),
		RF_REG_PAIR(4,	58, 0x07),
		RF_REG_PAIR(4,	59, 0xa8),
		RF_REG_PAIR(4,	60, 0xd7),
		RF_REG_PAIR(4,	61, 0x10),
		RF_REG_PAIR(4,	62, 0x1c),
		RF_REG_PAIR(4,	63, 0x00), /* reserved */
	};
	static const struct mt76_reg_pair rf_vga[] = {
		RF_REG_PAIR(5,	 0, 0x47),
		RF_REG_PAIR(5,	 1, 0x00),
		RF_REG_PAIR(5,	 2, 0x00),
		RF_REG_PAIR(5,	 3, 0x08),
		RF_REG_PAIR(5,	 4, 0x04),
		RF_REG_PAIR(5,	 5, 0x20),
		RF_REG_PAIR(5,	 6, 0x3a),
		RF_REG_PAIR(5,	 7, 0x3a),
		RF_REG_PAIR(5,	 8, 0x00),
		RF_REG_PAIR(5,	 9, 0x00),
		RF_REG_PAIR(5,	10, 0x10),
		RF_REG_PAIR(5,	11, 0x10),
		RF_REG_PAIR(5,	12, 0x10),
		RF_REG_PAIR(5,	13, 0x10),
		RF_REG_PAIR(5,	14, 0x10),
		RF_REG_PAIR(5,	15, 0x20),
		RF_REG_PAIR(5,	16, 0x22),
		RF_REG_PAIR(5,	17, 0x7c),
		RF_REG_PAIR(5,	18, 0x00),
		RF_REG_PAIR(5,	19, 0x00),
		RF_REG_PAIR(5,	20, 0x00),
		RF_REG_PAIR(5,	21, 0xf1),
		RF_REG_PAIR(5,	22, 0x11),
		RF_REG_PAIR(5,	23, 0x02),
		RF_REG_PAIR(5,	24, 0x41),
		RF_REG_PAIR(5,	25, 0x20),
		RF_REG_PAIR(5,	26, 0x00),
		RF_REG_PAIR(5,	27, 0xd7),
		RF_REG_PAIR(5,	28, 0xa2),
		RF_REG_PAIR(5,	29, 0x20),
		RF_REG_PAIR(5,	30, 0x49),
		RF_REG_PAIR(5,	31, 0x20),
		RF_REG_PAIR(5,	32, 0x04),
		RF_REG_PAIR(5,	33, 0xf1),
		RF_REG_PAIR(5,	34, 0xa1),
		RF_REG_PAIR(5,	35, 0x01),
		RF_REG_PAIR(5,	41, 0x00),
		RF_REG_PAIR(5,	42, 0x00),
		RF_REG_PAIR(5,	43, 0x00),
		RF_REG_PAIR(5,	44, 0x00),
		RF_REG_PAIR(5,	45, 0x00),
		RF_REG_PAIR(5,	46, 0x00),
		RF_REG_PAIR(5,	47, 0x00),
		RF_REG_PAIR(5,	48, 0x00),
		RF_REG_PAIR(5,	49, 0x00),
		RF_REG_PAIR(5,	50, 0x00),
		RF_REG_PAIR(5,	51, 0x00),
		RF_REG_PAIR(5,	52, 0x00),
		RF_REG_PAIR(5,	53, 0x00),
		RF_REG_PAIR(5,	54, 0x00),
		RF_REG_PAIR(5,	55, 0x00),
		RF_REG_PAIR(5,	56, 0x00),
		RF_REG_PAIR(5,	57, 0x00),
		RF_REG_PAIR(5,	58, 0x31),
		RF_REG_PAIR(5,	59, 0x31),
		RF_REG_PAIR(5,	60, 0x0a),
		RF_REG_PAIR(5,	61, 0x02),
		RF_REG_PAIR(5,	62, 0x00),
		RF_REG_PAIR(5,	63, 0x00),
	};

	dev->rf_pa_mode[0] = mt7601u_rr(dev, MT_RF_PA_MODE_CFG0);
	dev->rf_pa_mode[1] = mt7601u_rr(dev, MT_RF_PA_MODE_CFG1);

	ret = mt7601u_rf_wr(dev, 0, 12, dev->ee->rf_freq_off);
	if (ret)
		return ret;
	ret = mt7601u_write_reg_pairs(dev, 0, rf_central,
				      ARRAY_SIZE(rf_central));
	if (ret)
		return ret;
	ret = mt7601u_write_reg_pairs(dev, 0, rf_channel,
				      ARRAY_SIZE(rf_channel));
	if (ret)
		return ret;
	ret = mt7601u_write_reg_pairs(dev, 0, rf_vga, ARRAY_SIZE(rf_vga));
	if (ret)
		return ret;

	ret = mt7601u_init_cal(dev);
	if (ret)
		return ret;

	dev->prev_pwr_diff = 100;

	INIT_DELAYED_WORK(&dev->cal_work, mt7601u_phy_calibrate);
	INIT_DELAYED_WORK(&dev->freq_cal.work, mt7601u_phy_freq_cal);

	return 0;
}
