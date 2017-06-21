/* $Id: DrvAudio.cpp $ */
/** @file
 * Intermediate audio driver header.
 *
 * @remarks Intermediate audio driver for connecting the audio device emulation
 *          with the host backend.
 */

/*
 * Copyright (C) 2006-2016 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 * --------------------------------------------------------------------
 *
 * This code is based on: audio.c from QEMU AUDIO subsystem.
 *
 * QEMU Audio subsystem
 *
 * Copyright (c) 2003-2005 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#define LOG_GROUP LOG_GROUP_DRV_AUDIO
#include <VBox/log.h>
#include <VBox/vmm/pdm.h>
#include <VBox/err.h>
#include <VBox/vmm/mm.h>
#include <VBox/vmm/pdmaudioifs.h>

#include <iprt/alloc.h>
#include <iprt/asm-math.h>
#include <iprt/assert.h>
#include <iprt/circbuf.h>
#include <iprt/string.h>
#include <iprt/uuid.h>

#include "VBoxDD.h"

#include <ctype.h>
#include <stdlib.h>

#include "DrvAudio.h"
#include "AudioMixBuffer.h"

typedef struct fixed_settings
{
    int enabled;
    int cStreams;
    int greedy;
    PDMAUDIOSTREAMCFG settings;
} fixed_settings;

static struct {
    struct fixed_settings fixed_out;
    struct fixed_settings fixed_in;
    union {
        int hz;
        int64_t ticks;
    } period;
    int plive;
} conf = {

    /* Fixed output settings. */
    {                           /* DAC fixed settings */
        1,                      /* enabled */
        1,                      /* cStreams */
        1,                      /* greedy */
        {
            44100,              /* freq */
            2,                  /* nchannels */
            AUD_FMT_S16,        /* fmt */
            PDMAUDIOHOSTENDIANNESS
        }
    },

    /* Fixed input settings. */
    {                           /* ADC fixed settings */
        1,                      /* enabled */
        2,                      /* cStreams */
        1,                      /* greedy */
        {
            44100,              /* freq */
            2,                  /* nchannels */
            AUD_FMT_S16,        /* fmt */
            PDMAUDIOHOSTENDIANNESS
        }
    },

    { 200 },                    /* frequency (in Hz) */
    0,                          /* plive */ /** @todo Disable pending live? */
};

static int drvAudioDestroyGstIn(PDRVAUDIO pThis, PPDMAUDIOGSTSTRMIN pGstStrmIn);

static int drvAudioAllocHstIn(PDRVAUDIO pThis, const char *pszName, PPDMAUDIOSTREAMCFG pCfg, PDMAUDIORECSOURCE enmRecSource, PPDMAUDIOHSTSTRMIN *ppHstStrmIn);
static int drvAudioDestroyHstIn(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMIN pHstStrmIn);

int drvAudioAddHstOut(PDRVAUDIO pThis, const char *pszName, PPDMAUDIOSTREAMCFG pCfg, PPDMAUDIOHSTSTRMOUT *ppHstStrmOut)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pszName, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg, VERR_INVALID_POINTER);

    PPDMAUDIOHSTSTRMOUT pHstStrmOut = NULL;

    int rc;
    if (   conf.fixed_out.enabled /** @todo Get rid of these settings! */
        && conf.fixed_out.greedy)
    {
        rc = drvAudioAllocHstOut(pThis, pszName, pCfg, &pHstStrmOut);
    }
    else
        rc = VERR_NOT_FOUND;

    if (RT_FAILURE(rc))
    {
        pHstStrmOut = drvAudioFindSpecificOut(pThis, NULL, pCfg);
        if (!pHstStrmOut)
        {
            rc = drvAudioAllocHstOut(pThis, pszName, pCfg, &pHstStrmOut);
            if (RT_FAILURE(rc))
                pHstStrmOut = drvAudioFindAnyHstOut(pThis, NULL /* pHstStrmOut */);
        }

        rc = pHstStrmOut ? VINF_SUCCESS : rc;
    }

    if (RT_SUCCESS(rc))
        *ppHstStrmOut = pHstStrmOut;

    return rc;
}

static PDMAUDIOFMT drvAudioGetConfFormat(PCFGMNODE pCfgHandle, const char *pszKey,
                                         PDMAUDIOFMT enmDefault, bool *pfDefault)
{
    if (   pCfgHandle == NULL
        || pszKey == NULL)
    {
        *pfDefault = true;
        return enmDefault;
    }

    char *pszValue = NULL;
    int rc = CFGMR3QueryStringAlloc(pCfgHandle, pszKey, &pszValue);
    if (RT_FAILURE(rc))
    {
        *pfDefault = true;
        return enmDefault;
    }

    PDMAUDIOFMT fmt = drvAudioHlpStringToFormat(pszValue);
    if (fmt == AUD_FMT_INVALID)
    {
         *pfDefault = true;
        return enmDefault;
    }

    *pfDefault = false;
    return fmt;
}

static int drvAudioGetConfInt(PCFGMNODE pCfgHandle, const char *pszKey,
                              int iDefault, bool *pfDefault)
{

    if (   pCfgHandle == NULL
        || pszKey == NULL)
    {
        *pfDefault = true;
        return iDefault;
    }

    uint64_t u64Data = 0;
    int rc = CFGMR3QueryInteger(pCfgHandle, pszKey, &u64Data);
    if (RT_FAILURE(rc))
    {
        *pfDefault = true;
        return iDefault;

    }

    *pfDefault = false;
    return u64Data;
}

static const char *drvAudioGetConfStr(PCFGMNODE pCfgHandle, const char *pszKey,
                                      const char *pszDefault, bool *pfDefault)
{
    if (   pCfgHandle == NULL
        || pszKey == NULL)
    {
        *pfDefault = true;
        return pszDefault;
    }

    char *pszValue = NULL;
    int rc = CFGMR3QueryStringAlloc(pCfgHandle, pszKey, &pszValue);
    if (RT_FAILURE(rc))
    {
        *pfDefault = true;
        return pszDefault;
    }

    *pfDefault = false;
    return pszValue;
}

static int drvAudioProcessOptions(PCFGMNODE pCfgHandle, const char *pszPrefix, struct audio_option *opt)
{
    AssertPtrReturn(pCfgHandle, VERR_INVALID_POINTER);
    AssertPtrReturn(pszPrefix, VERR_INVALID_POINTER);
    AssertPtrReturn(opt, VERR_INVALID_POINTER);

    PCFGMNODE pCfgChildHandle = NULL;
    PCFGMNODE pCfgChildChildHandle = NULL;

   /* If pCfgHandle is NULL, let NULL be passed to get int and get string functions..
    * The getter function will return default values.
    */
    if (pCfgHandle != NULL)
    {
       /* If its audio general setting, need to traverse to one child node.
        * /Devices/ichac97/0/LUN#0/Config/Audio
        */
       if(!strncmp(pszPrefix, "AUDIO", 5)) /** @todo Use a \#define */
       {
            pCfgChildHandle = CFGMR3GetFirstChild(pCfgHandle);
            if(pCfgChildHandle)
                pCfgHandle = pCfgChildHandle;
        }
        else
        {
            /* If its driver specific configuration , then need to traverse two level deep child
             * child nodes. for eg. in case of DirectSoundConfiguration item
             * /Devices/ichac97/0/LUN#0/Config/Audio/DirectSoundConfig
             */
            pCfgChildHandle = CFGMR3GetFirstChild(pCfgHandle);
            if (pCfgChildHandle)
            {
                pCfgChildChildHandle = CFGMR3GetFirstChild(pCfgChildHandle);
                if (pCfgChildChildHandle)
                    pCfgHandle = pCfgChildChildHandle;
            }
        }
    }

    for (; opt->name; opt++)
    {
        LogFlowFunc(("Option value pointer for `%s' is not set\n",
                     opt->name));
        if (!opt->valp) {
            LogFlowFunc(("Option value pointer for `%s' is not set\n",
                   opt->name));
            continue;
        }

        bool fUseDefault;

        switch (opt->tag)
        {
            case AUD_OPT_BOOL:
            case AUD_OPT_INT:
            {
                int *intp = (int *)opt->valp;
                *intp = drvAudioGetConfInt(pCfgHandle, opt->name, *intp, &fUseDefault);

                break;
            }

            case AUD_OPT_FMT:
            {
                PDMAUDIOFMT *fmtp = (PDMAUDIOFMT *)opt->valp;
                *fmtp = drvAudioGetConfFormat(pCfgHandle, opt->name, *fmtp, &fUseDefault);

                break;
            }

            case AUD_OPT_STR:
            {
                const char **strp = (const char **)opt->valp;
                *strp = drvAudioGetConfStr(pCfgHandle, opt->name, *strp, &fUseDefault);

                break;
            }

            default:
                LogFlowFunc(("Bad value tag for option `%s' - %d\n", opt->name, opt->tag));
                fUseDefault = false;
                break;
        }

        if (!opt->overridenp)
            opt->overridenp = &opt->overriden;

        *opt->overridenp = !fUseDefault;
    }

    return VINF_SUCCESS;
}

static bool drvAudioStreamCfgIsValid(PPDMAUDIOSTREAMCFG pCfg)
{
    bool fValid = (   pCfg->cChannels == 1
                   || pCfg->cChannels == 2); /* Either stereo (2) or mono (1), per stream. */

    fValid |= (   pCfg->enmEndianness == PDMAUDIOENDIANNESS_LITTLE
               || pCfg->enmEndianness == PDMAUDIOENDIANNESS_BIG);

    if (fValid)
    {
        switch (pCfg->enmFormat)
        {
            case AUD_FMT_S8:
            case AUD_FMT_U8:
            case AUD_FMT_S16:
            case AUD_FMT_U16:
            case AUD_FMT_S32:
            case AUD_FMT_U32:
                break;
            default:
                fValid = false;
                break;
        }
    }

    /** @todo Check for defined frequencies supported. */
    fValid |= pCfg->uHz > 0;

#ifdef DEBUG
    drvAudioStreamCfgPrint(pCfg);
#endif

    LogFlowFunc(("pCfg=%p, fValid=%RTbool\n", pCfg, fValid));
    return fValid;
}

/**
 * Clears a sample buffer by the given amount of audio samples.
 *
 * @return  IPRT status code.
 * @param   pPCMProps               PCM properties to use for the buffer to clear.
 * @param   pvBuf                   Buffer to clear.
 * @param   cbBuf                   Size (in bytes) of the buffer.
 * @param   cSamples                Number of audio samples to clear in the buffer.
 */
void DrvAudioClearBuf(PPDMPCMPROPS pPCMProps, void *pvBuf, size_t cbBuf, uint32_t cSamples)
{
    AssertPtrReturnVoid(pPCMProps);
    AssertPtrReturnVoid(pvBuf);

    if (!cbBuf || !cSamples)
        return;

    Log2Func(("pPCMInfo=%p, pvBuf=%p, cSamples=%RU32, fSigned=%RTbool, cBits=%RU8, cShift=%RU8\n",
              pPCMProps, pvBuf, cSamples, pPCMProps->fSigned, pPCMProps->cBits, pPCMProps->cShift));

    if (pPCMProps->fSigned)
    {
        memset(pvBuf, 0, cSamples << pPCMProps->cShift);
    }
    else
    {
        switch (pPCMProps->cBits)
        {
            case 8:
            {
                memset(pvBuf, 0x80, cSamples << pPCMProps->cShift);
                break;
            }

            case 16:
            {
                uint16_t *p = (uint16_t *)pvBuf;
                int shift = pPCMProps->cChannels - 1;
                short s = INT16_MAX;

                if (pPCMProps->fSwapEndian)
                    s = RT_BSWAP_U16(s);

                for (unsigned i = 0; i < cSamples << shift; i++)
                    p[i] = s;

                break;
            }

            case 32:
            {
                uint32_t *p = (uint32_t *)pvBuf;
                int shift = pPCMProps->cChannels - 1;
                int32_t s = INT32_MAX;

                if (pPCMProps->fSwapEndian)
                    s = RT_BSWAP_U32(s);

                for (unsigned i = 0; i < cSamples << shift; i++)
                    p[i] = s;

                break;
            }

            default:
            {
                AssertMsgFailed(("Invalid bits: %RU8\n", pPCMProps->cBits));
                break;
            }
        }
    }
}

