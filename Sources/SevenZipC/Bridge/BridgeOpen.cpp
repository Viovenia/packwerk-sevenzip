// BridgeOpen.cpp - opening archives (volumes, password), item list.
// Own file of the SevenZip package (GNU LGPL 2.1 or later), not part of 7-Zip.

#include "BridgeInternal.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace szb {

// ---------- open callback: volumes and password ----------

Z7_CLASS_IMP_COM_3(COpenCallback, IArchiveOpenCallback, IArchiveOpenVolumeCallback, ICryptoGetTextPassword)
public:
  std::string directory;        // directory of the first volume
  std::wstring firstName;       // file name of the first volume
  sz_volume_resolver resolver;
  void *context;
  const sz_cancel_token *cancel;
  bool hasPassword;
  std::wstring password;
  bool passwordAsked;
  std::vector<std::string> missing;
  std::vector<std::string> openedPaths;
  COpenCallback(): resolver(NULL), context(NULL), cancel(NULL), hasPassword(false), passwordAsked(false) {}
  ~COpenCallback() { Wipe(password); }
};

Z7_COM7F_IMF(COpenCallback::SetTotal(const UInt64 *, const UInt64 *))
{
  return IsCancelled(cancel) ? E_ABORT : S_OK;
}

Z7_COM7F_IMF(COpenCallback::SetCompleted(const UInt64 *, const UInt64 *))
{
  return IsCancelled(cancel) ? E_ABORT : S_OK;
}

Z7_COM7F_IMF(COpenCallback::GetProperty(PROPID propID, PROPVARIANT *value))
{
  if (propID == kpidName)
  {
    value->vt = VT_BSTR;
    value->bstrVal = SysAllocString(firstName.c_str());  // the library frees it
    if (!value->bstrVal)
    {
      value->vt = VT_EMPTY;
      return E_OUTOFMEMORY;
    }
  }
  return S_OK;
}

Z7_COM7F_IMF(COpenCallback::GetStream(const wchar_t *name, IInStream **inStream))
{
  *inStream = NULL;
  if (IsCancelled(cancel))
    return E_ABORT;
  const std::string requested = WideToBytes(name, wcslen(name), NULL);
  std::string path;
  if (resolver)
  {
    char buffer[PATH_MAX + 1];
    buffer[0] = 0;
    if (resolver(context, requested.c_str(), buffer, sizeof(buffer)) != 0)
      path = buffer;
  }
  else if (requested.find('/') == std::string::npos)
  {
    path = directory + "/" + requested;
  }
  CInFile *spec = new CInFile;
  CMyComPtr<IInStream> stream = spec;
  spec->cancel = cancel;
  if (path.empty() || !spec->Open(path.c_str()))
  {
    missing.push_back(requested);
    return S_FALSE;
  }
  openedPaths.push_back(path);
  *inStream = stream.Detach();
  return S_OK;
}

Z7_COM7F_IMF(COpenCallback::CryptoGetTextPassword(BSTR *pw))
{
  passwordAsked = true;
  if (!hasPassword)
    return E_ABORT;
  *pw = SysAllocString(password.c_str());  // allocator of the library, it frees the string
  return *pw ? S_OK : E_OUTOFMEMORY;
}

// ---------- helpers ----------

static bool ExtensionMatches(const std::wstring &list, const std::wstring &ext)
{
  if (ext.empty())
    return false;
  size_t i = 0;
  while (i < list.size())
  {
    size_t j = list.find(L' ', i);
    if (j == std::wstring::npos)
      j = list.size();
    if (j - i == ext.size() && wcsncasecmp(list.c_str() + i, ext.c_str(), ext.size()) == 0)
      return true;
    i = j + 1;
  }
  return false;
}

static std::wstring ExtensionOf(const std::wstring &name)
{
  size_t dot = name.rfind(L'.');
  if (dot == std::wstring::npos || dot + 1 >= name.size())
    return std::wstring();
  return name.substr(dot + 1);
}

enum OpenOutcome
{
  kOpened,
  kNotOpened,
  kAborted
};

