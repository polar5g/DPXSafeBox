/* $Id: DrvAudioVRDE.cpp $ */
/** @file
 * VRDE audio backend for Main.
 */

/*
 * Copyright (C) 2013-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DRV_VRDE_AUDIO
#include <VBox/log.h>
#include "DrvAudioVRDE.h"
#include "ConsoleImpl.h"
#include "ConsoleVRDPServer.h"

#include "Logging.h"

#include "../../Devices/Audio/DrvAudio.h"
#include "../../Devices/Audio/AudioMixBuffer.h"

#include <iprt/mem.h>
#include <iprt/cdefs.h>
#include <iprt/circbuf.h>

#include <VBox/vmm/pdmaudioifs.h>
#include <VBox/vmm/pdmdrv.h>
#include <VBox/RemoteDesktop/VRDE.h>
#include <VBox/vmm/cfgm.h>
#include <VBox/err.h>


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * Audio VRDE driver instance data.
 */
typedef struct DRVAUDIOVRDE
{
    /** Pointer to audio VRDE object. */
    AudioVRDE           *pAudioVRDE;
    PPDMDRVINS           pDrvIns;
    /** Pointer to the driver instance structure. */
    PDMIHOSTAUDIO        IHostAudio;
    /** Pointer to the VRDP's console object. */
    ConsoleVRDPServer   *pConsoleVRDPServer;
    /** Pointer to the DrvAudio port interface that is above us. */
    PPDMIAUDIOCONNECTOR  pDrvAudio;
    /** Whether this driver is enabled or not. */
    bool                 fEnabled;
} DRVAUDIOVRDE, *PDRVAUDIOVRDE;

typedef struct VRDESTREAMIN
{
    /** Associated host input stream. */
    PDMAUDIOHSTSTRMIN    HstStrmIn;
    /** Number of samples captured asynchronously in the
     *  onVRDEInputXXX callbacks. */
    uint32_t             cSamplesCaptured;
    /** Critical section. */
    RTCRITSECT           CritSect;
} VRDESTREAMIN, *PVRDESTREAMIN;

typedef struct VRDESTREAMOUT
{
    /** Associated host output stream. */
    PDMAUDIOHSTSTRMOUT HstStrmOut;
    uint64_t old_ticks;
    uint64_t cSamplesSentPerSec;
} VRDESTREAMOUT, *PVRDESTREAMOUT;



static DECLCALLBACK(int) drvAudioVRDEInit(PPDMIHOSTAUDIO pInterface)
{
    RT_NOREF(pInterface);
    LogFlowFuncEnter();

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvAudioVRDEInitIn(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMIN pHstStrmIn,
                                            PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq,
                                            PDMAUDIORECSOURCE enmRecSource, uint32_t *pcSamples)
{
    RT_NOREF(pCfgAcq, enmRecSource);
    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturn(pDrv, VERR_INVALID_POINTER);

    PVRDESTREAMIN pVRDEStrmIn = (PVRDESTREAMIN)pHstStrmIn;
    AssertPtrReturn(pVRDEStrmIn, VERR_INVALID_POINTER);

    if (pcSamples)
        *pcSamples = _4K; /** @todo Make this configurable. */

    return DrvAudioStreamCfgToProps(pCfgReq, &pVRDEStrmIn->HstStrmIn.Props);
}

static DECLCALLBACK(int) drvAudioVRDEInitOut(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMOUT pHstStrmOut,
                                             PPDMAUDIOSTREAMCFG pCfgReq, PPDMAUDIOSTREAMCFG pCfgAcq, uint32_t *pcSamples)
{
    RT_NOREF(pCfgAcq);
    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturn(pDrv, VERR_INVALID_POINTER);

    PVRDESTREAMOUT pVRDEStrmOut = (PVRDESTREAMOUT)pHstStrmOut;
    AssertPtrReturn(pVRDEStrmOut, VERR_INVALID_POINTER);

    if (pcSamples)
        *pcSamples = _4K; /** @todo Make this configurable. */

    return DrvAudioStreamCfgToProps(pCfgReq, &pVRDEStrmOut->HstStrmOut.Props);
}

static DECLCALLBACK(bool) drvAudioVRDEIsEnabled(PPDMIHOSTAUDIO pInterface, PDMAUDIODIR enmDir)
{
    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturn(pDrv, false);

    NOREF(enmDir);

    if (!pDrv->fEnabled)
        return false;

    return true;
}

/**
 * {FIXME - Missing brief description - FIXME}
 *
 * Transfers audio input formerly sent by a connected RDP client / VRDE backend
 * (using the onVRDEInputXXX methods) over to the VRDE host (VM). The audio device
 * emulation then will read and send the data to the guest.
 *
 * @return  IPRT status code.
 * @param   pInterface
 * @param   pHstStrmIn
 * @param   pcSamplesCaptured
 */
static DECLCALLBACK(int) drvAudioVRDECaptureIn(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMIN pHstStrmIn,
                                               uint32_t *pcSamplesCaptured)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pHstStrmIn, VERR_INVALID_POINTER);
    AssertPtrReturn(pcSamplesCaptured, VERR_INVALID_POINTER);

    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturn(pDrv, VERR_INVALID_POINTER);

    PVRDESTREAMIN pVRDEStrmIn = (PVRDESTREAMIN)pHstStrmIn;
    AssertPtrReturn(pVRDEStrmIn, VERR_INVALID_POINTER);

    /** @todo Use CritSect! */

    int rc;

    uint32_t cProcessed = 0;
    if (pVRDEStrmIn->cSamplesCaptured)
    {
        rc = AudioMixBufMixToParent(&pVRDEStrmIn->HstStrmIn.MixBuf, pVRDEStrmIn->cSamplesCaptured,
                                    &cProcessed);
    }
    else
        rc = VINF_SUCCESS;

    if (RT_SUCCESS(rc))
    {
        *pcSamplesCaptured = cProcessed;

        Assert(pVRDEStrmIn->cSamplesCaptured >= cProcessed);
        pVRDEStrmIn->cSamplesCaptured -= cProcessed;
    }

    LogFlowFunc(("cSamplesCaptured=%RU32, cProcessed=%RU32 rc=%Rrc\n", pVRDEStrmIn->cSamplesCaptured, cProcessed, rc));
    return rc;
}