static int drvAudioControlHstIn(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMIN pHstStrmIn, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pHstStrmIn, VERR_INVALID_POINTER);

    int rc = RTCritSectEnter(&pHstStrmIn->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        {
            if (!(pHstStrmIn->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED))
            {
                rc = pThis->pHostDrvAudio->pfnControlIn(pThis->pHostDrvAudio, pHstStrmIn, PDMAUDIOSTREAMCMD_ENABLE);
                if (RT_SUCCESS(rc))
                {
                    pHstStrmIn->fStatus |= PDMAUDIOSTRMSTS_FLAG_ENABLED;
                }
                else
                    LogFlowFunc(("Backend reported an error when opening input stream, rc=%Rrc\n", rc));
            }
            else
                rc = VINF_SUCCESS;

            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
        {
            if (pHstStrmIn->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED)
            {
                rc = pThis->pHostDrvAudio->pfnControlIn(pThis->pHostDrvAudio, pHstStrmIn, PDMAUDIOSTREAMCMD_DISABLE);
                if (RT_SUCCESS(rc))
                {
                    pHstStrmIn->fStatus = PDMAUDIOSTRMSTS_FLAG_NONE; /* Clear all. */
                    AudioMixBufClear(&pHstStrmIn->MixBuf);
                }
                else
                    LogFlowFunc(("Backend vetoed closing output stream, rc=%Rrc\n", rc));
            }
            else
                rc = VINF_SUCCESS;

            break;
        }

        case PDMAUDIOSTREAMCMD_PAUSE:
        {
            if (!(pHstStrmIn->fStatus & PDMAUDIOSTRMSTS_FLAG_PAUSED))
            {
                Assert(pHstStrmIn->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED);
                rc = pThis->pHostDrvAudio->pfnControlIn(pThis->pHostDrvAudio, pHstStrmIn, PDMAUDIOSTREAMCMD_PAUSE);
                if (RT_SUCCESS(rc))
                {
                    LogFunc(("[%s] Pausing stream\n", pHstStrmIn->MixBuf.pszName));
                    pHstStrmIn->fStatus |= PDMAUDIOSTRMSTS_FLAG_PAUSED;
                }
                else
                    LogFlowFunc(("Backend vetoed pausing input stream, rc=%Rrc\n", rc));
            }
            else
                rc = VINF_SUCCESS;

            break;
        }

        case PDMAUDIOSTREAMCMD_RESUME:
        {
            if (pHstStrmIn->fStatus & PDMAUDIOSTRMSTS_FLAG_PAUSED)
            {
                Assert(pHstStrmIn->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED);
                rc = pThis->pHostDrvAudio->pfnControlIn(pThis->pHostDrvAudio, pHstStrmIn, PDMAUDIOSTREAMCMD_RESUME);
                if (RT_SUCCESS(rc))
                {
                    pHstStrmIn->fStatus &= ~PDMAUDIOSTRMSTS_FLAG_PAUSED;
                    LogFunc(("[%s] Resumed stream\n", pHstStrmIn->MixBuf.pszName));
                }
                else
                    LogFlowFunc(("Backend vetoed resuming input stream, rc=%Rrc\n", rc));
            }
            else
                rc = VINF_SUCCESS;

            break;
        }

        default:
            AssertMsgFailed(("Command %ld not implemented\n", enmStreamCmd));
            rc = VERR_NOT_IMPLEMENTED;
            break;
    }

    int rc2 = RTCritSectLeave(&pHstStrmIn->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    return rc;
}

static int drvAudioControlHstOut(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMOUT pHstStrmOut, PDMAUDIOSTREAMCMD enmStreamCmd)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pHstStrmOut, VERR_INVALID_POINTER);

    int rc = RTCritSectEnter(&pHstStrmOut->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    switch (enmStreamCmd)
    {
        case PDMAUDIOSTREAMCMD_ENABLE:
        {
            if (!(pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED))
            {
                rc = pThis->pHostDrvAudio->pfnControlOut(pThis->pHostDrvAudio, pHstStrmOut, PDMAUDIOSTREAMCMD_ENABLE);
                if (RT_SUCCESS(rc))
                {
                    Assert(!(pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_PENDING_DISABLE));
                    pHstStrmOut->fStatus |= PDMAUDIOSTRMSTS_FLAG_ENABLED;
                    LogFunc(("[%s] Enabled stream\n", pHstStrmOut->MixBuf.pszName));
                }
                else
                    LogFlowFunc(("[%s] Backend reported an error when enabling output stream, rc=%Rrc\n",
                                 pHstStrmOut->MixBuf.pszName, rc));
            }
            else
                rc = VINF_SUCCESS;

            break;
        }

        case PDMAUDIOSTREAMCMD_DISABLE:
        {
            if (pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED)
            {
                rc = pThis->pHostDrvAudio->pfnControlOut(pThis->pHostDrvAudio, pHstStrmOut, PDMAUDIOSTREAMCMD_DISABLE);
                if (RT_SUCCESS(rc))
                {
                    pHstStrmOut->fStatus = PDMAUDIOSTRMSTS_FLAG_NONE; /* Clear all. */
                    AudioMixBufClear(&pHstStrmOut->MixBuf);

                    LogFunc(("[%s] Disabled stream\n", pHstStrmOut->MixBuf.pszName));
                }
                else
                    LogFlowFunc(("[%s] Backend vetoed disabling output stream, rc=%Rrc\n", pHstStrmOut->MixBuf.pszName, rc));
            }
            else
                rc = VINF_SUCCESS;

            break;
        }

        case PDMAUDIOSTREAMCMD_PAUSE:
        {
            if (!(pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_PAUSED))
            {
                Assert(pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED);
                rc = pThis->pHostDrvAudio->pfnControlOut(pThis->pHostDrvAudio, pHstStrmOut, PDMAUDIOSTREAMCMD_PAUSE);
                if (RT_SUCCESS(rc))
                {
                    pHstStrmOut->fStatus |= PDMAUDIOSTRMSTS_FLAG_PAUSED;
                    LogFunc(("[%s] Pausing stream\n", pHstStrmOut->MixBuf.pszName));
                }
                else
                    LogFlowFunc(("[%s] Backend vetoed pausing output stream, rc=%Rrc\n", pHstStrmOut->MixBuf.pszName, rc));
            }
            else
                rc = VINF_SUCCESS;

            break;
        }

        case PDMAUDIOSTREAMCMD_RESUME:
        {
            if (pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_PAUSED)
            {
                Assert(pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED);
                rc = pThis->pHostDrvAudio->pfnControlOut(pThis->pHostDrvAudio, pHstStrmOut, PDMAUDIOSTREAMCMD_RESUME);
                if (RT_SUCCESS(rc))
                {
                    pHstStrmOut->fStatus &= ~PDMAUDIOSTRMSTS_FLAG_PAUSED;
                    LogFunc(("[%s] Resumed stream\n", pHstStrmOut->MixBuf.pszName));
                }
                else
                    LogFlowFunc(("[%s] Backend vetoed resuming output stream, rc=%Rrc\n", pHstStrmOut->MixBuf.pszName, rc));
            }
            else
                rc = VINF_SUCCESS;

            break;
        }

        default:
            AssertMsgFailed(("Command %ld not implemented\n", enmStreamCmd));
            rc = VERR_NOT_IMPLEMENTED;
            break;
    }

    int rc2 = RTCritSectLeave(&pHstStrmOut->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    return rc;
}

int drvAudioDestroyHstOut(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMOUT pHstStrmOut)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pHstStrmOut, VERR_INVALID_POINTER);

    LogFlowFunc(("%s\n", pHstStrmOut->MixBuf.pszName));

    int rc;
    if (RTListIsEmpty(&pHstStrmOut->lstGstStrmOut))
    {
        rc = pThis->pHostDrvAudio->pfnFiniOut(pThis->pHostDrvAudio, pHstStrmOut);
        if (RT_SUCCESS(rc))
        {
            drvAudioHstOutFreeRes(pHstStrmOut);

            /* Remove from driver instance list. */
            RTListNodeRemove(&pHstStrmOut->Node);

            if (RTCritSectIsInitialized(&pHstStrmOut->CritSect))
            {
                int rc2 = RTCritSectDelete(&pHstStrmOut->CritSect);
                AssertRC(rc2);
            }

            RTMemFree(pHstStrmOut);
            pThis->cFreeOutputStreams++;
            return VINF_SUCCESS;
        }
    }
    else
    {
        rc = VERR_ACCESS_DENIED;
        LogFlowFunc(("[%s] Still is being used, rc=%Rrc\n", pHstStrmOut->MixBuf.pszName, rc));
    }

    return rc;
}

int drvAudioDestroyGstOut(PDRVAUDIO pThis, PPDMAUDIOGSTSTRMOUT pGstStrmOut)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);

    if (!pGstStrmOut)
        return VINF_SUCCESS;

    if (pGstStrmOut->State.cRefs > 1) /* Do other objects still have a reference to it? Bail out. */
        return VERR_WRONG_ORDER;

    drvAudioGstOutFreeRes(pGstStrmOut);

    if (pGstStrmOut->pHstStrmOut)
    {
        /* Unregister from parent first. */
        RTListNodeRemove(&pGstStrmOut->Node);

        /* Try destroying the associated host output stream. This could
         * be skipped if there are other guest output streams with this
         * host stream. */
        drvAudioDestroyHstOut(pThis, pGstStrmOut->pHstStrmOut);
    }

    RTMemFree(pGstStrmOut);

    return VINF_SUCCESS;
}

PPDMAUDIOHSTSTRMIN drvAudioFindNextHstIn(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMIN pHstStrmIn)
{
    if (pHstStrmIn)
    {
        if (RTListNodeIsLast(&pThis->lstHstStrmIn, &pHstStrmIn->Node))
            return NULL;

        return RTListNodeGetNext(&pHstStrmIn->Node, PDMAUDIOHSTSTRMIN, Node);
    }

    return RTListGetFirst(&pThis->lstHstStrmIn, PDMAUDIOHSTSTRMIN, Node);
}

PPDMAUDIOHSTSTRMIN drvAudioFindNextEnabledHstIn(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMIN pHstStrmIn)
{
    while ((pHstStrmIn = drvAudioFindNextHstIn(pThis, pHstStrmIn)))
        if (pHstStrmIn->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED)
            return pHstStrmIn;

    return NULL;
}

PPDMAUDIOHSTSTRMIN drvAudioFindNextEqHstIn(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMIN pHstStrmIn,
                                           PPDMAUDIOSTREAMCFG pCfg)
{
    while ((pHstStrmIn = drvAudioFindNextHstIn(pThis, pHstStrmIn)))
        if (drvAudioPCMPropsAreEqual(&pHstStrmIn->Props, pCfg))
            return pHstStrmIn;

    return NULL;
}

