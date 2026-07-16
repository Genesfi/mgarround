/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2007-2023 Adobe Inc.                                  */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  All information contained herein is, and remains the   */
/* property of Adobe Inc. and its suppliers, if                    */
/* any.  The intellectual and technical concepts contained         */
/* herein are proprietary to Adobe Inc. and its                    */
/* suppliers and may be covered by U.S. and Foreign Patents,       */
/* patents in process, and are protected by trade secret or        */
/* copyright law.  Dissemination of this information or            */
/* reproduction of this material is strictly forbidden unless      */
/* prior written permission is obtained from Adobe Inc.            */
/* Incorporated.                                                   */
/*                                                                 */
/*******************************************************************/

/*
        mgAround.h
*/

#pragma once

#ifndef MGAROUND_H
#define MGAROUND_H

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned short u_int16;
typedef unsigned long u_long;
typedef short int int16;
#define PF_TABLE_BITS 12
#define PF_TABLE_SZ_16 4096

#define PF_DEEP_COLOR_AWARE                                                    \
  1 // make sure we get 16bpc pixels;
    // AE_Effect.h checks for this.

#include "AEConfig.h"

#ifdef AE_OS_WIN
typedef unsigned short PixelType;
#include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"


#include "mgAround_Strings.h"

/* Versioning information */

#define MAJOR_VERSION 1
#define MINOR_VERSION 1
#define BUG_VERSION 0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 1

/* Parameter defaults & limits */

#define MGA_BOX_OPACITY_MIN 0
#define MGA_BOX_OPACITY_MAX 100
#define MGA_BOX_OPACITY_DFLT 100

#define MGA_OUTLINE_WIDTH_MIN 0
#define MGA_OUTLINE_WIDTH_MAX 100
#define MGA_OUTLINE_WIDTH_DFLT 3

#define MGA_BORDER_RADIUS_MIN 0
#define MGA_BORDER_RADIUS_MAX 500
#define MGA_BORDER_RADIUS_DFLT 0

#define MGA_PADDING_MIN -5000
#define MGA_PADDING_MAX 5000
#define MGA_PADDING_DFLT 10

#define MGA_SCALE_MIN 0
#define MGA_SCALE_MAX 5000
#define MGA_SCALE_DFLT 100

#define MGA_OFFSET_MIN -2000
#define MGA_OFFSET_MAX 2000
#define MGA_OFFSET_DFLT 0

#define MGA_GAP_MIN 0.1
#define MGA_GAP_MAX 5.0
#define MGA_GAP_WORD_DFLT 0.4
#define MGA_GAP_LINE_DFLT 0.5

enum {
  MGA_INPUT = 0,
  MGA_MODE,
  MGA_TARGET,
  MGA_SIZE_MODE,
  MGA_BOX_COLOR,
  MGA_BOX_OPACITY,
  MGA_OUTLINE_COLOR,
  MGA_OUTLINE_OPACITY,
  MGA_OUTLINE_WIDTH,
  MGA_BORDER_RADIUS,
  MGA_PADDING_LEFT,
  MGA_PADDING_RIGHT,
  MGA_PADDING_TOP,
  MGA_PADDING_BOTTOM,
  MGA_CUSTOM_LAYER,
  MGA_UNIFORM_SCALE,
  MGA_KEEP_ASPECT,
  MGA_SCALE_X,
  MGA_SCALE_Y,
  MGA_OFFSET_X,
  MGA_OFFSET_Y,
  MGA_ROTATE,
  MGA_COMP_MODE,
  MGA_WORD_GAP,
  MGA_LINE_GAP,
  MGA_MB_ENABLE,
  MGA_MB_SAMPLES,
  MGA_FILL_OUTLINE,
  MGA_NUM_PARAMS
};

enum {
  MODE_DISK_ID = 1,
  TARGET_DISK_ID,
  SIZE_MODE_DISK_ID,
  BOX_COLOR_DISK_ID,
  BOX_OPACITY_DISK_ID,
  OUTLINE_COLOR_DISK_ID,
  OUTLINE_OPACITY_DISK_ID,
  OUTLINE_WIDTH_DISK_ID,
  BORDER_RADIUS_DISK_ID,
  PADDING_LEFT_DISK_ID,
  PADDING_RIGHT_DISK_ID,
  PADDING_TOP_DISK_ID,
  PADDING_BOTTOM_DISK_ID,
  CUSTOM_LAYER_DISK_ID,
  UNIFORM_SCALE_DISK_ID,
  KEEP_ASPECT_DISK_ID,
  SCALE_X_DISK_ID,
  SCALE_Y_DISK_ID,
  OFFSET_X_DISK_ID,
  OFFSET_Y_DISK_ID,
  ROTATE_DISK_ID,
  COMP_MODE_DISK_ID,
  WORD_GAP_DISK_ID,
  LINE_GAP_DISK_ID,
  MB_ENABLE_DISK_ID,
  MB_SAMPLES_DISK_ID,
  FILL_OUTLINE_DISK_ID
};

/* Box Mode choices */
enum {
  MGA_MODE_OUTLINE = 1,
  MGA_MODE_SOLID,
  MGA_MODE_BOTH,
  MGA_MODE_CUSTOM_LAYER,
  MGA_MODE_TEXT_OUTLINE,
  MGA_MODE_TEXT_SOLID,
  MGA_MODE_TEXT_BOTH
};

/* Target choices */
enum { MGA_TARGET_WHOLE = 1, MGA_TARGET_WORD, MGA_TARGET_CHAR };

/* Size Mode choices */
enum { MGA_SIZE_AUTO_FIT = 1, MGA_SIZE_CONSISTENT };

/* Composite Mode choices */
enum { MGA_COMP_BEHIND = 1, MGA_COMP_ON_TOP, MGA_COMP_BOX_ONLY };

extern "C" {

DllExport PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data,
                            PF_OutData *out_data, PF_ParamDef *params[],
                            PF_LayerDef *output, void *extra);
}

#endif // MGAROUND_H