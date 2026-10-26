// Shared between SqRepextend.cpp and SqRepextend_PiPL.r.
// The .cpp static_asserts these against the SDK constants so the PiPL can never drift.
#pragma once

#define SQ_NAME            "Sq_Repextend"
// Kept from v1.0 so projects that already use the effect still find it.
#define SQ_MATCH_NAME      "Sq Repextend"
#define SQ_CATEGORY        "Sq"
#define SQ_SUPPORT_URL     "https://github.com/Sqkurq/Sq_Repextend"
// ASCII only: parameter names are not UTF-8 in every host locale.
#define SQ_LICENSE         "(C) 2026 Sqkurq"

#define SQ_MAJOR_VERSION   1
#define SQ_MINOR_VERSION   1
#define SQ_BUG_VERSION     0
#define SQ_BUILD_VERSION   1

// PF_VERSION(1, 1, 0, PF_Stage_RELEASE, 1)
#define SQ_PIPL_VERSION    558593

// PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_I_EXPAND_BUFFER
#define SQ_OUTFLAGS        0x02000200

// PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_SUPPORTS_THREADED_RENDERING
#define SQ_OUTFLAGS2       0x08001400