/**
 * Transfers VM audio output to remote client.
 *
 * Transfers VM audio output over to the VRDE instance for playing remotely
 * on the client.
 *
 * @return  IPRT status code.
 * @param   pInterface
 * @param   pHstStrmOut
 * @param   pcSamplesPlayed
 */
static DECLCALLBACK(int) drvAudioVRDEPlayOut(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMOUT pHstStrmOut,
                                             uint32_t *pcSamplesPlayed)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pHstStrmOut, VERR_INVALID_POINTER);
    /* pcSamplesPlayed is optional. */

    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturn(pDrv, VERR_INVALID_POINTER);

    PVRDESTREAMOUT pVRDEStrmOut = (PVRDESTREAMOUT)pHstStrmOut;
    AssertPtrReturn(pVRDEStrmOut, VERR_INVALID_POINTER);

    uint32_t live = AudioMixBufAvail(&pHstStrmOut->MixBuf);
    uint64_t now = PDMDrvHlpTMGetVirtualTime(pDrv->pDrvIns);
    uint64_t ticks = now  - pVRDEStrmOut->old_ticks;
    uint64_t ticks_per_second = PDMDrvHlpTMGetVirtualFreq(pDrv->pDrvIns);

    /* Minimize the rounding error: samples = int((ticks * freq) / ticks_per_second + 0.5). */
    uint32_t cSamplesPlayed = (int)((2 * ticks * pHstStrmOut->Props.uHz + ticks_per_second) / ticks_per_second / 2);

    /* Don't play more than available. */
    if (cSamplesPlayed > live)
        cSamplesPlayed = live;

    /* Remember when samples were consumed. */
    pVRDEStrmOut->old_ticks = now;

    VRDEAUDIOFORMAT format = VRDE_AUDIO_FMT_MAKE(pHstStrmOut->Props.uHz,
                                                 pHstStrmOut->Props.cChannels,
                                                 pHstStrmOut->Props.cBits,
                                                 pHstStrmOut->Props.fSigned);

    int cSamplesToSend = cSamplesPlayed;

    LogFlowFunc(("uFreq=%RU32, cChan=%RU8, cBits=%RU8, fSigned=%RTbool, enmFormat=%ld, cSamplesToSend=%RU32\n",
                 pHstStrmOut->Props.uHz, pHstStrmOut->Props.cChannels,
                 pHstStrmOut->Props.cBits, pHstStrmOut->Props.fSigned,
                 format, cSamplesToSend));

    /*
     * Call the VRDP server with the data.
     */
    uint32_t cReadTotal = 0;

    PPDMAUDIOSAMPLE pSamples;
    uint32_t cRead;
    int rc = AudioMixBufAcquire(&pHstStrmOut->MixBuf, cSamplesToSend,
                                &pSamples, &cRead);
    if (   RT_SUCCESS(rc)
        && cRead)
    {
        cReadTotal = cRead;
        pDrv->pConsoleVRDPServer->SendAudioSamples(pSamples, cRead, format);

        if (rc == VINF_TRY_AGAIN)
        {
            rc = AudioMixBufAcquire(&pHstStrmOut->MixBuf, cSamplesToSend - cRead,
                                    &pSamples, &cRead);
            if (RT_SUCCESS(rc))
                pDrv->pConsoleVRDPServer->SendAudioSamples(pSamples, cRead, format);

            cReadTotal += cRead;
        }
    }

    AudioMixBufFinish(&pHstStrmOut->MixBuf, cSamplesToSend);

    /*
     * Always report back all samples acquired, regardless of whether the
     * VRDP server actually did process those.
     */
    if (pcSamplesPlayed)
        *pcSamplesPlayed = cReadTotal;

    LogFlowFunc(("cReadTotal=%RU32, rc=%Rrc\n", cReadTotal, rc));
    return rc;
}

