/* $Id: AudioMixBuffer.cpp $ */
/** @file
 * VBox audio: Audio mixing buffer for converting reading/writing audio
 *             samples.
 */

/*
 * Copyright (C) 2014-2016 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#define LOG_GROUP LOG_GROUP_AUDIO_MIXER_BUFFER
#include <VBox/log.h>

/*
 * DEBUG_DUMP_PCM_DATA enables dumping the raw PCM data
 * to a file on the host. Be sure to adjust DEBUG_DUMP_PCM_DATA_PATH
 * to your needs before using this!
 */
#if 0
# define DEBUG_DUMP_PCM_DATA
# ifdef RT_OS_WINDOWS
#  define DEBUG_DUMP_PCM_DATA_PATH "c:\\temp\\"
# else
#  define DEBUG_DUMP_PCM_DATA_PATH "/tmp/"
# endif
#endif

#include <iprt/asm-math.h>
#include <iprt/assert.h>
#ifdef DEBUG_DUMP_PCM_DATA
# include <iprt/file.h>
#endif
#include <iprt/mem.h>
#include <iprt/string.h> /* For RT_BZERO. */

#ifdef TESTCASE
# define LOG_ENABLED
# include <iprt/stream.h>
#endif
#include <VBox/err.h>

#include "AudioMixBuffer.h"

#if 0
# define AUDMIXBUF_LOG(x) LogFlowFunc(x)
#else
# if defined(TESTCASE)
#  define AUDMIXBUF_LOG(x) LogFunc(x)
# else
#  define AUDMIXBUF_LOG(x) do {} while (0)
# endif
#endif


/*
 *   Soft Volume Control
 *
 * The external code supplies an 8-bit volume (attenuation) value in the
 * 0 .. 255 range. This represents 0 to -96dB attenuation where an input
 * value of 0 corresponds to -96dB and 255 corresponds to 0dB (unchanged).
 *
 * Each step thus correspons to 96 / 256 or 0.375dB. Every 6dB (16 steps)
 * represents doubling the sample value.
 *
 * For internal use, the volume control needs to be converted to a 16-bit
 * (sort of) exponential value between 1 and 65536. This is used with fixed
 * point arithmetic such that 65536 means 1.0 and 1 means 1/65536.
 *
 * For actual volume calculation, 33.31 fixed point is used. Maximum (or
 * unattenuated) volume is represented as 0x40000000; conveniently, this
 * value fits into a uint32_t.
 *
 * To enable fast processing, the maximum volume must be a power of two
 * and must not have a sign when converted to int32_t. While 0x80000000
 * violates these constraints, 0x40000000 does not.
 */


/** Logarithmic/exponential volume conversion table. */
static uint32_t s_aVolumeConv[256] = {
        1,     1,     1,     1,     1,     1,     1,     1, /*   7 */
        1,     2,     2,     2,     2,     2,     2,     2, /*  15 */
        2,     2,     2,     2,     2,     3,     3,     3, /*  23 */
        3,     3,     3,     3,     4,     4,     4,     4, /*  31 */
        4,     4,     5,     5,     5,     5,     5,     6, /*  39 */
        6,     6,     6,     7,     7,     7,     8,     8, /*  47 */
        8,     9,     9,    10,    10,    10,    11,    11, /*  55 */
       12,    12,    13,    13,    14,    15,    15,    16, /*  63 */
       17,    17,    18,    19,    20,    21,    22,    23, /*  71 */
       24,    25,    26,    27,    28,    29,    31,    32, /*  79 */
       33,    35,    36,    38,    40,    41,    43,    45, /*  87 */
       47,    49,    52,    54,    56,    59,    61,    64, /*  95 */
       67,    70,    73,    76,    79,    83,    87,    91, /* 103 */
       95,    99,   103,   108,   112,   117,   123,   128, /* 111 */
      134,   140,   146,   152,   159,   166,   173,   181, /* 119 */
      189,   197,   206,   215,   225,   235,   245,   256, /* 127 */
      267,   279,   292,   304,   318,   332,   347,   362, /* 135 */
      378,   395,   412,   431,   450,   470,   490,   512, /* 143 */
      535,   558,   583,   609,   636,   664,   693,   724, /* 151 */
      756,   790,   825,   861,   899,   939,   981,  1024, /* 159 */
     1069,  1117,  1166,  1218,  1272,  1328,  1387,  1448, /* 167 */
     1512,  1579,  1649,  1722,  1798,  1878,  1961,  2048, /* 175 */
     2139,  2233,  2332,  2435,  2543,  2656,  2774,  2896, /* 183 */
     3025,  3158,  3298,  3444,  3597,  3756,  3922,  4096, /* 191 */
     4277,  4467,  4664,  4871,  5087,  5312,  5547,  5793, /* 199 */
     6049,  6317,  6597,  6889,  7194,  7512,  7845,  8192, /* 207 */
     8555,  8933,  9329,  9742, 10173, 10624, 11094, 11585, /* 215 */
    12098, 12634, 13193, 13777, 14387, 15024, 15689, 16384, /* 223 */
    17109, 17867, 18658, 19484, 20347, 21247, 22188, 23170, /* 231 */
    24196, 25268, 26386, 27554, 28774, 30048, 31379, 32768, /* 239 */
    34219, 35734, 37316, 38968, 40693, 42495, 44376, 46341, /* 247 */
    48393, 50535, 52773, 55109, 57549, 60097, 62757, 65536, /* 255 */
};

/* Bit shift for fixed point conversion. */
#define AUDIOMIXBUF_VOL_SHIFT       30

/* Internal representation of 0dB volume (1.0 in fixed point). */
#define AUDIOMIXBUF_VOL_0DB         (1 << AUDIOMIXBUF_VOL_SHIFT)

AssertCompile(AUDIOMIXBUF_VOL_0DB <= 0x40000000);   /* Must always hold. */
AssertCompile(AUDIOMIXBUF_VOL_0DB == 0x40000000);   /* For now -- when only attenuation is used. */

/**
 * Structure for holding sample conversion parameters for
 * the audioMixBufConvFromXXX / audioMixBufConvToXXX macros.
 */
typedef struct AUDMIXBUF_CONVOPTS
{
    /** Number of audio samples to convert. */
    uint32_t       cSamples;
    /** Volume to apply during conversion. Pass 0
     *  to convert the original values. May not apply to
     *  all conversion functions. */
    PDMAUDIOVOLUME Volume;
} AUDMIXBUF_CONVOPTS, *PAUDMIXBUF_CONVOPTS;

/*
 * When running the audio testcases we want to verfiy
 * the macro-generated routines separately, so unmark them as being
 * inlined + static.
 */
#ifdef TESTCASE
# define AUDMIXBUF_MACRO_FN
#else
# define AUDMIXBUF_MACRO_FN static inline
#endif

#ifdef DEBUG
static uint64_t s_cSamplesMixedTotal = 0;
static inline void audioMixBufPrint(PPDMAUDIOMIXBUF pMixBuf);
#endif

typedef uint32_t (AUDMIXBUF_FN_CONVFROM) (PPDMAUDIOSAMPLE paDst, const void *pvSrc, uint32_t cbSrc, const PAUDMIXBUF_CONVOPTS pOpts);
typedef AUDMIXBUF_FN_CONVFROM *PAUDMIXBUF_FN_CONVFROM;

typedef void (AUDMIXBUF_FN_CONVTO) (void *pvDst, const PPDMAUDIOSAMPLE paSrc, const PAUDMIXBUF_CONVOPTS pOpts);
typedef AUDMIXBUF_FN_CONVTO *PAUDMIXBUF_FN_CONVTO;

/* Can return VINF_TRY_AGAIN for getting next pointer at beginning (circular) */

/**
 * Acquires (reads) a mutable pointer to the mixing buffer's audio samples without
 * any conversion done.
 ** @todo Rename to AudioMixBufPeek(Mutable/Raw)?
 ** @todo Protect the buffer's data?
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to acquire audio samples from.
 * @param   cSamplesToRead          Number of audio samples to read.
 * @param   ppvSamples              Returns a mutable pointer to the buffer's audio sample data.
 * @param   pcSamplesRead           Number of audio samples read (acquired).
 *
 * @remark  This function is not thread safe!
 */
int AudioMixBufAcquire(PPDMAUDIOMIXBUF pMixBuf, uint32_t cSamplesToRead,
                       PPDMAUDIOSAMPLE *ppvSamples, uint32_t *pcSamplesRead)
{
    AssertPtrReturn(pMixBuf, VERR_INVALID_POINTER);
    AssertPtrReturn(ppvSamples, VERR_INVALID_POINTER);
    AssertPtrReturn(pcSamplesRead, VERR_INVALID_POINTER);

    int rc;

    if (!cSamplesToRead)
    {
        *pcSamplesRead = 0;
        return VINF_SUCCESS;
    }

    uint32_t cSamplesRead;
    if (pMixBuf->offReadWrite + cSamplesToRead > pMixBuf->cSamples)
    {
        cSamplesRead = pMixBuf->cSamples - pMixBuf->offReadWrite;
        rc = VINF_TRY_AGAIN;
    }
    else
    {
        cSamplesRead = cSamplesToRead;
        rc = VINF_SUCCESS;
    }

    *ppvSamples = &pMixBuf->pSamples[pMixBuf->offReadWrite];
    AssertPtr(ppvSamples);

    pMixBuf->offReadWrite = (pMixBuf->offReadWrite + cSamplesRead) % pMixBuf->cSamples;
    Assert(pMixBuf->offReadWrite <= pMixBuf->cSamples);
    pMixBuf->cProcessed -= RT_MIN(cSamplesRead, pMixBuf->cProcessed);

    *pcSamplesRead = cSamplesRead;

    return rc;
}

