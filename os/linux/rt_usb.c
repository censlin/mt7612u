/*
 ***************************************************************************
 * Ralink Tech Inc.
 * 4F, No. 2 Technology	5th	Rd.
 * Science-based Industrial	Park
 * Hsin-chu, Taiwan, R.O.C.
 *
 * (c) Copyright 2002-2006, Ralink Technology, Inc.
 *
 * All rights reserved.	Ralink's source	code is	an unpublished work	and	the
 * use of a	copyright notice does not imply	otherwise. This	source code
 * contains	confidential trade secret material of Ralink Tech. Any attemp
 * or participation	in deciphering,	decoding, reverse engineering or in	any
 * way altering	the	source code	is stricitly prohibited, unless	the	prior
 * written consent of Ralink Technology, Inc. is obtained.
 ***************************************************************************

	Module Name:
	rtusb_bulk.c

	Abstract:

	Revision History:
	Who			When		What
	--------	----------	----------------------------------------------
	Name		Date		Modification logs

*/

#include "rt_config.h"


/*
========================================================================
Routine Description:
    Create kernel threads & tasklets.

Arguments:
    *net_dev			Pointer to wireless net device interface

Return Value:
	NDIS_STATUS_SUCCESS
	NDIS_STATUS_FAILURE

Note:
========================================================================
*/
int  RtmpMgmtTaskInit(
	IN struct rtmp_adapter *pAd)
{
	RTMP_OS_TASK *pTask;
	int status;

	/*
		Creat TimerQ Thread, We need init timerQ related structure before create the timer thread.
	*/
	RtmpTimerQInit(pAd);

	pTask = &pAd->timerTask;
	RTMP_OS_TASK_INIT(pTask, "RtmpTimerTask", pAd);
	status = RtmpOSTaskAttach(pTask, RtmpTimerQThread, (ULONG)pTask);
	if (status == NDIS_STATUS_FAILURE)
	{
		printk (KERN_WARNING "%s: unable to start RtmpTimerQThread\n", RTMP_OS_NETDEV_GET_DEVNAME(pAd->net_dev));
		return NDIS_STATUS_FAILURE;
	}

	/* Creat Command Thread */
	pTask = &pAd->cmdQTask;
	RTMP_OS_TASK_INIT(pTask, "RtmpCmdQTask", pAd);
	status = RtmpOSTaskAttach(pTask, RTUSBCmdThread, (ULONG)pTask);
	if (status == NDIS_STATUS_FAILURE)
	{
		printk (KERN_WARNING "%s: unable to start RTUSBCmdThread\n", RTMP_OS_NETDEV_GET_DEVNAME(pAd->net_dev));
		return NDIS_STATUS_FAILURE;
	}


	return NDIS_STATUS_SUCCESS;
}



/*
========================================================================
Routine Description:
    Close kernel threads.

Arguments:
	*pAd				the raxx interface data pointer

Return Value:
    NONE

Note:
========================================================================
*/
VOID RtmpMgmtTaskExit(
	IN struct rtmp_adapter *pAd)
{
	INT			ret;
	RTMP_OS_TASK	*pTask;

	/* Sleep 50 milliseconds so pending io might finish normally */
	RtmpusecDelay(50000);

	/* We want to wait until all pending receives and sends to the */
	/* device object. We cancel any */
	/* irps. Wait until sends and receives have stopped. */
	RTUSBCancelPendingIRPs(pAd);

	/* We need clear timerQ related structure before exits of the timer thread. */
	RtmpTimerQExit(pAd);

	/* Terminate cmdQ thread */
	pTask = &pAd->cmdQTask;
	RTMP_OS_TASK_LEGALITY(pTask)
	{
		NdisAcquireSpinLock(&pAd->CmdQLock);
		pAd->CmdQ.CmdQState = RTMP_TASK_STAT_STOPED;
		NdisReleaseSpinLock(&pAd->CmdQLock);

		/*RTUSBCMDUp(&pAd->cmdQTask); */
		ret = RtmpOSTaskKill(pTask);
		if (ret == NDIS_STATUS_FAILURE)
		{
/*			DBGPRINT(RT_DEBUG_ERROR, ("%s: kill task(%s) failed!\n", */
/*					RTMP_OS_NETDEV_GET_DEVNAME(pAd->net_dev), pTask->taskName)); */
			DBGPRINT(RT_DEBUG_ERROR, ("kill command task failed!\n"));
		}
		pAd->CmdQ.CmdQState = RTMP_TASK_STAT_UNKNOWN;
	}

	/* Terminate timer thread */
	pTask = &pAd->timerTask;
	ret = RtmpOSTaskKill(pTask);
	if (ret == NDIS_STATUS_FAILURE)
	{
/*		DBGPRINT(RT_DEBUG_ERROR, ("%s: kill task(%s) failed!\n", */
/*					RTMP_OS_NETDEV_GET_DEVNAME(pAd->net_dev), pTask->taskName)); */
		DBGPRINT(RT_DEBUG_ERROR, ("kill timer task failed!\n"));
	}


}