static DECLCALLBACK(int) drvAudioVRDEFiniIn(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMIN pHstStrmIn)
{
    RT_NOREF(pHstStrmIn);
    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturn(pDrv, VERR_INVALID_POINTER);

    if (pDrv->pConsoleVRDPServer)
        pDrv->pConsoleVRDPServer->SendAudioInputEnd(NULL);

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvAudioVRDEFiniOut(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMOUT pHstStrmOut)
{
    RT_NOREF(pHstStrmOut);
    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturn(pDrv, VERR_INVALID_POINTER);

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvAudioVRDEControlOut(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMOUT pHstStrmOut,
                                                PDMAUDIOSTREAMCMD enmStreamCmd)
{
    RT_NOREF(enmStreamCmd);
    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturn(pDrv, VERR_INVALID_POINTER);

    PVRDESTREAMOUT pVRDEStrmOut = (PVRDESTREAMOUT)pHstStrmOut;
    AssertPtrReturn(pVRDEStrmOut, VERR_INVALID_POINTER);

    LogFlowFunc(("enmStreamCmd=%ld\n", enmStreamCmd));

    AudioMixBufReset(&pHstStrmOut->MixBuf);

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvAudioVRDEControlIn(PPDMIHOSTAUDIO pInterface, PPDMAUDIOHSTSTRMIN pHstStrmIn,
                                               PDMAUDIOSTREAMCMD enmStreamCmd)
{
    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturn(pDrv, VERR_INVALID_POINTER);

    PVRDESTREAMIN pVRDEStrmIn = (PVRDESTREAMIN)pHstStrmIn;
    AssertPtrReturn(pVRDEStrmIn, VERR_INVALID_POINTER);

    PPDMAUDIOHSTSTRMIN pThisStrmIn = &pVRDEStrmIn->HstStrmIn;

    LogFlowFunc(("enmStreamCmd=%ld\n", enmStreamCmd));

    if (!pDrv->pConsoleVRDPServer)
        return VINF_SUCCESS;

    AudioMixBufReset(&pThisStrmIn->MixBuf);

    /* Initialize only if not already done. */
    int rc;
    if (enmStreamCmd == PDMAUDIOSTREAMCMD_ENABLE)
    {
        rc = pDrv->pConsoleVRDPServer->SendAudioInputBegin(NULL, pVRDEStrmIn, AudioMixBufSize(&pThisStrmIn->MixBuf),
                                                           pThisStrmIn->Props.uHz,
                                                           pThisStrmIn->Props.cChannels, pThisStrmIn->Props.cBits);
        if (rc == VERR_NOT_SUPPORTED)
        {
            LogFlowFunc(("No RDP client connected, so no input recording supported\n"));
            rc = VINF_SUCCESS;
        }
    }
    else if (enmStreamCmd == PDMAUDIOSTREAMCMD_DISABLE)
    {
        pDrv->pConsoleVRDPServer->SendAudioInputEnd(NULL /* pvUserCtx */);
        rc = VINF_SUCCESS;
    }
    else
        rc = VERR_INVALID_PARAMETER;

    return rc;
}

static DECLCALLBACK(int) drvAudioVRDEGetConf(PPDMIHOSTAUDIO pInterface, PPDMAUDIOBACKENDCFG pCfg)
{
    RT_NOREF(pInterface);
    pCfg->cbStreamOut     = sizeof(VRDESTREAMOUT);
    pCfg->cbStreamIn      = sizeof(VRDESTREAMIN);
    pCfg->cMaxHstStrmsOut = 1;
    pCfg->cMaxHstStrmsIn  = 2; /* Microphone in + Line in. */

    return VINF_SUCCESS;
}

static DECLCALLBACK(void) drvAudioVRDEShutdown(PPDMIHOSTAUDIO pInterface)
{
    PDRVAUDIOVRDE pDrv = RT_FROM_MEMBER(pInterface, DRVAUDIOVRDE, IHostAudio);
    AssertPtrReturnVoid(pDrv);

    if (pDrv->pConsoleVRDPServer)
        pDrv->pConsoleVRDPServer->SendAudioInputEnd(NULL);
}

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) drvAudioVRDEQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVAUDIOVRDE pThis = PDMINS_2_DATA(pDrvIns, PDRVAUDIOVRDE);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIHOSTAUDIO, &pThis->IHostAudio);
    return NULL;
}

