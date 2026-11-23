/*
 * SevenZipBridge.h - small C interface to the 7-Zip format library.
 *
 * Part of the SevenZip package (GNU LGPL 2.1 or later, see LICENSE and NOTICE.md).
 * Own file, not part of the original 7-Zip 26.06 sources.
 *
 * Only the functions declared here are exported from the dynamic library.
 * All strings are UTF-8. All paths are absolute POSIX paths.
 * Every function is synchronous and may block for a long time (I/O, decoding);
 * callers must not call them on a UI thread.
 *
 * Callbacks are invoked on the calling thread. A handle (sz_archive) must not be
 * used from two threads at the same time.
 */

#ifndef SEVENZIP_BRIDGE_H
#define SEVENZIP_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define SZ_API __attribute__((visibility("default")))
#else
#define SZ_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Status codes                                                              */
/* ------------------------------------------------------------------------- */

typedef int32_t sz_status;

#define SZ_STATUS_OK 0
/** No format handler could open the input. */
#define SZ_STATUS_UNSUPPORTED 1
/** The archive needs a password and none was given. */
#define SZ_STATUS_PASSWORD_REQUIRED 2
/** A password was given and the handler rejected it (encrypted headers). */
#define SZ_STATUS_WRONG_PASSWORD 3
/** A volume was requested and could not be resolved; see sz_error.volume_name. */
#define SZ_STATUS_MISSING_VOLUME 4
/** The operation was cancelled through the cancel token. */
#define SZ_STATUS_CANCELLED 5
/** Reading or writing a file failed; see sz_error.os_error. */
#define SZ_STATUS_IO 6
/** The archive is damaged beyond what the handler can read. */
#define SZ_STATUS_CORRUPT 7
/** A decoder needs more memory than allowed; see sz_error.required_memory. */
#define SZ_STATUS_MEMORY_LIMIT 8
/** An argument was invalid (NULL pointer, unknown format, ...). */
#define SZ_STATUS_INVALID_ARGUMENT 9
/** Any other failure; see sz_error.message. */
#define SZ_STATUS_FAILED 10

typedef struct sz_error {
  sz_status status;
  /** errno of the failing system call, 0 if unknown. */
  int32_t os_error;
  /** Required bytes for SZ_STATUS_MEMORY_LIMIT. */
  uint64_t required_memory;
  /** File name of the first missing volume for SZ_STATUS_MISSING_VOLUME (UTF-8). */
  char volume_name[1024];
  /** Short English description for logs. Never contains a password. */
  char message[256];
} sz_error;

/* ------------------------------------------------------------------------- */
/* Library                                                                   */
/* ------------------------------------------------------------------------- */

/** 7-Zip version of the bundled sources, for example "26.06". */
SZ_API const char *sz_version(void);

/** Number of archive format handlers in the library. */
SZ_API int32_t sz_format_count(void);

/** Writes the name of handler `index` into `buffer`. Returns the length or -1. */
SZ_API int32_t sz_format_name(int32_t index, char *buffer, int32_t buffer_size);

/* ------------------------------------------------------------------------- */
/* Cancellation                                                              */
/* ------------------------------------------------------------------------- */

typedef struct sz_cancel_token sz_cancel_token;

SZ_API sz_cancel_token *sz_cancel_token_create(void);
/** Thread-safe. Running operations stop at the next callback or write. */
SZ_API void sz_cancel_token_cancel(sz_cancel_token *token);
SZ_API int32_t sz_cancel_token_is_cancelled(const sz_cancel_token *token);
SZ_API void sz_cancel_token_destroy(sz_cancel_token *token);

/* ------------------------------------------------------------------------- */
/* Opening and listing                                                       */
/* ------------------------------------------------------------------------- */

/**
 * Resolves the volume `name` (file name requested by the handler, UTF-8).
 * Writes the absolute path into `path_buffer` and returns 1, or returns 0 if
 * the volume does not exist.
 */
typedef int32_t (*sz_volume_resolver)(void *context, const char *name, char *path_buffer, size_t path_buffer_size);

/** Byte progress. `total` is 0 if unknown. */
typedef void (*sz_progress_fn)(void *context, uint64_t completed, uint64_t total);