/**
 * Returns available number of samples for reading.
 *
 * @return  uint32_t                Number of samples available for reading.
 * @param   pMixBuf                 Mixing buffer to return value for.
 */
uint32_t AudioMixBufAvail(PPDMAUDIOMIXBUF pMixBuf)
{
    AssertPtrReturn(pMixBuf, true);

    uint32_t cAvail;
    if (pMixBuf->pParent) /* Child */
        cAvail = pMixBuf->cMixed;
    else
        cAvail = pMixBuf->cProcessed;

    Assert(cAvail <= pMixBuf->cSamples);
    return cAvail;
}

/**
 * Clears the entire sample buffer.
 *
 * @param   pMixBuf                 Mixing buffer to clear.
 *
 */
void AudioMixBufClear(PPDMAUDIOMIXBUF pMixBuf)
{
    AssertPtrReturnVoid(pMixBuf);

    if (pMixBuf->cSamples)
        RT_BZERO(pMixBuf->pSamples, pMixBuf->cSamples * sizeof(PDMAUDIOSAMPLE));
}

/**
 * Clears (zeroes) the buffer by a certain amount of (processed) samples and
 * keeps track to eventually assigned children buffers.
 *
 * @param   pMixBuf                 Mixing buffer to clear.
 * @param   cSamplesToClear         Number of audio samples to clear.
 */
void AudioMixBufFinish(PPDMAUDIOMIXBUF pMixBuf, uint32_t cSamplesToClear)
{
    AUDMIXBUF_LOG(("cSamples=%RU32\n", cSamplesToClear));
    AUDMIXBUF_LOG(("%s: offReadWrite=%RU32, cProcessed=%RU32\n",
                   pMixBuf->pszName, pMixBuf->offReadWrite, pMixBuf->cProcessed));

    PPDMAUDIOMIXBUF pIter;
    RTListForEach(&pMixBuf->lstBuffers, pIter, PDMAUDIOMIXBUF, Node)
    {
        AUDMIXBUF_LOG(("\t%s: cMixed=%RU32 -> %RU32\n",
                       pIter->pszName, pIter->cMixed, pIter->cMixed - cSamplesToClear));

        pIter->cMixed -= RT_MIN(pIter->cMixed, cSamplesToClear);
        pIter->offReadWrite = 0;
    }

    uint32_t cLeft = RT_MIN(cSamplesToClear, pMixBuf->cSamples);
    uint32_t offClear;

    if (cLeft > pMixBuf->offReadWrite) /* Zero end of buffer first (wrap-around). */
    {
        AUDMIXBUF_LOG(("Clearing1: %RU32 - %RU32\n",
                       (pMixBuf->cSamples - (cLeft - pMixBuf->offReadWrite)),
                        pMixBuf->cSamples));

        RT_BZERO(pMixBuf->pSamples + (pMixBuf->cSamples - (cLeft - pMixBuf->offReadWrite)),
                 (cLeft - pMixBuf->offReadWrite) * sizeof(PDMAUDIOSAMPLE));

        cLeft -= cLeft - pMixBuf->offReadWrite;
        offClear = 0;
    }
    else
        offClear = pMixBuf->offReadWrite - cLeft;

    if (cLeft)
    {
        AUDMIXBUF_LOG(("Clearing2: %RU32 - %RU32\n",
                       offClear, offClear + cLeft));
        RT_BZERO(pMixBuf->pSamples + offClear, cLeft * sizeof(PDMAUDIOSAMPLE));
    }
}

/**
 * Destroys (uninitializes) a mixing buffer.
 *
 * @param   pMixBuf                 Mixing buffer to destroy.
 */
void AudioMixBufDestroy(PPDMAUDIOMIXBUF pMixBuf)
{
    if (!pMixBuf)
        return;

    AudioMixBufUnlink(pMixBuf);

    if (pMixBuf->pszName)
    {
        AUDMIXBUF_LOG(("%s\n", pMixBuf->pszName));

        RTStrFree(pMixBuf->pszName);
        pMixBuf->pszName = NULL;
    }

    if (pMixBuf->pRate)
    {
        RTMemFree(pMixBuf->pRate);
        pMixBuf->pRate = NULL;
    }

    if (pMixBuf->pSamples)
    {
        Assert(pMixBuf->cSamples);

        RTMemFree(pMixBuf->pSamples);
        pMixBuf->pSamples = NULL;
    }

    pMixBuf->cSamples = 0;
}

/**
 * Returns the size (in audio samples) of free audio buffer space.
 *
 * @return  uint32_t                Size (in audio samples) of free audio buffer space.
 * @param   pMixBuf                 Mixing buffer to return free size for.
 */
uint32_t AudioMixBufFree(PPDMAUDIOMIXBUF pMixBuf)
{
    AssertPtrReturn(pMixBuf, 0);

    uint32_t cSamplesFree;
    if (pMixBuf->pParent)
    {
        /*
         * As a linked child buffer we want to know how many samples
         * already have been consumed by the parent.
         */
        Assert(pMixBuf->cMixed <= pMixBuf->pParent->cSamples);
        cSamplesFree = pMixBuf->pParent->cSamples - pMixBuf->cMixed;
    }
    else /* As a parent. */
    {
        Assert(pMixBuf->cSamples >= pMixBuf->cProcessed);
        cSamplesFree = pMixBuf->cSamples - pMixBuf->cProcessed;
    }

    AUDMIXBUF_LOG(("%s: cSamplesFree=%RU32\n", pMixBuf->pszName, cSamplesFree));
    return cSamplesFree;
}

/**
 * Returns the size (in bytes) of free audio buffer space.
 *
 * @return  uint32_t                Size (in bytes) of free audio buffer space.
 * @param   pMixBuf                 Mixing buffer to return free size for.
 */
uint32_t AudioMixBufFreeBytes(PPDMAUDIOMIXBUF pMixBuf)
{
    return AUDIOMIXBUF_S2B(pMixBuf, AudioMixBufFree(pMixBuf));
}

/**
 * Allocates the internal audio sample buffer.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to allocate sample buffer for.
 * @param   cSamples                Number of audio samples to allocate.
 */
static int audioMixBufAlloc(PPDMAUDIOMIXBUF pMixBuf, uint32_t cSamples)
{
    AssertPtrReturn(pMixBuf, VERR_INVALID_POINTER);
    AssertReturn(cSamples, VERR_INVALID_PARAMETER);

    AUDMIXBUF_LOG(("%s: cSamples=%RU32\n", pMixBuf->pszName, cSamples));

    size_t cbSamples = cSamples * sizeof(PDMAUDIOSAMPLE);
    if (!cbSamples)
        return VERR_INVALID_PARAMETER;

    pMixBuf->pSamples = (PPDMAUDIOSAMPLE)RTMemAllocZ(cbSamples);
    if (!pMixBuf->pSamples)
        return VERR_NO_MEMORY;

    pMixBuf->cSamples = cSamples;

    return VINF_SUCCESS;
}

/** Note: Enabling this will generate huge logs! */
//#define DEBUG_MACROS

#ifdef DEBUG_MACROS
# define AUDMIXBUF_MACRO_LOG(x) AUDMIXBUF_LOG(x)
#elif defined(TESTCASE)
# define AUDMIXBUF_MACRO_LOG(x) RTPrintf x
#else
# define AUDMIXBUF_MACRO_LOG(x) do {} while (0)
#endif

/**
 * Macro for generating the conversion routines from/to different formats.
 * Be careful what to pass in/out, as most of the macros are optimized for speed and
 * thus don't do any bounds checking!
 *
 * Note: Currently does not handle any endianness conversion yet!
 */