static void rtusb_dataout_complete(unsigned long data)
{
	struct rtmp_adapter *	pAd;
	purbb_t				pUrb;
	struct os_cookie *		pObj;
	PHT_TX_CONTEXT		pHTTXContext;
	UCHAR				BulkOutPipeId;
	int 		Status;
	unsigned long		IrqFlags;


	pUrb			= (purbb_t)data;
/*	pHTTXContext	= (PHT_TX_CONTEXT)pUrb->context; */
	pHTTXContext	= (PHT_TX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	Status			= RTMP_USB_URB_STATUS_GET(pUrb);
	pAd				= pHTTXContext->pAd;
	pObj 			= pAd->OS_Cookie;
/*	Status			= pUrb->status; */

	/* Store BulkOut PipeId */
	BulkOutPipeId = pHTTXContext->BulkOutPipeId;
	pAd->BulkOutDataOneSecCount++;

	/*DBGPRINT(RT_DEBUG_LOUD, ("Done-B(%d):I=0x%lx, CWPos=%ld, NBPos=%ld, ENBPos=%ld, bCopy=%d!\n", BulkOutPipeId, in_interrupt(), pHTTXContext->CurWritePosition, */
	/*		pHTTXContext->NextBulkOutPosition, pHTTXContext->ENextBulkOutPosition, pHTTXContext->bCopySavePad)); */

	RTMP_IRQ_LOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);
	pAd->BulkOutPending[BulkOutPipeId] = FALSE;
	pHTTXContext->IRPPending = FALSE;
	pAd->watchDogTxPendingCnt[BulkOutPipeId] = 0;

	if (Status == USB_ST_NOERROR)
	{
		pAd->BulkOutComplete++;

		RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);

		pAd->Counters8023.GoodTransmits++;
		/*RTMP_IRQ_LOCK(&pAd->TxContextQueueLock[BulkOutPipeId], IrqFlags); */
		FREE_HTTX_RING(pAd, BulkOutPipeId, pHTTXContext);
		/*RTMP_IRQ_UNLOCK(&pAd->TxContextQueueLock[BulkOutPipeId], IrqFlags); */

#ifdef UAPSD_SUPPORT
#ifdef CONFIG_AP_SUPPORT
		IF_DEV_CONFIG_OPMODE_ON_AP(pAd)
		{
			UAPSD_UnTagFrame(pAd, BulkOutPipeId, pHTTXContext->NextBulkOutPosition, pHTTXContext->ENextBulkOutPosition);
		}
#endif /* CONFIG_AP_SUPPORT */
#endif /* UAPSD_SUPPORT */

	}
	else	/* STATUS_OTHER */
	{
		u8 *pBuf;

		pAd->BulkOutCompleteOther++;

		pBuf = &pHTTXContext->TransferBuffer->field.WirelessPacket[pHTTXContext->NextBulkOutPosition];

		if (!RTMP_TEST_FLAG(pAd, (fRTMP_ADAPTER_RESET_IN_PROGRESS |
									fRTMP_ADAPTER_HALT_IN_PROGRESS |
									fRTMP_ADAPTER_NIC_NOT_EXIST |
									fRTMP_ADAPTER_BULKOUT_RESET)))
		{
			RTMP_SET_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET);
			pAd->bulkResetPipeid = BulkOutPipeId;
			pAd->bulkResetReq[BulkOutPipeId] = pAd->BulkOutReq;
		}
		RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[BulkOutPipeId], IrqFlags);

		DBGPRINT_RAW(RT_DEBUG_ERROR, ("BulkOutDataPacket failed: ReasonCode=%d!\n", Status));
		DBGPRINT_RAW(RT_DEBUG_ERROR, ("\t>>BulkOut Req=0x%lx, Complete=0x%lx, Other=0x%lx\n", pAd->BulkOutReq, pAd->BulkOutComplete, pAd->BulkOutCompleteOther));
		DBGPRINT_RAW(RT_DEBUG_ERROR, ("\t>>BulkOut Header:%x %x %x %x %x %x %x %x\n", pBuf[0], pBuf[1], pBuf[2], pBuf[3], pBuf[4], pBuf[5], pBuf[6], pBuf[7]));
		/*DBGPRINT_RAW(RT_DEBUG_ERROR, (">>BulkOutCompleteCancel=0x%x, BulkOutCompleteOther=0x%x\n", pAd->BulkOutCompleteCancel, pAd->BulkOutCompleteOther)); */

	}

	/* */
	/* bInUse = TRUE, means some process are filling TX data, after that must turn on bWaitingBulkOut */
	/* bWaitingBulkOut = TRUE, means the TX data are waiting for bulk out. */
	/* */
	/*RTMP_IRQ_LOCK(&pAd->TxContextQueueLock[BulkOutPipeId], IrqFlags); */
	if (((pHTTXContext->ENextBulkOutPosition != pHTTXContext->CurWritePosition) &&
		(pHTTXContext->ENextBulkOutPosition != (pHTTXContext->CurWritePosition+8)) &&
		!RTUSB_TEST_BULK_FLAG(pAd, (fRTUSB_BULK_OUT_DATA_FRAG << BulkOutPipeId)))
#ifdef USB_BULK_BUF_ALIGMENT
				||	(pHTTXContext->NextBulkIdx != pHTTXContext->CurWriteIdx)
#endif /* USB_BULK_BUF_ALIGMENT */
	)
	{
		/* Indicate There is data avaliable */
		RTUSB_SET_BULK_FLAG(pAd, (fRTUSB_BULK_OUT_DATA_NORMAL << BulkOutPipeId));
	}
	/*RTMP_IRQ_UNLOCK(&pAd->TxContextQueueLock[BulkOutPipeId], IrqFlags); */

	/* Always call Bulk routine, even reset bulk. */
	/* The protection of rest bulk should be in BulkOut routine */
	RTUSBKickBulkOut(pAd);
}


