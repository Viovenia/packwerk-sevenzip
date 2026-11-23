// LzmaDecOpt_arm64.S - own wrapper of the SevenZip package (GNU LGPL 2.1 or later), not part of 7-Zip.
// SwiftPM has no per-architecture sources: the unchanged 7-Zip assembly is
// included only for arm64 (x86_64 simulator builds compile the C path).
#if defined(__aarch64__)
#include "../Asm/arm64/LzmaDecOpt.S"
#if defined(__APPLE__)
// Keep the symbol out of the dynamic library's export table.
        .private_extern _LzmaDec_DecodeReal_3
#endif
#endif