#define AUDMIXBUF_CONVERT(_aName, _aType, _aMin, _aMax, _aSigned, _aShift) \
    /* Clips a specific output value to a single sample value. */ \
    AUDMIXBUF_MACRO_FN int64_t audioMixBufClipFrom##_aName(_aType aVal) \
    { \
        if (_aSigned) \
            return ((int64_t) aVal) << (32 - _aShift); \
        return ((int64_t) aVal - ((_aMax >> 1) + 1)) << (32 - _aShift); \
    } \
    \
    /* Clips a single sample value to a specific output value. */ \
    AUDMIXBUF_MACRO_FN _aType audioMixBufClipTo##_aName(int64_t iVal) \
    { \
        if (iVal >= 0x7fffffff) \
            return _aMax; \
        else if (iVal < -INT64_C(0x80000000)) \
            return _aMin; \
        \
        if (_aSigned) \
            return (_aType) (iVal >> (32 - _aShift)); \
        return ((_aType) ((iVal >> (32 - _aShift)) + ((_aMax >> 1) + 1))); \
    } \
    \
    AUDMIXBUF_MACRO_FN uint32_t audioMixBufConvFrom##_aName##Stereo(PPDMAUDIOSAMPLE paDst, const void *pvSrc, uint32_t cbSrc, \
                                                                    const PAUDMIXBUF_CONVOPTS pOpts) \
    { \
        _aType *pSrc = (_aType *)pvSrc; \
        uint32_t cSamples = (uint32_t)RT_MIN(pOpts->cSamples, cbSrc / sizeof(_aType)); \
        AUDMIXBUF_MACRO_LOG(("cSamples=%RU32, sizeof(%zu), lVol=%RU32, rVol=%RU32\n", \
                             pOpts->cSamples, sizeof(_aType), pOpts->Volume.uLeft, pOpts->Volume.uRight)); \
        for (uint32_t i = 0; i < cSamples; i++) \
        { \
            AUDMIXBUF_MACRO_LOG(("%p: l=%RI16, r=%RI16\n", paDst, *pSrc, *(pSrc + 1))); \
            paDst->i64LSample = ASMMult2xS32RetS64((int32_t)audioMixBufClipFrom##_aName(*pSrc++), pOpts->Volume.uLeft ) >> AUDIOMIXBUF_VOL_SHIFT; \
            paDst->i64RSample = ASMMult2xS32RetS64((int32_t)audioMixBufClipFrom##_aName(*pSrc++), pOpts->Volume.uRight) >> AUDIOMIXBUF_VOL_SHIFT; \
            AUDMIXBUF_MACRO_LOG(("\t-> l=%RI64, r=%RI64\n", paDst->i64LSample, paDst->i64RSample)); \
            paDst++; \
        } \
        \
        return cSamples; \
    } \
    \
    AUDMIXBUF_MACRO_FN uint32_t audioMixBufConvFrom##_aName##Mono(PPDMAUDIOSAMPLE paDst, const void *pvSrc, uint32_t cbSrc, \
                                                                  const PAUDMIXBUF_CONVOPTS pOpts) \
    { \
        _aType *pSrc = (_aType *)pvSrc; \
        uint32_t cSamples = (uint32_t)RT_MIN(pOpts->cSamples, cbSrc / sizeof(_aType)); \
        AUDMIXBUF_MACRO_LOG(("cSamples=%RU32, sizeof(%zu), lVol=%RU32, rVol=%RU32\n", \
                             cSamples, sizeof(_aType), pOpts->Volume.uLeft, pOpts->Volume.uRight)); \
        for (uint32_t i = 0; i < cSamples; i++) \
        { \
            AUDMIXBUF_MACRO_LOG(("%p: s=%RI16\n", paDst, *pSrc)); \
            paDst->i64LSample = ASMMult2xS32RetS64((int32_t)audioMixBufClipFrom##_aName(*pSrc), pOpts->Volume.uLeft)  >> AUDIOMIXBUF_VOL_SHIFT; \
            paDst->i64RSample = ASMMult2xS32RetS64((int32_t)audioMixBufClipFrom##_aName(*pSrc), pOpts->Volume.uRight) >> AUDIOMIXBUF_VOL_SHIFT; \
            ++pSrc; \
            AUDMIXBUF_MACRO_LOG(("\t-> l=%RI64, r=%RI64\n", paDst->i64LSample, paDst->i64RSample)); \
            paDst++; \
        } \
        \
        return cSamples; \
    } \
    \
    AUDMIXBUF_MACRO_FN void audioMixBufConvTo##_aName##Stereo(void *pvDst, const PPDMAUDIOSAMPLE paSrc, \
                                                              const PAUDMIXBUF_CONVOPTS pOpts) \
    { \
        PPDMAUDIOSAMPLE pSrc = paSrc; \
        _aType *pDst = (_aType *)pvDst; \
        _aType l, r; \
        uint32_t cSamples = pOpts->cSamples; \
        while (cSamples--) \
        { \
            AUDMIXBUF_MACRO_LOG(("%p: l=%RI64, r=%RI64\n", pSrc, pSrc->i64LSample, pSrc->i64RSample)); \
            l = audioMixBufClipTo##_aName(pSrc->i64LSample); \
            r = audioMixBufClipTo##_aName(pSrc->i64RSample); \
            AUDMIXBUF_MACRO_LOG(("\t-> l=%RI16, r=%RI16\n", l, r)); \
            *pDst++ = l; \
            *pDst++ = r; \
            pSrc++; \
        } \
    } \
    \
    AUDMIXBUF_MACRO_FN void audioMixBufConvTo##_aName##Mono(void *pvDst, const PPDMAUDIOSAMPLE paSrc, \
                                                            const PAUDMIXBUF_CONVOPTS pOpts) \
    { \
        PPDMAUDIOSAMPLE pSrc = paSrc; \
        _aType *pDst = (_aType *)pvDst; \
        uint32_t cSamples = pOpts->cSamples; \
        while (cSamples--) \
        { \
            *pDst++ = audioMixBufClipTo##_aName((pSrc->i64LSample + pSrc->i64RSample) / 2); \
            pSrc++; \
        } \
    }

/* audioMixBufConvXXXS8: 8 bit, signed. */
AUDMIXBUF_CONVERT(S8 /* Name */,  int8_t,   INT8_MIN  /* Min */, INT8_MAX   /* Max */, true  /* fSigned */, 8  /* cShift */)
/* audioMixBufConvXXXU8: 8 bit, unsigned. */
AUDMIXBUF_CONVERT(U8 /* Name */,  uint8_t,  0         /* Min */, UINT8_MAX  /* Max */, false /* fSigned */, 8  /* cShift */)
/* audioMixBufConvXXXS16: 16 bit, signed. */
AUDMIXBUF_CONVERT(S16 /* Name */, int16_t,  INT16_MIN /* Min */, INT16_MAX  /* Max */, true  /* fSigned */, 16 /* cShift */)
/* audioMixBufConvXXXU16: 16 bit, unsigned. */
AUDMIXBUF_CONVERT(U16 /* Name */, uint16_t, 0         /* Min */, UINT16_MAX /* Max */, false /* fSigned */, 16 /* cShift */)
/* audioMixBufConvXXXS32: 32 bit, signed. */
AUDMIXBUF_CONVERT(S32 /* Name */, int32_t,  INT32_MIN /* Min */, INT32_MAX  /* Max */, true  /* fSigned */, 32 /* cShift */)
/* audioMixBufConvXXXU32: 32 bit, unsigned. */
AUDMIXBUF_CONVERT(U32 /* Name */, uint32_t, 0         /* Min */, UINT32_MAX /* Max */, false /* fSigned */, 32 /* cShift */)

#undef AUDMIXBUF_CONVERT

#define AUDMIXBUF_MIXOP(_aName, _aOp) \
    AUDMIXBUF_MACRO_FN void audioMixBufOp##_aName(PPDMAUDIOSAMPLE paDst, uint32_t cDstSamples, \
                                                  PPDMAUDIOSAMPLE paSrc, uint32_t cSrcSamples, \
                                                  PPDMAUDIOSTRMRATE pRate, \
                                                  uint32_t *pcDstWritten, uint32_t *pcSrcRead) \
    { \
        AUDMIXBUF_MACRO_LOG(("cSrcSamples=%RU32, cDstSamples=%RU32\n", cSrcSamples, cDstSamples)); \
        AUDMIXBUF_MACRO_LOG(("pRate=%p: srcOffset=0x%RX32 (%RU32), dstOffset=0x%RX32 (%RU32), dstInc=0x%RX64 (%RU64)\n", \
                             pRate, pRate->srcOffset, pRate->srcOffset, \
                             (uint32_t)(pRate->dstOffset >> 32), (uint32_t)(pRate->dstOffset >> 32), \
                             pRate->dstInc, pRate->dstInc)); \
        \
        if (pRate->dstInc == (UINT64_C(1) + UINT32_MAX)) /* No conversion needed? */ \
        { \
            uint32_t cSamples = RT_MIN(cSrcSamples, cDstSamples); \
            AUDMIXBUF_MACRO_LOG(("cSamples=%RU32\n", cSamples)); \
            for (uint32_t i = 0; i < cSamples; i++) \
            { \
                paDst[i].i64LSample _aOp paSrc[i].i64LSample; \
                paDst[i].i64RSample _aOp paSrc[i].i64RSample; \
            } \
            \
            if (pcDstWritten) \
                *pcDstWritten = cSamples; \
            if (pcSrcRead) \
                *pcSrcRead = cSamples; \
            return; \
        } \
        \
        PPDMAUDIOSAMPLE paSrcStart = paSrc; \
        PPDMAUDIOSAMPLE paSrcEnd   = paSrc + cSrcSamples; \
        PPDMAUDIOSAMPLE paDstStart = paDst; \
        PPDMAUDIOSAMPLE paDstEnd   = paDst + cDstSamples; \
        PDMAUDIOSAMPLE  samCur = { 0 }; \
        PDMAUDIOSAMPLE  samOut; \
        PDMAUDIOSAMPLE  samLast    = pRate->srcSampleLast; \
        uint64_t        lDelta = 0; \
        \
        AUDMIXBUF_MACRO_LOG(("Start: paDstEnd=%p - paDstStart=%p -> %zu\n", paDstEnd, paDst, paDstEnd - paDstStart)); \
        AUDMIXBUF_MACRO_LOG(("Start: paSrcEnd=%p - paSrcStart=%p -> %zu\n", paSrcEnd, paSrc, paSrcEnd - paSrcStart)); \
        \
        while (paDst < paDstEnd) \
        { \
            Assert(paSrc <= paSrcEnd); \
            Assert(paDst <= paDstEnd); \
            if (paSrc == paSrcEnd) \
                break; \
            \
            lDelta = 0; \
            while (pRate->srcOffset <= (pRate->dstOffset >> 32)) \
            { \
                Assert(paSrc <= paSrcEnd); \
                samLast = *paSrc++; \
                pRate->srcOffset++; \
                lDelta++; \
                if (paSrc == paSrcEnd) \
                    break; \
            } \
            \
            Assert(paSrc <= paSrcEnd); \
            if (paSrc == paSrcEnd) \
                break; \
            \
            samCur = *paSrc; \
            \
            /* Interpolate. */ \
            int64_t iDstOffInt = pRate->dstOffset & UINT32_MAX; \
            \
            samOut.i64LSample = (samLast.i64LSample * ((int64_t) (INT64_C(1) << 32) - iDstOffInt) + samCur.i64LSample * iDstOffInt) >> 32; \
            samOut.i64RSample = (samLast.i64RSample * ((int64_t) (INT64_C(1) << 32) - iDstOffInt) + samCur.i64RSample * iDstOffInt) >> 32; \
            \
            paDst->i64LSample _aOp samOut.i64LSample; \
            paDst->i64RSample _aOp samOut.i64RSample; \
            \
            AUDMIXBUF_MACRO_LOG(("\tlDelta=0x%RX64 (%RU64), iDstOffInt=0x%RX64 (%RI64), l=%RI64, r=%RI64 (cur l=%RI64, r=%RI64)\n", \
                                 lDelta, lDelta, iDstOffInt, iDstOffInt, \
                                 paDst->i64LSample, paDst->i64RSample, \
                                 samCur.i64LSample, samCur.i64RSample)); \
            \
            paDst++; \
            pRate->dstOffset += pRate->dstInc; \
            \
            AUDMIXBUF_MACRO_LOG(("\t\tpRate->dstOffset=0x%RX32 (%RU32)\n", pRate->dstOffset, pRate->dstOffset >> 32)); \
            \
        } \
        \
        AUDMIXBUF_MACRO_LOG(("End: paDst=%p - paDstStart=%p -> %zu\n", paDst, paDstStart, paDst - paDstStart)); \
        AUDMIXBUF_MACRO_LOG(("End: paSrc=%p - paSrcStart=%p -> %zu\n", paSrc, paSrcStart, paSrc - paSrcStart)); \
        \
        pRate->srcSampleLast = samLast; \
        \
        AUDMIXBUF_MACRO_LOG(("pRate->srcSampleLast l=%RI64, r=%RI64, lDelta=0x%RX64 (%RU64)\n", \
                              pRate->srcSampleLast.i64LSample, pRate->srcSampleLast.i64RSample, lDelta, lDelta)); \
        \
        if (pcDstWritten) \
            *pcDstWritten = paDst - paDstStart; \
        if (pcSrcRead) \
            *pcSrcRead = paSrc - paSrcStart; \
    }