typedef struct sz_open_options {
  /** Path of the archive; for multi-volume archives the first volume. */
  const char *path;
  /** Password or NULL. Handed to the library with its own string allocator. */
  const char *password;
  /** Decoder threads (7-Zip "mt"); 0 keeps the library default (all cores). */
  uint32_t threads;
  /** Largest dictionary a decoder may request in bytes; 0 means no limit. */
  uint64_t memory_limit;
  /** Volume lookup; NULL looks next to `path`. */
  sz_volume_resolver resolve_volume;
  void *context;
  /** May be NULL. */
  sz_cancel_token *cancel;
} sz_open_options;

typedef struct sz_archive sz_archive;

/** Opens an archive. Returns NULL and fills `error` on failure. */
SZ_API sz_archive *sz_archive_open(const sz_open_options *options, sz_error *error);
SZ_API void sz_archive_close(sz_archive *archive);

typedef struct sz_archive_info {
  /** Handler name of the archive that holds the items, e.g. "zip", "Rar5", "7z", "gzip". */
  const char *format;
  /** "Split" when opened through numbered parts (.001), otherwise "". */
  const char *container_format;
  uint32_t item_count;
  /** Number of volumes that were opened, including the first one. */
  uint32_t volume_count;
  int32_t is_solid;
  /** The handler asked for a password while reading the item list. */
  int32_t headers_encrypted;
  /** Physical size reported by the handler, 0 if unknown. */
  uint64_t physical_size;
  /** 7-Zip kpv_ErrorFlags_* of the archive (e.g. unexpected end). */
  uint32_t error_flags;
  uint32_t warning_flags;
} sz_archive_info;

SZ_API void sz_archive_get_info(const sz_archive *archive, sz_archive_info *info);

/** Path of opened volume `index` (0 = first), or NULL. */
SZ_API const char *sz_archive_volume_path(const sz_archive *archive, uint32_t index);

typedef struct sz_item {
  uint32_t index;
  /**
   * Path as stored, '/'-separated, NUL-terminated. Unicode formats give UTF-8.
   * Legacy names (ZIP without UTF-8 flag, old RAR, tar) give the raw bytes,
   * which may not be valid UTF-8. Surrogate pairs are joined.
   */
  const uint8_t *path;
  uint32_t path_length;
  /** 1 if `path` is valid UTF-8. */
  int32_t path_is_valid_utf8;
  /** 1 if the archive declares the name as UTF-8 (ZIP general purpose bit 11). */
  int32_t path_utf8_flag;
  /** Link target stored as metadata (tar, RAR5, ...), UTF-8, or NULL. */
  const char *link_target;
  uint64_t size;
  uint64_t packed_size;
  int32_t has_packed_size;
  int64_t mtime_seconds;
  int32_t mtime_nanoseconds;
  int32_t has_mtime;
  uint32_t attributes;
  int32_t has_attributes;
  uint32_t posix_mode;
  int32_t has_posix_mode;
  uint32_t crc;
  int32_t has_crc;
  int32_t is_dir;
  int32_t is_encrypted;
  /** 1 if the attributes or metadata mark the item as a symbolic link. */
  int32_t is_symlink;
  /** 1 for NTFS alternate streams and auxiliary/deleted items of disk images. */
  int32_t is_auxiliary;
} sz_item;

/** Fills `item`. Pointers stay valid until sz_archive_close. Returns 0 if `index` is out of range. */
SZ_API int32_t sz_archive_get_item(const sz_archive *archive, uint32_t index, sz_item *item);

/** Replaces the password used for extraction (NULL removes it). */
SZ_API void sz_archive_set_password(sz_archive *archive, const char *password);

/* ------------------------------------------------------------------------- */
/* Extraction                                                                */
/* ------------------------------------------------------------------------- */

/** Return values of sz_extract_target_fn. */
#define SZ_TARGET_SKIP 0
#define SZ_TARGET_FILE 1
#define SZ_TARGET_DIRECTORY 2
/** Item data is collected in memory (at most 1 MiB) and passed to sz_extract_done_fn. */
#define SZ_TARGET_MEMORY 3

