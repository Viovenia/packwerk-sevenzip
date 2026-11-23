// swift-tools-version: 6.0
// SevenZip: the 7-Zip 26.06 format library (unchanged sources) plus a small C
// interface, built from source as a separate dynamic library/framework.
// Licence: GNU LGPL 2.1 or later with the unRAR restriction, see LICENSE and NOTICE.md.
import PackageDescription

// Same defines as 7-Zip's own makefile (CPP/7zip/cmpl_mac_arm64.mak, Format7zF bundle).
let defines: [(String, String?)] = [
    ("NDEBUG", nil),
    ("_REENTRANT", nil),
    ("_FILE_OFFSET_BITS", "64"),
    ("_LARGEFILE_SOURCE", nil),
    ("Z7_EXTERNAL_CODECS", nil),
]

// -fno-modules: 7-Zip's intrinsics headers do not build as Clang modules on x86_64.
// -fvisibility=hidden: only the functions marked SZ_API in SevenZipBridge.h are exported.
let flags = ["-fno-modules", "-fvisibility=hidden"]

let package = Package(
    name: "SevenZip",
    platforms: [.iOS("26.0"), .macOS("15.0")],
    products: [
        .library(name: "SevenZip", type: .dynamic, targets: ["SevenZipC"]),
    ],
    targets: [
        .target(
            name: "SevenZipC",
            path: "Sources/SevenZipC",
            exclude: [
                // Compiled through Bridge/LzmaDecOpt_arm64.S and Bridge/LzmaDec_wrap.c.
                "Asm/arm64/LzmaDecOpt.S",
                "Asm/arm64/7zAsm.S",
                "C/LzmaDec.c",
            ],
            // The framework's own privacy manifest: 7-Zip and the bridge call stat/fstat/lstat.
            resources: [.copy("Resources/PrivacyInfo.xcprivacy")],
            publicHeadersPath: "include",
            cSettings: defines.map { CSetting.define($0.0, to: $0.1) } + [.unsafeFlags(flags)],
            cxxSettings: defines.map { CXXSetting.define($0.0, to: $0.1) }
                + [.unsafeFlags(flags + ["-fvisibility-inlines-hidden"])]
        ),
    ],
    cxxLanguageStandard: .cxx11
)
