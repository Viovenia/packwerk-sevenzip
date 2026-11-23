// LzmaDec_wrap.c - own wrapper of the SevenZip package (GNU LGPL 2.1 or later), not part of 7-Zip.
// Compiles the unchanged C/LzmaDec.c; on arm64 with the assembly fast path
// (Z7_LZMA_DEC_OPT), because SwiftPM has no per-architecture defines.
#if defined(__aarch64__)
#define Z7_LZMA_DEC_OPT
#endif
#include "../C/LzmaDec.c"
