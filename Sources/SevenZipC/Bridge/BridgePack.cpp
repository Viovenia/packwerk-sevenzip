// BridgePack.cpp - writing new 7z and ZIP archives.
// Own file of the SevenZip package (GNU LGPL 2.1 or later), not part of 7-Zip.
// RAR archives are never written: the library contains no RAR encoder, and the
// unRAR licence forbids using the RAR decoder code to create one.

#include "BridgeInternal.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace szb {

Z7_CLASS_IMP_COM_2(CUpdateCallback, IArchiveUpdateCallback, ICryptoGetTextPassword2)
  Z7_IFACE_COM7_IMP(IProgress)
public:
  const sz_pack_item *items;
  UInt32 count;
  const sz_pack_options *opts;
  std::vector<std::wstring> paths;
  UInt64 total;
  int failedOsError;
  bool hasPassword;
  std::wstring password;
  CUpdateCallback(): items(NULL), count(0), opts(NULL), total(0), failedOsError(0), hasPassword(false) {}
  ~CUpdateCallback() { Wipe(password); }
};

static void SetBool(PROPVARIANT *v, bool b)
{
  v->vt = VT_BOOL;
  v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

static void SetU32(PROPVARIANT *v, UInt32 x)
{
  v->vt = VT_UI4;
  v->ulVal = x;
}

static void SetU64(PROPVARIANT *v, UInt64 x)
{
  v->vt = VT_UI8;
  v->uhVal.QuadPart = x;
}

static void SetFileTime(PROPVARIANT *v, Int64 seconds, Int32 nanoseconds)
{
  const Int64 kUnixOffset = (Int64)116444736 * 1000000000;
  const UInt64 ticks = (UInt64)(seconds * 10000000 + nanoseconds / 100 + kUnixOffset);
  v->vt = VT_FILETIME;
  v->filetime.dwLowDateTime = (DWORD)ticks;
  v->filetime.dwHighDateTime = (DWORD)(ticks >> 32);
}

Z7_COM7F_IMF(CUpdateCallback::SetTotal(UInt64 size))
{
  total = size;
  return IsCancelled(opts->cancel) ? E_ABORT : S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::SetCompleted(const UInt64 *completeValue))
{
  if (IsCancelled(opts->cancel))
    return E_ABORT;
  if (completeValue && opts->progress)
    opts->progress(opts->context, *completeValue, total);
  return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::GetUpdateItemInfo(UInt32, Int32 *newData, Int32 *newProps, UInt32 *indexInArchive))
{
  if (newData)
    *newData = 1;
  if (newProps)
    *newProps = 1;
  if (indexInArchive)
    *indexInArchive = (UInt32)(Int32)-1;
  return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  value->vt = VT_EMPTY;
  if (propID == kpidIsAnti)
  {
    SetBool(value, false);
    return S_OK;
  }
  if (index >= count)
    return E_INVALIDARG;
  const sz_pack_item &it = items[index];
  switch (propID)
  {
    case kpidPath:
      value->bstrVal = SysAllocString(paths[index].c_str());
      if (!value->bstrVal)
        return E_OUTOFMEMORY;
      value->vt = VT_BSTR;
      break;
    case kpidIsDir:
      SetBool(value, it.is_dir != 0);
      break;
    case kpidSize:
      SetU64(value, it.is_dir ? 0 : it.size);
      break;
    case kpidMTime:
      SetFileTime(value, it.mtime_seconds, it.mtime_nanoseconds);
      break;
    case kpidAttrib:
    {
      // Same mapping as 7-Zip on POSIX: Windows bits plus the POSIX mode in the high word.
      const UInt32 mode = it.posix_mode & 0xFFFF;
      UInt32 attrib = it.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
      if ((mode & S_IWUSR) == 0)
        attrib |= FILE_ATTRIBUTE_READONLY;
      SetU32(value, attrib | FILE_ATTRIBUTE_UNIX_EXTENSION | (mode << 16));
      break;
    }
    case kpidPosixAttrib:
      SetU32(value, it.posix_mode & 0xFFFF);
      break;
    default:
      break;
  }
  return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::GetStream(UInt32 index, ISequentialInStream **inStream))
{
  *inStream = NULL;
  if (IsCancelled(opts->cancel))
    return E_ABORT;
  if (index >= count)
    return E_INVALIDARG;
  const sz_pack_item &it = items[index];
  if (it.is_dir)
    return S_OK;
  CInFile *spec = new CInFile;
  CMyComPtr<IInStream> stream = spec;
  spec->cancel = opts->cancel;
  if (!it.source_path || !spec->Open(it.source_path))
  {
    failedOsError = it.source_path ? errno : EINVAL;
    if (failedOsError == 0)
      failedOsError = EIO;
    return E_FAIL;
  }
  *inStream = stream.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::SetOperationResult(Int32))
{
  return IsCancelled(opts->cancel) ? E_ABORT : S_OK;
}

Z7_COM7F_IMF(CUpdateCallback::CryptoGetTextPassword2(Int32 *passwordIsDefined, BSTR *pw))
{
  *passwordIsDefined = hasPassword ? 1 : 0;
  *pw = SysAllocString(hasPassword ? password.c_str() : L"");  // the library frees it
  return *pw ? S_OK : E_OUTOFMEMORY;
}

static HRESULT ApplyProperties(IOutArchive *out, const sz_pack_options *o, bool isZip)
{
  CMyComPtr<ISetProperties> setProps;
  out->QueryInterface(IID_ISetProperties, (void **)&setProps);
  if (!setProps)
    return E_NOTIMPL;

  const wchar_t *names[7];
  PROPVARIANT values[7];
  memset(values, 0, sizeof(values));
  UInt32 n = 0;

  names[n] = L"x";
  SetU32(&values[n++], o->level > 9 ? 9 : o->level);
  if (o->threads > 0)
  {
    names[n] = L"mt";
    SetU32(&values[n++], o->threads);
  }
  BSTR aes = NULL;
  if (isZip)
  {
    names[n] = L"cu";  // store names as UTF-8 and set the UTF-8 flag
    SetBool(&values[n++], true);
    if (o->password)
    {
      aes = SysAllocString(L"AES256");
      if (!aes)
        return E_OUTOFMEMORY;
      names[n] = L"em";
      values[n].vt = VT_BSTR;
      values[n++].bstrVal = aes;
    }
  }
  BSTR dictionary = NULL;
  if (!isZip)
  {
    if (o->password && o->encrypt_headers)
    {
      names[n] = L"he";
      SetBool(&values[n++], true);
    }
    if (o->dictionary_size > 0)
    {
      wchar_t text[32];
      swprintf(text, sizeof(text) / sizeof(text[0]), L"%llub", (unsigned long long)o->dictionary_size);
      dictionary = SysAllocString(text);
      if (!dictionary)
        return E_OUTOFMEMORY;
      names[n] = L"d";
      values[n].vt = VT_BSTR;
      values[n++].bstrVal = dictionary;
    }
  }
  const HRESULT hr = setProps->SetProperties(names, values, n);
  if (aes)
    SysFreeString(aes);
  if (dictionary)
    SysFreeString(dictionary);
  return hr;
}

} // namespace szb

