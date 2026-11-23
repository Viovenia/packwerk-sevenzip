# SevenZip – the 7-Zip library in Packwerk

This is the complete source code of `SevenZip.framework`, the dynamic library inside the app
**Packwerk** (iPhone and iPad, version 1.0): the unchanged 7-Zip 26.06 format library and a small
C interface (`Sources/SevenZipC/include/SevenZipBridge.h`), together with the files that build it.
Packwerk uses this library, and the library and its use are covered by the GNU LGPL.

| | |
| --- | --- |
| Library | 7-Zip 26.06 by Igor Pavlov, Copyright (C) 1999-2026 Igor Pavlov |
| Upstream | `7z2606-src.tar.xz` from [7-zip.org](https://www.7-zip.org/download.html), see [NOTICE.md](NOTICE.md) for the download address, size and SHA-256 |
| License | GNU LGPL 2.1 or later, with the unRAR license restriction for the RAR decoder, see [LICENSE](LICENSE) |
| Used in | Packwerk 1.0 for iOS 26 and later |

The source code of the app itself is not part of this repository. The app talks to 7-Zip only
through the C functions declared in `SevenZipBridge.h`.

## unRAR restriction

The RAR decoder (`Sources/SevenZipC/CPP/7zip/Compress/Rar*`) may only be used to read RAR archives.
**The unRAR code must not be used to develop a RAR (WinRAR) compatible archiver or to recreate the
RAR compression algorithm, which is proprietary.** Packwerk only reads RAR archives. The full text
is in [LICENSE](LICENSE) (`DOC/unRarLicense.txt`).

## Build

Built and checked with Xcode 27.2 (Swift 6.4 toolchain) on macOS 27.2. The package has no
dependencies.

```sh
# macOS library
swift build -c release            # → .build/release/libSevenZip.dylib

# iOS device and simulator
xcodebuild build -scheme SevenZip -configuration Release \
  -destination 'generic/platform=iOS' -derivedDataPath .build-dd/ios CODE_SIGNING_ALLOWED=NO
xcodebuild build -scheme SevenZip -configuration Release \
  -destination 'generic/platform=iOS Simulator' -derivedDataPath .build-dd/sim CODE_SIGNING_ALLOWED=NO
# → .build-dd/ios/Build/Products/Release-iphoneos/PackageFrameworks/SevenZip.framework
```

Check that only the C interface is exported (15 functions, all `_sz_…`):

```sh
nm -gU .build-dd/ios/Build/Products/Release-iphoneos/PackageFrameworks/SevenZip.framework/SevenZip
```

The linker prints notes such as "could not associate debug note to atom …" for constants of the
unchanged assembler decoder (`Asm/arm64/LzmaDecOpt.S`). They are harmless.

`Package.swift` uses `unsafeFlags` (`-fno-modules`, `-fvisibility=hidden`). Swift Package Manager
accepts these only in root and local packages, so add a clone of this repository as a local package.

## Replacing the library (GNU LGPL 2.1, section 6)

You may change this library and use your version with Packwerk. The app loads it at run time as
`@rpath/SevenZip.framework/SevenZip` and calls only the functions declared in `SevenZipBridge.h`.
A replacement must keep that interface (names, structures, meaning).

1. Get the source code of this repository at the tag of the Packwerk version you use
   (`packwerk-1.0-7zip-26.06` for Packwerk 1.0).
2. Change the 7-Zip sources or the bridge as you like.
3. Build the framework for iOS as shown above.
4. Replace `Packwerk.app/Frameworks/SevenZip.framework` with your framework, sign the framework and
   the app with your own signing identity (`codesign --force --sign "<identity>" …`) and install the
   app on your device with your developer account.

iOS only runs apps that are signed by Apple or with your own developer certificate, so installing a
changed app needs your own signing. Packwerk contains nothing that checks for or prevents a
replaced framework.

## Layout

| Path | Content |
| --- | --- |
| `Sources/SevenZipC/C`, `CPP`, `Asm` | 7-Zip 26.06, unchanged |
| `Sources/SevenZipC/Bridge` | Own C++ bridge (open, list, extract, pack) |
| `Sources/SevenZipC/include/SevenZipBridge.h` | Public C interface |
| `Sources/SevenZipC/Resources/PrivacyInfo.xcprivacy` | Privacy manifest of the library |
| `Package.swift` | Build description with the compiler settings |
| `LICENSE`, `NOTICE.md` | License texts, origin and the list of own files |
