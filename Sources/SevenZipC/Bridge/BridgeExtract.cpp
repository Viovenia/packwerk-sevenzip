// BridgeExtract.cpp - extraction with per-item targets, progress and cancellation.
// Own file of the SevenZip package (GNU LGPL 2.1 or later), not part of 7-Zip.

#include "BridgeInternal.h"

#include <algorithm>
#include <errno.h>
#include <limits.h>
#include <string.h>

namespace szb {

Z7_CLASS_IMP_COM_3(CExtractCallback, IArchiveExtractCallback, ICryptoGetTextPassword,
                   IArchiveRequestMemoryUseCallback)
  Z7_IFACE_COM7_IMP(IProgress)
public:
  sz_archive *arc;
  const sz_extract_options *opts;
  UInt64 total;
  bool passwordMissing;
  bool memoryExceeded;
  UInt64 requiredMemory;
  int fatalOsError;

  // current item
  bool active;
  UInt32 index;
  int32_t kind;
  int openError;
  COutFile *fileSpec;
  CMemOut *memSpec;
  CMyComPtr<ISequentialOutStream> stream;

  CExtractCallback(): arc(NULL), opts(NULL), total(0), passwordMissing(false), memoryExceeded(false),
      requiredMemory(0), fatalOsError(0), active(false), index(0), kind(SZ_TARGET_SKIP), openError(0),
      fileSpec(NULL), memSpec(NULL) {}

  void Finish(int32_t result);
};

void CExtractCallback::Finish(int32_t result)
{
  if (!active)
    return;
  int osError = 0;
  if (fileSpec)
  {
    const int writeError = fileSpec->lastError;
    if (!fileSpec->Close() && result == SZ_ITEM_OK)
      result = SZ_ITEM_WRITE_ERROR;
    if (writeError != 0 || fileSpec->lastError != 0)
    {
      osError = writeError != 0 ? writeError : fileSpec->lastError;
      if (result == SZ_ITEM_OK || result == SZ_ITEM_DATA_ERROR)
        result = SZ_ITEM_WRITE_ERROR;
      fatalOsError = osError;
    }
  }
  if (openError != 0)
  {
    result = SZ_ITEM_WRITE_ERROR;
    osError = openError;
  }
  const uint8_t *data = NULL;
  size_t length = 0;
  if (memSpec)
  {
    data = (const uint8_t *)memSpec->data.data();
    length = memSpec->data.size();
  }
  if (opts->done)
    opts->done(opts->context, index, result, osError, data, length);
  stream.Release();
  fileSpec = NULL;
  memSpec = NULL;
  active = false;
  openError = 0;
}

Z7_COM7F_IMF(CExtractCallback::SetTotal(UInt64 size))
{
  total = size;
  return IsCancelled(opts->cancel) ? E_ABORT : S_OK;
}

Z7_COM7F_IMF(CExtractCallback::SetCompleted(const UInt64 *completeValue))
{
  if (IsCancelled(opts->cancel))
    return E_ABORT;
  if (completeValue && opts->progress)
    opts->progress(opts->context, *completeValue, total);
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::GetStream(UInt32 itemIndex, ISequentialOutStream **outStream, Int32 askExtractMode))
{
  *outStream = NULL;
  Finish(SZ_ITEM_DATA_ERROR);  // an item without SetOperationResult did not finish
  if (IsCancelled(opts->cancel))
    return E_ABORT;
  if (askExtractMode != NArchive::NExtract::NAskMode::kExtract)
    return S_OK;

  char path[PATH_MAX + 1];
  path[0] = 0;
  kind = opts->target ? opts->target(opts->context, itemIndex, path, sizeof(path)) : SZ_TARGET_SKIP;
  index = itemIndex;
  switch (kind)
  {
    case SZ_TARGET_DIRECTORY:
      active = true;
      return S_OK;
    case SZ_TARGET_FILE:
    {
      active = true;
      fileSpec = new COutFile;
      stream = fileSpec;
      fileSpec->cancel = opts->cancel;
      if (!path[0] || !fileSpec->Create(path, true))
      {
        openError = path[0] ? fileSpec->lastError : EINVAL;
        if (openError == 0)
          openError = EIO;
        stream.Release();
        fileSpec = NULL;
        return S_OK;  // the handler skips the data; Finish reports the error
      }
      *outStream = stream;
      (*outStream)->AddRef();
      return S_OK;
    }
    case SZ_TARGET_MEMORY:
    {
      active = true;
      memSpec = new CMemOut;
      stream = memSpec;
      *outStream = stream;
      (*outStream)->AddRef();
      return S_OK;
    }
    default:
      active = false;
      return S_OK;
  }
}