static void rtusb_null_frame_done_tasklet(unsigned long data)
{
	struct rtmp_adapter *pAd;
	PTX_CONTEXT		pNullContext;
	purbb_t			pUrb;
	int 	Status;
	unsigned long	irqFlag;


	pUrb			= (purbb_t)data;
/*	pNullContext	= (PTX_CONTEXT)pUrb->context; */
	pNullContext	= (PTX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	Status			= RTMP_USB_URB_STATUS_GET(pUrb);
	pAd 			= pNullContext->pAd;
/*	Status 			= pUrb->status; */

	/* Reset Null frame context flags */
	RTMP_IRQ_LOCK(&pAd->BulkOutLock[0], irqFlag);
	pNullContext->IRPPending 	= FALSE;
	pNullContext->InUse 		= FALSE;
	pAd->BulkOutPending[0] = FALSE;
	pAd->watchDogTxPendingCnt[0] = 0;

	if (Status == USB_ST_NOERROR)
	{
		RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[0], irqFlag);

		RTMPDeQueuePacket(pAd, FALSE, NUM_OF_TX_RING, MAX_TX_PROCESS);
	}
	else	/* STATUS_OTHER */
	{
		if ((!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_RESET_IN_PROGRESS)) &&
			(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_HALT_IN_PROGRESS)) &&
			(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NIC_NOT_EXIST)) &&
			(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET)))
		{
			DBGPRINT_RAW(RT_DEBUG_ERROR, ("Bulk Out Null Frame Failed, ReasonCode=%d!\n", Status));
			RTMP_SET_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET);
			pAd->bulkResetPipeid = (MGMTPIPEIDX | BULKOUT_MGMT_RESET_FLAG);
			RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[0], irqFlag);
			RTEnqueueInternalCmd(pAd, CMDTHREAD_RESET_BULK_OUT, NULL, 0);
		}
		else
		{
			RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[0], irqFlag);
		}
	}

	/* Always call Bulk routine, even reset bulk. */
	/* The protectioon of rest bulk should be in BulkOut routine */
	RTUSBKickBulkOut(pAd);
}


static void rtusb_pspoll_frame_done_tasklet(unsigned long data)
{
	struct rtmp_adapter *pAd;
	PTX_CONTEXT		pPsPollContext;
	purbb_t			pUrb;
	int 	Status;



	pUrb			= (purbb_t)data;
/*	pPsPollContext	= (PTX_CONTEXT)pUrb->context; */
	pPsPollContext	= (PTX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	Status			= RTMP_USB_URB_STATUS_GET(pUrb);
	pAd				= pPsPollContext->pAd;
/*	Status			= pUrb->status; */

	/* Reset PsPoll context flags */
	pPsPollContext->IRPPending	= FALSE;
	pPsPollContext->InUse		= FALSE;
	pAd->watchDogTxPendingCnt[0] = 0;

	if (Status == USB_ST_NOERROR)
	{
		RTMPDeQueuePacket(pAd, FALSE, NUM_OF_TX_RING, MAX_TX_PROCESS);
	}
	else /* STATUS_OTHER */
	{
		if ((!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_RESET_IN_PROGRESS)) &&
			(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_HALT_IN_PROGRESS)) &&
			(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NIC_NOT_EXIST)) &&
			(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET)))
		{
			DBGPRINT_RAW(RT_DEBUG_ERROR, ("Bulk Out PSPoll Failed\n"));
			RTMP_SET_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET);
			pAd->bulkResetPipeid = (MGMTPIPEIDX | BULKOUT_MGMT_RESET_FLAG);
			RTEnqueueInternalCmd(pAd, CMDTHREAD_RESET_BULK_OUT, NULL, 0);
		}
	}

	RTMP_SEM_LOCK(&pAd->BulkOutLock[0]);
	pAd->BulkOutPending[0] = FALSE;
	RTMP_SEM_UNLOCK(&pAd->BulkOutLock[0]);

	/* Always call Bulk routine, even reset bulk. */
	/* The protectioon of rest bulk should be in BulkOut routine */
	RTUSBKickBulkOut(pAd);

}


