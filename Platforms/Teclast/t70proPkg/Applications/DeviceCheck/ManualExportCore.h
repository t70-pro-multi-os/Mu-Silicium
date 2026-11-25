// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_MANUAL_EXPORT_CORE_H
#define T70_MANUAL_EXPORT_CORE_H
#include <stddef.h>
#include <stdint.h>
typedef enum { T70ExportLog=1, T70ExportScreenshot=2 } T70_EXPORT_KIND;
typedef enum { T70ExportOk, T70ExportRejected, T70ExportCreateFailed,
  T70ExportWriteFailed, T70ExportShortWrite, T70ExportFlushFailed,
  T70ExportCloseFailed } T70_EXPORT_STATUS;
/* Snapshot supplied by a future trusted adapter, not arbitrary UI booleans.
 * It must bind the explicit selection to one still-present SD FAT volume and
 * reject mixed/unknown ancestry, media change, RO media and no device path.
 * Neither filesystem discovery nor a firmware adapter is implemented here. */
typedef struct { uint64_t Selection; unsigned Manual, SdFat, Writable; } T70_EXPORT_REQUEST;
typedef struct {
  void *Context;
  int (*ValidateSelection)(void *,uint64_t);
  /* Must atomically fail if Name exists and never open/truncate an old file.
   * EFI_FILE_MODE_CREATE alone DOES NOT satisfy this contract. On failure
   * the callback owns cleanup and must leave *File NULL. No adapter yet. */
  int (*CreateNew)(void *,uint64_t,const char *Name,void **File);
  int (*Write)(void *,void *File,const void *,size_t,size_t *Written);
  int (*Flush)(void *,void *File);
  int (*Close)(void *,void *File);
} T70_EXPORT_IO;
typedef struct {
  T70_EXPORT_STATUS Status;
  int OperationError, CloseError;
  size_t Written;
  unsigned Created, FlushSucceeded;
} T70_EXPORT_RESULT;
/* Manual only; payload is already captured RAM. No capture or device access.
 * Success means adapter Flush/Close succeeded, NOT independent media proof.
 * Failed newly-created files are retained; no delete/retry/overwrite. */
T70_EXPORT_RESULT T70ManualExport(const T70_EXPORT_REQUEST *,T70_EXPORT_KIND,
  uint32_t Number,const void *Data,size_t Size,const T70_EXPORT_IO *);
#endif