#if 0 // unused
/* audioMixBufOpAssign: Assigns values from source buffer to destination bufffer, overwriting the destination. */
AUDMIXBUF_MIXOP(Assign /* Name */,  = /* Operation */)
#endif
/* audioMixBufOpBlend: Blends together the values from both, the source and the destination buffer. */
AUDMIXBUF_MIXOP(Blend  /* Name */, += /* Operation */)

#undef AUDMIXBUF_MIXOP
#undef AUDMIXBUF_MACRO_LOG

/** Dummy conversion used when the source is muted. */
AUDMIXBUF_MACRO_FN uint32_t audioMixBufConvFromSilence(PPDMAUDIOSAMPLE paDst, const void *pvSrc,
                                                       uint32_t cbSrc, const PAUDMIXBUF_CONVOPTS pOpts)
{
    RT_NOREF(cbSrc, pvSrc);
    /* Internally zero always corresponds to silence. */
    memset(paDst, 0, pOpts->cSamples * sizeof(paDst[0]));
    return pOpts->cSamples;
}

/**
 * Looks up the matching conversion (macro) routine for converting
 * audio samples from a source format.
 *
 ** @todo Speed up the lookup by binding it to the actual stream state.
 *
 * @return  PAUDMIXBUF_FN_CONVFROM  Function pointer to conversion macro if found, NULL if not supported.
 * @param   enmFmt                  Audio format to lookup conversion macro for.
 * @param   fMuted                  Flag determining whether the source is muted.
 */
static inline PAUDMIXBUF_FN_CONVFROM audioMixBufConvFromLookup(PDMAUDIOMIXBUFFMT enmFmt, bool fMuted)
{
    if (fMuted)
        return audioMixBufConvFromSilence;

    if (AUDMIXBUF_FMT_SIGNED(enmFmt))
    {
        if (AUDMIXBUF_FMT_CHANNELS(enmFmt) == 2)
        {
            switch (AUDMIXBUF_FMT_BITS_PER_SAMPLE(enmFmt))
            {
                case 8:  return audioMixBufConvFromS8Stereo;
                case 16: return audioMixBufConvFromS16Stereo;
                case 32: return audioMixBufConvFromS32Stereo;
                default: return NULL;
            }
        }
        else if (AUDMIXBUF_FMT_CHANNELS(enmFmt) == 1)
        {
            switch (AUDMIXBUF_FMT_BITS_PER_SAMPLE(enmFmt))
            {
                case 8:  return audioMixBufConvFromS8Mono;
                case 16: return audioMixBufConvFromS16Mono;
                case 32: return audioMixBufConvFromS32Mono;
                default: return NULL;
            }
        }
    }
    else /* Unsigned */
    {
        if (AUDMIXBUF_FMT_CHANNELS(enmFmt) == 2)
        {
            switch (AUDMIXBUF_FMT_BITS_PER_SAMPLE(enmFmt))
            {
                case 8:  return audioMixBufConvFromU8Stereo;
                case 16: return audioMixBufConvFromU16Stereo;
                case 32: return audioMixBufConvFromU32Stereo;
                default: return NULL;
            }
        }
        else if (AUDMIXBUF_FMT_CHANNELS(enmFmt) == 1)
        {
            switch (AUDMIXBUF_FMT_BITS_PER_SAMPLE(enmFmt))
            {
                case 8:  return audioMixBufConvFromU8Mono;
                case 16: return audioMixBufConvFromU16Mono;
                case 32: return audioMixBufConvFromU32Mono;
                default: return NULL;
            }
        }
    }

    return NULL;
}

/**
 * Looks up the matching conversion (macro) routine for converting
 * audio samples to a destination format.
 *
 ** @todo Speed up the lookup by binding it to the actual stream state.
 *
 * @return  PAUDMIXBUF_FN_CONVTO    Function pointer to conversion macro if found, NULL if not supported.
 * @param   enmFmt                  Audio format to lookup conversion macro for.
 */
static inline PAUDMIXBUF_FN_CONVTO audioMixBufConvToLookup(PDMAUDIOMIXBUFFMT enmFmt)
{
    if (AUDMIXBUF_FMT_SIGNED(enmFmt))
    {
        if (AUDMIXBUF_FMT_CHANNELS(enmFmt) == 2)
        {
            switch (AUDMIXBUF_FMT_BITS_PER_SAMPLE(enmFmt))
            {
                case 8:  return audioMixBufConvToS8Stereo;
                case 16: return audioMixBufConvToS16Stereo;
                case 32: return audioMixBufConvToS32Stereo;
                default: return NULL;
            }
        }
        else if (AUDMIXBUF_FMT_CHANNELS(enmFmt) == 1)
        {
            switch (AUDMIXBUF_FMT_BITS_PER_SAMPLE(enmFmt))
            {
                case 8:  return audioMixBufConvToS8Mono;
                case 16: return audioMixBufConvToS16Mono;
                case 32: return audioMixBufConvToS32Mono;
                default: return NULL;
            }
        }
    }
    else /* Unsigned */
    {
        if (AUDMIXBUF_FMT_CHANNELS(enmFmt) == 2)
        {
            switch (AUDMIXBUF_FMT_BITS_PER_SAMPLE(enmFmt))
            {
                case 8:  return audioMixBufConvToU8Stereo;
                case 16: return audioMixBufConvToU16Stereo;
                case 32: return audioMixBufConvToU32Stereo;
                default: return NULL;
            }
        }
        else if (AUDMIXBUF_FMT_CHANNELS(enmFmt) == 1)
        {
            switch (AUDMIXBUF_FMT_BITS_PER_SAMPLE(enmFmt))
            {
                case 8:  return audioMixBufConvToU8Mono;
                case 16: return audioMixBufConvToU16Mono;
                case 32: return audioMixBufConvToU32Mono;
                default: return NULL;
            }
        }
    }

    return NULL;
}

/**
 * Initializes a mixing buffer.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to initialize.
 * @param   pszName                 Name of mixing buffer for easier identification. Optional.
 * @param   pProps                  PCM audio properties to use for the mixing buffer.
 * @param   cSamples                Maximum number of audio samples the mixing buffer can hold.
 */
int AudioMixBufInit(PPDMAUDIOMIXBUF pMixBuf, const char *pszName, PPDMPCMPROPS pProps, uint32_t cSamples)
{
    AssertPtrReturn(pMixBuf, VERR_INVALID_POINTER);
    AssertPtrReturn(pszName, VERR_INVALID_POINTER);
    AssertPtrReturn(pProps,  VERR_INVALID_POINTER);

    pMixBuf->pParent = NULL;
    RTListInit(&pMixBuf->lstBuffers);

    pMixBuf->pSamples = NULL;
    pMixBuf->cSamples = 0;

    pMixBuf->offReadWrite = 0;
    pMixBuf->cMixed       = 0;
    pMixBuf->cProcessed   = 0;

    /* Set initial volume to max. */
    pMixBuf->Volume.fMuted = false;
    pMixBuf->Volume.uLeft  = AUDIOMIXBUF_VOL_0DB;
    pMixBuf->Volume.uRight = AUDIOMIXBUF_VOL_0DB;

    /* Prevent division by zero.
     * Do a 1:1 conversion according to AUDIOMIXBUF_S2B_RATIO. */
    pMixBuf->iFreqRatio = 1 << 20;

    pMixBuf->pRate = NULL;

    pMixBuf->AudioFmt = AUDMIXBUF_AUDIO_FMT_MAKE(pProps->uHz,
                                                 pProps->cChannels,
                                                 pProps->cBits,
                                                 pProps->fSigned);
    pMixBuf->cShift = pProps->cShift;
    pMixBuf->pszName = RTStrDup(pszName);
    if (!pMixBuf->pszName)
        return VERR_NO_MEMORY;

    AUDMIXBUF_LOG(("%s: uHz=%RU32, cChan=%RU8, cBits=%RU8, fSigned=%RTbool\n",
                   pMixBuf->pszName,
                   AUDMIXBUF_FMT_SAMPLE_FREQ(pMixBuf->AudioFmt),
                   AUDMIXBUF_FMT_CHANNELS(pMixBuf->AudioFmt),
                   AUDMIXBUF_FMT_BITS_PER_SAMPLE(pMixBuf->AudioFmt),
                   RT_BOOL(AUDMIXBUF_FMT_SIGNED(pMixBuf->AudioFmt))));

    return audioMixBufAlloc(pMixBuf, cSamples);
}

