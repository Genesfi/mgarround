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

#include "mgAround.h"

typedef struct {
	A_u_long	index;
	A_char		str[256];
} TableString;

TableString		g_strs[StrID_NUMTYPES] = {
	StrID_NONE,						"",
	StrID_Name,						"mgAround",
	StrID_Description,				"Draw boxes and outlines around text or elements per word or character.\rCopyright 2026",
	StrID_Mode_Param_Name,			"Shape Mode",
	StrID_Target_Param_Name,		"Target Style",
	StrID_SizeMode_Param_Name,		"Consistent Size",
	StrID_BoxColor_Param_Name,		"Fill Color",
	StrID_BoxOpacity_Param_Name,	"Fill Opacity",
	StrID_OutlineColor_Param_Name,	"Outline Color",
	StrID_OutlineOpacity_Param_Name,"Outline Opacity",
	StrID_OutlineWidth_Param_Name,	"Outline Width",
	StrID_BorderRadius_Param_Name,	"Border Radius",
	StrID_PaddingLeft_Param_Name,	"Padding Left",
	StrID_PaddingRight_Param_Name,	"Padding Right",
	StrID_PaddingTop_Param_Name,	"Padding Top",
	StrID_PaddingBottom_Param_Name,	"Padding Bottom",
	StrID_CustomLayer_Param_Name,	"Custom Shape Layer",
	StrID_UniformScale_Param_Name,	"Uniform Scale",
	StrID_KeepAspect_Param_Name,	"Keep Sprite Aspect Ratio",
	StrID_ScaleX_Param_Name,		"Scale X",
	StrID_ScaleY_Param_Name,		"Scale Y",
	StrID_OffsetX_Param_Name,		"Offset X",
	StrID_OffsetY_Param_Name,		"Offset Y",
	StrID_Rotate_Param_Name,		"Rotation",
	StrID_CompMode_Param_Name,		"Composite Mode",
	StrID_WordGap_Param_Name,		"Word Gap",
	StrID_LineGap_Param_Name,		"Line Gap",
	StrID_MBEnable_Param_Name,		"Motion Blur",
	StrID_MBSamples_Param_Name,		"Motion Blur Samples",
	StrID_FillOutline_Param_Name,	"Fill Outline",
};

char	*GetStringPtr(int strNum)
{
	return g_strs[strNum].str;
}