static int drvAudioHstInAdd(PDRVAUDIO pThis, const char *pszName, PPDMAUDIOSTREAMCFG pCfg, PDMAUDIORECSOURCE enmRecSource,
                            PPDMAUDIOHSTSTRMIN *ppHstStrmIn)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg, VERR_INVALID_POINTER);
    AssertPtrReturn(ppHstStrmIn, VERR_INVALID_POINTER);

    PPDMAUDIOHSTSTRMIN pHstStrmIn;
    int rc = drvAudioAllocHstIn(pThis, pszName, pCfg, enmRecSource, &pHstStrmIn);
    if (RT_SUCCESS(rc))
        *ppHstStrmIn = pHstStrmIn;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int drvAudioGstOutInit(PPDMAUDIOGSTSTRMOUT pGstStrmOut, PPDMAUDIOHSTSTRMOUT pHostStrmOut,
                       const char *pszName, PPDMAUDIOSTREAMCFG pCfg)
{
    AssertPtrReturn(pGstStrmOut,  VERR_INVALID_POINTER);
    AssertPtrReturn(pHostStrmOut, VERR_INVALID_POINTER);
    AssertPtrReturn(pszName,      VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg,         VERR_INVALID_POINTER);

    int rc = DrvAudioStreamCfgToProps(pCfg, &pGstStrmOut->Props);
    if (RT_SUCCESS(rc))
    {
        char *pszTemp;
        if (RTStrAPrintf(&pszTemp, "%s (Guest)", pszName) <= 0)
            return VERR_NO_MEMORY;

        rc = AudioMixBufInit(&pGstStrmOut->MixBuf, pszTemp, &pGstStrmOut->Props, AudioMixBufSize(&pHostStrmOut->MixBuf));
        if (RT_SUCCESS(rc))
            rc = AudioMixBufLinkTo(&pGstStrmOut->MixBuf, &pHostStrmOut->MixBuf);

        RTStrFree(pszTemp);

        if (RT_SUCCESS(rc))
        {
            pGstStrmOut->State.cRefs   = 1;
            pGstStrmOut->State.fActive = false;
            pGstStrmOut->State.fEmpty  = true;

            pGstStrmOut->State.pszName = RTStrDup(pszName);
            if (!pGstStrmOut->State.pszName)
                return VERR_NO_MEMORY;

            pGstStrmOut->pHstStrmOut = pHostStrmOut;
        }
    }

    LogFlowFunc(("pszName=%s, rc=%Rrc\n", pszName, rc));
    return rc;
}

int drvAudioAllocHstOut(PDRVAUDIO pThis, const char *pszName, PPDMAUDIOSTREAMCFG pCfg, PPDMAUDIOHSTSTRMOUT *ppHstStrmOut)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pszName, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg, VERR_INVALID_POINTER);

    if (!pThis->cFreeOutputStreams)
    {
        LogFlowFunc(("Maximum number of host output streams reached\n"));
        return VERR_NO_MORE_HANDLES;
    }

    /* Validate backend configuration. */
    if (!pThis->BackendCfg.cbStreamOut)
    {
        LogFlowFunc(("Backend output configuration not valid, bailing out\n"));
        return VERR_INVALID_PARAMETER;
    }

    PPDMAUDIOHSTSTRMOUT pHstStrmOut = (PPDMAUDIOHSTSTRMOUT)RTMemAllocZ(pThis->BackendCfg.cbStreamOut);
    if (!pHstStrmOut)
    {
        LogFlowFunc(("Error allocating host output stream with %zu bytes\n",
                     pThis->BackendCfg.cbStreamOut));
        return VERR_NO_MEMORY;
    }

    int rc;
    bool fInitialized = false;

    do
    {
        RTListInit(&pHstStrmOut->lstGstStrmOut);

        /* Make the acquired configuration the requested configuration by default. */
        PDMAUDIOSTREAMCFG CfgAcq;
        memcpy(&CfgAcq, pCfg, sizeof(PDMAUDIOSTREAMCFG));

        uint32_t cSamples;
        rc = pThis->pHostDrvAudio->pfnInitOut(pThis->pHostDrvAudio, pHstStrmOut,
                                              pCfg /* pCfgReq */, &CfgAcq /* pCfgAcq */,
                                              &cSamples);
        if (RT_FAILURE(rc))
        {
            LogFlowFunc(("Initializing host backend failed with rc=%Rrc\n", rc));
            break;
        }

        fInitialized = true;

        char *pszTemp;
        if (RTStrAPrintf(&pszTemp, "%s (Host)", pszName) <= 0)
        {
            rc = VERR_NO_MEMORY;
            break;
        }

#ifdef DEBUG
        LogFunc(("%s: Requested format:\n", pszTemp));
        drvAudioStreamCfgPrint(pCfg);
        LogFunc(("%s: Acquired format:\n",  pszTemp));
        drvAudioStreamCfgPrint(&CfgAcq);
#else
        LogRel2(("Audio: Acquired output format for '%s': %RU32Hz, %s, %RU8 %s\n",
                 pszTemp, CfgAcq.uHz, drvAudioHlpFormatToString(CfgAcq.enmFormat),
                 CfgAcq.cChannels, CfgAcq.cChannels == 0 ? "Channel" : "Channels"));
#endif
        rc = DrvAudioStreamCfgToProps(&CfgAcq, &pHstStrmOut->Props);
        if (RT_SUCCESS(rc))
        {
            rc = AudioMixBufInit(&pHstStrmOut->MixBuf, pszTemp, &pHstStrmOut->Props, cSamples);
            if (RT_SUCCESS(rc))
                rc = RTCritSectInit(&pHstStrmOut->CritSect);

            if (RT_SUCCESS(rc))
            {
                RTListPrepend(&pThis->lstHstStrmOut, &pHstStrmOut->Node);
                pThis->cFreeOutputStreams--;
            }
        }

        RTStrFree(pszTemp);

    } while (0);

    if (RT_FAILURE(rc))
    {
        if (fInitialized)
        {
            int rc2 = pThis->pHostDrvAudio->pfnFiniOut(pThis->pHostDrvAudio, pHstStrmOut);
            AssertRC(rc2);
        }

        drvAudioHstOutFreeRes(pHstStrmOut);
        RTMemFree(pHstStrmOut);
    }
    else
        *ppHstStrmOut = pHstStrmOut;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

int drvAudioCreateStreamPairOut(PDRVAUDIO pThis, const char *pszName,
                                PPDMAUDIOSTREAMCFG pCfg, PPDMAUDIOGSTSTRMOUT *ppGstStrmOut)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pszName, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg, VERR_INVALID_POINTER);

    /*
     * Try figuring out which audio stream configuration this backend
     * should use. If fixed output is enabled the backend will be tied
     * to a fixed rate (in Hz, among other parameters), regardless of
     * what the backend could do else.
     */
    PPDMAUDIOSTREAMCFG pBackendCfg;
    if (conf.fixed_out.enabled)
        pBackendCfg = &conf.fixed_out.settings;
    else
        pBackendCfg = pCfg;

    AssertPtrReturn(pBackendCfg, VERR_INVALID_POINTER);

    LogFlowFunc(("Using fixed audio output settings: %RTbool\n",
                 RT_BOOL(conf.fixed_out.enabled)));

    PPDMAUDIOGSTSTRMOUT pGstStrmOut =
        (PPDMAUDIOGSTSTRMOUT)RTMemAllocZ(sizeof(PDMAUDIOGSTSTRMOUT));
    if (!pGstStrmOut)
    {
        LogFlowFunc(("Failed to allocate memory for guest output stream \"%s\"\n", pszName));
        return VERR_NO_MEMORY;
    }

    /*
     * The host stream always will get the backend audio stream configuration.
     */
    PPDMAUDIOHSTSTRMOUT pHstStrmOut;
    int rc = drvAudioAddHstOut(pThis, pszName, pBackendCfg, &pHstStrmOut);
    if (RT_FAILURE(rc))
    {
        LogFlowFunc(("Error adding host output stream \"%s\", rc=%Rrc\n", pszName, rc));

        RTMemFree(pGstStrmOut);
        return rc;
    }

    /*
     * The guest stream always will get the audio stream configuration told
     * by the device emulation (which in turn was/could be set by the guest OS).
     */
    rc = drvAudioGstOutInit(pGstStrmOut, pHstStrmOut, pszName, pCfg);
    if (RT_SUCCESS(rc))
    {
        RTListPrepend(&pHstStrmOut->lstGstStrmOut, &pGstStrmOut->Node);

        if (ppGstStrmOut)
            *ppGstStrmOut = pGstStrmOut;
    }

    if (RT_FAILURE(rc))
        drvAudioDestroyGstOut(pThis, pGstStrmOut);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int drvAudioCreateStreamPairIn(PDRVAUDIO pThis, const char *pszName, PDMAUDIORECSOURCE enmRecSource,
                                      PPDMAUDIOSTREAMCFG pCfg, PPDMAUDIOGSTSTRMIN *ppGstStrmIn)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pszName, VERR_INVALID_POINTER);

    /*
     * Try figuring out which audio stream configuration this backend
     * should use for the audio input data. If fixed input is enabled
     * the backend will be tied to a fixed rate (in Hz, among other parameters),
     * regardless of what the backend initially wanted to use.
     */
    PPDMAUDIOSTREAMCFG pBackendCfg;
    if (conf.fixed_in.enabled)
        pBackendCfg = &conf.fixed_in.settings;
    else
        pBackendCfg = pCfg;

    AssertPtrReturn(pBackendCfg, VERR_INVALID_POINTER);

    LogFlowFunc(("Using fixed audio input settings: %RTbool\n",
                 RT_BOOL(conf.fixed_in.enabled)));

    PPDMAUDIOGSTSTRMIN pGstStrmIn = (PPDMAUDIOGSTSTRMIN)RTMemAllocZ(sizeof(PDMAUDIOGSTSTRMIN));
    if (!pGstStrmIn)
        return VERR_NO_MEMORY;

    /*
     * The host stream always will get the backend audio stream configuration.
     */
    PPDMAUDIOHSTSTRMIN pHstStrmIn;
    int rc = drvAudioHstInAdd(pThis, pszName, pBackendCfg, enmRecSource, &pHstStrmIn);
    if (RT_FAILURE(rc))
    {
        LogFunc(("Failed to add host audio input stream \"%s\", rc=%Rrc\n", pszName, rc));

        RTMemFree(pGstStrmIn);
        return rc;
    }

    /*
     * The guest stream always will get the audio stream configuration told
     * by the device emulation (which in turn was/could be set by the guest OS).
     */
    rc = drvAudioGstInInit(pGstStrmIn, pHstStrmIn, pszName, pCfg);
    if (RT_SUCCESS(rc))
    {
        pHstStrmIn->pGstStrmIn = pGstStrmIn;

        if (ppGstStrmIn)
            *ppGstStrmIn = pGstStrmIn;
    }
    else
        drvAudioDestroyGstIn(pThis, pGstStrmIn);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Initializes a guest input stream.
 *
 * @return  IPRT status code.
 * @param   pGstStrmIn          Pointer to guest stream to initialize.
 * @param   pHstStrmIn          Pointer to host input stream to associate this guest
 *                              stream with.
 * @param   pszName             Pointer to stream name to use for this stream.
 * @param   pCfg                Pointer to stream configuration to use.
 */