/**
 * Returns @true if there are any audio samples available for processing,
 * @false if not.
 *
 * @return  bool                    @true if there are any audio samples available for processing, @false if not.
 * @param   pMixBuf                 Mixing buffer to return value for.
 */
bool AudioMixBufIsEmpty(PPDMAUDIOMIXBUF pMixBuf)
{
    AssertPtrReturn(pMixBuf, true);

    if (pMixBuf->pParent)
        return (pMixBuf->cMixed == 0);
    return (pMixBuf->cProcessed == 0);
}

/**
 * Links an audio mixing buffer to a parent mixing buffer. A parent mixing
 * buffer can have multiple children mixing buffers [1:N], whereas a child only can
 * have one parent mixing buffer [N:1].
 *
 * The mixing direction always goes from the child/children buffer(s) to the
 * parent buffer.
 *
 * For guest audio output the host backend owns the parent mixing buffer, the
 * device emulation owns the child/children.
 *
 * The audio format of each mixing buffer can vary; the internal mixing code
 * then will autiomatically do the (needed) conversion.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to link parent to.
 * @param   pParent                 Parent mixing buffer to use for linking.
 *
 * @remark  Circular linking is not allowed.
 */
int AudioMixBufLinkTo(PPDMAUDIOMIXBUF pMixBuf, PPDMAUDIOMIXBUF pParent)
{
    AssertPtrReturn(pMixBuf, VERR_INVALID_POINTER);
    AssertPtrReturn(pParent, VERR_INVALID_POINTER);

    AssertMsgReturn(AUDMIXBUF_FMT_SAMPLE_FREQ(pParent->AudioFmt),
                    ("Parent sample frequency (Hz) not set\n"), VERR_INVALID_PARAMETER);
    AssertMsgReturn(AUDMIXBUF_FMT_SAMPLE_FREQ(pMixBuf->AudioFmt),
                    ("Buffer sample frequency (Hz) not set\n"), VERR_INVALID_PARAMETER);
    AssertMsgReturn(pMixBuf != pParent,
                    ("Circular linking not allowed\n"), VERR_INVALID_PARAMETER);

    if (pMixBuf->pParent) /* Already linked? */
    {
        AUDMIXBUF_LOG(("%s: Already linked to \"%s\"\n",
                       pMixBuf->pszName, pMixBuf->pParent->pszName));
        return VERR_ACCESS_DENIED;
    }

    RTListAppend(&pParent->lstBuffers, &pMixBuf->Node);
    pMixBuf->pParent = pParent;

    /* Calculate the frequency ratio. */
    pMixBuf->iFreqRatio = ((int64_t)AUDMIXBUF_FMT_SAMPLE_FREQ(pParent->AudioFmt) << 32)
                        /           AUDMIXBUF_FMT_SAMPLE_FREQ(pMixBuf->AudioFmt);

    if (pMixBuf->iFreqRatio == 0) /* Catch division by zero. */
        pMixBuf->iFreqRatio = 1 << 20; /* Do a 1:1 conversion instead. */

    uint32_t cSamples = (uint32_t)RT_MIN(  ((uint64_t)pParent->cSamples << 32)
                                         / pMixBuf->iFreqRatio, _64K /* 64K samples max. */);
    if (!cSamples)
        cSamples = pParent->cSamples;

    int rc = VINF_SUCCESS;

    if (cSamples != pMixBuf->cSamples)
    {
        AUDMIXBUF_LOG(("%s: Reallocating samples %RU32 -> %RU32\n",
                       pMixBuf->pszName, pMixBuf->cSamples, cSamples));

        uint32_t cbSamples = cSamples * sizeof(PDMAUDIOSAMPLE);
        Assert(cbSamples);
        pMixBuf->pSamples = (PPDMAUDIOSAMPLE)RTMemRealloc(pMixBuf->pSamples, cbSamples);
        if (!pMixBuf->pSamples)
            rc = VERR_NO_MEMORY;

        if (RT_SUCCESS(rc))
        {
            pMixBuf->cSamples = cSamples;

            /* Make sure to zero the reallocated buffer so that it can be
             * used properly when blending with another buffer later. */
            RT_BZERO(pMixBuf->pSamples, cbSamples);
        }
    }

    if (RT_SUCCESS(rc))
    {
        if (!pMixBuf->pRate)
        {
            /* Create rate conversion. */
            pMixBuf->pRate = (PPDMAUDIOSTRMRATE)RTMemAllocZ(sizeof(PDMAUDIOSTRMRATE));
            if (!pMixBuf->pRate)
                return VERR_NO_MEMORY;
        }
        else
            RT_BZERO(pMixBuf->pRate, sizeof(PDMAUDIOSTRMRATE));

        pMixBuf->pRate->dstInc = ((uint64_t)AUDMIXBUF_FMT_SAMPLE_FREQ(pMixBuf->AudioFmt) << 32)
                               /            AUDMIXBUF_FMT_SAMPLE_FREQ(pParent->AudioFmt);

        AUDMIXBUF_LOG(("uThisHz=%RU32, uParentHz=%RU32, iFreqRatio=0x%RX64 (%RI64), uRateInc=0x%RX64 (%RU64), cSamples=%RU32 (%RU32 parent)\n",
                       AUDMIXBUF_FMT_SAMPLE_FREQ(pMixBuf->AudioFmt),
                       AUDMIXBUF_FMT_SAMPLE_FREQ(pParent->AudioFmt),
                       pMixBuf->iFreqRatio, pMixBuf->iFreqRatio,
                       pMixBuf->pRate->dstInc, pMixBuf->pRate->dstInc,
                       pMixBuf->cSamples,
                       pParent->cSamples));
        AUDMIXBUF_LOG(("%s (%RU32Hz) -> %s (%RU32Hz)\n",
                       pMixBuf->pszName, AUDMIXBUF_FMT_SAMPLE_FREQ(pMixBuf->AudioFmt),
                       pMixBuf->pParent->pszName, AUDMIXBUF_FMT_SAMPLE_FREQ(pParent->AudioFmt)));
    }

    return rc;
}

/**
 * Returns the number of audio samples mixed (processed) by
 * the parent mixing buffer.
 *
 * @return  uint32_t                Number of audio samples mixed (processed).
 * @param   pMixBuf                 Mixing buffer to return number from.
 */
uint32_t AudioMixBufMixed(PPDMAUDIOMIXBUF pMixBuf)
{
    AssertPtrReturn(pMixBuf, 0);

    AssertMsgReturn(VALID_PTR(pMixBuf->pParent),
                              ("Buffer is not linked to a parent buffer\n"),
                              0);

    AUDMIXBUF_LOG(("%s: cMixed=%RU32\n", pMixBuf->pszName, pMixBuf->cMixed));
    return pMixBuf->cMixed;
}

/**
 * Mixes audio samples from a source mixing buffer to a destination mixing buffer.
 *
 * @return  IPRT status code.
 * @param   pDst                    Destination mixing buffer.
 * @param   pSrc                    Source mixing buffer.
 * @param   cSamples                Number of source audio samples to mix.
 * @param   pcProcessed             Number of audio samples successfully mixed.
 */
static int audioMixBufMixTo(PPDMAUDIOMIXBUF pDst, PPDMAUDIOMIXBUF pSrc, uint32_t cSamples, uint32_t *pcProcessed)
{
    AssertPtrReturn(pDst, VERR_INVALID_POINTER);
    AssertPtrReturn(pSrc, VERR_INVALID_POINTER);
    AssertReturn(cSamples, VERR_INVALID_PARAMETER);
    /* pcProcessed is optional. */

    /* Live samples indicate how many samples there are in the source buffer
     * which have not been processed yet by the destination buffer. */
    uint32_t cLive = pSrc->cMixed;
    if (cLive >= pDst->cSamples)
        AUDMIXBUF_LOG(("Destination buffer \"%s\" full (%RU32 samples max), live samples = %RU32\n",
                       pDst->pszName, pDst->cSamples, cLive));

    /* Dead samples are the number of samples in the destination buffer which
     * will not be needed, that is, are not needed in order to process the live
     * samples of the source buffer. */
    uint32_t cDead = pDst->cSamples - cLive;

    uint32_t cToReadTotal = (uint32_t)RT_MIN(cSamples, AUDIOMIXBUF_S2S_RATIO(pSrc, cDead));
    uint32_t offRead = 0;

    uint32_t cReadTotal = 0;
    uint32_t cWrittenTotal = 0;
    uint32_t offWrite = (pDst->offReadWrite + cLive) % pDst->cSamples;

    AUDMIXBUF_LOG(("pSrc=%s (%RU32 samples), pDst=%s (%RU32 samples), cLive=%RU32, cDead=%RU32, cToReadTotal=%RU32, offWrite=%RU32\n",
                   pSrc->pszName, pSrc->cSamples, pDst->pszName, pDst->cSamples, cLive, cDead, cToReadTotal, offWrite));

    uint32_t cToRead, cToWrite;
    uint32_t cWritten, cRead;

    while (cToReadTotal)
    {
        cDead = pDst->cSamples - cLive;

        cToRead  = cToReadTotal;
        cToWrite = RT_MIN(cDead, pDst->cSamples - offWrite);
        if (!cToWrite)
        {
            AUDMIXBUF_LOG(("Warning: Destination buffer \"%s\" full\n", pDst->pszName));
            break;
        }

        Assert(offWrite + cToWrite <= pDst->cSamples);
        Assert(offRead + cToRead <= pSrc->cSamples);

        AUDMIXBUF_LOG(("\t%RU32Hz -> %RU32Hz\n", AUDMIXBUF_FMT_SAMPLE_FREQ(pSrc->AudioFmt), AUDMIXBUF_FMT_SAMPLE_FREQ(pDst->AudioFmt)));
        AUDMIXBUF_LOG(("\tcDead=%RU32, offWrite=%RU32, cToWrite=%RU32, offRead=%RU32, cToRead=%RU32\n",
                       cDead, offWrite, cToWrite, offRead, cToRead));

        audioMixBufOpBlend(pDst->pSamples + offWrite, cToWrite,
                           pSrc->pSamples + offRead, cToRead,
                           pSrc->pRate, &cWritten, &cRead);

        AUDMIXBUF_LOG(("\t\tcWritten=%RU32, cRead=%RU32\n", cWritten, cRead));

        cReadTotal    += cRead;
        cWrittenTotal += cWritten;

        offRead += cRead;
        Assert(cToReadTotal >= cRead);
        cToReadTotal -= cRead;

        offWrite = (offWrite + cWritten) % pDst->cSamples;

        cLive += cWritten;
    }

    pSrc->cMixed     += cWrittenTotal;
    pDst->cProcessed += cWrittenTotal;
#ifdef DEBUG
    s_cSamplesMixedTotal += cWrittenTotal;
    audioMixBufPrint(pDst);
#endif

    if (pcProcessed)
        *pcProcessed = cReadTotal;

    AUDMIXBUF_LOG(("cReadTotal=%RU32 (pcProcessed), cWrittenTotal=%RU32, cSrcMixed=%RU32, cDstProc=%RU32\n",
                   cReadTotal, cWrittenTotal, pSrc->cMixed, pDst->cProcessed));
    return VINF_SUCCESS;
}

