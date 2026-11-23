// BridgeCommon.cpp - strings, errors, cancel token, formats, streams, properties.
// Own file of the SevenZip package (GNU LGPL 2.1 or later), not part of 7-Zip.

#include "BridgeInternal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wctype.h>

namespace szb {

// ---------- strings ----------

std::wstring Utf8ToWide(const char *s)
{
  std::wstring r;
  if (!s)
    return r;
  const unsigned char *p = (const unsigned char *)s;
  while (*p)
  {
    unsigned c = *p;
    unsigned need = 0;
    unsigned v = 0;
    if (c < 0x80) { r += (wchar_t)c; p++; continue; }
    else if (c >= 0xC2 && c <= 0xDF) { need = 1; v = c & 0x1F; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; v = c & 0x0F; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; v = c & 0x07; }
    else { r += (wchar_t)(0xEF00 + c); p++; continue; }  // invalid lead byte: 7-Zip escape
    unsigned i = 1;
    for (; i <= need; i++)
    {
      if ((p[i] & 0xC0) != 0x80)
        break;
      v = (v << 6) | (p[i] & 0x3F);
    }
    if (i <= need)
    {
      r += (wchar_t)(0xEF00 + c);
      p++;
      continue;
    }
    if (v >= 0x10000)
    {
      // 7-Zip keeps UTF-16 in wchar_t on every platform: the 7z writer stores each wchar_t
      // as one UTF-16 unit, and its UTF-8 conversion joins surrogate pairs again.
      v -= 0x10000;
      r += (wchar_t)(0xD800 + (v >> 10));
      r += (wchar_t)(0xDC00 + (v & 0x3FF));
    }
    else
      r += (wchar_t)v;
    p += need + 1;
  }
  return r;
}

static void AppendUtf8(std::string &r, unsigned v)
{
  if (v < 0x80)
    r += (char)v;
  else if (v < 0x800)
  {
    r += (char)(0xC0 | (v >> 6));
    r += (char)(0x80 | (v & 0x3F));
  }
  else if (v < 0x10000)
  {
    r += (char)(0xE0 | (v >> 12));
    r += (char)(0x80 | ((v >> 6) & 0x3F));
    r += (char)(0x80 | (v & 0x3F));
  }
  else
  {
    r += (char)(0xF0 | (v >> 18));
    r += (char)(0x80 | ((v >> 12) & 0x3F));
    r += (char)(0x80 | ((v >> 6) & 0x3F));
    r += (char)(0x80 | (v & 0x3F));
  }
}

std::string WideToBytes(const wchar_t *w, size_t length, bool *hadRawBytes)
{
  std::string r;
  bool raw = false;
  if (w)
  {
    for (size_t i = 0; i < length; i++)
    {
      unsigned v = (unsigned)w[i];
      if (v >= 0xD800 && v <= 0xDBFF && i + 1 < length)
      {
        unsigned lo = (unsigned)w[i + 1];
        if (lo >= 0xDC00 && lo <= 0xDFFF)
        {
          v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
          i++;
        }
      }
      if (v >= 0xEF80 && v <= 0xEFFF)
      {
        r += (char)(v - 0xEF00);
        raw = true;
        continue;
      }
      if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
        v = 0xFFFD;
      AppendUtf8(r, v);
    }
  }
  if (hadRawBytes)
    *hadRawBytes = raw;
  return r;
}

bool IsValidUtf8(const std::string &bytes)
{
  const unsigned char *p = (const unsigned char *)bytes.data();
  const size_t n = bytes.size();
  size_t i = 0;
  while (i < n)
  {
    unsigned c = p[i];
    if (c < 0x80) { i++; continue; }
    unsigned need;
    unsigned min;
    if (c >= 0xC2 && c <= 0xDF) { need = 1; min = 0x80; }
    else if (c >= 0xE0 && c <= 0xEF) { need = 2; min = 0x800; }
    else if (c >= 0xF0 && c <= 0xF4) { need = 3; min = 0x10000; }
    else return false;
    if (i + need >= n)  // continuation bytes p[i+1] ... p[i+need] must exist
      return false;
    unsigned v = c & (0x3F >> need);
    for (unsigned k = 1; k <= need; k++)
    {
      unsigned t = p[i + k];
      if ((t & 0xC0) != 0x80)
        return false;
      v = (v << 6) | (t & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
      return false;
    i += need + 1;
  }
  return true;
}

void Wipe(std::wstring &s)
{
  volatile wchar_t *p = &s[0];
  for (size_t i = 0; i < s.size(); i++)
    p[i] = 0;
  s.clear();
}

void CopyToBuffer(char *buffer, size_t bufferSize, const std::string &s)
{
  if (!buffer || bufferSize == 0)
    return;
  size_t n = s.size();
  if (n >= bufferSize)
    n = bufferSize - 1;
  memcpy(buffer, s.data(), n);
  buffer[n] = 0;
}

void SetError(sz_error *error, sz_status status, const char *message, int osError)
{
  if (!error)
    return;
  error->status = status;
  error->os_error = osError;
  CopyToBuffer(error->message, sizeof(error->message), message ? std::string(message) : std::string());
}

bool IsCancelled(const sz_cancel_token *token)
{
  return token && token->cancelled.load(std::memory_order_relaxed) != 0;
}

// ---------- formats ----------

static std::vector<Format> LoadFormats()
{
  std::vector<Format> r;
  UInt32 n = 0;
  if (GetNumberOfFormats(&n) != S_OK)
    return r;
  for (UInt32 i = 0; i < n; i++)
  {
    Format f;
    memset(&f.clsid, 0, sizeof(f.clsid));
    f.canUpdate = false;
    f.flags = 0;
    PROPVARIANT p;
    memset(&p, 0, sizeof(p));
    if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kName, &p) == S_OK && p.vt == VT_BSTR && p.bstrVal)
      f.name = p.bstrVal;
    VariantClear((VARIANTARG *)&p);
    if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kExtension, &p) == S_OK && p.vt == VT_BSTR && p.bstrVal)
      f.extensions = p.bstrVal;
    VariantClear((VARIANTARG *)&p);
    for (size_t k = 0; k < f.extensions.size(); k++)
      f.extensions[k] = (wchar_t)towlower((wint_t)f.extensions[k]);
    if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kClassID, &p) == S_OK && p.vt == VT_BSTR && p.bstrVal &&
        SysStringByteLen(p.bstrVal) == sizeof(GUID))
      memcpy(&f.clsid, p.bstrVal, sizeof(GUID));
    VariantClear((VARIANTARG *)&p);
    if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kUpdate, &p) == S_OK && p.vt == VT_BOOL)
      f.canUpdate = (p.boolVal != VARIANT_FALSE);
    VariantClear((VARIANTARG *)&p);
    if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kFlags, &p) == S_OK && p.vt == VT_UI4)
      f.flags = p.ulVal;
    VariantClear((VARIANTARG *)&p);
    r.push_back(f);
  }
  return r;
}