using namespace szb;

extern "C" SZ_API sz_status sz_pack(const char *outputPath, const sz_pack_item *items, uint32_t itemCount,
                                    const sz_pack_options *options, sz_error *error)
{
  if (error)
    memset(error, 0, sizeof(*error));
  if (!outputPath || !options || (itemCount > 0 && !items))
  {
    SetError(error, SZ_STATUS_INVALID_ARGUMENT, "missing argument");
    return SZ_STATUS_INVALID_ARGUMENT;
  }
  const bool isZip = (options->format == SZ_PACK_FORMAT_ZIP);
  if (!isZip && options->format != SZ_PACK_FORMAT_7Z)
  {
    SetError(error, SZ_STATUS_INVALID_ARGUMENT, "unknown pack format");
    return SZ_STATUS_INVALID_ARGUMENT;
  }
  const Format *format = FindFormat(isZip ? L"zip" : L"7z");
  if (!format || !format->canUpdate)
  {
    SetError(error, SZ_STATUS_UNSUPPORTED, "format cannot be written");
    return SZ_STATUS_UNSUPPORTED;
  }
  for (uint32_t i = 0; i < itemCount; i++)
  {
    if (!items[i].archive_path || !items[i].archive_path[0])
    {
      SetError(error, SZ_STATUS_INVALID_ARGUMENT, "item without archive path");
      return SZ_STATUS_INVALID_ARGUMENT;
    }
  }

  CMyComPtr<IOutArchive> out;
  if (CreateObject(&format->clsid, &IID_IOutArchive, (void **)&out) != S_OK || !out)
  {
    SetError(error, SZ_STATUS_FAILED, "cannot create the archive writer");
    return SZ_STATUS_FAILED;
  }
  if (ApplyProperties(out, options, isZip) != S_OK)
  {
    SetError(error, SZ_STATUS_INVALID_ARGUMENT, "unsupported pack options");
    return SZ_STATUS_INVALID_ARGUMENT;
  }

  CUpdateCallback *cbSpec = new CUpdateCallback;
  CMyComPtr<IArchiveUpdateCallback> callback = cbSpec;
  cbSpec->items = items;
  cbSpec->count = itemCount;
  cbSpec->opts = options;
  cbSpec->hasPassword = (options->password != NULL);
  if (options->password)
    cbSpec->password = Utf8ToWide(options->password);
  cbSpec->paths.reserve(itemCount);
  for (uint32_t i = 0; i < itemCount; i++)
    cbSpec->paths.push_back(Utf8ToWide(items[i].archive_path));

  COutFile *outSpec = new COutFile;
  CMyComPtr<IOutStream> outStream = outSpec;
  outSpec->cancel = options->cancel;
  if (!outSpec->Create(outputPath, true))
  {
    SetError(error, SZ_STATUS_IO, "cannot create the output file", outSpec->lastError);
    return SZ_STATUS_IO;
  }

  const HRESULT hr = out->UpdateItems(outStream, itemCount, callback);
  const bool closed = outSpec->Close();
  if (hr == S_OK && closed && !IsCancelled(options->cancel))
    return SZ_STATUS_OK;

  unlink(outputPath);
  sz_status st;
  int osError = 0;
  if (IsCancelled(options->cancel))
    st = SZ_STATUS_CANCELLED;
  else if (cbSpec->failedOsError != 0)
  {
    st = SZ_STATUS_IO;
    osError = cbSpec->failedOsError;
  }
  else if (outSpec->lastError != 0)
  {
    st = SZ_STATUS_IO;
    osError = outSpec->lastError;
  }
  else if (hr == E_OUTOFMEMORY)
    st = SZ_STATUS_MEMORY_LIMIT;
  else
    st = SZ_STATUS_FAILED;
  SetError(error, st, cbSpec->failedOsError != 0 ? "cannot read a source file" : "packing failed", osError);
  return st;
}