/// Tries the handlers whose extension matches first, then all others.
static OpenOutcome TryOpen(const std::wstring &ext, IInStream *stream, COpenCallback *callbackSpec,
                           IArchiveOpenCallback *callback, UInt32 threads, CMyComPtr<IInArchive> &result,
                           std::wstring &formatName, bool *anyExtensionMatch)
{
  const std::vector<Format> &formats = Formats();
  *anyExtensionMatch = false;
  for (int pass = 0; pass < 2; pass++)
  {
    for (size_t i = 0; i < formats.size(); i++)
    {
      const bool match = ExtensionMatches(formats[i].extensions, ext);
      if ((pass == 0) != match)
        continue;
      // Handlers without a signature (Base64, ...) would accept any text file, and the Split
      // handler goes by the name of the first part (it would take the joined stream again).
      if (!match && ((formats[i].flags & NArcInfoFlags::kByExtOnlyOpen) != 0 ||
                     wcscasecmp(formats[i].name.c_str(), L"Split") == 0))
        continue;
      if (match)
        *anyExtensionMatch = true;
      CMyComPtr<IInArchive> archive;
      if (CreateObject(&formats[i].clsid, &IID_IInArchive, (void **)&archive) != S_OK || !archive)
        continue;
      SetThreads(archive, threads);
      if (stream->Seek(0, STREAM_SEEK_SET, NULL) != S_OK)
        return kNotOpened;
      const bool askedBefore = callbackSpec->passwordAsked;
      const HRESULT hr = archive->Open(stream, NULL, callback);
      if (hr == S_OK)
      {
        result = archive;
        formatName = formats[i].name;
        return kOpened;
      }
      archive->Close();
      if (hr == E_ABORT || IsCancelled(callbackSpec->cancel))
        return kAborted;
      // A handler that asked for the password recognised the archive; another
      // handler would only produce a misleading result.
      if (!askedBefore && callbackSpec->passwordAsked)
        return kNotOpened;
    }
  }
  return kNotOpened;
}

static sz_status FailureStatus(const COpenCallback *cb, OpenOutcome outcome, bool anyExtensionMatch)
{
  if (IsCancelled(cb->cancel))
    return SZ_STATUS_CANCELLED;
  if (cb->passwordAsked)
    return cb->hasPassword ? SZ_STATUS_WRONG_PASSWORD : SZ_STATUS_PASSWORD_REQUIRED;
  if (outcome == kAborted)
    return SZ_STATUS_FAILED;
  if (!cb->missing.empty())
    return SZ_STATUS_MISSING_VOLUME;
  return anyExtensionMatch ? SZ_STATUS_CORRUPT : SZ_STATUS_UNSUPPORTED;
}

static void FillMissing(sz_error *error, const COpenCallback *cb)
{
  if (error && !cb->missing.empty())
    CopyToBuffer(error->volume_name, sizeof(error->volume_name), cb->missing[0]);
}

static void ReadItems(sz_archive *a)
{
  IInArchive *arc = a->archive;
  UInt32 count = 0;
  if (arc->GetNumberOfItems(&count) != S_OK)
    count = 0;
  const bool isZip = (wcscasecmp(a->formatName.c_str(), L"zip") == 0);
  a->items.resize(count);
  for (UInt32 i = 0; i < count; i++)
  {
    Item &it = a->items[i];
    memset(&it.info, 0, sizeof(it.info));
    it.info.index = i;

    bool present = false;
    const std::wstring wpath = GetStringProp(arc, i, kpidPath, &present);
    bool raw = false;
    it.path = WideToBytes(wpath.c_str(), wpath.size(), &raw);  // separator is '/' on POSIX builds
    it.pathIsValidUtf8 = !raw && IsValidUtf8(it.path);
    it.pathUtf8Flag = false;
    if (isZip)
    {
      const std::wstring characts = GetStringProp(arc, i, kpidCharacts);
      it.pathUtf8Flag = (characts.find(L"UTF8") != std::wstring::npos);
    }

    bool hasLink = false;
    const std::wstring link = GetStringProp(arc, i, kpidSymLink, &hasLink);
    it.hasLinkTarget = hasLink && !link.empty();
    if (it.hasLinkTarget)
      it.linkTarget = WideToBytes(link.c_str(), link.size(), NULL);

    sz_item &info = it.info;
    bool has = false;
    info.size = GetU64Prop(arc, i, kpidSize, &has);
    info.packed_size = GetU64Prop(arc, i, kpidPackSize, &has);
    info.has_packed_size = has ? 1 : 0;
    Int64 sec = 0;
    Int32 nsec = 0;
    if (GetTimeProp(arc, i, kpidMTime, &sec, &nsec))
    {
      info.mtime_seconds = sec;
      info.mtime_nanoseconds = nsec;
      info.has_mtime = 1;
    }
    info.attributes = GetU32Prop(arc, i, kpidAttrib, &has);
    info.has_attributes = has ? 1 : 0;
    info.posix_mode = GetU32Prop(arc, i, kpidPosixAttrib, &has);
    info.has_posix_mode = has ? 1 : 0;
    if (!info.has_posix_mode && info.has_attributes && (info.attributes & FILE_ATTRIBUTE_UNIX_EXTENSION))
    {
      info.posix_mode = info.attributes >> 16;
      info.has_posix_mode = 1;
    }
    info.crc = GetU32Prop(arc, i, kpidCRC, &has);
    info.has_crc = has ? 1 : 0;
    info.is_dir = GetBoolProp(arc, i, kpidIsDir) ? 1 : 0;
    info.is_encrypted = GetBoolProp(arc, i, kpidEncrypted) ? 1 : 0;
    const bool modeIsLink = info.has_posix_mode && S_ISLNK(info.posix_mode);
    info.is_symlink = (modeIsLink || it.hasLinkTarget) && !info.is_dir ? 1 : 0;
    info.is_auxiliary = (GetBoolProp(arc, i, kpidIsAltStream) || GetBoolProp(arc, i, kpidIsAux) ||
                         GetBoolProp(arc, i, kpidIsDeleted)) ? 1 : 0;
  }
  // Pointers into the strings are taken after the vector stopped growing.
  for (UInt32 i = 0; i < count; i++)
  {
    Item &it = a->items[i];
    it.info.path = (const uint8_t *)it.path.c_str();
    it.info.path_length = (uint32_t)it.path.size();
    it.info.path_is_valid_utf8 = it.pathIsValidUtf8 ? 1 : 0;
    it.info.path_utf8_flag = it.pathUtf8Flag ? 1 : 0;
    it.info.link_target = it.hasLinkTarget ? it.linkTarget.c_str() : NULL;
  }
}

} // namespace szb