/*
========================================================================
Routine Description:
    Handle received packets.

Arguments:
	data				- URB information pointer

Return Value:
    None

Note:
========================================================================
*/
static void rx_done_tasklet(unsigned long data)
{
	purbb_t 			pUrb;
	PRX_CONTEXT			pRxContext;
	struct rtmp_adapter *	pAd;
	int 		Status;
	unsigned int		IrqFlags;

	pUrb		= (purbb_t)data;
/*	pRxContext	= (PRX_CONTEXT)pUrb->context; */
	pRxContext	= (PRX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	Status		= RTMP_USB_URB_STATUS_GET(pUrb);
	pAd 		= pRxContext->pAd;
/*	Status = pUrb->status; */


	RTMP_IRQ_LOCK(&pAd->BulkInLock, IrqFlags);
	pRxContext->InUse = FALSE;
	pRxContext->IRPPending = FALSE;
	pRxContext->BulkInOffset += RTMP_USB_URB_LEN_GET(pUrb); /*pUrb->actual_length; */
	/*NdisInterlockedDecrement(&pAd->PendingRx); */

	if (pAd->PendingRx > 0)
		pAd->PendingRx--;

	if (Status == USB_ST_NOERROR)
	{
		pAd->BulkInComplete++;
		pAd->NextRxBulkInPosition = 0;
		if (pRxContext->BulkInOffset)	/* As jan's comment, it may bulk-in success but size is zero. */
		{
			pRxContext->Readable = TRUE;
			INC_RING_INDEX(pAd->NextRxBulkInIndex, RX_RING_SIZE);
		}
		RTMP_IRQ_UNLOCK(&pAd->BulkInLock, IrqFlags);
	}
	else	 /* STATUS_OTHER */
	{
		pAd->BulkInCompleteFail++;
		/* Still read this packet although it may comtain wrong bytes. */
		pRxContext->Readable = FALSE;
		RTMP_IRQ_UNLOCK(&pAd->BulkInLock, IrqFlags);

		/* Parsing all packets. because after reset, the index will reset to all zero. */
		if ((!RTMP_TEST_FLAG(pAd, (fRTMP_ADAPTER_RESET_IN_PROGRESS |
									fRTMP_ADAPTER_BULKIN_RESET |
									fRTMP_ADAPTER_HALT_IN_PROGRESS |
									fRTMP_ADAPTER_RADIO_OFF |
									fRTMP_ADAPTER_NIC_NOT_EXIST))))
		{

			DBGPRINT_RAW(RT_DEBUG_ERROR, ("Bulk In Failed. Status=%d, BIIdx=0x%x, BIRIdx=0x%x, actual_length= 0x%x\n",
							Status, pAd->NextRxBulkInIndex, pAd->NextRxBulkInReadIndex, RTMP_USB_URB_LEN_GET(pRxContext->pUrb))); /*->actual_length)); */

			RTMP_SET_FLAG(pAd, fRTMP_ADAPTER_BULKIN_RESET);
			RTEnqueueInternalCmd(pAd, CMDTHREAD_RESET_BULK_IN, NULL, 0);
		}
	}

	ASSERT((pRxContext->InUse == pRxContext->IRPPending));

	RTUSBBulkReceive(pAd);


	return;

}



static void rtusb_mgmt_dma_done_tasklet(unsigned long data)
{
	struct rtmp_adapter *	pAd;
	PTX_CONTEXT		pMLMEContext;
	int				index;
	struct sk_buff *pPacket;
	purbb_t			pUrb;
	int 	Status;
	unsigned long	IrqFlags;
	HEADER_802_11  *pHeader;


	pUrb			= (purbb_t)data;
/*	pMLMEContext	= (PTX_CONTEXT)pUrb->context; */
	pMLMEContext	= (PTX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	Status			= RTMP_USB_URB_STATUS_GET(pUrb);
	pAd 			= pMLMEContext->pAd;
/*	Status			= pUrb->status; */
	index 			= pMLMEContext->SelfIdx;

	ASSERT((pAd->MgmtRing.TxDmaIdx == index));

	RTMP_IRQ_LOCK(&pAd->BulkOutLock[MGMTPIPEIDX], IrqFlags);


#ifdef UAPSD_SUPPORT
	/* Qos Null frame with EOSP shall have valid Wcid value. reference RtmpUSBMgmtKickOut() API. */
	/* otherwise will be value of MCAST_WCID. */
#ifdef CONFIG_AP_SUPPORT
        IF_DEV_CONFIG_OPMODE_ON_AP(pAd)
	{
		/* Qos Null frame with EOSP shall have valid Wcid value. reference RtmpUSBMgmtKickOut() API. */
		/* otherwise will be value of MCAST_WCID. */
		if ((pMLMEContext->Wcid != MCAST_WCID) && (pMLMEContext->Wcid < MAX_LEN_OF_MAC_TABLE))
		{
			MAC_TABLE_ENTRY *pEntry = &pAd->MacTab.Content[pMLMEContext->Wcid];

			UAPSD_SP_Close(pAd, pEntry);
			pMLMEContext->Wcid = MCAST_WCID;
		}
	}
#endif /* CONFIG_AP_SUPPORT */
#endif /* UAPSD_SUPPORT */


	if (Status != USB_ST_NOERROR)
	{
		/*Bulk-Out fail status handle */
		if ((!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_RESET_IN_PROGRESS)) &&
			(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_HALT_IN_PROGRESS)) &&
			(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_NIC_NOT_EXIST)) &&
			(!RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET)))
		{
			DBGPRINT_RAW(RT_DEBUG_ERROR, ("Bulk Out MLME Failed, Status=%d!\n", Status));
			/* TODO: How to handle about the MLMEBulkOut failed issue. Need to resend the mgmt pkt? */
			RTMP_SET_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET);
			pAd->bulkResetPipeid = (MGMTPIPEIDX | BULKOUT_MGMT_RESET_FLAG);
		}
	}

	pAd->BulkOutPending[MGMTPIPEIDX] = FALSE;
	RTMP_IRQ_UNLOCK(&pAd->BulkOutLock[MGMTPIPEIDX], IrqFlags);

	RTMP_IRQ_LOCK(&pAd->MLMEBulkOutLock, IrqFlags);
	/* Reset MLME context flags */
	pMLMEContext->IRPPending = FALSE;
	pMLMEContext->InUse = FALSE;
	pMLMEContext->bWaitingBulkOut = FALSE;
	pMLMEContext->BulkOutSize = 0;

	pPacket = pAd->MgmtRing.Cell[index].pNdisPacket;
	pAd->MgmtRing.Cell[index].pNdisPacket = NULL;

	/* Increase MgmtRing Index */
	INC_RING_INDEX(pAd->MgmtRing.TxDmaIdx, MGMT_RING_SIZE);
	pAd->MgmtRing.TxSwFreeIdx++;
	RTMP_IRQ_UNLOCK(&pAd->MLMEBulkOutLock, IrqFlags);


