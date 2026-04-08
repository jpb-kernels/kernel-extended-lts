// SPDX-License-Identifier: (GPL-2.0 OR MIT)
/* Google virtual Ethernet (gve) driver
 *
 * Copyright (C) 2015-2019 Google, Inc.
 */

#include <linux/rtnetlink.h>
#include "gve.h"

static void gve_get_drvinfo(struct net_device *netdev,
			    struct ethtool_drvinfo *info)
{
	struct gve_priv *priv = netdev_priv(netdev);

	strlcpy(info->driver, "gve", sizeof(info->driver));
	strlcpy(info->version, gve_version_str, sizeof(info->version));
	strlcpy(info->bus_info, pci_name(priv->pdev), sizeof(info->bus_info));
}

static void gve_set_msglevel(struct net_device *netdev, u32 value)
{
	struct gve_priv *priv = netdev_priv(netdev);

	priv->msg_enable = value;
}

static u32 gve_get_msglevel(struct net_device *netdev)
{
	struct gve_priv *priv = netdev_priv(netdev);

	return priv->msg_enable;
}

static const char gve_gstrings_main_stats[][ETH_GSTRING_LEN] = {
	"rx_packets", "tx_packets", "rx_bytes", "tx_bytes",
	"rx_dropped", "tx_dropped", "tx_timeouts",
};

#define GVE_MAIN_STATS_LEN  ARRAY_SIZE(gve_gstrings_main_stats)
#define NUM_GVE_TX_CNTS	5
#define NUM_GVE_RX_CNTS	2

static void gve_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	struct gve_priv *priv = netdev_priv(netdev);
	char *s = (char *)data;
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	memcpy(s, *gve_gstrings_main_stats,
	       sizeof(gve_gstrings_main_stats));
	s += sizeof(gve_gstrings_main_stats);
	for (i = 0; i < priv->rx_cfg.num_queues; i++) {
		snprintf(s, ETH_GSTRING_LEN, "rx_desc_cnt[%u]", i);
		s += ETH_GSTRING_LEN;
		snprintf(s, ETH_GSTRING_LEN, "rx_desc_fill_cnt[%u]", i);
		s += ETH_GSTRING_LEN;
	}
	for (i = 0; i < priv->tx_cfg.num_queues; i++) {
		snprintf(s, ETH_GSTRING_LEN, "tx_req[%u]", i);
		s += ETH_GSTRING_LEN;
		snprintf(s, ETH_GSTRING_LEN, "tx_done[%u]", i);
		s += ETH_GSTRING_LEN;
		snprintf(s, ETH_GSTRING_LEN, "tx_wake[%u]", i);
		s += ETH_GSTRING_LEN;
		snprintf(s, ETH_GSTRING_LEN, "tx_stop[%u]", i);
		s += ETH_GSTRING_LEN;
		snprintf(s, ETH_GSTRING_LEN, "tx_event_counter[%u]", i);
		s += ETH_GSTRING_LEN;
	}
}

static int gve_get_sset_count(struct net_device *netdev, int sset)
{
	struct gve_priv *priv = netdev_priv(netdev);

	switch (sset) {
	case ETH_SS_STATS:
		return GVE_MAIN_STATS_LEN +
		       (priv->rx_cfg.num_queues * NUM_GVE_RX_CNTS) +
		       (priv->tx_cfg.num_queues * NUM_GVE_TX_CNTS);
	default:
		return -EOPNOTSUPP;
	}
}

static void
gve_get_ethtool_stats(struct net_device *netdev,
		      struct ethtool_stats *stats, u64 *data)
{
	u64 tmp_rx_pkts, tmp_rx_bytes, tmp_rx_skb_alloc_fail,	tmp_rx_buf_alloc_fail,
		tmp_rx_desc_err_dropped_pkt, tmp_tx_pkts, tmp_tx_bytes;
	u64 rx_buf_alloc_fail, rx_desc_err_dropped_pkt, rx_pkts,
		rx_skb_alloc_fail, rx_bytes, tx_pkts, tx_bytes;
	int rx_base_stats_idx, max_rx_stats_idx, max_tx_stats_idx;
	int stats_idx, stats_region_len, nic_stats_len;
	struct stats *report_stats;
	int *rx_qid_to_stats_idx;
	int *tx_qid_to_stats_idx;
	struct gve_priv *priv;
	bool skip_nic_stats;
	unsigned int start;
	int ring;
	int i;

	ASSERT_RTNL();