/**
 * Mixes (multiplexes) audio samples to all connected mixing buffer children.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to use.
 * @param   cSamples                Number of audio samples to mix to children.
 * @param   pcProcessed             Maximum number of audio samples successfully mixed
 *                                  to all children. Optional.
 */
int AudioMixBufMixToChildren(PPDMAUDIOMIXBUF pMixBuf, uint32_t cSamples,
                             uint32_t *pcProcessed)
{
    AssertPtrReturn(pMixBuf, VERR_INVALID_POINTER);

    if (!cSamples)
    {
        if (pcProcessed)
            *pcProcessed = 0;
        return VINF_SUCCESS;
    }

    int rc = VINF_SUCCESS;

    uint32_t cProcessed;
    uint32_t cProcessedMax = 0;

    PPDMAUDIOMIXBUF pIter;
    RTListForEach(&pMixBuf->lstBuffers, pIter, PDMAUDIOMIXBUF, Node)
    {
        rc = audioMixBufMixTo(pIter, pMixBuf, cSamples, &cProcessed);
        if (RT_FAILURE(rc))
            break;

        cProcessedMax = RT_MAX(cProcessedMax, cProcessed);
    }

    if (pcProcessed)
        *pcProcessed = cProcessedMax;

    return rc;
}

/**
 * Mixes audio samples down to the parent mixing buffer.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to mix samples down to parent.
 * @param   cSamples                Number of audio samples to mix down.
 * @param   pcProcessed             Number of audio samples successfully processed. Optional.
 */
int AudioMixBufMixToParent(PPDMAUDIOMIXBUF pMixBuf, uint32_t cSamples,
                           uint32_t *pcProcessed)
{
    AssertMsgReturn(VALID_PTR(pMixBuf->pParent),
                    ("Buffer is not linked to a parent buffer\n"),
                    VERR_INVALID_PARAMETER);

    return audioMixBufMixTo(pMixBuf->pParent, pMixBuf, cSamples, pcProcessed);
}

#ifdef DEBUG
/**
 * Prints statistics and status of a mixing buffer to the logger.
 * For debug versions only.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to print.
 */
static inline void audioMixBufPrint(PPDMAUDIOMIXBUF pMixBuf)
{
    PPDMAUDIOMIXBUF pParent = pMixBuf;
    if (pMixBuf->pParent)
        pParent = pMixBuf->pParent;

    AUDMIXBUF_LOG(("********************************************\n"));
    AUDMIXBUF_LOG(("%s: offReadWrite=%RU32, cProcessed=%RU32, cMixed=%RU32 (BpS=%RU32)\n",
                   pParent->pszName,
                   pParent->offReadWrite, pParent->cProcessed, pParent->cMixed,
                   AUDIOMIXBUF_S2B(pParent, 1)));

    PPDMAUDIOMIXBUF pIter;
    RTListForEach(&pParent->lstBuffers, pIter, PDMAUDIOMIXBUF, Node)
    {
        AUDMIXBUF_LOG(("\t%s: offReadWrite=%RU32, cProcessed=%RU32, cMixed=%RU32 (BpS=%RU32)\n",
                       pIter->pszName,
                       pIter->offReadWrite, pIter->cProcessed, pIter->cMixed,
                       AUDIOMIXBUF_S2B(pIter, 1)));
    }
    AUDMIXBUF_LOG(("Total samples mixed: %RU64\n", s_cSamplesMixedTotal));
    AUDMIXBUF_LOG(("********************************************\n"));
}
#endif

/**
 * Returns the total number of samples processed.
 *
 * @return  uint32_t
 * @param   pMixBuf
 */
uint32_t AudioMixBufProcessed(PPDMAUDIOMIXBUF pMixBuf)
{
    AssertPtrReturn(pMixBuf, 0);

    AUDMIXBUF_LOG(("%s: cProcessed=%RU32\n", pMixBuf->pszName, pMixBuf->cProcessed));
    return pMixBuf->cProcessed;
}

/**
 * Reads audio samples at a specific offset.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to read audio samples from.
 * @param   offSamples              Offset (in audio samples) to start reading from.
 * @param   pvBuf                   Pointer to buffer to write output to.
 * @param   cbBuf                   Size (in bytes) of buffer to write to.
 * @param   pcbRead                 Size (in bytes) of data read. Optional.
 */
int AudioMixBufReadAt(PPDMAUDIOMIXBUF pMixBuf,
                      uint32_t offSamples,
                      void *pvBuf, uint32_t cbBuf,
                      uint32_t *pcbRead)
{
    return AudioMixBufReadAtEx(pMixBuf, pMixBuf->AudioFmt,
                               offSamples, pvBuf, cbBuf, pcbRead);
}

/**
 * Reads audio samples at a specific offset.
 * If the audio format of the mixing buffer and the requested audio format do
 * not match the output will be converted accordingly.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to read audio samples from.
 * @param   enmFmt                  Audio format to use for output.
 * @param   offSamples              Offset (in audio samples) to start reading from.
 * @param   pvBuf                   Pointer to buffer to write output to.
 * @param   cbBuf                   Size (in bytes) of buffer to write to.
 * @param   pcbRead                 Size (in bytes) of data read. Optional.
 */
int AudioMixBufReadAtEx(PPDMAUDIOMIXBUF pMixBuf, PDMAUDIOMIXBUFFMT enmFmt,
                        uint32_t offSamples,
                        void *pvBuf, uint32_t cbBuf,
                        uint32_t *pcbRead)
{
    AssertPtrReturn(pMixBuf, VERR_INVALID_POINTER);
    AssertPtrReturn(pvBuf, VERR_INVALID_POINTER);
    /* pcbRead is optional. */

    uint32_t cDstSamples = pMixBuf->cSamples;
    uint32_t cLive = pMixBuf->cProcessed;

    uint32_t cDead = cDstSamples - cLive;
    uint32_t cToProcess = (uint32_t)AUDIOMIXBUF_S2S_RATIO(pMixBuf, cDead);
    cToProcess = RT_MIN(cToProcess, AUDIOMIXBUF_B2S(pMixBuf, cbBuf));

    AUDMIXBUF_LOG(("%s: offSamples=%RU32, cLive=%RU32, cDead=%RU32, cToProcess=%RU32\n",
                   pMixBuf->pszName, offSamples, cLive, cDead, cToProcess));

    int rc;
    if (cToProcess)
    {
        PAUDMIXBUF_FN_CONVTO pConv = audioMixBufConvToLookup(enmFmt);
        if (pConv)
        {
            AUDMIXBUF_CONVOPTS convOpts = { cToProcess, pMixBuf->Volume };
            pConv(pvBuf, pMixBuf->pSamples + offSamples, &convOpts);

            rc = VINF_SUCCESS;
        }
        else
            rc = VERR_INVALID_PARAMETER;

#ifdef DEBUG
        audioMixBufPrint(pMixBuf);
#endif
    }
    else
        rc = VINF_SUCCESS;

    if (RT_SUCCESS(rc))
    {
        if (pcbRead)
            *pcbRead = AUDIOMIXBUF_S2B(pMixBuf, cToProcess);
    }

    AUDMIXBUF_LOG(("cbRead=%RU32, rc=%Rrc\n", AUDIOMIXBUF_S2B(pMixBuf, cToProcess), rc));
    return rc;
}

/**
 * Reads audio samples. The audio format of the mixing buffer will be used.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to read audio samples from.
 * @param   pvBuf                   Pointer to buffer to write output to.
 * @param   cbBuf                   Size (in bytes) of buffer to write to.
 * @param   pcRead                  Number of audio samples read. Optional.
 */
int AudioMixBufReadCirc(PPDMAUDIOMIXBUF pMixBuf,
                        void *pvBuf, uint32_t cbBuf, uint32_t *pcRead)
{
    return AudioMixBufReadCircEx(pMixBuf, pMixBuf->AudioFmt,
                                 pvBuf, cbBuf, pcRead);
}

/**
 * Reads audio samples in a specific audio format.
 * If the audio format of the mixing buffer and the requested audio format do
 * not match the output will be converted accordingly.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to read audio samples from.
 * @param   enmFmt                  Audio format to use for output.
 * @param   pvBuf                   Pointer to buffer to write output to.
 * @param   cbBuf                   Size (in bytes) of buffer to write to.
 * @param   pcRead                  Number of audio samples read. Optional.
 */
