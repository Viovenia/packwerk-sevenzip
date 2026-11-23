# SevenZip package – notice

This package builds the 7-Zip format library as a dynamic framework (`SevenZip.framework`) for
Packwerk on iOS and iPadOS. License texts: [LICENSE](LICENSE).

Complete source code of this framework, with build and replacement instructions:
https://github.com/Viovenia/packwerk-sevenzip

## Origin of the 7-Zip code

| | |
| --- | --- |
| Product | 7-Zip 26.06 by Igor Pavlov, Copyright (C) 1999-2026 Igor Pavlov |
| Source archive | `7z2606-src.tar.xz`, 1,567,328 bytes |
| SHA-256 | `a4e374c8f864f51df48c2cfd0e1413ec3b2ea030deae852d6a655402ebb61ac4` |
| Download | https://www.7-zip.org/download.html → https://github.com/ip7z/7zip/releases/download/26.06/7z2606-src.tar.xz |
| Fetched | 11.11.2026 |
| Licenses | GNU LGPL 2.1 or later; RAR decoder (`CPP/7zip/Compress/Rar*`) GNU LGPL with the unRAR license restriction; `CPP/7zip/Compress/LzfseDecoder.cpp` and `C/ZstdDec.c` BSD 3-clause; `C/Xxh64.c` BSD 2-clause |

`Sources/SevenZipC/C`, `Sources/SevenZipC/CPP` and `Sources/SevenZipC/Asm` hold 774 files of
the archive, the sources of the `Format7zF` bundle (all archive formats and codecs, no user
interface) and the headers they need. **They are unchanged**: on 11.11.2026 every one of the 774
files was compared byte by byte (`cmp`) with the extracted archive, with no difference.

## unRAR license restriction

The RAR decoder may only be used to read RAR archives. **The unRAR code must not be used to
create a RAR-compatible archiver or to recreate the RAR compression algorithm.** Packwerk only
reads RAR archives; it packs ZIP and 7z only. See `DOC/unRarLicense.txt` in [LICENSE](LICENSE).

## Own and derived files

Everything below was written for Packwerk (11.11.2026) and is distributed under the GNU LGPL 2.1
or later together with the library.

| File | What it is |
| --- | --- |
| `Package.swift` | Build description. Same defines as 7-Zip's macOS makefile (`NDEBUG`, `_REENTRANT`, `_FILE_OFFSET_BITS=64`, `_LARGEFILE_SOURCE`, `Z7_EXTERNAL_CODECS`), `-fvisibility=hidden` so that only the C interface is exported. Excludes `C/LzmaDec.c`, `Asm/arm64/LzmaDecOpt.S` and `Asm/arm64/7zAsm.S`, which are compiled through the two wrappers below. |
| `Sources/SevenZipC/include/SevenZipBridge.h` | The C interface: the only header the app sees, 15 exported functions (`sz_*`). |
| `Sources/SevenZipC/Bridge/BridgeInternal.h`, `BridgeCommon.cpp` | Shared helpers: UTF-8 ⇄ 7-Zip strings (UTF-16 units, raw-byte escapes), file streams with `O_NOFOLLOW` and cancel checks, format table, cancel token. |
| `Sources/SevenZipC/Bridge/BridgeOpen.cpp` | Opening by path with volume and password callbacks, numbered parts through the Split handler, item listing. |
| `Sources/SevenZipC/Bridge/BridgeExtract.cpp` | Extracting selected items to paths chosen by the caller, per-item results, memory limit for RAR5 dictionaries. |
| `Sources/SevenZipC/Bridge/BridgePack.cpp` | Writing 7z and ZIP (level, dictionary, threads, AES-256, encrypted 7z headers). |
| `Sources/SevenZipC/Bridge/LzmaDec_wrap.c` | Includes the unchanged `C/LzmaDec.c`; on arm64 with `Z7_LZMA_DEC_OPT` so that the assembler decoder is used. |
| `Sources/SevenZipC/Bridge/LzmaDecOpt_arm64.S` | Includes the unchanged `Asm/arm64/LzmaDecOpt.S` on arm64 only (x86_64 simulator builds use the C decoder) and marks `_LzmaDec_DecodeReal_3` as not exported. |
| `Sources/SevenZipC/Resources/PrivacyInfo.xcprivacy` | Privacy manifest of the library (file metadata of archives and results; no data collected). |
| `LICENSE`, `NOTICE.md`, `README.md` | License texts (copied unchanged from `DOC/`), this notice, build and replacement instructions. |

The bridge uses the library only through its public COM-style interfaces (`IInArchive`,
`IOutArchive` and their callbacks), as shown in 7-Zip's sample client
`CPP/7zip/UI/Client7z/Client7z.cpp`; no code of the 7-Zip user interface is included.
