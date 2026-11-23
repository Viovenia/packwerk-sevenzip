// BridgeInternal.h - shared declarations of the C interface implementation.
// Own file of the SevenZip package (GNU LGPL 2.1 or later), not part of 7-Zip.
// Uses only the public COM-style interfaces of the 7-Zip format library
// (IInArchive, IOutArchive and their callbacks), in the way the 7-Zip example
// client CPP/7zip/UI/Client7z/Client7z.cpp shows.

#ifndef SEVENZIP_BRIDGE_INTERNAL_H
#define SEVENZIP_BRIDGE_INTERNAL_H

#include "../CPP/Common/MyWindows.h"
#include "../CPP/Common/MyCom.h"
#include "../CPP/7zip/Archive/IArchive.h"
#include "../CPP/7zip/IPassword.h"
#include "../CPP/7zip/PropID.h"

#include "../include/SevenZipBridge.h"

#include <atomic>
#include <string>
#include <vector>

// Exports of the format library (defined in CPP/7zip/Archive/DllExports2.cpp and
// ArchiveExports.cpp). Hidden from the dynamic library's export table.
STDAPI CreateObject(const GUID *clsid, const GUID *iid, void **outObject);
STDAPI GetNumberOfFormats(UInt32 *numFormats);
STDAPI GetHandlerProperty2(UInt32 formatIndex, PROPID propID, PROPVARIANT *value);

// CPP/Common/StringConvert.cpp: true makes legacy names arrive as UTF-8 with the
// bytes 0x80..0xFF escaped as U+EF80..U+EFFF, independent of the process locale.
extern bool g_ForceToUTF8;

struct sz_cancel_token
{
  std::atomic<int32_t> cancelled;
  sz_cancel_token(): cancelled(0) {}
};

namespace szb {

// ---------- strings ----------

/// UTF-8 to wchar_t holding UTF-16 units (surrogate pairs above U+FFFF), as 7-Zip expects on
/// every platform; invalid bytes become 7-Zip escapes U+EF80..U+EFFF.
std::wstring Utf8ToWide(const char *s);

/// wchar_t to bytes: joins UTF-16 surrogate pairs and turns 7-Zip escapes
/// U+EF80..U+EFFF back into the raw bytes 0x80..0xFF.
std::string WideToBytes(const wchar_t *w, size_t length, bool *hadRawBytes);

/// True if `bytes` is well-formed UTF-8.
bool IsValidUtf8(const std::string &bytes);

/// Overwrites the characters before the string is released.
void Wipe(std::wstring &s);

/// Copies `s` into a C buffer, always NUL-terminated.
void CopyToBuffer(char *buffer, size_t bufferSize, const std::string &s);

void SetError(sz_error *error, sz_status status, const char *message, int osError = 0);

bool IsCancelled(const sz_cancel_token *token);

// ---------- formats ----------

struct Format
{
  std::wstring name;
  std::wstring extensions;  // space separated, lower case
  GUID clsid;
  bool canUpdate;
  UInt32 flags;  // NArcInfoFlags
};

const std::vector<Format> &Formats();
const Format *FindFormat(const wchar_t *name);

// ---------- streams ----------

Z7_CLASS_IMP_COM_1(CInFile, IInStream)
  Z7_IFACE_COM7_IMP(ISequentialInStream)
public:
  int fd;
  const sz_cancel_token *cancel;
  CInFile(): fd(-1), cancel(NULL) {}
  ~CInFile();
  bool Open(const char *path);
};

/// Seekable output file (7z writes its start header last).
Z7_CLASS_IMP_COM_1(COutFile, IOutStream)
  Z7_IFACE_COM7_IMP(ISequentialOutStream)
public:
  int fd;
  int lastError;
  UInt64 written;
  const sz_cancel_token *cancel;
  COutFile(): fd(-1), lastError(0), written(0), cancel(NULL) {}
  ~COutFile();
  bool Create(const char *path, bool truncateExisting);
  bool Close();
};

/// Collects a small item (symbolic link target) in memory.
Z7_CLASS_IMP_COM_1(CMemOut, ISequentialOutStream)
public:
  std::string data;
  size_t limit;
  bool overflow;
  CMemOut(): limit(1 << 20), overflow(false) {}
};

// ---------- property helpers ----------

std::wstring GetStringProp(IInArchive *archive, UInt32 index, PROPID id, bool *present = NULL);
bool GetBoolProp(IInArchive *archive, UInt32 index, PROPID id, bool *present = NULL);
UInt64 GetU64Prop(IInArchive *archive, UInt32 index, PROPID id, bool *present);
UInt32 GetU32Prop(IInArchive *archive, UInt32 index, PROPID id, bool *present);
bool GetTimeProp(IInArchive *archive, UInt32 index, PROPID id, Int64 *seconds, Int32 *nanoseconds);
UInt64 GetArchiveU64Prop(IInArchive *archive, PROPID id, bool *present);
UInt32 GetArchiveU32Prop(IInArchive *archive, PROPID id, bool *present);
bool GetArchiveBoolProp(IInArchive *archive, PROPID id);

/// Sets "mt" (threads) on a handler that supports ISetProperties.
void SetThreads(IUnknown *handler, UInt32 threads);

// ---------- archive handle ----------

struct Item
{
  std::string path;
  bool pathIsValidUtf8;
  bool pathUtf8Flag;
  bool hasLinkTarget;
  std::string linkTarget;
  sz_item info;
};

} // namespace szb

struct sz_archive
{
  CMyComPtr<IInArchive> archive;      // archive that holds the items
  CMyComPtr<IInArchive> container;    // Split handler for .001 parts, else NULL
  CMyComPtr<IUnknown> openCallback;   // keeps volume streams alive
  std::wstring formatName;
  std::wstring containerName;
  std::string formatUtf8;
  std::string containerUtf8;
  std::vector<std::string> volumePaths;
  std::vector<szb::Item> items;
  bool headersEncrypted;
  bool hasPassword;
  std::wstring password;
  uint32_t threads;
  uint64_t memoryLimit;
  sz_archive(): headersEncrypted(false), hasPassword(false), threads(0), memoryLimit(0) {}
  ~sz_archive();
};

#endif