#if RT_CFG80211_SUPPORT
	pHeader = (HEADER_802_11 *)(GET_OS_PKT_DATAPTR(pPacket)+ TXINFO_SIZE + pAd->chipCap.TXWISize);
	CFG80211_SendMgmtFrameDone(pAd, pHeader->Sequence);
#endif /* RT_CFG80211_SUPPORT */

	/* No-matter success or fail, we free the mgmt packet. */
	if (pPacket)
		RTMPFreeNdisPacket(pAd, pPacket);

	if ((RTMP_TEST_FLAG(pAd, (fRTMP_ADAPTER_RESET_IN_PROGRESS |
								fRTMP_ADAPTER_HALT_IN_PROGRESS |
								fRTMP_ADAPTER_NIC_NOT_EXIST))))
	{
		/* do nothing and return directly. */
	}
	else
	{
		if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET) &&
			((pAd->bulkResetPipeid & BULKOUT_MGMT_RESET_FLAG) == BULKOUT_MGMT_RESET_FLAG))
		{	/* For Mgmt Bulk-Out failed, ignore it now. */
			RTEnqueueInternalCmd(pAd, CMDTHREAD_RESET_BULK_OUT, NULL, 0);
		}
		else
		{

			/* Always call Bulk routine, even reset bulk. */
			/* The protectioon of rest bulk should be in BulkOut routine */
			if (pAd->MgmtRing.TxSwFreeIdx < MGMT_RING_SIZE /* pMLMEContext->bWaitingBulkOut == TRUE */)
			{
				RTUSB_SET_BULK_FLAG(pAd, fRTUSB_BULK_OUT_MLME);
			}
				RTUSBKickBulkOut(pAd);
			}
		}


#ifdef CONFIG_STA_SUPPORT
#endif /* CONFIG_STA_SUPPORT */
}


static void rtusb_hcca_dma_done_tasklet(unsigned long data)
{
	struct rtmp_adapter *	pAd;
	PHT_TX_CONTEXT		pHTTXContext;
	UCHAR				BulkOutPipeId = 4;
	purbb_t				pUrb;


	DBGPRINT_RAW(RT_DEBUG_ERROR, ("--->hcca_dma_done_tasklet\n"));


	pUrb			= (purbb_t)data;
/*	pHTTXContext	= (PHT_TX_CONTEXT)pUrb->context; */
	pHTTXContext	= (PHT_TX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	pAd				= pHTTXContext->pAd;

	rtusb_dataout_complete((unsigned long)pUrb);

	if ((RTMP_TEST_FLAG(pAd, (fRTMP_ADAPTER_RESET_IN_PROGRESS |
								fRTMP_ADAPTER_HALT_IN_PROGRESS |
								fRTMP_ADAPTER_NIC_NOT_EXIST))))
	{
		/* do nothing and return directly. */
	}
	else
	{
		if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET))
		{
			RTEnqueueInternalCmd(pAd, CMDTHREAD_RESET_BULK_OUT, NULL, 0);
		}
		else
		{	pHTTXContext = &pAd->TxContext[BulkOutPipeId];
			if ((pAd->TxSwQueue[BulkOutPipeId].Number > 0) &&
				(pAd->DeQueueRunning[BulkOutPipeId] == FALSE) &&
				(pHTTXContext->bCurWriting == FALSE))
			{
				RTMPDeQueuePacket(pAd, FALSE, BulkOutPipeId, MAX_TX_PROCESS);
			}

			RTUSB_SET_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NORMAL);
			RTUSBKickBulkOut(pAd);
		}
	}

	DBGPRINT_RAW(RT_DEBUG_ERROR, ("<---hcca_dma_done_tasklet\n"));

		return;
}