sz_archive::~sz_archive()
{
  if (archive)
    archive->Close();
  if (container)
    container->Close();
  szb::Wipe(password);
}

using namespace szb;

extern "C" SZ_API sz_archive *sz_archive_open(const sz_open_options *options, sz_error *error)
{
  if (error)
    memset(error, 0, sizeof(*error));
  if (!options || !options->path || !options->path[0])
  {
    SetError(error, SZ_STATUS_INVALID_ARGUMENT, "missing path");
    return NULL;
  }
  Formats();  // sets g_ForceToUTF8 before the first handler reads names

  const std::string path = options->path;
  const size_t slash = path.rfind('/');
  const std::string directory = (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
  const std::wstring name = Utf8ToWide(path.c_str() + (slash == std::string::npos ? 0 : slash + 1));

  CInFile *inSpec = new CInFile;
  CMyComPtr<IInStream> inStream = inSpec;
  inSpec->cancel = options->cancel;
  if (!inSpec->Open(path.c_str()))
  {
    SetError(error, SZ_STATUS_IO, "cannot open archive file", errno);
    return NULL;
  }

  COpenCallback *cbSpec = new COpenCallback;
  CMyComPtr<IArchiveOpenCallback> callback = cbSpec;
  cbSpec->directory = directory;
  cbSpec->firstName = name;
  cbSpec->resolver = options->resolve_volume;
  cbSpec->context = options->context;
  cbSpec->cancel = options->cancel;
  cbSpec->hasPassword = (options->password != NULL);
  if (options->password)
    cbSpec->password = Utf8ToWide(options->password);
  cbSpec->openedPaths.push_back(path);

  CMyComPtr<IInArchive> archive;
  std::wstring formatName;
  bool anyMatch = false;
  OpenOutcome outcome = TryOpen(ExtensionOf(name), inStream, cbSpec, callback, options->threads, archive,
                                formatName, &anyMatch);
  if (outcome != kOpened)
  {
    const sz_status st = FailureStatus(cbSpec, outcome, anyMatch);
    SetError(error, st, "cannot open archive");
    if (st == SZ_STATUS_MISSING_VOLUME)
      FillMissing(error, cbSpec);
    return NULL;
  }

  sz_archive *a = new sz_archive();
  a->openCallback = callback;
  a->threads = options->threads;
  a->memoryLimit = options->memory_limit;
  a->hasPassword = cbSpec->hasPassword;
  if (cbSpec->hasPassword)
    a->password = cbSpec->password;

  if (wcscasecmp(formatName.c_str(), L"Split") == 0)
  {
    // Numbered parts (.001): the Split handler joins them; the real archive is inside.
    CMyComPtr<IInArchiveGetStream> getStream;
    archive->QueryInterface(IID_IInArchiveGetStream, (void **)&getStream);
    CMyComPtr<ISequentialInStream> seq;
    if (getStream)
      getStream->GetStream(0, &seq);
    CMyComPtr<IInStream> sub;
    if (seq)
      seq->QueryInterface(IID_IInStream, (void **)&sub);
    const std::wstring innerName = GetStringProp(archive, 0, kpidPath);
    CMyComPtr<IInArchive> inner;
    std::wstring innerFormat;
    bool innerMatch = false;
    OpenOutcome innerOutcome = kNotOpened;
    const size_t missingBefore = cbSpec->missing.size();
    if (sub)
      innerOutcome = TryOpen(ExtensionOf(innerName), sub, cbSpec, callback, options->threads, inner, innerFormat,
                             &innerMatch);
    if (innerOutcome != kOpened)
    {
      sz_status st = FailureStatus(cbSpec, innerOutcome, innerMatch);
      // The Split handler stops at the first part it cannot find. If the joined
      // data is not a readable archive, that part is the most likely cause.
      if ((st == SZ_STATUS_CORRUPT || st == SZ_STATUS_UNSUPPORTED) && missingBefore > 0)
        st = SZ_STATUS_MISSING_VOLUME;
      SetError(error, st, "cannot open the archive inside the numbered parts");
      if (st == SZ_STATUS_MISSING_VOLUME)
        FillMissing(error, cbSpec);
      archive->Close();
      delete a;
      return NULL;
    }
    // The Split handler always asks for the part after the last one, so a missing name alone
    // is the normal end of the set. Only if the joined archive ends too early is a part missing
    // (ZIP opens from the local headers of the first part and would list a few files only).
    bool hasFlags = false;
    const UInt32 innerErrors = GetArchiveU32Prop(inner, kpidErrorFlags, &hasFlags);
    const UInt32 truncated = kpv_ErrorFlags_UnexpectedEnd | kpv_ErrorFlags_HeadersError | kpv_ErrorFlags_IsNotArc;
    if (hasFlags && (innerErrors & truncated) != 0 && !cbSpec->missing.empty())
    {
      SetError(error, SZ_STATUS_MISSING_VOLUME, "the numbered parts end too early");
      FillMissing(error, cbSpec);
      inner->Close();
      archive->Close();
      delete a;
      return NULL;
    }
    cbSpec->missing.clear();
    a->container = archive;
    a->containerName = formatName;
    a->archive = inner;
    a->formatName = innerFormat;
  }
  else
  {
    a->archive = archive;
    a->formatName = formatName;
  }

  if (!cbSpec->missing.empty())
  {
    SetError(error, SZ_STATUS_MISSING_VOLUME, "missing volume");
    FillMissing(error, cbSpec);
    delete a;
    return NULL;
  }

  a->headersEncrypted = cbSpec->passwordAsked;
  a->volumePaths = cbSpec->openedPaths;
  a->formatUtf8 = WideToBytes(a->formatName.c_str(), a->formatName.size(), NULL);
  a->containerUtf8 = WideToBytes(a->containerName.c_str(), a->containerName.size(), NULL);
  ReadItems(a);
  if (IsCancelled(options->cancel))
  {
    SetError(error, SZ_STATUS_CANCELLED, "cancelled");
    delete a;
    return NULL;
  }
  return a;
}

extern "C" SZ_API void sz_archive_close(sz_archive *archive)
{
  delete archive;
}

extern "C" SZ_API void sz_archive_get_info(const sz_archive *a, sz_archive_info *info)
{
  if (!info)
    return;
  memset(info, 0, sizeof(*info));
  if (!a)
    return;
  info->format = a->formatUtf8.c_str();
  info->container_format = a->containerUtf8.c_str();
  info->item_count = (uint32_t)a->items.size();
  info->volume_count = (uint32_t)a->volumePaths.size();
  info->is_solid = GetArchiveBoolProp(a->archive, kpidSolid) ? 1 : 0;
  info->headers_encrypted = a->headersEncrypted ? 1 : 0;
  bool has = false;
  info->physical_size = GetArchiveU64Prop(a->archive, kpidPhySize, &has);
  info->error_flags = GetArchiveU32Prop(a->archive, kpidErrorFlags, &has);
  info->warning_flags = GetArchiveU32Prop(a->archive, kpidWarningFlags, &has);
}

extern "C" SZ_API const char *sz_archive_volume_path(const sz_archive *a, uint32_t index)
{
  if (!a || index >= a->volumePaths.size())
    return NULL;
  return a->volumePaths[index].c_str();
}

extern "C" SZ_API int32_t sz_archive_get_item(const sz_archive *a, uint32_t index, sz_item *item)
{
  if (!a || !item || index >= a->items.size())
    return 0;
  *item = a->items[index].info;
  return 1;
}

extern "C" SZ_API void sz_archive_set_password(sz_archive *a, const char *password)
{
  if (!a)
    return;
  Wipe(a->password);
  a->hasPassword = (password != NULL);
  if (password)
    a->password = Utf8ToWide(password);
}