int AudioMixBufReadCircEx(PPDMAUDIOMIXBUF pMixBuf, PDMAUDIOMIXBUFFMT enmFmt,
                          void *pvBuf, uint32_t cbBuf, uint32_t *pcRead)
{
    AssertPtrReturn(pMixBuf, VERR_INVALID_POINTER);
    AssertPtrReturn(pvBuf, VERR_INVALID_POINTER);
    /* pcbRead is optional. */

    if (!cbBuf)
        return VINF_SUCCESS;

    uint32_t cToRead = RT_MIN(AUDIOMIXBUF_B2S(pMixBuf, cbBuf), pMixBuf->cProcessed);

    AUDMIXBUF_LOG(("%s: pvBuf=%p, cbBuf=%zu (%RU32 samples), cToRead=%RU32\n",
                   pMixBuf->pszName, pvBuf, cbBuf, AUDIOMIXBUF_B2S(pMixBuf, cbBuf), cToRead));

    if (!cToRead)
    {
#ifdef DEBUG
        audioMixBufPrint(pMixBuf);
#endif
        if (pcRead)
            *pcRead = 0;
        return VINF_SUCCESS;
    }

    PAUDMIXBUF_FN_CONVTO pConv = audioMixBufConvToLookup(enmFmt);
    if (!pConv) /* Audio format not supported. */
        return VERR_NOT_SUPPORTED;

    PPDMAUDIOSAMPLE pSamplesSrc1 = pMixBuf->pSamples + pMixBuf->offReadWrite;
    uint32_t cLenSrc1 = cToRead;

    PPDMAUDIOSAMPLE pSamplesSrc2 = NULL;
    uint32_t cLenSrc2 = 0;

    uint32_t offRead = pMixBuf->offReadWrite + cToRead;

    /*
     * Do we need to wrap around to read all requested data, that is,
     * starting at the beginning of our circular buffer? This then will
     * be the optional second part to do.
     */
    if (offRead >= pMixBuf->cSamples)
    {
        Assert(pMixBuf->offReadWrite <= pMixBuf->cSamples);
        cLenSrc1 = pMixBuf->cSamples - pMixBuf->offReadWrite;

        pSamplesSrc2 = pMixBuf->pSamples;
        Assert(cToRead >= cLenSrc1);
        cLenSrc2 = RT_MIN(cToRead - cLenSrc1, pMixBuf->cSamples);

        /* Save new read offset. */
        offRead = cLenSrc2;
    }

    AUDMIXBUF_CONVOPTS convOpts;
    convOpts.Volume = pMixBuf->Volume;

    /* Anything to do at all? */
    int rc = VINF_SUCCESS;
    if (cLenSrc1)
    {
        convOpts.cSamples = cLenSrc1;

        AUDMIXBUF_LOG(("P1: offRead=%RU32, cToRead=%RU32\n", pMixBuf->offReadWrite, cLenSrc1));
        pConv(pvBuf, pSamplesSrc1, &convOpts);
    }

    /* Second part present? */
    if (   RT_LIKELY(RT_SUCCESS(rc))
        && cLenSrc2)
    {
        AssertPtr(pSamplesSrc2);

        convOpts.cSamples = cLenSrc2;

        AUDMIXBUF_LOG(("P2: cToRead=%RU32, offWrite=%RU32 (%zu bytes)\n", cLenSrc2, cLenSrc1,
                       AUDIOMIXBUF_S2B(pMixBuf, cLenSrc1)));
        pConv((uint8_t *)pvBuf + AUDIOMIXBUF_S2B(pMixBuf, cLenSrc1), pSamplesSrc2, &convOpts);
    }

    if (RT_SUCCESS(rc))
    {
#ifdef DEBUG_DUMP_PCM_DATA
        RTFILE fh;
        rc = RTFileOpen(&fh, DEBUG_DUMP_PCM_DATA_PATH "mixbuf_readcirc.pcm",
                        RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND | RTFILE_O_WRITE | RTFILE_O_DENY_NONE);
        if (RT_SUCCESS(rc))
        {
            RTFileWrite(fh, pvBuf, AUDIOMIXBUF_S2B(pMixBuf, cLenSrc1 + cLenSrc2), NULL);
            RTFileClose(fh);
        }
#endif
        pMixBuf->offReadWrite  = offRead % pMixBuf->cSamples;
        pMixBuf->cProcessed   -= RT_MIN(cLenSrc1 + cLenSrc2, pMixBuf->cProcessed);

        if (pcRead)
            *pcRead = cLenSrc1 + cLenSrc2;
    }

#ifdef DEBUG
    audioMixBufPrint(pMixBuf);
#endif

    AUDMIXBUF_LOG(("cRead=%RU32 (%zu bytes), rc=%Rrc\n",
                   cLenSrc1 + cLenSrc2,
                   AUDIOMIXBUF_S2B(pMixBuf, cLenSrc1 + cLenSrc2), rc));
    return rc;
}

/**
 * Resets a mixing buffer.
 *
 * @param   pMixBuf                 Mixing buffer to reset.
 */
void AudioMixBufReset(PPDMAUDIOMIXBUF pMixBuf)
{
    AssertPtrReturnVoid(pMixBuf);

    AUDMIXBUF_LOG(("%s\n", pMixBuf->pszName));

    pMixBuf->offReadWrite = 0;
    pMixBuf->cMixed       = 0;
    pMixBuf->cProcessed   = 0;

    AudioMixBufClear(pMixBuf);
}

/**
 * Sets the overall (master) volume.
 *
 * @param   pMixBuf                 Mixing buffer to set volume for.
 * @param   pVol                    Pointer to volume structure to set.
 */
void AudioMixBufSetVolume(PPDMAUDIOMIXBUF pMixBuf, PPDMAUDIOVOLUME pVol)
{
    AssertPtrReturnVoid(pMixBuf);
    AssertPtrReturnVoid(pVol);

    LogFlowFunc(("%s: lVol=%RU32, rVol=%RU32\n", pMixBuf->pszName, pVol->uLeft, pVol->uRight));

    pMixBuf->Volume.fMuted = pVol->fMuted;
    /** @todo Ensure that the input is in the correct range/initialized! */
    pMixBuf->Volume.uLeft  = s_aVolumeConv[pVol->uLeft  & 0xFF] * (AUDIOMIXBUF_VOL_0DB >> 16);
    pMixBuf->Volume.uRight = s_aVolumeConv[pVol->uRight & 0xFF] * (AUDIOMIXBUF_VOL_0DB >> 16);

    LogFlowFunc(("\t-> lVol=%#RX32, rVol=%#RX32\n", pMixBuf->Volume.uLeft, pMixBuf->Volume.uRight));
}

/**
 * Returns the maximum amount of audio samples this buffer can hold.
 *
 * @return  uint32_t                Size (in audio samples) the mixing buffer can hold.
 * @param   pMixBuf                 Mixing buffer to retrieve maximum for.
 */
uint32_t AudioMixBufSize(PPDMAUDIOMIXBUF pMixBuf)
{
    AssertPtrReturn(pMixBuf, 0);
    return pMixBuf->cSamples;
}

/**
 * Returns the maximum amount of bytes this buffer can hold.
 *
 * @return  uint32_t                Size (in bytes) the mixing buffer can hold.
 * @param   pMixBuf                 Mixing buffer to retrieve maximum for.
 */
uint32_t AudioMixBufSizeBytes(PPDMAUDIOMIXBUF pMixBuf)
{
    AssertPtrReturn(pMixBuf, 0);
    return AUDIOMIXBUF_S2B(pMixBuf, pMixBuf->cSamples);
}

/**
 * Unlinks a mixing buffer from its parent, if any.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Mixing buffer to unlink from parent.
 */
void AudioMixBufUnlink(PPDMAUDIOMIXBUF pMixBuf)
{
    if (!pMixBuf || !pMixBuf->pszName)
        return;

    AUDMIXBUF_LOG(("%s\n", pMixBuf->pszName));

    if (pMixBuf->pParent)
    {
        AUDMIXBUF_LOG(("%s: Unlinking from parent \"%s\"\n",
                       pMixBuf->pszName, pMixBuf->pParent->pszName));

        RTListNodeRemove(&pMixBuf->Node);

        /* Make sure to reset the parent mixing buffer each time it gets linked
         * to a new child. */
        AudioMixBufReset(pMixBuf->pParent);
        pMixBuf->pParent = NULL;
    }

    PPDMAUDIOMIXBUF pIter;
    while (!RTListIsEmpty(&pMixBuf->lstBuffers))
    {
        pIter = RTListGetFirst(&pMixBuf->lstBuffers, PDMAUDIOMIXBUF, Node);

        AUDMIXBUF_LOG(("\tUnlinking \"%s\"\n", pIter->pszName));

        AudioMixBufReset(pIter->pParent);
        pIter->pParent = NULL;

        RTListNodeRemove(&pIter->Node);
    }

    if (pMixBuf->pRate)
    {
        pMixBuf->pRate->dstOffset = pMixBuf->pRate->srcOffset = 0;
        pMixBuf->pRate->dstInc = 0;
    }

    pMixBuf->iFreqRatio = 1; /* Prevent division by zero. */
}

/**
 * Writes audio samples at a specific offset.
 * The sample format being written must match the format of the mixing buffer.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Pointer to mixing buffer to write to.
 * @param   enmFmt                  Audio format supplied in the buffer.
 * @param   offSamples              Offset (in samples) starting to write at.
 * @param   pvBuf                   Pointer to audio buffer to be written.
 * @param   cbBuf                   Size (in bytes) of audio buffer.
 * @param   pcWritten               Returns number of audio samples written. Optional.
 */