int drvAudioGstInInit(PPDMAUDIOGSTSTRMIN pGstStrmIn, PPDMAUDIOHSTSTRMIN pHstStrmIn,
                      const char *pszName, PPDMAUDIOSTREAMCFG pCfg)
{
    AssertPtrReturn(pGstStrmIn, VERR_INVALID_POINTER);
    AssertPtrReturn(pHstStrmIn, VERR_INVALID_POINTER);
    AssertPtrReturn(pszName, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg, VERR_INVALID_POINTER);

    int rc = DrvAudioStreamCfgToProps(pCfg, &pGstStrmIn->Props);
    if (RT_SUCCESS(rc))
    {
        char *pszTemp;
        if (RTStrAPrintf(&pszTemp, "%s (Guest)", pszName) <= 0)
            return VERR_NO_MEMORY;

        rc = AudioMixBufInit(&pGstStrmIn->MixBuf, pszTemp, &pGstStrmIn->Props, AudioMixBufSize(&pHstStrmIn->MixBuf));
        if (RT_SUCCESS(rc))
            rc = AudioMixBufLinkTo(&pHstStrmIn->MixBuf, &pGstStrmIn->MixBuf);

        RTStrFree(pszTemp);

        if (RT_SUCCESS(rc))
        {
#ifdef DEBUG
            drvAudioStreamCfgPrint(pCfg);
#endif
            pGstStrmIn->State.cRefs   = 1;
            pGstStrmIn->State.fActive = false;
            pGstStrmIn->State.fEmpty  = true;

            pGstStrmIn->State.pszName = RTStrDup(pszName);
            if (!pGstStrmIn->State.pszName)
                return VERR_NO_MEMORY;

            pGstStrmIn->pHstStrmIn = pHstStrmIn;
        }
    }

    LogFlowFunc(("pszName=%s, rc=%Rrc\n", pszName, rc));
    return rc;
}

static int drvAudioAllocHstIn(PDRVAUDIO pThis, const char *pszName, PPDMAUDIOSTREAMCFG pCfg,
                              PDMAUDIORECSOURCE enmRecSource, PPDMAUDIOHSTSTRMIN *ppHstStrmIn)
{
    if (!pThis->cFreeInputStreams)
    {
        LogFlowFunc(("No more input streams free to use, bailing out\n"));
        return VERR_NO_MORE_HANDLES;
    }

    /* Validate backend configuration. */
    if (!pThis->BackendCfg.cbStreamIn)
    {
        LogFlowFunc(("Backend input configuration not valid, bailing out\n"));
        return VERR_INVALID_PARAMETER;
    }

    PPDMAUDIOHSTSTRMIN pHstStrmIn =
        (PPDMAUDIOHSTSTRMIN)RTMemAllocZ(pThis->BackendCfg.cbStreamIn);
    if (!pHstStrmIn)
    {
        LogFlowFunc(("Error allocating host innput stream with %RU32 bytes\n",
                     pThis->BackendCfg.cbStreamOut));
        return VERR_NO_MEMORY;
    }

    int rc;
    bool fInitialized = false;

    do
    {
        /* Make the acquired configuration the requested configuration by default. */
        PDMAUDIOSTREAMCFG CfgAcq;
        memcpy(&CfgAcq, pCfg, sizeof(PDMAUDIOSTREAMCFG));

#ifndef DEBUG
        LogRel2(("Audio: Requested input format: %RU32Hz, %s, %RU8 %s\n",
                 CfgAcq.uHz, drvAudioHlpFormatToString(CfgAcq.enmFormat),
                 CfgAcq.cChannels, CfgAcq.cChannels == 0 ? "Channel" : "Channels"));
#endif
        uint32_t cSamples;
        rc = pThis->pHostDrvAudio->pfnInitIn(pThis->pHostDrvAudio, pHstStrmIn,
                                             pCfg /* pCfgReq */, &CfgAcq /* pCfgAcq */,
                                             enmRecSource, &cSamples);
        if (RT_FAILURE(rc))
        {
            LogFlowFunc(("Initializing host backend failed with rc=%Rrc\n", rc));
            break;
        }

        fInitialized = true;

        char *pszTemp;
        if (RTStrAPrintf(&pszTemp, "%s (Host)", pszName) <= 0)
        {
            rc = VERR_NO_MEMORY;
            break;
        }

#ifdef DEBUG
        LogFunc(("%s: Requested format:\n", pszTemp));
        drvAudioStreamCfgPrint(pCfg);
        LogFunc(("%s: Acquired format:\n",  pszTemp));
        drvAudioStreamCfgPrint(&CfgAcq);
#else
        LogRel2(("Audio: Acquired input format for '%s': %RU32Hz, %s, %RU8 %s\n",
                 pszTemp, CfgAcq.uHz, drvAudioHlpFormatToString(CfgAcq.enmFormat),
                 CfgAcq.cChannels, CfgAcq.cChannels == 0 ? "Channel" : "Channels"));
#endif
        rc = DrvAudioStreamCfgToProps(&CfgAcq, &pHstStrmIn->Props);
        if (RT_SUCCESS(rc))
        {
            rc = AudioMixBufInit(&pHstStrmIn->MixBuf, pszTemp, &pHstStrmIn->Props, cSamples);
            if (RT_SUCCESS(rc))
                rc = RTCritSectInit(&pHstStrmIn->CritSect);

            if (RT_SUCCESS(rc))
            {
                RTListPrepend(&pThis->lstHstStrmIn, &pHstStrmIn->Node);
                pThis->cFreeInputStreams--;
            }
        }

        RTStrFree(pszTemp);

    } while (0);

    if (RT_FAILURE(rc))
    {
        if (fInitialized)
        {
            int rc2 = pThis->pHostDrvAudio->pfnFiniIn(pThis->pHostDrvAudio,
                                                      pHstStrmIn);
            AssertRC(rc2);
        }

        drvAudioHstInFreeRes(pHstStrmIn);
        RTMemFree(pHstStrmIn);
    }
    else
        *ppHstStrmIn = pHstStrmIn;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Writes VM audio output data from the guest stream into the host stream.
 * The attached host driver backend then will play out the audio in a
 * later step then.
 *
 * @return  IPRT status code.
 * @return  int
 * @param   pThis
 * @param   pGstStrmOut
 * @param   pvBuf
 * @param   cbBuf
 * @param   pcbWritten
 */
static DECLCALLBACK(int) drvAudioWrite(PPDMIAUDIOCONNECTOR pInterface, PPDMAUDIOGSTSTRMOUT pGstStrmOut,
                                       const void *pvBuf, uint32_t cbBuf, uint32_t *pcbWritten)
{
    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);

    AssertPtrReturn(pGstStrmOut, VERR_INVALID_POINTER);
    AssertPtrReturn(pvBuf,       VERR_INVALID_POINTER);
    AssertReturn(cbBuf,          VERR_INVALID_PARAMETER);
    /* pcbWritten is optional. */

    int rc = RTCritSectEnter(&pThis->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    if (!pThis->pHostDrvAudio->pfnIsEnabled(pThis->pHostDrvAudio, PDMAUDIODIR_OUT))
    {
        rc = RTCritSectLeave(&pThis->CritSect);
        AssertRC(rc);

        return VERR_NOT_AVAILABLE;
    }

    PPDMAUDIOHSTSTRMOUT pHstStrmOut = pGstStrmOut->pHstStrmOut;
    AssertPtrReturn(pHstStrmOut, VERR_INVALID_POINTER);

    AssertMsg(pGstStrmOut->pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED,
              ("Writing to disabled host output stream \"%s\" not possible\n",
              pHstStrmOut->MixBuf.pszName));

    if (!AudioMixBufFreeBytes(&pGstStrmOut->MixBuf))
    {
        if (pcbWritten)
            *pcbWritten = 0;

        return RTCritSectLeave(&pThis->CritSect);
    }

    /*
     * First, write data from the device emulation into our
     * guest mixing buffer.
     */
    uint32_t cWritten;
    rc = AudioMixBufWriteAt(&pGstStrmOut->MixBuf, 0 /* Offset in samples */, pvBuf, cbBuf, &cWritten);

    /*
     * Second, mix the guest mixing buffer with the host mixing
     * buffer so that the host backend can play the data lateron.
     */
    uint32_t cMixed;
    if (   RT_SUCCESS(rc)
        && cWritten)
    {
        rc = AudioMixBufMixToParent(&pGstStrmOut->MixBuf, cWritten, &cMixed);
    }
    else
        cMixed = 0;

    if (RT_SUCCESS(rc))
    {
        /*
         * Return the number of samples which actually have been mixed
         * down to the parent, regardless how much samples were written
         * into the children buffer.
         */
        if (pcbWritten)
            *pcbWritten = AUDIOMIXBUF_S2B(&pGstStrmOut->MixBuf, cMixed);
    }

    LogFlowFunc(("%s -> %s: Written pvBuf=%p, cbBuf=%RU32, cWritten=%RU32 (%RU32 bytes), cMixed=%RU32, rc=%Rrc\n",
                 pGstStrmOut->MixBuf.pszName, pHstStrmOut->MixBuf.pszName, pvBuf, cbBuf, cWritten,
                 AUDIOMIXBUF_S2B(&pGstStrmOut->MixBuf, cWritten), cMixed, rc));

    int rc2 = RTCritSectLeave(&pThis->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    return rc;
}

PPDMAUDIOHSTSTRMOUT drvAudioFindAnyHstOut(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMOUT pHstStrmOut)
{
    if (pHstStrmOut)
    {
        if (RTListNodeIsLast(&pThis->lstHstStrmOut, &pHstStrmOut->Node))
            return NULL;

        return RTListNodeGetNext(&pHstStrmOut->Node, PDMAUDIOHSTSTRMOUT, Node);
    }

    return RTListGetFirst(&pThis->lstHstStrmOut, PDMAUDIOHSTSTRMOUT, Node);
}

PPDMAUDIOHSTSTRMOUT drvAudioHstFindAnyEnabledOut(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMOUT pHostStrmOut)
{
    while ((pHostStrmOut = drvAudioFindAnyHstOut(pThis, pHostStrmOut)))
    {
        if (pHostStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED)
            return pHostStrmOut;
    }

    return NULL;
}

PPDMAUDIOHSTSTRMOUT drvAudioFindSpecificOut(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMOUT pHstStrmOut,
                                            PPDMAUDIOSTREAMCFG pCfg)
{
    while ((pHstStrmOut = drvAudioFindAnyHstOut(pThis, pHstStrmOut)))
    {
        if (drvAudioPCMPropsAreEqual(&pHstStrmOut->Props, pCfg))
            return pHstStrmOut;
    }

    return NULL;
}

int drvAudioDestroyHstIn(PDRVAUDIO pThis, PPDMAUDIOHSTSTRMIN pHstStrmIn)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    AssertPtrReturn(pHstStrmIn, VERR_INVALID_POINTER);

    LogFlowFunc(("%s\n", pHstStrmIn->MixBuf.pszName));

    int rc;
    if (!pHstStrmIn->pGstStrmIn) /* No parent anymore? */
    {
        rc = pThis->pHostDrvAudio->pfnFiniIn(pThis->pHostDrvAudio, pHstStrmIn);
        if (RT_SUCCESS(rc))
        {
            drvAudioHstInFreeRes(pHstStrmIn);

            if (RTCritSectIsInitialized(&pHstStrmIn->CritSect))
            {
                int rc2 = RTCritSectDelete(&pHstStrmIn->CritSect);
                AssertRC(rc2);
            }

            /* Remove from driver instance list. */
            RTListNodeRemove(&pHstStrmIn->Node);

            RTMemFree(pHstStrmIn);
            pThis->cFreeInputStreams++;
        }
    }
    else
    {
        rc = VERR_ACCESS_DENIED;
        LogFlowFunc(("[%s] Still is being used, rc=%Rrc\n", pHstStrmIn->MixBuf.pszName, rc));
    }

    return rc;
}

static int drvAudioDestroyGstIn(PDRVAUDIO pThis, PPDMAUDIOGSTSTRMIN pGstStrmIn)
{
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);

    LogFlowFunc(("%s\n", pGstStrmIn->MixBuf.pszName));

    if (!pGstStrmIn)
        return VINF_SUCCESS;

    if (pGstStrmIn->State.cRefs > 1) /* Do other objects still have a reference to it? Bail out. */
        return VERR_WRONG_ORDER;

    drvAudioGstInFreeRes(pGstStrmIn);

    if (pGstStrmIn->pHstStrmIn)
    {
        /* Unlink child. */
        pGstStrmIn->pHstStrmIn->pGstStrmIn = NULL;

        /* Try destroying the associated host input stream. This could
         * be skipped if there are other guest input streams with this
         * host stream. */
        drvAudioDestroyHstIn(pThis, pGstStrmIn->pHstStrmIn);
    }

    RTMemFree(pGstStrmIn);

    return VINF_SUCCESS;
}