/** Per-item results (7-Zip NExtract::NOperationResult plus own codes). */
#define SZ_ITEM_OK 0
#define SZ_ITEM_UNSUPPORTED_METHOD 1
#define SZ_ITEM_DATA_ERROR 2
#define SZ_ITEM_CRC_ERROR 3
#define SZ_ITEM_UNAVAILABLE 4
#define SZ_ITEM_UNEXPECTED_END 5
#define SZ_ITEM_DATA_AFTER_END 6
#define SZ_ITEM_IS_NOT_ARCHIVE 7
#define SZ_ITEM_HEADERS_ERROR 8
#define SZ_ITEM_WRONG_PASSWORD 9
/** The destination file could not be created or written. */
#define SZ_ITEM_WRITE_ERROR 100

/**
 * Decides where item `index` goes. For SZ_TARGET_FILE write the absolute path
 * into `path_buffer`; parent directories must exist. The file is created with
 * O_NOFOLLOW and truncated.
 */
typedef int32_t (*sz_extract_target_fn)(void *context, uint32_t index, char *path_buffer, size_t path_buffer_size);

/**
 * Called once per item that was not skipped. `result` is SZ_ITEM_*.
 * `data`/`length` hold the item bytes for SZ_TARGET_MEMORY, otherwise NULL/0.
 * `os_error` is the errno for SZ_ITEM_WRITE_ERROR.
 */
typedef void (*sz_extract_done_fn)(void *context, uint32_t index, int32_t result, int32_t os_error,
                                   const uint8_t *data, size_t length);

typedef struct sz_extract_options {
  /** Item indices; NULL or count 0 extracts all items. Order does not matter. */
  const uint32_t *indices;
  uint32_t index_count;
  sz_extract_target_fn target;
  sz_extract_done_fn done;
  /** May be NULL. */
  sz_progress_fn progress;
  void *context;
  /** May be NULL. */
  sz_cancel_token *cancel;
} sz_extract_options;

/**
 * Extracts items. Per-item failures (CRC, data, wrong password) are reported
 * through `done` and do not stop the run; the return value is SZ_STATUS_OK then.
 * Fatal problems (cancel, missing password, write errors the handler cannot
 * continue after) return an error status.
 */
SZ_API sz_status sz_archive_extract(sz_archive *archive, const sz_extract_options *options, sz_error *error);

/* ------------------------------------------------------------------------- */
/* Packing                                                                   */
/* ------------------------------------------------------------------------- */

#define SZ_PACK_FORMAT_7Z 1
#define SZ_PACK_FORMAT_ZIP 2

typedef struct sz_pack_item {
  /** Relative path inside the archive, '/'-separated, UTF-8. */
  const char *archive_path;
  /** Absolute source path for files; NULL for directories. */
  const char *source_path;
  int32_t is_dir;
  uint64_t size;
  int64_t mtime_seconds;
  int32_t mtime_nanoseconds;
  /** POSIX mode including the file type bits (S_IFREG, S_IFDIR). */
  uint32_t posix_mode;
} sz_pack_item;

typedef struct sz_pack_options {
  int32_t format;
  /** 0 (store) to 9 (ultra). */
  uint32_t level;
  /**
   * 7z only: dictionary size in bytes ("d"); 0 keeps the level's default. 7-Zip 26 uses
   * 32 MB at level 5 and up to 256 MB at level 9; the encoder needs about 11 times that.
   */
  uint64_t dictionary_size;
  /** Encoder threads (7-Zip "mt"); 0 keeps the library default. */
  uint32_t threads;
  /** NULL: no encryption. Otherwise AES-256 (ZIP: WinZip AES, 7z: 7zAES). */
  const char *password;
  /** 7z only: also encrypt the file list ("he=on"). */
  int32_t encrypt_headers;
  sz_progress_fn progress;
  void *context;
  sz_cancel_token *cancel;
} sz_pack_options;

/**
 * Writes a new archive to `output_path` (created or truncated). On failure the
 * output file is removed.
 */
SZ_API sz_status sz_pack(const char *output_path, const sz_pack_item *items, uint32_t item_count,
                         const sz_pack_options *options, sz_error *error);

#ifdef __cplusplus
}
#endif

#endif /* SEVENZIP_BRIDGE_H */