	for (rx_pkts = 0, rx_bytes = 0, ring = 0;
	     ring < priv->rx_cfg.num_queues; ring++) {
		if (priv->rx) {
			do {
				start =
				  u64_stats_fetch_begin(&priv->rx[ring].statss);
				rx_pkts += priv->rx[ring].rpackets;
				rx_bytes += priv->rx[ring].rbytes;
			} while (u64_stats_fetch_retry(&priv->rx[ring].statss,
						       start));
		}
	}
	for (tx_pkts = 0, tx_bytes = 0, ring = 0;
	     ring < priv->tx_cfg.num_queues; ring++) {
		if (priv->tx) {
			do {
				start =
				  u64_stats_fetch_begin(&priv->tx[ring].statss);
				tx_pkts += priv->tx[ring].pkt_done;
				tx_bytes += priv->tx[ring].bytes_done;
			} while (u64_stats_fetch_retry(&priv->tx[ring].statss,
						       start));
		}
	}

	i = 0;
	data[i++] = rx_pkts;
	data[i++] = tx_pkts;
	data[i++] = rx_bytes;
	data[i++] = tx_bytes;
	/* total rx dropped packets */
	data[i++] = rx_skb_alloc_fail + rx_desc_err_dropped_pkt;
	/* Skip tx_dropped */
	i++;

	data[i++] = priv->tx_timeo_cnt;
	i = GVE_MAIN_STATS_LEN;

	rx_base_stats_idx = 0;
	max_rx_stats_idx = 0;
	max_tx_stats_idx = 0;
	stats_region_len = priv->stats_report_len -
				sizeof(struct gve_stats_report);
	nic_stats_len = (NIC_RX_STATS_REPORT_NUM * priv->rx_cfg.num_queues +
		NIC_TX_STATS_REPORT_NUM * priv->tx_cfg.num_queues) *
		sizeof(struct stats);
	if (unlikely((stats_region_len -
				nic_stats_len) % sizeof(struct stats))) {
		net_err_ratelimited("Starting index of NIC stats should be multiple of stats size");
	} else {
		/* For rx cross-reporting stats,
		 * start from nic rx stats in report
		 */
		rx_base_stats_idx = (stats_region_len - nic_stats_len) /
							sizeof(struct stats);
		max_rx_stats_idx = NIC_RX_STATS_REPORT_NUM *
			priv->rx_cfg.num_queues +
			rx_base_stats_idx;
		max_tx_stats_idx = NIC_TX_STATS_REPORT_NUM *
			priv->tx_cfg.num_queues +
			max_rx_stats_idx;
	}
	/* Preprocess the stats report for rx, map queue id to start index */
	skip_nic_stats = false;
	for (stats_idx = rx_base_stats_idx; stats_idx < max_rx_stats_idx;
		stats_idx += NIC_RX_STATS_REPORT_NUM) {
		u32 stat_name = be32_to_cpu(report_stats[stats_idx].stat_name);
		u32 queue_id = be32_to_cpu(report_stats[stats_idx].queue_id);

		if (stat_name == 0) {
			/* no stats written by NIC yet */
			skip_nic_stats = true;
			break;
		}
		rx_qid_to_stats_idx[queue_id] = stats_idx;
	}
	/* walk RX rings */
	if (priv->rx) {
		for (ring = 0; ring < priv->rx_cfg.num_queues; ring++) {
			struct gve_rx_ring *rx = &priv->rx[ring];

			data[i++] = rx->cnt;
			data[i++] = rx->fill_cnt;
			data[i++] = rx->cnt;
			do {
				start =
				  u64_stats_fetch_begin_irq(&priv->rx[ring].statss);
				tmp_rx_bytes = rx->rbytes;
				tmp_rx_skb_alloc_fail = rx->rx_skb_alloc_fail;
				tmp_rx_buf_alloc_fail = rx->rx_buf_alloc_fail;
				tmp_rx_desc_err_dropped_pkt =
					rx->rx_desc_err_dropped_pkt;
			} while (u64_stats_fetch_retry_irq(&priv->rx[ring].statss,
						       start));
			data[i++] = tmp_rx_bytes;
			/* rx dropped packets */
			data[i++] = tmp_rx_skb_alloc_fail +
				tmp_rx_desc_err_dropped_pkt;
			data[i++] = rx->rx_copybreak_pkt;
			data[i++] = rx->rx_copied_pkt;
			/* stats from NIC */
			if (skip_nic_stats) {
				/* skip NIC rx stats */
				i += NIC_RX_STATS_REPORT_NUM;
				continue;
			}
			for (j = 0; j < NIC_RX_STATS_REPORT_NUM; j++) {
				u64 value =
				be64_to_cpu(report_stats[rx_qid_to_stats_idx[ring] + j].value);

				data[i++] = value;
			}
		}
	} else {
		i += priv->rx_cfg.num_queues * NUM_GVE_RX_CNTS;
	}