static DECLCALLBACK(int) drvAudioQueryStatus(PPDMIAUDIOCONNECTOR pInterface,
                                             uint32_t *pcbAvailIn, uint32_t *pcbFreeOut,
                                             uint32_t *pcSamplesLive)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    /* pcbAvailIn is optional. */
    /* pcbFreeOut is optional. */
    /* pcSamplesLive is optional. */

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc = RTCritSectEnter(&pThis->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Playback.
     */
    uint32_t cSamplesLive = 0;
    uint32_t cbFreeOut    = UINT32_MAX;

    PPDMAUDIOHSTSTRMOUT pHstStrmOut = NULL;
    while ((pHstStrmOut = drvAudioHstFindAnyEnabledOut(pThis, pHstStrmOut)))
    {
        cSamplesLive = AudioMixBufAvail(&pHstStrmOut->MixBuf);

        /* Has this stream marked as disabled but there still were guest streams relying
         * on it? Check if this stream now can be closed and do so, if possible. */
        if (   (pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_PENDING_DISABLE)
            && !cSamplesLive)
        {
            /* Stop playing the current (pending) stream. */
            int rc2 = drvAudioControlHstOut(pThis, pHstStrmOut, PDMAUDIOSTREAMCMD_DISABLE);
            if (RT_SUCCESS(rc2))
            {
                pHstStrmOut->fStatus &= ~PDMAUDIOSTRMSTS_FLAG_PENDING_DISABLE;

                LogFunc(("[%s] Disabling stream\n", pHstStrmOut->MixBuf.pszName));
            }
            else
                LogFunc(("[%s] Backend vetoed against closing output stream, rc=%Rrc\n", pHstStrmOut->MixBuf.pszName, rc2));

            continue;
        }

        LogFlowFunc(("[%s] cSamplesLive=%RU32\n", pHstStrmOut->MixBuf.pszName, cSamplesLive));

        /*
         * No live samples to play at the moment?
         *
         * Tell the device emulation for each connected guest stream how many
         * bytes are free so that the device emulation can continue writing data to
         * these streams.
         */
        PPDMAUDIOGSTSTRMOUT pGstStrmOut;
        uint32_t cbFree2 = UINT32_MAX;
        RTListForEach(&pHstStrmOut->lstGstStrmOut, pGstStrmOut, PDMAUDIOGSTSTRMOUT, Node)
        {
            if (pGstStrmOut->State.fActive)
            {
                /* Tell the sound device emulation how many samples are free
                 * so that it can start writing PCM data to us. */
                cbFree2 = RT_MIN(cbFree2, AUDIOMIXBUF_S2B_RATIO(&pGstStrmOut->MixBuf,
                                                                AudioMixBufFree(&pGstStrmOut->MixBuf)));
#ifdef DEBUG_andy
                LogFlowFunc(("\t[%s] cbFreeOut=%RU32\n", pGstStrmOut->MixBuf.pszName, cbFree2));
#endif
            }
        }

        cbFreeOut = RT_MIN(cbFreeOut, cbFree2);
    }

    /*
     * Recording.
     */
    uint32_t cbAvailIn = 0;

    PPDMAUDIOHSTSTRMIN pHstStrmIn = NULL;
    while ((pHstStrmIn = drvAudioFindNextEnabledHstIn(pThis, pHstStrmIn)))
    {
        /* Call the host backend to capture the audio input data. */
        uint32_t cSamplesCaptured;
        int rc2 = pThis->pHostDrvAudio->pfnCaptureIn(pThis->pHostDrvAudio, pHstStrmIn,
                                                     &cSamplesCaptured);
        if (RT_FAILURE(rc2))
            continue;

        PPDMAUDIOGSTSTRMIN pGstStrmIn = pHstStrmIn->pGstStrmIn;
        AssertPtrBreak(pGstStrmIn);

        if (pGstStrmIn->State.fActive)
        {
            cbAvailIn = RT_MAX(cbAvailIn, AUDIOMIXBUF_S2B(&pHstStrmIn->MixBuf,
                                                          AudioMixBufMixed(&pHstStrmIn->MixBuf)));
#ifdef DEBUG_andy
            LogFlowFunc(("\t[%s] cbAvailIn=%RU32\n", pHstStrmIn->MixBuf.pszName, cbAvailIn));
#endif
        }
    }

    if (RT_SUCCESS(rc))
    {
        if (cbFreeOut == UINT32_MAX)
            cbFreeOut = 0;

        if (pcbAvailIn)
            *pcbAvailIn = cbAvailIn;

        if (pcbFreeOut)
            *pcbFreeOut = cbFreeOut;

        if (pcSamplesLive)
            *pcSamplesLive = cSamplesLive;
    }

    int rc2 = RTCritSectLeave(&pThis->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    if (RT_FAILURE(rc))
        LogFlowFuncLeaveRC(rc);

    return rc;
}

static DECLCALLBACK(int) drvAudioPlayOut(PPDMIAUDIOCONNECTOR pInterface, uint32_t *pcSamplesPlayed)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    /* pcSamplesPlayed is optional. */

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc = RTCritSectEnter(&pThis->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    /* Backend output (temporarily) disabled / unavailable? */
    if (!pThis->pHostDrvAudio->pfnIsEnabled(pThis->pHostDrvAudio, PDMAUDIODIR_OUT))
    {
        rc = pThis->pHostDrvAudio->pfnGetConf(pThis->pHostDrvAudio, &pThis->BackendCfg);
        AssertRC(rc);

        if (!pThis->BackendCfg.cMaxHstStrmsOut)
        {
            int rc2 = RTCritSectLeave(&pThis->CritSect);
            AssertRC(rc2);

            return VERR_NOT_AVAILABLE;
        }
    }

    /*
     * Process all enabled host output streams.
     */
    uint32_t            cSamplesPlayedMax = 0;
    PPDMAUDIOHSTSTRMOUT pHstStrmOut       = NULL;
    while ((pHstStrmOut = drvAudioHstFindAnyEnabledOut(pThis, pHstStrmOut)))
    {
#if 0
        uint32_t cStreamsLive;
        uint32_t cSamplesLive = drvAudioHstOutSamplesLive(pHstStrmOut, &cStreamsLive);
        if (!cStreamsLive)
            cSamplesLive = 0;

        /* Has this stream marked as disabled but there still were guest streams relying
         * on it? Check if this stream now can be closed and do so, if possible. */
        if (   pHstStrmOut->fPendingDisable
            && !cStreamsLive)
        {
            /* Stop playing the current (pending) stream. */
            int rc2 = pThis->pHostDrvAudio->pfnControlOut(pThis->pHostDrvAudio, pHstStrmOut,
                                                          PDMAUDIOSTREAMCMD_DISABLE);
            if (RT_SUCCESS(rc2))
            {
                pHstStrmOut->fEnabled        = false;
                pHstStrmOut->fPendingDisable = false;

                LogFunc(("\t%p: Disabling stream\n", pHstStrmOut));
            }
            else
                LogFunc(("\t%p: Backend vetoed against closing output stream, rc=%Rrc\n",
                         pHstStrmOut, rc2));

            continue;
        }
#endif

        uint32_t cSamplesPlayed = 0;
        int rc2 = pThis->pHostDrvAudio->pfnPlayOut(pThis->pHostDrvAudio, pHstStrmOut, &cSamplesPlayed);
        if (RT_FAILURE(rc2))
        {
            rc2 = pThis->pHostDrvAudio->pfnControlOut(pThis->pHostDrvAudio, pHstStrmOut, PDMAUDIOSTREAMCMD_DISABLE);
            AssertRC(rc2);
        }
        else
            cSamplesPlayedMax = RT_MAX(cSamplesPlayed, cSamplesPlayedMax);

        LogFlowFunc(("\t[%s] cSamplesPlayed=%RU32, cSamplesPlayedMax=%RU32, rc=%Rrc\n",
                     pHstStrmOut->MixBuf.pszName, cSamplesPlayed, cSamplesPlayedMax, rc2));

        bool fNeedsCleanup = false;

        PPDMAUDIOGSTSTRMOUT pGstStrmOut;
        RTListForEach(&pHstStrmOut->lstGstStrmOut, pGstStrmOut, PDMAUDIOGSTSTRMOUT, Node)
        {
            if (   !pGstStrmOut->State.fActive
                && pGstStrmOut->State.fEmpty)
                continue;

            if (AudioMixBufIsEmpty(&pGstStrmOut->MixBuf))
            {
                pGstStrmOut->State.fEmpty = true;
                fNeedsCleanup |= !pGstStrmOut->State.fActive;
            }
        }

        if (fNeedsCleanup)
        {
            RTListForEach(&pHstStrmOut->lstGstStrmOut, pGstStrmOut, PDMAUDIOGSTSTRMOUT, Node)
            {
                if (!pGstStrmOut->State.fActive)
                    drvAudioDestroyGstOut(pThis, pGstStrmOut);
            }
        }
    }

    if (RT_SUCCESS(rc))
    {
        if (pcSamplesPlayed)
            *pcSamplesPlayed = cSamplesPlayedMax;
    }

    int rc2 = RTCritSectLeave(&pThis->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    if (RT_FAILURE(rc))
        LogFlowFuncLeaveRC(rc);

    return rc;
}

#ifdef VBOX_WITH_AUDIO_CALLBACKS
static PPDMAUDIOCALLBACK drvAudioCallbackDuplicate(PPDMAUDIOCALLBACK pCB)
{
    PPDMAUDIOCALLBACK pCBCopy = (PPDMAUDIOCALLBACK)RTMemDup((void *)pCB, sizeof(PDMAUDIOCALLBACK));
    if (!pCBCopy)
        return NULL;

    if (pCB->pvCtx)
    {
        pCBCopy->pvCtx = RTMemDup(pCB->pvCtx, pCB->cbCtx);
        if (!pCBCopy->pvCtx)
        {
            RTMemFree(pCBCopy);
            return NULL;
        }

        pCBCopy->cbCtx = pCB->cbCtx;
    }

    return pCBCopy;
}

static void drvAudioCallbackDestroy(PPDMAUDIOCALLBACK pCB)
{
    if (!pCB)
        return;

    RTListNodeRemove(&pCB->Node);
    if (pCB->pvCtx)
    {
        Assert(pCB->cbCtx);
        RTMemFree(pCB->pvCtx);
    }
    RTMemFree(pCB);
}

static DECLCALLBACK(int) drvAudioRegisterCallbacks(PPDMIAUDIOCONNECTOR pInterface,
                                                   PPDMAUDIOCALLBACK paCallbacks, size_t cCallbacks)
{
    AssertPtrReturn(pInterface,  VERR_INVALID_POINTER);
    AssertPtrReturn(paCallbacks, VERR_INVALID_POINTER);
    AssertReturn(cCallbacks,     VERR_INVALID_PARAMETER);

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc = RTCritSectEnter(&pThis->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    for (size_t i = 0; i < cCallbacks; i++)
    {
        PPDMAUDIOCALLBACK pCB = drvAudioCallbackDuplicate(&paCallbacks[i]);
        if (!pCB)
        {
            rc = VERR_NO_MEMORY;
            break;
        }

        switch (pCB->enmType)
        {
            case PDMAUDIOCALLBACKTYPE_INPUT:
                RTListAppend(&pThis->lstCBIn, &pCB->Node);
                break;

            case PDMAUDIOCALLBACKTYPE_OUTPUT:
                RTListAppend(&pThis->lstCBOut, &pCB->Node);
                break;

            default:
                AssertMsgFailed(("Not supported\n"));
                break;
        }
    }

    /** @todo Undo allocations on error. */

    int rc2 = RTCritSectLeave(&pThis->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    return rc;
}

static DECLCALLBACK(int) drvAudioCallback(PPDMIAUDIOCONNECTOR pInterface, PDMAUDIOCALLBACKTYPE enmType,
                                          void *pvUser, size_t cbUser)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pvUser,     VERR_INVALID_POINTER);
    AssertReturn(cbUser,        VERR_INVALID_PARAMETER);

    PDRVAUDIO     pThis       = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);
    PRTLISTANCHOR pListAnchor = NULL;

    switch (enmType)
    {
        case PDMAUDIOCALLBACKTYPE_INPUT:
            pListAnchor = &pThis->lstCBIn;
            break;

        case PDMAUDIOCALLBACKTYPE_OUTPUT:
            pListAnchor = &pThis->lstCBOut;
            break;

        default:
            AssertMsgFailed(("Not supported\n"));
            break;
    }

    if (pListAnchor)
    {
        PPDMAUDIOCALLBACK pCB;
        RTListForEach(pListAnchor, pCB, PDMAUDIOCALLBACK, Node)
        {
            Assert(pCB->enmType == enmType);
            pCB->pfnCallback(enmType, pCB->pvCtx, pCB->cbCtx, pvUser, cbUser);
        }
    }

    return VINF_SUCCESS;
}
#endif

/**
 * Initializes the host backend and queries its initial configuration.
 * If the host backend fails, VERR_AUDIO_BACKEND_INIT_FAILED will be returned.
 *
 * Note: As this routine is called when attaching to the device LUN in the
 *       device emulation, we either check for success or VERR_AUDIO_BACKEND_INIT_FAILED.
 *       Everything else is considered as fatal and must be handled separately in
 *       the device emulation!
 *
 * @return  IPRT status code.
 * @param   pThis               Driver instance to be called.
 * @param   pCfgHandle          CFGM configuration handle to use for this driver.
 */
static int drvAudioHostInit(PCFGMNODE pCfgHandle, PDRVAUDIO pThis)
{
    /* pCfgHandle is optional. */
    RT_NOREF(pCfgHandle);
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);

    LogFlowFuncEnter();

    AssertPtr(pThis->pHostDrvAudio);
    int rc = pThis->pHostDrvAudio->pfnInit(pThis->pHostDrvAudio);
    if (RT_FAILURE(rc))
    {
        LogFlowFunc(("Initialization of lower driver failed with rc=%Rrc\n", rc));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    /* Get the configuration data from backend. */
    rc = pThis->pHostDrvAudio->pfnGetConf(pThis->pHostDrvAudio, &pThis->BackendCfg);
    if (RT_FAILURE(rc))
    {
        LogFlowFunc(("Getting backend configuration failed with rc=%Rrc\n", rc));
        return VERR_AUDIO_BACKEND_INIT_FAILED;
    }

    uint32_t cMaxHstStrmsOut = pThis->BackendCfg.cMaxHstStrmsOut;
    size_t cbHstStrmsOut     = pThis->BackendCfg.cbStreamOut;

    if (cbHstStrmsOut)
    {
        pThis->cFreeOutputStreams = cMaxHstStrmsOut;
    }
    else
        pThis->cFreeOutputStreams = 0;

    uint32_t cMaxHstStrmsIn = pThis->BackendCfg.cMaxHstStrmsIn;
    size_t cbHstStrmIn      = pThis->BackendCfg.cbStreamIn;

    if (cbHstStrmIn)
    {
        /*
         * Note:
         *  - Our AC'97 emulation has two inputs, line (ac97.pi) and microphone (ac97.mc).
         *  - Our HDA emulation currently has only line input (hda.pi).
         */
        pThis->cFreeInputStreams = cMaxHstStrmsIn;
    }
    else
        pThis->cFreeInputStreams = 0;

    LogFlowFunc(("cMaxHstStrmsOut=%RU32 (cb=%zu), cMaxHstStrmsIn=%RU32 (cb=%zu)\n",
                 cMaxHstStrmsOut, cbHstStrmsOut, cMaxHstStrmsIn, cbHstStrmIn));

    LogFlowFunc(("cFreeInputStreams=%RU8, cFreeOutputStreams=%RU8\n",
                 pThis->cFreeInputStreams, pThis->cFreeOutputStreams));

    LogRel(("Audio: Host audio backend supports %RU32 output streams and %RU32 input streams at once\n",
            /* Clamp for logging. Unlimited streams are defined by UINT32_MAX. */
            RT_MIN(64, cMaxHstStrmsOut), RT_MIN(64, cMaxHstStrmsIn)));

    LogFlowFuncLeave();
    return VINF_SUCCESS;
}

static void drvAudioStateHandler(PPDMDRVINS pDrvIns, PDMAUDIOSTREAMCMD enmCmd)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    PDRVAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVAUDIO);

    LogFlowFunc(("enmCmd=%RU32\n", enmCmd));

    if (!pThis->pHostDrvAudio)
        return;

    PPDMAUDIOHSTSTRMOUT pHstStrmOut = NULL;
    while ((pHstStrmOut = drvAudioHstFindAnyEnabledOut(pThis, pHstStrmOut)))
        drvAudioControlHstOut(pThis, pHstStrmOut, enmCmd);

    PPDMAUDIOHSTSTRMIN pHstStrmIn = NULL;
    while ((pHstStrmIn = drvAudioFindNextEnabledHstIn(pThis, pHstStrmIn)))
        drvAudioControlHstIn(pThis, pHstStrmIn, enmCmd);
}

