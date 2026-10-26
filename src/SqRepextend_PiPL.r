#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "SqRepextend_Flags.h"

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

resource 'PiPL' (16000) {
	{
		Kind {
			AEEffect
		},
		Name {
			SQ_NAME
		},
		Category {
			SQ_CATEGORY
		},
#ifdef AE_OS_WIN
	#if defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
	#else
		CodeWin64X86 {"EffectMain"},
	#endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
#endif
		AE_PiPL_Version {
			2,
			0
		},
		AE_Effect_Spec_Version {
			PF_PLUG_IN_VERSION,
			PF_PLUG_IN_SUBVERS
		},
		AE_Effect_Version {
			SQ_PIPL_VERSION
		},
		AE_Effect_Info_Flags {
			0
		},
		AE_Effect_Global_OutFlags {
			SQ_OUTFLAGS
		},
		AE_Effect_Global_OutFlags_2 {
			SQ_OUTFLAGS2
		},
		AE_Effect_Match_Name {
			SQ_MATCH_NAME
		},
		AE_Reserved_Info {
			0
		},
		AE_Effect_Support_URL {
			SQ_SUPPORT_URL
		}
	}
};