	skip_nic_stats = false;
	/* NIC TX stats start right after NIC RX stats */
	for (stats_idx = max_rx_stats_idx; stats_idx < max_tx_stats_idx;
		stats_idx += NIC_TX_STATS_REPORT_NUM) {
		u32 stat_name = be32_to_cpu(report_stats[stats_idx].stat_name);
		u32 queue_id = be32_to_cpu(report_stats[stats_idx].queue_id);

		if (stat_name == 0) {
			/* no stats written by NIC yet */
			skip_nic_stats = true;
			break;
		}
		tx_qid_to_stats_idx[queue_id] = stats_idx;
	}
	/* walk TX rings */
	if (priv->tx) {
		for (ring = 0; ring < priv->tx_cfg.num_queues; ring++) {
			struct gve_tx_ring *tx = &priv->tx[ring];

			data[i++] = tx->req;
			data[i++] = tx->done;
			data[i++] = tx->wake_queue;
			data[i++] = tx->stop_queue;
			data[i++] = be32_to_cpu(gve_tx_load_event_counter(priv,
									  tx));
		}
	} else {
		i += priv->tx_cfg.num_queues * NUM_GVE_TX_CNTS;
	}
}

static void gve_get_channels(struct net_device *netdev,
			     struct ethtool_channels *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);

	cmd->max_rx = priv->rx_cfg.max_queues;
	cmd->max_tx = priv->tx_cfg.max_queues;
	cmd->max_other = 0;
	cmd->max_combined = 0;
	cmd->rx_count = priv->rx_cfg.num_queues;
	cmd->tx_count = priv->tx_cfg.num_queues;
	cmd->other_count = 0;
	cmd->combined_count = 0;
}

static int gve_set_channels(struct net_device *netdev,
			    struct ethtool_channels *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);
	struct gve_queue_config new_tx_cfg = priv->tx_cfg;
	struct gve_queue_config new_rx_cfg = priv->rx_cfg;
	struct ethtool_channels old_settings;
	int new_tx = cmd->tx_count;
	int new_rx = cmd->rx_count;

	gve_get_channels(netdev, &old_settings);

	/* Changing combined is not allowed allowed */
	if (cmd->combined_count != old_settings.combined_count)
		return -EINVAL;

	if (!new_rx || !new_tx)
		return -EINVAL;

	if (!netif_carrier_ok(netdev)) {
		priv->tx_cfg.num_queues = new_tx;
		priv->rx_cfg.num_queues = new_rx;
		return 0;
	}

	new_tx_cfg.num_queues = new_tx;
	new_rx_cfg.num_queues = new_rx;

	return gve_adjust_queues(priv, new_rx_cfg, new_tx_cfg);
}

static void gve_get_ringparam(struct net_device *netdev,
			      struct ethtool_ringparam *cmd)
{
	struct gve_priv *priv = netdev_priv(netdev);

	cmd->rx_max_pending = priv->rx_desc_cnt;
	cmd->tx_max_pending = priv->tx_desc_cnt;
	cmd->rx_pending = priv->rx_desc_cnt;
	cmd->tx_pending = priv->tx_desc_cnt;
}

static int gve_user_reset(struct net_device *netdev, u32 *flags)
{
	struct gve_priv *priv = netdev_priv(netdev);

	if (*flags == ETH_RESET_ALL) {
		*flags = 0;
		return gve_reset(priv, true);
	}

	return -EOPNOTSUPP;
}

const struct ethtool_ops gve_ethtool_ops = {
	.get_drvinfo = gve_get_drvinfo,
	.get_strings = gve_get_strings,
	.get_sset_count = gve_get_sset_count,
	.get_ethtool_stats = gve_get_ethtool_stats,
	.set_msglevel = gve_set_msglevel,
	.get_msglevel = gve_get_msglevel,
	.set_channels = gve_set_channels,
	.get_channels = gve_get_channels,
	.get_link = ethtool_op_get_link,
	.get_ringparam = gve_get_ringparam,
	.reset = gve_user_reset,
};