static struct audio_option audio_options[] =
{
    /* DAC */
    {"DACFixedSettings", AUD_OPT_BOOL, &conf.fixed_out.enabled,
     "Use fixed settings for host DAC", NULL, 0},

    {"DACFixedFreq", AUD_OPT_INT, &conf.fixed_out.settings.uHz,
     "Frequency for fixed host DAC", NULL, 0},

    {"DACFixedFmt", AUD_OPT_FMT, &conf.fixed_out.settings.enmFormat,
     "Format for fixed host DAC", NULL, 0},

    {"DACFixedChannels", AUD_OPT_INT, &conf.fixed_out.settings.cChannels,
     "Number of channels for fixed DAC (1 - mono, 2 - stereo)", NULL, 0},

    {"DACVoices", AUD_OPT_INT, &conf.fixed_out.cStreams, /** @todo Rename! */
     "Number of streams for DAC", NULL, 0},

    /* ADC */
    {"ADCFixedSettings", AUD_OPT_BOOL, &conf.fixed_in.enabled,
     "Use fixed settings for host ADC", NULL, 0},

    {"ADCFixedFreq", AUD_OPT_INT, &conf.fixed_in.settings.uHz,
     "Frequency for fixed host ADC", NULL, 0},

    {"ADCFixedFmt", AUD_OPT_FMT, &conf.fixed_in.settings.enmFormat,
     "Format for fixed host ADC", NULL, 0},

    {"ADCFixedChannels", AUD_OPT_INT, &conf.fixed_in.settings.cChannels,
     "Number of channels for fixed ADC (1 - mono, 2 - stereo)", NULL, 0},

    {"ADCVoices", AUD_OPT_INT, &conf.fixed_in.cStreams, /** @todo Rename! */
     "Number of streams for ADC", NULL, 0},

    /* Misc */
    {"TimerFreq", AUD_OPT_INT, &conf.period.hz,
     "Timer frequency in Hz (0 - use lowest possible)", NULL, 0},

    {"PLIVE", AUD_OPT_BOOL, &conf.plive,
     "(undocumented)", NULL, 0}, /** @todo What is this? */

    {NULL, AUD_OPT_INT, NULL, NULL, NULL, 0}
};

static DECLCALLBACK(int) drvAudioInit(PCFGMNODE pCfgHandle, PPDMDRVINS pDrvIns)
{
    AssertPtrReturn(pCfgHandle, VERR_INVALID_POINTER);
    AssertPtrReturn(pDrvIns, VERR_INVALID_POINTER);

    LogRel(("Audio: Using VBox 5.0.x audio code!\n"));

    PDRVAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVAUDIO);
    LogFlowFunc(("pDrvAudio=%p, pDrvIns=%p\n", pThis, pDrvIns));

    RTListInit(&pThis->lstHstStrmIn);
    RTListInit(&pThis->lstHstStrmOut);
#ifdef VBOX_WITH_AUDIO_CALLBACKS
    RTListInit(&pThis->lstCBIn);
    RTListInit(&pThis->lstCBOut);