static void rtusb_ac3_dma_done_tasklet(unsigned long data)
{
	struct rtmp_adapter *	pAd;
	PHT_TX_CONTEXT		pHTTXContext;
	UCHAR				BulkOutPipeId = 3;
	purbb_t				pUrb;


	pUrb			= (purbb_t)data;
/*	pHTTXContext	= (PHT_TX_CONTEXT)pUrb->context; */
	pHTTXContext	= (PHT_TX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	pAd				= pHTTXContext->pAd;

	rtusb_dataout_complete((unsigned long)pUrb);

	if ((RTMP_TEST_FLAG(pAd, (fRTMP_ADAPTER_RESET_IN_PROGRESS |
								fRTMP_ADAPTER_HALT_IN_PROGRESS |
								fRTMP_ADAPTER_NIC_NOT_EXIST))))
	{
		/* do nothing and return directly. */
	}
	else
	{
		if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET))
		{
			RTEnqueueInternalCmd(pAd, CMDTHREAD_RESET_BULK_OUT, NULL, 0);
		}
		else
		{	pHTTXContext = &pAd->TxContext[BulkOutPipeId];
			if ((pAd->TxSwQueue[BulkOutPipeId].Number > 0) &&
				(pAd->DeQueueRunning[BulkOutPipeId] == FALSE) &&
				(pHTTXContext->bCurWriting == FALSE))
			{
				RTMPDeQueuePacket(pAd, FALSE, BulkOutPipeId, MAX_TX_PROCESS);
			}

			RTUSB_SET_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NORMAL<<3);
			RTUSBKickBulkOut(pAd);
		}
	}


	return;
}


static void rtusb_ac2_dma_done_tasklet(unsigned long data)
{
	struct rtmp_adapter *	pAd;
	PHT_TX_CONTEXT		pHTTXContext;
	UCHAR				BulkOutPipeId = 2;
	purbb_t				pUrb;


	pUrb			= (purbb_t)data;
/*	pHTTXContext	= (PHT_TX_CONTEXT)pUrb->context; */
	pHTTXContext	= (PHT_TX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	pAd				= pHTTXContext->pAd;

	rtusb_dataout_complete((unsigned long)pUrb);

	if ((RTMP_TEST_FLAG(pAd, (fRTMP_ADAPTER_RESET_IN_PROGRESS |
								fRTMP_ADAPTER_HALT_IN_PROGRESS |
								fRTMP_ADAPTER_NIC_NOT_EXIST))))
	{
		/* do nothing and return directly. */
	}
	else
	{
		if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET))
		{
			RTEnqueueInternalCmd(pAd, CMDTHREAD_RESET_BULK_OUT, NULL, 0);
		}
		else
		{	pHTTXContext = &pAd->TxContext[BulkOutPipeId];
			if ((pAd->TxSwQueue[BulkOutPipeId].Number > 0) &&
				(pAd->DeQueueRunning[BulkOutPipeId] == FALSE) &&
				(pHTTXContext->bCurWriting == FALSE))
			{
				RTMPDeQueuePacket(pAd, FALSE, BulkOutPipeId, MAX_TX_PROCESS);
			}

			RTUSB_SET_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NORMAL<<2);
			RTUSBKickBulkOut(pAd);
		}
	}


	return;
}


static void rtusb_ac1_dma_done_tasklet(unsigned long data)
{
	struct rtmp_adapter *	pAd;
	PHT_TX_CONTEXT		pHTTXContext;
	UCHAR				BulkOutPipeId = 1;
	purbb_t				pUrb;


	pUrb			= (purbb_t)data;
/*	pHTTXContext	= (PHT_TX_CONTEXT)pUrb->context; */
	pHTTXContext	= (PHT_TX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	pAd				= pHTTXContext->pAd;

	rtusb_dataout_complete((unsigned long)pUrb);

	if ((RTMP_TEST_FLAG(pAd, (fRTMP_ADAPTER_RESET_IN_PROGRESS |
								fRTMP_ADAPTER_HALT_IN_PROGRESS |
								fRTMP_ADAPTER_NIC_NOT_EXIST))))
	{
		/* do nothing and return directly. */
	}
	else
	{
		if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET))
		{
			RTEnqueueInternalCmd(pAd, CMDTHREAD_RESET_BULK_OUT, NULL, 0);
		}
		else
		{	pHTTXContext = &pAd->TxContext[BulkOutPipeId];
			if ((pAd->TxSwQueue[BulkOutPipeId].Number > 0) &&
				(pAd->DeQueueRunning[BulkOutPipeId] == FALSE) &&
				(pHTTXContext->bCurWriting == FALSE))
			{
				RTMPDeQueuePacket(pAd, FALSE, BulkOutPipeId, MAX_TX_PROCESS);
			}

			RTUSB_SET_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NORMAL<<1);
			RTUSBKickBulkOut(pAd);
		}
	}
	return;

}