AudioVRDE::AudioVRDE(Console *pConsole)
    : mpDrv(NULL),
      mParent(pConsole)
{
}

AudioVRDE::~AudioVRDE(void)
{
    if (mpDrv)
    {
        mpDrv->pAudioVRDE = NULL;
        mpDrv = NULL;
    }
}

int AudioVRDE::onVRDEControl(bool fEnable, uint32_t uFlags)
{
    RT_NOREF(uFlags);
    LogFlowThisFunc(("fEnable=%RTbool, uFlags=0x%x\n", fEnable, uFlags));

    if (mpDrv == NULL)
        return VERR_INVALID_STATE;

    mpDrv->fEnabled = fEnable;

    return VINF_SUCCESS; /* Never veto. */
}

/**
 * Marks the beginning of sending captured audio data from a connected
 * RDP client.
 *
 * @return  IPRT status code.
 * @param   pvContext               The context; in this case a pointer to a
 *                                  VRDESTREAMIN structure.
 * @param   pVRDEAudioBegin         Pointer to a VRDEAUDIOINBEGIN structure.
 */
int AudioVRDE::onVRDEInputBegin(void *pvContext, PVRDEAUDIOINBEGIN pVRDEAudioBegin)
{
    AssertPtrReturn(pvContext, VERR_INVALID_POINTER);
    AssertPtrReturn(pVRDEAudioBegin, VERR_INVALID_POINTER);

    PVRDESTREAMIN pVRDEStrmIn = (PVRDESTREAMIN)pvContext;
    AssertPtrReturn(pVRDEStrmIn, VERR_INVALID_POINTER);

#ifdef LOG_ENABLED
    VRDEAUDIOFORMAT audioFmt = pVRDEAudioBegin->fmt;
    int iSampleHz  = VRDE_AUDIO_FMT_SAMPLE_FREQ(audioFmt);
    int cChannels  = VRDE_AUDIO_FMT_CHANNELS(audioFmt);
    int cBits      = VRDE_AUDIO_FMT_BITS_PER_SAMPLE(audioFmt);
    bool fUnsigned = VRDE_AUDIO_FMT_SIGNED(audioFmt);
    LogFlowFunc(("cbSample=%RU32, iSampleHz=%d, cChannels=%d, cBits=%d, fUnsigned=%RTbool\n",
                 VRDE_AUDIO_FMT_BYTES_PER_SAMPLE(audioFmt), iSampleHz, cChannels, cBits, fUnsigned));
#endif
    return VINF_SUCCESS;
}

int AudioVRDE::onVRDEInputData(void *pvContext, const void *pvData, uint32_t cbData)
{
    PVRDESTREAMIN pVRDEStrmIn = (PVRDESTREAMIN)pvContext;
    AssertPtrReturn(pVRDEStrmIn, VERR_INVALID_POINTER);

    PPDMAUDIOHSTSTRMIN pHstStrmIn = &pVRDEStrmIn->HstStrmIn;
    AssertPtrReturn(pHstStrmIn, VERR_INVALID_POINTER);

    /** @todo Use CritSect! */

    uint32_t cWritten;
    int rc = AudioMixBufWriteCirc(&pHstStrmIn->MixBuf, pvData, cbData, &cWritten);
    if (RT_SUCCESS(rc))
        pVRDEStrmIn->cSamplesCaptured += cWritten;

    LogFlowFunc(("cbData=%RU32, cWritten=%RU32, cSamplesCaptured=%RU32, rc=%Rrc\n",
                 cbData, cWritten, pVRDEStrmIn->cSamplesCaptured, rc));
    return rc;
}