#endif

    int rc = RTCritSectInit(&pThis->CritSect);
    if (RT_SUCCESS(rc))
    {
        rc = drvAudioProcessOptions(pCfgHandle, "AUDIO", audio_options);
        /** @todo Check for invalid options? */

        pThis->cFreeOutputStreams = conf.fixed_out.cStreams;
        pThis->cFreeInputStreams  = conf.fixed_in.cStreams;

        if (!pThis->cFreeOutputStreams)
            pThis->cFreeOutputStreams = 1;

        if (!pThis->cFreeInputStreams)
            pThis->cFreeInputStreams = 1;
    }

    /*
     * If everything went well, initialize the lower driver.
     */
    if (RT_SUCCESS(rc))
        rc = drvAudioHostInit(pCfgHandle, pThis);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) drvAudioRead(PPDMIAUDIOCONNECTOR pInterface, PPDMAUDIOGSTSTRMIN pGstStrmIn,
                                      void *pvBuf, uint32_t cbBuf, uint32_t *pcbRead)
{
    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);

    AssertPtrReturn(pGstStrmIn, VERR_INVALID_POINTER);
    AssertPtrReturn(pvBuf,      VERR_INVALID_POINTER);
    AssertReturn(cbBuf,         VERR_INVALID_PARAMETER);
    /* pcbWritten is optional. */

    int rc = RTCritSectEnter(&pThis->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    if (!pThis->pHostDrvAudio->pfnIsEnabled(pThis->pHostDrvAudio, PDMAUDIODIR_IN))
    {
        if (pcbRead)
            *pcbRead = 0;

        return RTCritSectLeave(&pThis->CritSect);
    }

    PPDMAUDIOHSTSTRMIN pHstStrmIn = pGstStrmIn->pHstStrmIn;
    AssertPtrReturn(pHstStrmIn, VERR_INVALID_POINTER);

    AssertMsg(pGstStrmIn->pHstStrmIn->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED,
              ("Reading from disabled host input stream \"%s\" not possible\n", pGstStrmIn->MixBuf.pszName));

    /*
     * Read from the parent buffer (that is, the guest buffer) which
     * should have the audio data in the format the guest needs.
     */
    uint32_t cRead;
    rc = AudioMixBufReadCirc(&pGstStrmIn->MixBuf, pvBuf, cbBuf, &cRead);
    if (RT_SUCCESS(rc))
    {
        AudioMixBufFinish(&pGstStrmIn->MixBuf, cRead);

        if (pcbRead)
            *pcbRead = AUDIOMIXBUF_S2B(&pGstStrmIn->MixBuf, cRead);
    }

    LogFlowFunc(("cRead=%RU32 (%RU32 bytes), rc=%Rrc\n",
                 cRead, AUDIOMIXBUF_S2B(&pGstStrmIn->MixBuf, cRead), rc));

    int rc2 = RTCritSectLeave(&pThis->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    return rc;
}

static DECLCALLBACK(int) drvAudioEnableOut(PPDMIAUDIOCONNECTOR pInterface,
                                           PPDMAUDIOGSTSTRMOUT pGstStrmOut, bool fEnable)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    /* pGstStrmOut is optional. */

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc = VINF_SUCCESS;

    if (pGstStrmOut)
    {
        PPDMAUDIOHSTSTRMOUT pHstStrmOut = pGstStrmOut->pHstStrmOut;
        AssertPtr(pHstStrmOut);

        if (fEnable)
        {
            /* Is a pending disable outstanding? Then disable first. */
            if (pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_PENDING_DISABLE)
            {
                rc = drvAudioControlHstOut(pThis, pHstStrmOut, PDMAUDIOSTREAMCMD_DISABLE);
                if (RT_SUCCESS(rc))
                    pHstStrmOut->fStatus &= ~PDMAUDIOSTRMSTS_FLAG_PENDING_DISABLE;
            }

            if (RT_SUCCESS(rc))
                rc = drvAudioControlHstOut(pThis, pHstStrmOut, PDMAUDIOSTREAMCMD_ENABLE);
        }
        else /* Disable */
        {
            if (pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_ENABLED)
            {
                uint32_t cGstStrmsActive = 0;

                /*
                 * Check if there are any active guest streams assigned
                 * to this host stream which still are being marked as active.
                 *
                 * In that case we have to defer closing the host stream and
                 * wait until all guest streams have been finished.
                 */
                PPDMAUDIOGSTSTRMOUT pIter;
                RTListForEach(&pHstStrmOut->lstGstStrmOut, pIter, PDMAUDIOGSTSTRMOUT, Node)
                {
                    if (pIter->State.fActive)
                    {
                        cGstStrmsActive++;
                        break; /* At least one assigned & active guest stream is enough. */
                    }
                }

                /* Do we need to defer closing the host stream? */
                if (cGstStrmsActive >= 1)
                    pHstStrmOut->fStatus |= PDMAUDIOSTRMSTS_FLAG_PENDING_DISABLE;

                /* Can we close the host stream now instead of deferring it? */
                if (!(pHstStrmOut->fStatus & PDMAUDIOSTRMSTS_FLAG_PENDING_DISABLE))
                    rc = drvAudioControlHstOut(pThis, pHstStrmOut, PDMAUDIOSTREAMCMD_DISABLE);
            }
        }

        if (RT_SUCCESS(rc))
            pGstStrmOut->State.fActive = fEnable;

        LogFlowFunc(("%s: fEnable=%RTbool, fStatus=0x%x, rc=%Rrc\n",
                     pGstStrmOut->MixBuf.pszName, fEnable, pHstStrmOut->fStatus, rc));
    }

    return rc;
}

static DECLCALLBACK(int) drvAudioEnableIn(PPDMIAUDIOCONNECTOR pInterface,
                                          PPDMAUDIOGSTSTRMIN pGstStrmIn, bool fEnable)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    /* pGstStrmIn is optional. */

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc = VINF_SUCCESS;

    if (pGstStrmIn)
    {
        PPDMAUDIOHSTSTRMIN pHstStrmIn = pGstStrmIn->pHstStrmIn;
        AssertPtr(pHstStrmIn);

        LogFlowFunc(("%s: fEnable=%RTbool\n", pGstStrmIn->MixBuf.pszName, fEnable));

        rc = drvAudioControlHstIn(pThis, pHstStrmIn,
                                  fEnable ? PDMAUDIOSTREAMCMD_ENABLE : PDMAUDIOSTREAMCMD_DISABLE);
        if (RT_SUCCESS(rc))
            pGstStrmIn->State.fActive = fEnable;

        LogFlowFunc(("%s: fEnable=%RTbool, rc=%Rrc\n", pGstStrmIn->MixBuf.pszName, fEnable, rc));
    }

    return rc;
}

static DECLCALLBACK(bool) drvAudioIsValidIn(PPDMIAUDIOCONNECTOR pInterface, PPDMAUDIOGSTSTRMIN  pGstStrmIn)
{
    RT_NOREF(pInterface);
    return (pGstStrmIn != NULL);
}

static DECLCALLBACK(bool) drvAudioIsValidOut(PPDMIAUDIOCONNECTOR pInterface, PPDMAUDIOGSTSTRMOUT pGstStrmOut)
{
    RT_NOREF(pInterface);
    return (pGstStrmOut != NULL);
}

