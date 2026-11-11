#pragma once

#include "AEConfig.h"
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AE_Macros.h"
#include "AE_PluginData.h"
#include "Param_Utils.h"

#include "SqRepextend_Flags.h"

#ifndef DllExport
	#define DllExport __declspec(dllexport)
#endif

enum {
	SQ_INPUT = 0,
	SQ_EXPAND_RIGHT,
	SQ_EXPAND_LEFT,
	SQ_EXPAND_DOWN,
	SQ_EXPAND_UP,
	SQ_EXPAND_ALL,
	SQ_SHAPE,
	SQ_THRESHOLD,
	SQ_OUTPUT,
	SQ_OPAQUE_FILL,
	SQ_EXPAND_BOUNDS,
	SQ_LICENSE_START,
	SQ_LICENSE_END,
	SQ_EXTEND_FROM,			// added in v1.2; appended so v1.1 projects keep their parameter indices
	SQ_NUM_PARAMS
};

enum {
	SHAPE_SQUARE = 1,
	SHAPE_CIRCLE,
	SHAPE_DIAMOND
};

enum {
	EXTEND_FROM_BOUNDING_BOX = 1,
	EXTEND_FROM_ALPHA
};

enum {
	OUTPUT_OVER = 1,
	OUTPUT_REPLACE,
	OUTPUT_EXTENSION_ONLY
};

extern "C" {

DllExport
PF_Err EffectMain(
	PF_Cmd			cmd,
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output,
	void			*extra);

}
