/*
 ***************************************************************************
 * MediaTek Inc.
 *
 * All rights reserved. source code is an unpublished work and the
 * use of a copyright notice does not imply otherwise. This source code
 * contains confidential trade secret material of MediaTek. Any attemp
 * or participation in deciphering, decoding, reverse engineering or in any
 * way altering the source code is stricitly prohibited, unless the prior
 * written consent of MediaTek, Inc. is obtained.
 ***************************************************************************

	Module Name:
	mt_rf.c
*/

#include "rt_config.h"

int mt_rf_write(
	struct rtmp_adapter *ad,
	u8 rf_idx,
	u16 offset,
	u32 data)
{
	u32 i = 0;
	u32 value;
	int ret = 0;

#ifdef RTMP_MAC_USB
	if (IS_USB_INF(ad)) {
		RTMP_SEM_EVENT_WAIT(&ad->reg_atomic, ret);
		if (ret != 0) {
			DBGPRINT(RT_DEBUG_ERROR, ("reg_atomic get failed(ret=%d)\n", ret));
			return STATUS_UNSUCCESSFUL;
		}
	}
#endif /* RTMP_MAC_USB */

	/* rf data */
	mt7612u_write32(ad, W_RFDATA, data);

	/* rf control */
	value = mt7612u_read32(ad, RF_CTRL);

	/* rf address */
	value &= ~RF_ADDR_MASK;
	value |= RF_ADDR(offset);

	/* write control */
	value |= RF_R_W_CTRL;

	/* rf index */
	value &= ~RF_IDX_MASK;
	value |= RF_IDX(rf_idx);

	mt7612u_write32(ad, RF_CTRL, value);

	do {
		value = mt7612u_read32(ad, RF_CTRL);

		if (RF_READY(value))
			break;
		i++;
		//RtmpOsMsDelay(1);
		RtmpusecDelay(50);
	//} while ((i < MAX_BUSY_COUNT) && (!RTMP_TEST_FLAG(ad, fRTMP_ADAPTER_NIC_NOT_EXIST)));
	} while ((i < MAX_BUSY_COUNT_US) && (!RTMP_TEST_FLAG(ad, fRTMP_ADAPTER_NIC_NOT_EXIST)));

	//if ((i == MAX_BUSY_COUNT) || (RTMP_TEST_FLAG(ad, fRTMP_ADAPTER_NIC_NOT_EXIST))) {
	if ((i == MAX_BUSY_COUNT_US) || (RTMP_TEST_FLAG(ad, fRTMP_ADAPTER_NIC_NOT_EXIST))) {
		DBGPRINT_RAW(RT_DEBUG_ERROR, ("Retry count exhausted or device removed!!!\n"));
		ret = STATUS_UNSUCCESSFUL;
	}

#ifdef RTMP_MAC_USB
	if (IS_USB_INF(ad))
		RTMP_SEM_EVENT_UP(&ad->reg_atomic);
#endif /* RTMP_MAC_USB */

	return ret;
}

u32 mt_rf_read(struct rtmp_adapter *ad, u8 rf_idx, u16 offset)
{
	u32 i = 0;
	u32 data = 0;	/* ULLI : return value for this function */
	u32 value;
	int ret;

#ifdef RTMP_MAC_USB
	if (IS_USB_INF(ad)) {
		RTMP_SEM_EVENT_WAIT(&ad->reg_atomic, ret);
		if (ret != 0) {
			DBGPRINT(RT_DEBUG_ERROR, ("reg_atomic get failed(ret=%d)\n", ret));
			return -1;
		}
	}
#endif /* RTMP_MAC_USB */

	/* rf control */
	value = mt7612u_read32(ad, RF_CTRL);

	/* rf address */
	value &= ~RF_ADDR_MASK;
	value |= RF_ADDR(offset);

	/* read control */
	value &= ~RF_R_W_CTRL;

	/* rf index */
	value &= ~RF_IDX_MASK;
	value |= RF_IDX(rf_idx);

	mt7612u_write32(ad, RF_CTRL, value);

	do {
		value = mt7612u_read32(ad, RF_CTRL);

		if (RF_READY(value))
			break;
		i++;
		//RtmpOsMsDelay(1);
		RtmpusecDelay(50);
	//} while ((i < MAX_BUSY_COUNT) && (!RTMP_TEST_FLAG(ad, fRTMP_ADAPTER_NIC_NOT_EXIST)));
	} while ((i < MAX_BUSY_COUNT_US) && (!RTMP_TEST_FLAG(ad, fRTMP_ADAPTER_NIC_NOT_EXIST)));

	//if ((i == MAX_BUSY_COUNT) || (RTMP_TEST_FLAG(ad, fRTMP_ADAPTER_NIC_NOT_EXIST))) {
	if ((i == MAX_BUSY_COUNT_US) || (RTMP_TEST_FLAG(ad, fRTMP_ADAPTER_NIC_NOT_EXIST))) {
		DBGPRINT_RAW(RT_DEBUG_ERROR, ("Retry count exhausted or device removed!!!\n"));
		ret = STATUS_UNSUCCESSFUL;
		goto done;
	}

	/* rf data */
	data = mt7612u_read32(ad, R_RFDATA);

done:
#ifdef RTMP_MAC_USB
	if (IS_USB_INF(ad))
		RTMP_SEM_EVENT_UP(&ad->reg_atomic);
#endif /* RTMP_MAC_USB */

	return data;
}