static DECLCALLBACK(int) drvAudioCreateIn(PPDMIAUDIOCONNECTOR pInterface, const char *pszName,
                                          PDMAUDIORECSOURCE enmRecSource, PPDMAUDIOSTREAMCFG pCfg,
                                          PPDMAUDIOGSTSTRMIN *ppGstStrmIn)
{
    AssertPtrReturn(pInterface,  VERR_INVALID_POINTER);
    AssertPtrReturn(ppGstStrmIn, VERR_INVALID_POINTER);
    AssertPtrReturn(pszName,     VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg,        VERR_INVALID_POINTER);
    AssertPtrReturn(ppGstStrmIn, VERR_INVALID_POINTER);

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc = RTCritSectEnter(&pThis->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    LogFlowFunc(("pszName=%s, pCfg=%p\n", pszName, pCfg));

    if (!drvAudioStreamCfgIsValid(pCfg))
    {
        LogFunc(("Input stream configuration is not valid, bailing out\n"));
        rc = VERR_INVALID_PARAMETER;
    }

    PPDMAUDIOGSTSTRMIN pGstStrmIn = *ppGstStrmIn;
    if (   RT_SUCCESS(rc)
        && pGstStrmIn
        && drvAudioPCMPropsAreEqual(&pGstStrmIn->Props, pCfg))
    {
        LogFunc(("[%s] Exists and matches required configuration, skipping creation\n",
                 pGstStrmIn->MixBuf.pszName));
        rc = VWRN_ALREADY_EXISTS;
    }

    if (rc != VINF_SUCCESS) /* Note: Can be VWRN_ALREADY_EXISTS, so don't use VINF_SUCCESS here. */
    {
        int rc2 = RTCritSectLeave(&pThis->CritSect);
        AssertRC(rc2);

        return rc;
    }

    if (   !conf.fixed_in.enabled
        && pGstStrmIn)
    {
        drvAudioDestroyGstIn(pThis, pGstStrmIn);
        pGstStrmIn = NULL;
    }

    if (pGstStrmIn)
    {
        PPDMAUDIOHSTSTRMIN pHstStrmIn = pGstStrmIn->pHstStrmIn;
        AssertPtr(pHstStrmIn);

        drvAudioGstInFreeRes(pGstStrmIn);

        char *pszTemp;
        if (RTStrAPrintf(&pszTemp, "%s (Guest)", pszName) <= 0)
        {
            RTMemFree(pGstStrmIn);

            int rc2 = RTCritSectLeave(&pThis->CritSect);
            AssertRC(rc2);

            return VERR_NO_MEMORY;
        }

        rc = drvAudioGstInInit(pGstStrmIn, pHstStrmIn, pszName, pCfg);

        RTStrFree(pszTemp);
    }
    else
        rc = drvAudioCreateStreamPairIn(pThis, pszName, enmRecSource, pCfg, &pGstStrmIn);

    if (RT_SUCCESS(rc))
    {
        if (pGstStrmIn)
            *ppGstStrmIn = pGstStrmIn;
    }
    else
    {
        switch (rc)
        {
            case VERR_NO_MORE_HANDLES: /** @todo Find a better rc. */
                LogRel(("Audio: Skipping to create input stream \"%s\", " \
                        "as the host audio backend reached its maximum of concurrent audio input streams\n", pszName));
                break;

            default:
                break;
        }
    }

    int rc2 = RTCritSectLeave(&pThis->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) drvAudioCreateOut(PPDMIAUDIOCONNECTOR pInterface, const char *pszName,
                                           PPDMAUDIOSTREAMCFG pCfg, PPDMAUDIOGSTSTRMOUT *ppGstStrmOut)
{
    AssertPtrReturn(pInterface,   VERR_INVALID_POINTER);
    AssertPtrReturn(pszName,      VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg,         VERR_INVALID_POINTER);
    AssertPtrReturn(ppGstStrmOut, VERR_INVALID_POINTER);

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc = RTCritSectEnter(&pThis->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    LogFlowFunc(("pszName=%s, pCfg=%p\n", pszName, pCfg));

    if (!drvAudioStreamCfgIsValid(pCfg))
    {
        LogFunc(("Output stream configuration is not valid, bailing out\n"));
        rc = VERR_INVALID_PARAMETER;
    }

    PPDMAUDIOGSTSTRMOUT pGstStrmOut = *ppGstStrmOut;
    if (   RT_SUCCESS(rc)
        && pGstStrmOut
        && drvAudioPCMPropsAreEqual(&pGstStrmOut->Props, pCfg))
    {
        LogFunc(("[%s] Exists and matches required configuration, skipping creation\n",
                 pGstStrmOut->MixBuf.pszName));

        rc = VWRN_ALREADY_EXISTS;
    }

    if (rc != VINF_SUCCESS) /* Note: Can be VWRN_ALREADY_EXISTS, so don't use VINF_SUCCESS here. */
    {
        int rc2 = RTCritSectLeave(&pThis->CritSect);
        AssertRC(rc2);

        return rc;
    }

#if 0
    /* Any live samples that need to be updated after
     * we set the new parameters? */
    PPDMAUDIOGSTSTRMOUT pOldGstStrmOut = NULL;
    uint32_t cLiveSamples = 0;

    if (   conf.plive
        && pGstStrmOut
        && (   !pGstStrmOut->State.fActive
            && !pGstStrmOut->State.fEmpty))
    {
        cLiveSamples = pGstStrmOut->cTotalSamplesWritten;
        if (cLiveSamples)
        {
            pOldGstStrmOut = pGstStrmOut;
            pGstStrmOut = NULL;
        }
    }
#endif

    if (   pGstStrmOut
        && !conf.fixed_out.enabled)
    {
        drvAudioDestroyGstOut(pThis, pGstStrmOut);
        pGstStrmOut = NULL;
    }

    if (pGstStrmOut)
    {
        PPDMAUDIOHSTSTRMOUT pHstStrmOut = pGstStrmOut->pHstStrmOut;
        AssertPtr(pHstStrmOut);

        drvAudioGstOutFreeRes(pGstStrmOut);

        rc = drvAudioGstOutInit(pGstStrmOut, pHstStrmOut, pszName, pCfg);
    }
    else
    {
        rc = drvAudioCreateStreamPairOut(pThis, pszName, pCfg, &pGstStrmOut);
        if (RT_FAILURE(rc))
            LogFunc(("Failed to create output stream \"%s\", rc=%Rrc\n", pszName, rc));
    }

    if (RT_SUCCESS(rc))
    {
        if (pGstStrmOut)
            *ppGstStrmOut = pGstStrmOut;
#if 0
        /* Update remaining live samples with new rate. */
        if (cLiveSamples)
        {
            AssertPtr(pOldGstStrmOut);

            uint32_t cSamplesMixed =
                (cLiveSamples << pOldGstStrmOut->Props.cShift)
                * pOldGstStrmOut->Props.cbPerSec
                / (*ppGstStrmOut)->Props.cbPerSec;

            pGstStrmOut->cTotalSamplesWritten += cSamplesMixed;
        }
#endif
    }
    else
    {
        switch (rc)
        {
            case VERR_NO_MORE_HANDLES: /** @todo Find a better rc. */
                LogRel(("Audio: Skipping to create output stream \"%s\", " \
                        "as the host audio backend reached its maximum of concurrent audio output streams\n", pszName));
                break;

            default:
                break;
        }
    }

    int rc2 = RTCritSectLeave(&pThis->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(int) drvAudioGetConfiguration(PPDMIAUDIOCONNECTOR pInterface, PPDMAUDIOBACKENDCFG pCfg)
{
    AssertPtrReturn(pInterface, VERR_INVALID_POINTER);
    AssertPtrReturn(pCfg,       VERR_INVALID_POINTER);

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc = RTCritSectEnter(&pThis->CritSect);
    if (RT_FAILURE(rc))
        return rc;

    rc = pThis->pHostDrvAudio->pfnGetConf(pThis->pHostDrvAudio, pCfg);

    int rc2 = RTCritSectLeave(&pThis->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static DECLCALLBACK(bool) drvAudioIsActiveIn(PPDMIAUDIOCONNECTOR pInterface, PPDMAUDIOGSTSTRMIN pGstStrmIn)
{
    AssertPtrReturn(pInterface, false);
    /* pGstStrmIn is optional. */

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc2 = RTCritSectEnter(&pThis->CritSect);
    AssertRC(rc2);

    bool fRet = pGstStrmIn ? pGstStrmIn->State.fActive : false;

    rc2 = RTCritSectLeave(&pThis->CritSect);
    AssertRC(rc2);

    return fRet;
}

static DECLCALLBACK(bool) drvAudioIsActiveOut(PPDMIAUDIOCONNECTOR pInterface, PPDMAUDIOGSTSTRMOUT pGstStrmOut)
{
    AssertPtrReturn(pInterface,  false);
    /* pGstStrmOut is optional. */

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc2 = RTCritSectEnter(&pThis->CritSect);
    AssertRC(rc2);

    bool fRet = pGstStrmOut ? pGstStrmOut->State.fActive : false;

    rc2 = RTCritSectLeave(&pThis->CritSect);
    AssertRC(rc2);

    return fRet;
}

static DECLCALLBACK(void) drvAudioDestroyIn(PPDMIAUDIOCONNECTOR pInterface, PPDMAUDIOGSTSTRMIN pGstStrmIn)
{
    AssertPtrReturnVoid(pInterface);
    /* pGstStrmIn is optional. */

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc2 = RTCritSectEnter(&pThis->CritSect);
    AssertRC(rc2);

    if (pGstStrmIn)
        drvAudioDestroyGstIn(pThis, pGstStrmIn);

    rc2 = RTCritSectLeave(&pThis->CritSect);
    AssertRC(rc2);
}

static DECLCALLBACK(void) drvAudioDestroyOut(PPDMIAUDIOCONNECTOR pInterface, PPDMAUDIOGSTSTRMOUT pGstStrmOut)
{
    AssertPtrReturnVoid(pInterface);
    /* pGstStrmOut is optional. */

    PDRVAUDIO pThis = PDMIAUDIOCONNECTOR_2_DRVAUDIO(pInterface);

    int rc2 = RTCritSectEnter(&pThis->CritSect);
    AssertRC(rc2);

    if (pGstStrmOut)
        drvAudioDestroyGstOut(pThis, pGstStrmOut);

    rc2 = RTCritSectLeave(&pThis->CritSect);
    AssertRC(rc2);
}

/********************************************************************/

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) drvAudioQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    LogFlowFunc(("pInterface=%p, pszIID=%s\n", pInterface, pszIID));

    PPDMDRVINS pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVAUDIO  pThis   = PDMINS_2_DATA(pDrvIns, PDRVAUDIO);

    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIAUDIOCONNECTOR, &pThis->IAudioConnector);

    return NULL;
}

/**
 * Power Off notification.
 *
 * @param   pDrvIns     The driver instance data.
 */
static DECLCALLBACK(void) drvAudioPowerOff(PPDMDRVINS pDrvIns)
{
    LogFlowFuncEnter();
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);

    PDRVAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVAUDIO);

    if (!pThis->pHostDrvAudio)
        return;

    /* Tear down all host output streams. */
    PPDMAUDIOHSTSTRMOUT pHstStrmOut = NULL;
    while ((pHstStrmOut = drvAudioFindAnyHstOut(pThis, pHstStrmOut)))
    {
        drvAudioControlHstOut(pThis, pHstStrmOut, PDMAUDIOSTREAMCMD_DISABLE);
        pThis->pHostDrvAudio->pfnFiniOut(pThis->pHostDrvAudio, pHstStrmOut);
    }

    /* Tear down all host input streams. */
    PPDMAUDIOHSTSTRMIN pHstStrmIn = NULL;
    while ((pHstStrmIn = drvAudioFindNextHstIn(pThis, pHstStrmIn)))
    {
        drvAudioControlHstIn(pThis, pHstStrmIn, PDMAUDIOSTREAMCMD_DISABLE);
        pThis->pHostDrvAudio->pfnFiniIn(pThis->pHostDrvAudio, pHstStrmIn);
    }

    if (pThis->pHostDrvAudio->pfnShutdown)
        pThis->pHostDrvAudio->pfnShutdown(pThis->pHostDrvAudio);

#ifdef VBOX_WITH_AUDIO_CALLBACKS
    PPDMAUDIOCALLBACK pCB, pCBNext;
    RTListForEachSafe(&pThis->lstCBIn, pCB, pCBNext, PDMAUDIOCALLBACK, Node)
        drvAudioCallbackDestroy(pCB);

    RTListForEachSafe(&pThis->lstCBOut, pCB, pCBNext, PDMAUDIOCALLBACK, Node)
        drvAudioCallbackDestroy(pCB);
#endif

    LogFlowFuncLeave();
}

/**
 * Constructs an audio driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
static DECLCALLBACK(int) drvAudioConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfgHandle, uint32_t fFlags)
{
    LogFlowFunc(("pDrvIns=%#p, pCfgHandle=%#p, fFlags=%x\n", pDrvIns, pCfgHandle, fFlags));

    PDRVAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVAUDIO);
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);

    /*
     * Init the static parts.
     */
    pThis->pDrvIns                                   = pDrvIns;
    /* IBase. */
    pDrvIns->IBase.pfnQueryInterface                 = drvAudioQueryInterface;
    /* IAudioConnector. */
    pThis->IAudioConnector.pfnQueryStatus            = drvAudioQueryStatus;
    pThis->IAudioConnector.pfnRead                   = drvAudioRead;
    pThis->IAudioConnector.pfnWrite                  = drvAudioWrite;
    pThis->IAudioConnector.pfnGetConfiguration       = drvAudioGetConfiguration;
    pThis->IAudioConnector.pfnIsActiveIn             = drvAudioIsActiveIn;
    pThis->IAudioConnector.pfnIsActiveOut            = drvAudioIsActiveOut;
    pThis->IAudioConnector.pfnIsValidIn              = drvAudioIsValidIn;
    pThis->IAudioConnector.pfnIsValidOut             = drvAudioIsValidOut;
    pThis->IAudioConnector.pfnEnableOut              = drvAudioEnableOut;
    pThis->IAudioConnector.pfnEnableIn               = drvAudioEnableIn;
    pThis->IAudioConnector.pfnDestroyIn              = drvAudioDestroyIn;
    pThis->IAudioConnector.pfnDestroyOut             = drvAudioDestroyOut;
    pThis->IAudioConnector.pfnCreateIn               = drvAudioCreateIn;
    pThis->IAudioConnector.pfnCreateOut              = drvAudioCreateOut;
    pThis->IAudioConnector.pfnPlayOut                = drvAudioPlayOut;
#ifdef VBOX_WITH_AUDIO_CALLBACKS
    pThis->IAudioConnector.pfnRegisterCallbacks      = drvAudioRegisterCallbacks;
    pThis->IAudioConnector.pfnCallback               = drvAudioCallback;
#endif

    /*
     * Attach driver below and query its connector interface.
     */
    PPDMIBASE pDownBase;
    int rc = PDMDrvHlpAttach(pDrvIns, fFlags, &pDownBase);
    if (RT_FAILURE(rc))
    {
        LogRel(("Audio: Failed to attach to driver %p below (flags=0x%x), rc=%Rrc\n",
                pDrvIns, fFlags, rc));
        return rc;
    }

    pThis->pHostDrvAudio = PDMIBASE_QUERY_INTERFACE(pDownBase, PDMIHOSTAUDIO);
    if (!pThis->pHostDrvAudio)
    {
        LogRel(("Audio: Failed to query interface for underlying host driver\n"));
        return PDMDRV_SET_ERROR(pDrvIns, VERR_PDM_MISSING_INTERFACE_BELOW,
                                N_("Host audio backend missing or invalid"));
    }

#ifdef DEBUG_andy
    CFGMR3Dump(pCfgHandle);
#endif

    rc = drvAudioInit(pCfgHandle, pDrvIns);
    if (RT_SUCCESS(rc))
    {
        pThis->fTerminate = false;
        pThis->pDrvIns    = pDrvIns;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

/**
 * Destructs an audio driver instance.
 *
 * @copydoc FNPDMDRVDESTRUCT
 */
static DECLCALLBACK(void) drvAudioDestruct(PPDMDRVINS pDrvIns)
{
    LogFlowFuncEnter();

    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    PDRVAUDIO pThis = PDMINS_2_DATA(pDrvIns, PDRVAUDIO);

    if (RTCritSectIsInitialized(&pThis->CritSect))
    {
        int rc2 = RTCritSectDelete(&pThis->CritSect);
        AssertRC(rc2);
    }
}

/**
 * Suspend notification.
 *
 * @param   pDrvIns     The driver instance data.
 */
static DECLCALLBACK(void) drvAudioSuspend(PPDMDRVINS pDrvIns)
{
    drvAudioStateHandler(pDrvIns, PDMAUDIOSTREAMCMD_PAUSE);
}

/**
 * Resume notification.
 *
 * @param   pDrvIns     The driver instance data.
 */
static DECLCALLBACK(void) drvAudioResume(PPDMDRVINS pDrvIns)
{
    drvAudioStateHandler(pDrvIns, PDMAUDIOSTREAMCMD_RESUME);
}

/**
 * Audio driver registration record.
 */
const PDMDRVREG g_DrvAUDIO =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "AUDIO",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Audio connector driver",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass */
    PDM_DRVREG_CLASS_AUDIO,
    /* cMaxInstances */
    2,
    /* cbInstance */
    sizeof(DRVAUDIO),
    /* pfnConstruct */
    drvAudioConstruct,
    /* pfnDestruct */
    drvAudioDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    drvAudioSuspend,
    /* pfnResume */
    drvAudioResume,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    drvAudioPowerOff,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};