const std::vector<Format> &Formats()
{
  // Thread-safe initialisation (C++11 magic statics). The bridge always wants
  // lossless names, so g_ForceToUTF8 is set before the first use.
  static const std::vector<Format> formats = (g_ForceToUTF8 = true, LoadFormats());
  return formats;
}

const Format *FindFormat(const wchar_t *name)
{
  const std::vector<Format> &f = Formats();
  for (size_t i = 0; i < f.size(); i++)
    if (wcscasecmp(f[i].name.c_str(), name) == 0)
      return &f[i];
  return NULL;
}

// ---------- streams ----------

CInFile::~CInFile()
{
  if (fd >= 0)
    close(fd);
}

bool CInFile::Open(const char *path)
{
  fd = open(path, O_RDONLY | O_CLOEXEC);
  return fd >= 0;
}

Z7_COM7F_IMF(CInFile::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (IsCancelled(cancel))
    return E_ABORT;
  for (;;)
  {
    ssize_t n = read(fd, data, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      return E_FAIL;
    if (processedSize)
      *processedSize = (UInt32)n;
    return S_OK;
  }
}

Z7_COM7F_IMF(CInFile::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  int whence = (seekOrigin == STREAM_SEEK_SET) ? SEEK_SET : (seekOrigin == STREAM_SEEK_CUR) ? SEEK_CUR : SEEK_END;
  off_t r = lseek(fd, (off_t)offset, whence);
  if (r < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  if (newPosition)
    *newPosition = (UInt64)r;
  return S_OK;
}

COutFile::~COutFile()
{
  Close();
}

bool COutFile::Create(const char *path, bool truncateExisting)
{
  int flags = O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW;
  if (truncateExisting)
    flags |= O_TRUNC;
  fd = open(path, flags, 0644);
  if (fd < 0)
  {
    lastError = errno;
    return false;
  }
  return true;
}

bool COutFile::Close()
{
  if (fd < 0)
    return true;
  int r = close(fd);
  fd = -1;
  if (r != 0)
  {
    lastError = errno;
    return false;
  }
  return true;
}

Z7_COM7F_IMF(COutFile::Write(const void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (IsCancelled(cancel))
    return E_ABORT;
  const char *p = (const char *)data;
  UInt32 done = 0;
  while (done < size)
  {
    ssize_t n = write(fd, p + done, size - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
    {
      lastError = (n < 0) ? errno : ENOSPC;
      if (processedSize)
        *processedSize = done;
      return E_FAIL;
    }
    done += (UInt32)n;
  }
  written += done;
  if (processedSize)
    *processedSize = done;
  return S_OK;
}

Z7_COM7F_IMF(COutFile::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  int whence = (seekOrigin == STREAM_SEEK_SET) ? SEEK_SET : (seekOrigin == STREAM_SEEK_CUR) ? SEEK_CUR : SEEK_END;
  off_t r = lseek(fd, (off_t)offset, whence);
  if (r < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  if (newPosition)
    *newPosition = (UInt64)r;
  return S_OK;
}

Z7_COM7F_IMF(COutFile::SetSize(UInt64 newSize))
{
  if (ftruncate(fd, (off_t)newSize) != 0)
  {
    lastError = errno;
    return E_FAIL;
  }
  return S_OK;
}

Z7_COM7F_IMF(CMemOut::Write(const void *bytes, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = size;
  size_t room = (data.size() < limit) ? limit - data.size() : 0;
  if (size > room)
  {
    overflow = true;
    data.append((const char *)bytes, room);
  }
  else
    data.append((const char *)bytes, size);
  return S_OK;
}

// ---------- properties ----------

std::wstring GetStringProp(IInArchive *archive, UInt32 index, PROPID id, bool *present)
{
  PROPVARIANT p;
  memset(&p, 0, sizeof(p));
  std::wstring r;
  bool ok = false;
  if (archive->GetProperty(index, id, &p) == S_OK && p.vt == VT_BSTR && p.bstrVal)
  {
    r.assign(p.bstrVal, SysStringLen(p.bstrVal));
    ok = true;
  }
  VariantClear((VARIANTARG *)&p);
  if (present)
    *present = ok;
  return r;
}

bool GetBoolProp(IInArchive *archive, UInt32 index, PROPID id, bool *present)
{
  PROPVARIANT p;
  memset(&p, 0, sizeof(p));
  bool r = false;
  bool ok = false;
  if (archive->GetProperty(index, id, &p) == S_OK && p.vt == VT_BOOL)
  {
    r = (p.boolVal != VARIANT_FALSE);
    ok = true;
  }
  VariantClear((VARIANTARG *)&p);
  if (present)
    *present = ok;
  return r;
}

static bool ReadNumber(const PROPVARIANT &p, UInt64 *value)
{
  switch (p.vt)
  {
    case VT_UI8: *value = p.uhVal.QuadPart; return true;
    case VT_UI4: *value = p.ulVal; return true;
    case VT_UI2: *value = p.uiVal; return true;
    case VT_UI1: *value = p.bVal; return true;
    case VT_I8: *value = (UInt64)p.hVal.QuadPart; return true;
    case VT_I4: *value = (UInt64)(Int64)p.lVal; return true;
    default: return false;
  }
}

UInt64 GetU64Prop(IInArchive *archive, UInt32 index, PROPID id, bool *present)
{
  PROPVARIANT p;
  memset(&p, 0, sizeof(p));
  UInt64 r = 0;
  bool ok = (archive->GetProperty(index, id, &p) == S_OK) && ReadNumber(p, &r);
  VariantClear((VARIANTARG *)&p);
  if (present)
    *present = ok;
  return ok ? r : 0;
}

UInt32 GetU32Prop(IInArchive *archive, UInt32 index, PROPID id, bool *present)
{
  return (UInt32)GetU64Prop(archive, index, id, present);
}

bool GetTimeProp(IInArchive *archive, UInt32 index, PROPID id, Int64 *seconds, Int32 *nanoseconds)
{
  PROPVARIANT p;
  memset(&p, 0, sizeof(p));
  bool ok = false;
  if (archive->GetProperty(index, id, &p) == S_OK && p.vt == VT_FILETIME)
  {
    const UInt64 ticks = ((UInt64)p.filetime.dwHighDateTime << 32) | p.filetime.dwLowDateTime;
    if (ticks != 0)
    {
      // FILETIME: 100 ns since 1601-01-01. Unix epoch offset in ticks:
      const Int64 kUnixOffset = (Int64)116444736 * 1000000000;
      const Int64 t = (Int64)ticks - kUnixOffset;
      Int64 s = t / 10000000;
      Int64 rem = t % 10000000;
      if (rem < 0)
      {
        rem += 10000000;
        s -= 1;
      }
      *seconds = s;
      *nanoseconds = (Int32)(rem * 100);
      ok = true;
    }
  }
  VariantClear((VARIANTARG *)&p);
  return ok;
}

UInt64 GetArchiveU64Prop(IInArchive *archive, PROPID id, bool *present)
{
  PROPVARIANT p;
  memset(&p, 0, sizeof(p));
  UInt64 r = 0;
  bool ok = (archive->GetArchiveProperty(id, &p) == S_OK) && ReadNumber(p, &r);
  VariantClear((VARIANTARG *)&p);
  if (present)
    *present = ok;
  return ok ? r : 0;
}

UInt32 GetArchiveU32Prop(IInArchive *archive, PROPID id, bool *present)
{
  return (UInt32)GetArchiveU64Prop(archive, id, present);
}

bool GetArchiveBoolProp(IInArchive *archive, PROPID id)
{
  PROPVARIANT p;
  memset(&p, 0, sizeof(p));
  bool r = false;
  if (archive->GetArchiveProperty(id, &p) == S_OK && p.vt == VT_BOOL)
    r = (p.boolVal != VARIANT_FALSE);
  VariantClear((VARIANTARG *)&p);
  return r;
}

void SetThreads(IUnknown *handler, UInt32 threads)
{
  if (threads == 0 || !handler)
    return;
  CMyComPtr<ISetProperties> setProps;
  handler->QueryInterface(IID_ISetProperties, (void **)&setProps);
  if (!setProps)
    return;
  const wchar_t *names[1] = { L"mt" };
  PROPVARIANT v;
  memset(&v, 0, sizeof(v));
  v.vt = VT_UI4;
  v.ulVal = threads;
  setProps->SetProperties(names, &v, 1);
}

} // namespace szb

// ---------- exported: library and cancel token ----------

extern "C" SZ_API const char *sz_version(void)
{
  return "26.06";
}

extern "C" SZ_API int32_t sz_format_count(void)
{
  return (int32_t)szb::Formats().size();
}

extern "C" SZ_API int32_t sz_format_name(int32_t index, char *buffer, int32_t bufferSize)
{
  const std::vector<szb::Format> &f = szb::Formats();
  if (index < 0 || (size_t)index >= f.size() || !buffer || bufferSize <= 0)
    return -1;
  const std::string s = szb::WideToBytes(f[(size_t)index].name.c_str(), f[(size_t)index].name.size(), NULL);
  szb::CopyToBuffer(buffer, (size_t)bufferSize, s);
  return (int32_t)strlen(buffer);
}

extern "C" SZ_API sz_cancel_token *sz_cancel_token_create(void)
{
  return new sz_cancel_token();
}

extern "C" SZ_API void sz_cancel_token_cancel(sz_cancel_token *token)
{
  if (token)
    token->cancelled.store(1, std::memory_order_relaxed);
}

extern "C" SZ_API int32_t sz_cancel_token_is_cancelled(const sz_cancel_token *token)
{
  return szb::IsCancelled(token) ? 1 : 0;
}

extern "C" SZ_API void sz_cancel_token_destroy(sz_cancel_token *token)
{
  delete token;
}