Z7_COM7F_IMF(CExtractCallback::PrepareOperation(Int32))
{
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::SetOperationResult(Int32 opRes))
{
  Finish(opRes);
  return IsCancelled(opts->cancel) ? E_ABORT : S_OK;
}

Z7_COM7F_IMF(CExtractCallback::CryptoGetTextPassword(BSTR *pw))
{
  if (!arc->hasPassword)
  {
    passwordMissing = true;
    return E_ABORT;
  }
  *pw = SysAllocString(arc->password.c_str());  // the library frees it
  return *pw ? S_OK : E_OUTOFMEMORY;
}

Z7_COM7F_IMF(CExtractCallback::RequestMemoryUse(UInt32 flags, UInt32, UInt32, const wchar_t *,
                                                  UInt64 requiredSize, UInt64 *allowedSize, UInt32 *answerFlags))
{
  if (flags & NRequestMemoryUseFlags::k_IsReport)
    return S_OK;
  const UInt64 limit = arc->memoryLimit;
  if (limit != 0 && requiredSize > limit)
  {
    memoryExceeded = true;
    requiredMemory = requiredSize;
    *allowedSize = limit;
    *answerFlags = NRequestMemoryAnswerFlags::k_Limit_Exceeded | NRequestMemoryAnswerFlags::k_SkipArc;
    return S_OK;
  }
  if (*allowedSize < requiredSize)
    *allowedSize = requiredSize;
  *answerFlags = NRequestMemoryAnswerFlags::k_Allow;
  return S_OK;
}

} // namespace szb

using namespace szb;

/// RAR5 asks for permission (IArchiveRequestMemoryUseCallback) only above its own threshold of
/// 1 GiB unless "memx" is set; with it, the configured limit applies to every dictionary size.
/// SetProperties resets the handler's settings, so the thread count is passed again.
static void ApplyDecoderLimits(sz_archive *a)
{
  if (a->memoryLimit == 0 || wcscasecmp(a->formatName.c_str(), L"Rar5") != 0)
    return;
  CMyComPtr<ISetProperties> setProps;
  a->archive->QueryInterface(IID_ISetProperties, (void **)&setProps);
  if (!setProps)
    return;
  const wchar_t *names[2] = { L"memx", L"mt" };
  PROPVARIANT values[2];
  memset(values, 0, sizeof(values));
  values[0].vt = VT_UI8;
  values[0].uhVal.QuadPart = a->memoryLimit;
  values[1].vt = VT_UI4;
  values[1].ulVal = a->threads == 0 ? 1 : a->threads;
  setProps->SetProperties(names, values, 2);
}

extern "C" SZ_API sz_status sz_archive_extract(sz_archive *a, const sz_extract_options *options, sz_error *error)
{
  if (error)
    memset(error, 0, sizeof(*error));
  if (!a || !options || !options->target)
  {
    SetError(error, SZ_STATUS_INVALID_ARGUMENT, "missing archive or target callback");
    return SZ_STATUS_INVALID_ARGUMENT;
  }
  std::vector<UInt32> indices;
  if (options->indices && options->index_count > 0)
  {
    indices.assign(options->indices, options->indices + options->index_count);
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    while (!indices.empty() && indices.back() >= a->items.size())
      indices.pop_back();
    if (indices.empty())
      return SZ_STATUS_OK;
  }

  ApplyDecoderLimits(a);

  CExtractCallback *spec = new CExtractCallback;
  CMyComPtr<IArchiveExtractCallback> callback = spec;
  spec->arc = a;
  spec->opts = options;

  const HRESULT hr = indices.empty()
      ? a->archive->Extract(NULL, (UInt32)(Int32)-1, 0, callback)
      : a->archive->Extract(&indices[0], (UInt32)indices.size(), 0, callback);

  if (hr == S_OK && !IsCancelled(options->cancel))
  {
    spec->Finish(SZ_ITEM_DATA_ERROR);
    return SZ_STATUS_OK;
  }

  // Report the item that was interrupted so that the caller can remove it.
  spec->Finish(SZ_ITEM_DATA_ERROR);

  sz_status st;
  if (IsCancelled(options->cancel))
    st = SZ_STATUS_CANCELLED;
  else if (spec->passwordMissing)
    st = SZ_STATUS_PASSWORD_REQUIRED;
  else if (spec->memoryExceeded)
    st = SZ_STATUS_MEMORY_LIMIT;
  else if (spec->fatalOsError != 0)
    st = SZ_STATUS_IO;
  else if (hr == E_OUTOFMEMORY)
    st = SZ_STATUS_MEMORY_LIMIT;
  else
    st = SZ_STATUS_CORRUPT;
  SetError(error, st, "extraction stopped", spec->fatalOsError);
  if (error && st == SZ_STATUS_MEMORY_LIMIT)
    error->required_memory = spec->requiredMemory;
  return st;
}