static void rtusb_ac0_dma_done_tasklet(unsigned long data)
{
	struct rtmp_adapter *	pAd;
	PHT_TX_CONTEXT		pHTTXContext;
	UCHAR				BulkOutPipeId = 0;
	purbb_t				pUrb;


	pUrb			= (purbb_t)data;
/*	pHTTXContext	= (PHT_TX_CONTEXT)pUrb->context; */
	pHTTXContext	= (PHT_TX_CONTEXT)RTMP_USB_URB_DATA_GET(pUrb);
	pAd				= pHTTXContext->pAd;

	rtusb_dataout_complete((unsigned long)pUrb);

	if ((RTMP_TEST_FLAG(pAd, (fRTMP_ADAPTER_RESET_IN_PROGRESS |
								fRTMP_ADAPTER_HALT_IN_PROGRESS |
								fRTMP_ADAPTER_NIC_NOT_EXIST))))
	{
		/* do nothing and return directly. */
	}
	else
	{
		if (RTMP_TEST_FLAG(pAd, fRTMP_ADAPTER_BULKOUT_RESET))
		{
			RTEnqueueInternalCmd(pAd, CMDTHREAD_RESET_BULK_OUT, NULL, 0);
		}
		else
		{	pHTTXContext = &pAd->TxContext[BulkOutPipeId];
			if ((pAd->TxSwQueue[BulkOutPipeId].Number > 0) &&
				(pAd->DeQueueRunning[BulkOutPipeId] == FALSE) &&
				(pHTTXContext->bCurWriting == FALSE))
			{
				RTMPDeQueuePacket(pAd, FALSE, BulkOutPipeId, MAX_TX_PROCESS);
			}

			RTUSB_SET_BULK_FLAG(pAd, fRTUSB_BULK_OUT_DATA_NORMAL);
			RTUSBKickBulkOut(pAd);
		}
	}


	return;

}

int RtmpNetTaskInit(
	IN struct rtmp_adapter *pAd)
{
	struct os_cookie *pObj = pAd->OS_Cookie;

	/* Create receive tasklet */
	RTMP_OS_TASKLET_INIT(pAd, &pObj->rx_done_task, rx_done_tasklet, (ULONG)pAd);
	//RTMP_OS_TASKLET_INIT(pAd, &pObj->cmd_rsp_event_task, cmd_rsp_event_tasklet, (ULONG)pAd);
	RTMP_OS_TASKLET_INIT(pAd, &pObj->mgmt_dma_done_task, rtusb_mgmt_dma_done_tasklet, (unsigned long)pAd);
	RTMP_OS_TASKLET_INIT(pAd, &pObj->ac0_dma_done_task, rtusb_ac0_dma_done_tasklet, (unsigned long)pAd);
	RTMP_OS_TASKLET_INIT(pAd, &pObj->ac1_dma_done_task, rtusb_ac1_dma_done_tasklet, (unsigned long)pAd);
	RTMP_OS_TASKLET_INIT(pAd, &pObj->ac2_dma_done_task, rtusb_ac2_dma_done_tasklet, (unsigned long)pAd);
	RTMP_OS_TASKLET_INIT(pAd, &pObj->ac3_dma_done_task, rtusb_ac3_dma_done_tasklet, (unsigned long)pAd);
	RTMP_OS_TASKLET_INIT(pAd, &pObj->hcca_dma_done_task, rtusb_hcca_dma_done_tasklet, (unsigned long)pAd);
	RTMP_OS_TASKLET_INIT(pAd, &pObj->tbtt_task, tbtt_tasklet, (unsigned long)pAd);
	RTMP_OS_TASKLET_INIT(pAd, &pObj->null_frame_complete_task, rtusb_null_frame_done_tasklet, (unsigned long)pAd);
	RTMP_OS_TASKLET_INIT(pAd, &pObj->pspoll_frame_complete_task, rtusb_pspoll_frame_done_tasklet, (unsigned long)pAd);

	return NDIS_STATUS_SUCCESS;
}


void RtmpNetTaskExit(IN struct rtmp_adapter *pAd)
{
	struct os_cookie *pObj;

	pObj = pAd->OS_Cookie;

	RTMP_OS_TASKLET_KILL(&pObj->rx_done_task);
	RTMP_OS_TASKLET_KILL(&pObj->cmd_rsp_event_task);
	RTMP_OS_TASKLET_KILL(&pObj->mgmt_dma_done_task);
	RTMP_OS_TASKLET_KILL(&pObj->ac0_dma_done_task);
	RTMP_OS_TASKLET_KILL(&pObj->ac1_dma_done_task);
	RTMP_OS_TASKLET_KILL(&pObj->ac2_dma_done_task);
	RTMP_OS_TASKLET_KILL(&pObj->ac3_dma_done_task);
	RTMP_OS_TASKLET_KILL(&pObj->hcca_dma_done_task);
	RTMP_OS_TASKLET_KILL(&pObj->tbtt_task);
	RTMP_OS_TASKLET_KILL(&pObj->null_frame_complete_task);
	RTMP_OS_TASKLET_KILL(&pObj->pspoll_frame_complete_task);
}