int AudioMixBufWriteAt(PPDMAUDIOMIXBUF pMixBuf,
                       uint32_t offSamples,
                       const void *pvBuf, uint32_t cbBuf,
                       uint32_t *pcWritten)
{
    return AudioMixBufWriteAtEx(pMixBuf, pMixBuf->AudioFmt,
                                offSamples, pvBuf, cbBuf, pcWritten);
}

/**
 * Writes audio samples at a specific offset. The audio sample format
 * to be written can be different from the audio format the mixing buffer
 * operates on.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Pointer to mixing buffer to write to.
 * @param   enmFmt                  Audio format supplied in the buffer.
 * @param   offSamples              Offset (in samples) starting to write at.
 * @param   pvBuf                   Pointer to audio buffer to be written.
 * @param   cbBuf                   Size (in bytes) of audio buffer.
 * @param   pcWritten               Returns number of audio samples written. Optional.
 */
int AudioMixBufWriteAtEx(PPDMAUDIOMIXBUF pMixBuf, PDMAUDIOMIXBUFFMT enmFmt,
                         uint32_t offSamples,
                         const void *pvBuf, uint32_t cbBuf,
                         uint32_t *pcWritten)
{
    AssertPtrReturn(pMixBuf, VERR_INVALID_POINTER);
    AssertPtrReturn(pvBuf, VERR_INVALID_POINTER);
    /* pcWritten is optional. */

    uint32_t cDstSamples = pMixBuf->pParent
                         ? pMixBuf->pParent->cSamples : pMixBuf->cSamples;
    uint32_t cLive = pMixBuf->cProcessed;

    uint32_t cDead = cDstSamples - cLive;
    uint32_t cToProcess = (uint32_t)AUDIOMIXBUF_S2S_RATIO(pMixBuf, cDead);
    cToProcess = RT_MIN(cToProcess, AUDIOMIXBUF_B2S(pMixBuf, cbBuf));

    AUDMIXBUF_LOG(("%s: offSamples=%RU32, cLive=%RU32, cDead=%RU32, cToProcess=%RU32\n",
                   pMixBuf->pszName, offSamples, cLive, cDead, cToProcess));

    if (offSamples + cToProcess > pMixBuf->cSamples)
        return VERR_BUFFER_OVERFLOW;

    PAUDMIXBUF_FN_CONVFROM pConv = audioMixBufConvFromLookup(enmFmt, pMixBuf->Volume.fMuted);
    if (!pConv)
        return VERR_NOT_SUPPORTED;

    int rc;
    uint32_t cWritten;

#ifdef DEBUG_DUMP_PCM_DATA
    RTFILE fh;
    rc = RTFileOpen(&fh, DEBUG_DUMP_PCM_DATA_PATH "mixbuf_writeat.pcm",
                    RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND | RTFILE_O_WRITE | RTFILE_O_DENY_NONE);
    if (RT_SUCCESS(rc))
    {
        RTFileWrite(fh, pvBuf, cbBuf, NULL);
        RTFileClose(fh);
    }
#endif

    if (cToProcess)
    {
        AUDMIXBUF_CONVOPTS convOpts = { cToProcess, pMixBuf->Volume };

        cWritten = pConv(pMixBuf->pSamples + offSamples, pvBuf, cbBuf, &convOpts);
#ifdef DEBUG
        audioMixBufPrint(pMixBuf);
#endif
        rc = cWritten ? VINF_SUCCESS : VERR_GENERAL_FAILURE; /** @todo Fudge! */
    }
    else
    {
        cWritten = 0;
        rc = VINF_SUCCESS;
    }

    if (RT_SUCCESS(rc))
    {
        if (pcWritten)
            *pcWritten = cWritten;
    }

    AUDMIXBUF_LOG(("cWritten=%RU32, rc=%Rrc\n", cWritten, rc));
    return rc;
}

/**
 * Writes audio samples. The sample format being written must match the
 * format of the mixing buffer.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Pointer to mixing buffer to write to.
 * @param   pvBuf                   Pointer to audio buffer to be written.
 * @param   cbBuf                   Size (in bytes) of audio buffer.
 * @param   pcWritten               Returns number of audio samples written. Optional.
 */
int AudioMixBufWriteCirc(PPDMAUDIOMIXBUF pMixBuf,
                         const void *pvBuf, uint32_t cbBuf,
                         uint32_t *pcWritten)
{
    return AudioMixBufWriteCircEx(pMixBuf, pMixBuf->AudioFmt, pvBuf, cbBuf, pcWritten);
}

/**
 * Writes audio samples of a specific format.
 *
 * @return  IPRT status code.
 * @param   pMixBuf                 Pointer to mixing buffer to write to.
 * @param   enmFmt                  Audio format supplied in the buffer.
 * @param   pvBuf                   Pointer to audio buffer to be written.
 * @param   cbBuf                   Size (in bytes) of audio buffer.
 * @param   pcWritten               Returns number of audio samples written. Optional.
 */
int AudioMixBufWriteCircEx(PPDMAUDIOMIXBUF pMixBuf, PDMAUDIOMIXBUFFMT enmFmt,
                           const void *pvBuf, uint32_t cbBuf,
                           uint32_t *pcWritten)
{
    AssertPtrReturn(pMixBuf, VERR_INVALID_POINTER);
    AssertPtrReturn(pvBuf, VERR_INVALID_POINTER);
    /* pcbWritten is optional. */

    if (!cbBuf)
    {
        if (pcWritten)
            *pcWritten = 0;
        return VINF_SUCCESS;
    }

    PPDMAUDIOMIXBUF pParent = pMixBuf->pParent;

    AUDMIXBUF_LOG(("%s: enmFmt=%ld, pBuf=%p, cbBuf=%zu, pParent=%p (%RU32)\n",
                   pMixBuf->pszName, enmFmt, pvBuf, cbBuf, pParent, pParent ? pParent->cSamples : 0));

    if (   pParent
        && pParent->cSamples <= pMixBuf->cMixed)
    {
        if (pcWritten)
            *pcWritten = 0;

        AUDMIXBUF_LOG(("%s: Parent buffer %s is full\n",
                       pMixBuf->pszName, pMixBuf->pParent->pszName));

        return VINF_SUCCESS;
    }

    PAUDMIXBUF_FN_CONVFROM pConv = audioMixBufConvFromLookup(enmFmt, pMixBuf->Volume.fMuted);
    if (!pConv)
        return VERR_NOT_SUPPORTED;

    int rc = VINF_SUCCESS;

    uint32_t cToWrite = AUDIOMIXBUF_B2S(pMixBuf, cbBuf);
    AssertMsg(cToWrite, ("cToWrite is 0 (cbBuf=%zu)\n", cbBuf));

    PPDMAUDIOSAMPLE pSamplesDst1 = pMixBuf->pSamples + pMixBuf->offReadWrite;
    uint32_t cLenDst1 = cToWrite;

    PPDMAUDIOSAMPLE pSamplesDst2 = NULL;
    uint32_t cLenDst2 = 0;

    uint32_t offWrite = pMixBuf->offReadWrite + cToWrite;

    /*
     * Do we need to wrap around to write all requested data, that is,
     * starting at the beginning of our circular buffer? This then will
     * be the optional second part to do.
     */
    if (offWrite >= pMixBuf->cSamples)
    {
        Assert(pMixBuf->offReadWrite <= pMixBuf->cSamples);
        cLenDst1 = pMixBuf->cSamples - pMixBuf->offReadWrite;

        pSamplesDst2 = pMixBuf->pSamples;
        Assert(cToWrite >= cLenDst1);
        cLenDst2 = RT_MIN(cToWrite - cLenDst1, pMixBuf->cSamples);

        /* Save new read offset. */
        offWrite = cLenDst2;
    }

    uint32_t cWrittenTotal = 0;

    AUDMIXBUF_CONVOPTS convOpts;
    convOpts.Volume = pMixBuf->Volume;

    /* Anything to do at all? */
    if (cLenDst1)
    {
        convOpts.cSamples = cLenDst1;
        cWrittenTotal = pConv(pSamplesDst1, pvBuf, cbBuf, &convOpts);
    }

    /* Second part present? */
    if (   RT_LIKELY(RT_SUCCESS(rc))
        && cLenDst2)
    {
        AssertPtr(pSamplesDst2);

        convOpts.cSamples = cLenDst2;
        cWrittenTotal += pConv(pSamplesDst2, (uint8_t *)pvBuf + AUDIOMIXBUF_S2B(pMixBuf, cLenDst1), cbBuf, &convOpts);
    }

#ifdef DEBUG_DUMP_PCM_DATA
        RTFILE fh;
        RTFileOpen(&fh, DEBUG_DUMP_PCM_DATA_PATH "mixbuf_writeex.pcm",
                   RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND | RTFILE_O_WRITE | RTFILE_O_DENY_NONE);
        RTFileWrite(fh, pSamplesDst1, AUDIOMIXBUF_S2B(pMixBuf, cLenDst1), NULL);
        RTFileClose(fh);
#endif

    AUDMIXBUF_LOG(("cLenDst1=%RU32, cLenDst2=%RU32, offWrite=%RU32\n",
                   cLenDst1, cLenDst2, offWrite));

    if (RT_SUCCESS(rc))
    {
        pMixBuf->offReadWrite = offWrite % pMixBuf->cSamples;
        pMixBuf->cProcessed = RT_MIN(pMixBuf->cProcessed + cLenDst1 + cLenDst2,
                                     pMixBuf->cSamples /* Max */);
        if (pcWritten)
            *pcWritten = cLenDst1 + cLenDst2;
    }

#ifdef DEBUG
    audioMixBufPrint(pMixBuf);
#endif

    AUDMIXBUF_LOG(("cWritten=%RU32 (%zu bytes), rc=%Rrc\n",
                   cLenDst1 + cLenDst2,
                   AUDIOMIXBUF_S2B(pMixBuf, cLenDst1 + cLenDst2), rc));
    return rc;
}