int AudioVRDE::onVRDEInputEnd(void *pvContext)
{
    NOREF(pvContext);

    return VINF_SUCCESS;
}

int AudioVRDE::onVRDEInputIntercept(bool fEnabled)
{
    RT_NOREF(fEnabled);
    return VINF_SUCCESS; /* Never veto. */
}

/**
 * Construct a VRDE audio driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
/* static */
DECLCALLBACK(int) AudioVRDE::drvConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    RT_NOREF(fFlags);
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PDRVAUDIOVRDE pThis = PDMINS_2_DATA(pDrvIns, PDRVAUDIOVRDE);
    AssertPtrReturn(pDrvIns, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg, VERR_INVALID_POINTER);

    LogRel(("Audio: Initializing VRDE driver\n"));
    LogFlowFunc(("fFlags=0x%x\n", fFlags));

    AssertMsgReturn(PDMDrvHlpNoAttach(pDrvIns) == VERR_PDM_NO_ATTACHED_DRIVER,
                    ("Configuration error: Not possible to attach anything to this driver!\n"),
                    VERR_PDM_DRVINS_NO_ATTACH);

    /*
     * Init the static parts.
     */
    pThis->pDrvIns                   = pDrvIns;
    /* IBase */
    pDrvIns->IBase.pfnQueryInterface = drvAudioVRDEQueryInterface;
    /* IHostAudio */
    PDMAUDIO_IHOSTAUDIO_CALLBACKS(drvAudioVRDE);

    /* Init defaults. */
    pThis->fEnabled = false;

    /*
     * Get the ConsoleVRDPServer object pointer.
     */
    void *pvUser;
    int rc = CFGMR3QueryPtr(pCfg, "ObjectVRDPServer", &pvUser);
    AssertMsgRCReturn(rc, ("Confguration error: No/bad \"ObjectVRDPServer\" value, rc=%Rrc\n", rc), rc);

    /* CFGM tree saves the pointer to ConsoleVRDPServer in the Object node of AudioVRDE. */
    pThis->pConsoleVRDPServer = (ConsoleVRDPServer *)pvUser;

    /*
     * Get the AudioVRDE object pointer.
     */
    pvUser = NULL;
    rc = CFGMR3QueryPtr(pCfg, "Object", &pvUser);
    AssertMsgRCReturn(rc, ("Confguration error: No/bad \"Object\" value, rc=%Rrc\n", rc), rc);

    pThis->pAudioVRDE = (AudioVRDE *)pvUser;
    pThis->pAudioVRDE->mpDrv = pThis;

    /*
     * Get the interface for the above driver (DrvAudio) to make mixer/conversion calls.
     * Described in CFGM tree.
     */
    pThis->pDrvAudio = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIAUDIOCONNECTOR);
    AssertMsgReturn(pThis->pDrvAudio, ("Configuration error: No upper interface specified!\n"), VERR_PDM_MISSING_INTERFACE_ABOVE);

    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMDRVREG,pfnDestruct}
 */
/* static */
DECLCALLBACK(void) AudioVRDE::drvDestruct(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    PDRVAUDIOVRDE pThis = PDMINS_2_DATA(pDrvIns, PDRVAUDIOVRDE);
    LogFlowFuncEnter();

    /*
     * If the AudioVRDE object is still alive, we must clear it's reference to
     * us since we'll be invalid when we return from this method.
     */
    if (pThis->pAudioVRDE)
    {
        pThis->pAudioVRDE->mpDrv = NULL;
        pThis->pAudioVRDE = NULL;
    }
}


/**
 * VRDE audio driver registration record.
 */
const PDMDRVREG AudioVRDE::DrvReg =
{
    PDM_DRVREG_VERSION,
    /* szName */
    "AudioVRDE",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Audio driver for VRDE backend",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_AUDIO,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVAUDIOVRDE),
    /* pfnConstruct */
    AudioVRDE::drvConstruct,
    /* pfnDestruct */
    AudioVRDE::drvDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};