/*
========================================================================
Routine Description:
    USB command kernel thread.

Arguments:
	*Context			the pAd, driver control block pointer

Return Value:
    0					close the thread

Note:
========================================================================
*/
INT RTUSBCmdThread(
	IN ULONG Context)
{
	struct rtmp_adapter *pAd;
	RTMP_OS_TASK *pTask;
	int status;
	status = 0;

	pTask = (RTMP_OS_TASK *)Context;
	pAd = (struct rtmp_adapter *)RTMP_OS_TASK_DATA_GET(pTask);

	if (pAd == NULL)
		return 0;

	RtmpOSTaskCustomize(pTask);

	NdisAcquireSpinLock(&pAd->CmdQLock);
	pAd->CmdQ.CmdQState = RTMP_TASK_STAT_RUNNING;
	NdisReleaseSpinLock(&pAd->CmdQLock);

	while (pAd->CmdQ.CmdQState == RTMP_TASK_STAT_RUNNING)
	{
		if (RtmpOSTaskWait(pAd, pTask, &status) == FALSE)
		{
			RTMP_SET_FLAG(pAd, fRTMP_ADAPTER_HALT_IN_PROGRESS);
			break;
		}

		if (pAd->CmdQ.CmdQState == RTMP_TASK_STAT_STOPED)
			break;

		if (!pAd->PM_FlgSuspend)
			CMDHandler(pAd);
	}

	if (!pAd->PM_FlgSuspend)
	{	/* Clear the CmdQElements. */
		CmdQElmt	*pCmdQElmt = NULL;

		NdisAcquireSpinLock(&pAd->CmdQLock);
		pAd->CmdQ.CmdQState = RTMP_TASK_STAT_STOPED;
		while(pAd->CmdQ.size)
		{
			RTThreadDequeueCmd(&pAd->CmdQ, &pCmdQElmt);
			if (pCmdQElmt)
			{
				if (pCmdQElmt->CmdFromNdis == TRUE)
				{
					if (pCmdQElmt->buffer != NULL)
						kfree(pCmdQElmt->buffer);
					kfree((u8 *)pCmdQElmt);
				}
				else
				{
					if ((pCmdQElmt->buffer != NULL) && (pCmdQElmt->bufferlength != 0))
						kfree(pCmdQElmt->buffer);
					kfree((u8 *)pCmdQElmt);
				}
			}
		}

		NdisReleaseSpinLock(&pAd->CmdQLock);
	}
	/* notify the exit routine that we're actually exiting now
	 *
	 * complete()/wait_for_completion() is similar to up()/down(),
	 * except that complete() is safe in the case where the structure
	 * is getting deleted in a parallel mode of execution (i.e. just
	 * after the down() -- that's necessary for the thread-shutdown
	 * case.
	 *
	 * complete_and_exit() goes even further than this -- it is safe in
	 * the case that the thread of the caller is going away (not just
	 * the structure) -- this is necessary for the module-remove case.
	 * This is important in preemption kernels, which transfer the flow
	 * of execution immediately upon a complete().
	 */
	DBGPRINT(RT_DEBUG_TRACE,( "<---RTUSBCmdThread\n"));

	RtmpOSTaskNotifyToExit(pTask);
	return 0;

}

void InitUSBDevice(RT_CMD_USB_INIT *config, VOID *ad_src)
{
	struct rtmp_adapter *ad = (struct rtmp_adapter *)ad_src;
	uint32_t value;
	ad->infType = RTMP_DEV_INF_USB;

	RTMP_SEM_EVENT_INIT(&(ad->UsbVendorReq_semaphore), &ad->RscSemMemList);
	RTMP_SEM_EVENT_INIT(&(ad->reg_atomic), &ad->RscSemMemList);
	RTMP_SEM_EVENT_INIT(&(ad->hw_atomic), &ad->RscSemMemList);
	RTMP_SEM_EVENT_INIT(&(ad->mcu_atomic), &ad->RscSemMemList);
	ad->UsbVendorReqBuf = kmalloc(MAX_PARAM_BUFFER_SIZE - 1, GFP_ATOMIC);

	if (ad->UsbVendorReqBuf == NULL) {
		DBGPRINT(RT_DEBUG_ERROR, ("Allocate vendor request temp buffer failed!\n"));
		return;
	}

	value = mt7612u_read32(ad, 0x00);
	ad->ChipID = value;
	if (IS_RT65XX(ad))
		rlt_wlan_chip_onoff(ad, TRUE, TRUE);
#ifdef MT76x2
	if (IS_MT76x2(ad))
		mt76x2_pwrOn(ad);
#endif

	RtmpRaDevCtrlInit(ad, ad->infType);
}

