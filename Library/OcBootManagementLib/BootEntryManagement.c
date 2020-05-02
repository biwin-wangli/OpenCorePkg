/** @file
  Copyright (C) 2019, vit9696. All rights reserved.

  All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
**/

#include "BootManagementInternal.h"

#include <Guid/AppleVariable.h>
#include <Guid/GlobalVariable.h>
#include <Guid/OcVariables.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/OcDebugLogLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcBootManagementLib.h>
#include <Library/OcDevicePathLib.h>
#include <Library/OcFileLib.h>
#include <Library/OcStringLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/PrintLib.h>

EFI_STATUS
OcDescribeBootEntry (
  IN     APPLE_BOOT_POLICY_PROTOCOL *BootPolicy,
  IN OUT OC_BOOT_ENTRY              *BootEntry
  )
{
  EFI_STATUS                       Status;
  CHAR16                           *BootDirectoryName;
  CHAR16                           *RecoveryBootName;
  EFI_HANDLE                       Device;
  EFI_HANDLE                       ApfsVolumeHandle;
  UINT32                           BcdSize;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;

  //
  // Custom entries need no special description.
  //
  if (BootEntry->Type == OC_BOOT_EXTERNAL_OS || BootEntry->Type == OC_BOOT_EXTERNAL_TOOL) {
    return EFI_SUCCESS;
  }

  Status = BootPolicy->DevicePathToDirPath (
    BootEntry->DevicePath,
    &BootDirectoryName,
    &Device,
    &ApfsVolumeHandle
    );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (
    Device,
    &gEfiSimpleFileSystemProtocolGuid,
    (VOID **) &FileSystem
    );

  if (EFI_ERROR (Status)) {
    FreePool (BootDirectoryName);
    return Status;
  }

  //
  // Try to use APFS-style label or legacy HFS one.
  //
  BootEntry->Name = InternalGetAppleDiskLabel (FileSystem, BootDirectoryName, L".contentDetails");
  if (BootEntry->Name == NULL) {
    BootEntry->Name = InternalGetAppleDiskLabel (FileSystem, BootDirectoryName, L".disk_label.contentDetails");
  }

  //
  // With FV2 encryption on HFS+ the actual boot happens from "Recovery HD/S/L/CoreServices".
  // For some reason "Recovery HD/S/L/CoreServices/.disk_label" may not get updated immediately,
  // and will contain "Recovery HD" despite actually pointing to "Macintosh HD".
  // This also spontaneously happens with renamed APFS volumes. The workaround is to manually
  // edit the file or sometimes choose the boot volume once more in preferences.
  //
  // TODO: Bugreport this to Apple, as this is clearly their bug, which should be reproducible
  // on original hardware.
  //
  // There exists .root_uuid, which contains real partition UUID in ASCII, however, Apple
  // BootPicker only uses it for entry deduplication, and we cannot figure out the name
  // on an encrypted volume anyway.
  //

  //
  // Windows boot entry may have a custom name, so ensure OC_BOOT_WINDOWS is set correctly.
  //
  if (BootEntry->Type == OC_BOOT_UNKNOWN) {
    DEBUG ((DEBUG_INFO, "Trying to detect Microsoft BCD\n"));
    Status = ReadFileSize (FileSystem, L"\\EFI\\Microsoft\\Boot\\BCD", &BcdSize);
    if (!EFI_ERROR (Status)) {
      BootEntry->Type = OC_BOOT_WINDOWS;
      if (BootEntry->Name == NULL) {
        BootEntry->Name = AllocateCopyPool (sizeof (L"Windows"), L"Windows");
      }
    }
  }

  if (BootEntry->Name == NULL) {
    BootEntry->Name = GetVolumeLabel (FileSystem);
    if (BootEntry->Name != NULL
      && (!StrCmp (BootEntry->Name, L"Recovery HD")
       || !StrCmp (BootEntry->Name, L"Recovery"))) {
      if (BootEntry->Type == OC_BOOT_UNKNOWN || BootEntry->Type == OC_BOOT_APPLE_OS) {
        BootEntry->Type = OC_BOOT_APPLE_RECOVERY;
      }
      RecoveryBootName = InternalGetAppleRecoveryName (FileSystem, BootDirectoryName);
      if (RecoveryBootName != NULL) {
        FreePool (BootEntry->Name);
        BootEntry->Name = RecoveryBootName;
      }
    }
  }

  if (BootEntry->Name == NULL) {
    FreePool (BootDirectoryName);
    return EFI_NOT_FOUND;
  }

  BootEntry->PathName = BootDirectoryName;

  return EFI_SUCCESS;
}

EFI_STATUS
OcGetBootEntryLabelImage (
  IN  OC_PICKER_CONTEXT          *Context,
  IN  APPLE_BOOT_POLICY_PROTOCOL *BootPolicy,
  IN  OC_BOOT_ENTRY              *BootEntry,
  IN  UINT8                      Scale,
  OUT VOID                       **ImageData,
  OUT UINT32                     *DataLength
  )
{
  EFI_STATUS                       Status;
  CHAR16                           *BootDirectoryName;
  EFI_HANDLE                       Device;
  EFI_HANDLE                       ApfsVolumeHandle;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;

  *ImageData = NULL;
  *DataLength = 0;

  if (BootEntry->Type == OC_BOOT_EXTERNAL_TOOL || BootEntry->Type == OC_BOOT_RESET_NVRAM) {
    ASSERT (Context->CustomDescribe != NULL);

    Status = Context->CustomDescribe (
      Context->CustomEntryContext,
      BootEntry,
      Scale,
      NULL,
      NULL,
      ImageData,
      DataLength
      );

    DEBUG ((DEBUG_INFO, "OCB: Get custom label %s - %r\n", BootEntry->Name, Status));
    return Status;
  }

  ASSERT (BootEntry->DevicePath != NULL);

  Status = BootPolicy->DevicePathToDirPath (
    BootEntry->DevicePath,
    &BootDirectoryName,
    &Device,
    &ApfsVolumeHandle
    );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (
    Device,
    &gEfiSimpleFileSystemProtocolGuid,
    (VOID **) &FileSystem
    );

  if (EFI_ERROR (Status)) {
    FreePool (BootDirectoryName);
    return Status;
  }

  Status = InternalGetAppleImage (
    FileSystem,
    BootDirectoryName,
    Scale == 2 ? L".disk_label_2x" : L".disk_label",
    ImageData,
    DataLength
    );

  DEBUG ((DEBUG_INFO, "OCB: Get normal label %s - %r\n", BootEntry->Name, Status));
  FreePool (BootDirectoryName);

  return Status;
}

EFI_STATUS
OcGetBootEntryIcon (
  IN  OC_PICKER_CONTEXT          *Context,
  IN  APPLE_BOOT_POLICY_PROTOCOL *BootPolicy,
  IN  OC_BOOT_ENTRY              *BootEntry,
  OUT VOID                       **ImageData,
  OUT UINT32                     *DataLength
  )
{
  EFI_STATUS                       Status;
  CHAR16                           *BootDirectoryName;
  EFI_HANDLE                       Device;
  EFI_HANDLE                       ApfsVolumeHandle;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;

  *ImageData = NULL;
  *DataLength = 0;

  if (BootEntry->Type == OC_BOOT_EXTERNAL_TOOL || BootEntry->Type == OC_BOOT_RESET_NVRAM) {
    ASSERT (Context->CustomDescribe != NULL);

    Status = Context->CustomDescribe (
      Context->CustomEntryContext,
      BootEntry,
      0,
      ImageData,
      DataLength,
      NULL,
      NULL
      );

    DEBUG ((DEBUG_INFO, "Get custom icon %s - %r\n", BootEntry->Name, Status));
    return Status;
  }

  ASSERT (BootEntry->DevicePath != NULL);

  Status = BootPolicy->DevicePathToDirPath (
    BootEntry->DevicePath,
    &BootDirectoryName,
    &Device,
    &ApfsVolumeHandle
    );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol (
    Device,
    &gEfiSimpleFileSystemProtocolGuid,
    (VOID **) &FileSystem
    );

  if (EFI_ERROR (Status)) {
    FreePool (BootDirectoryName);
    return Status;
  }

  Status = InternalGetAppleImage (
    FileSystem,
    L"",
    L".VolumeIcon.icns",
    ImageData,
    DataLength
    );

  DEBUG ((DEBUG_INFO, "OCB: Get normal icon %s - %r\n", BootEntry->Name, Status));

  FreePool (BootDirectoryName);

  return Status;
}

VOID
OcResetBootEntry (
  IN OUT OC_BOOT_ENTRY              *BootEntry
  )
{
  if (BootEntry->DevicePath != NULL) {
    FreePool (BootEntry->DevicePath);
    BootEntry->DevicePath = NULL;
  }

  if (BootEntry->Name != NULL) {
    FreePool (BootEntry->Name);
    BootEntry->Name = NULL;
  }

  if (BootEntry->PathName != NULL) {
    FreePool (BootEntry->PathName);
    BootEntry->PathName = NULL;
  }

  if (BootEntry->LoadOptions != NULL) {
    FreePool (BootEntry->LoadOptions);
    BootEntry->LoadOptions     = NULL;
    BootEntry->LoadOptionsSize = 0;
  }
}

VOID
OcFreeBootEntries (
  IN OUT OC_BOOT_ENTRY              *BootEntries,
  IN     UINTN                      Count
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; ++Index) {
    OcResetBootEntry (&BootEntries[Index]);
  }

  FreePool (BootEntries);
}

typedef struct OC_BOOT_FILESYSTEM_ {
  LIST_ENTRY  Link;
  EFI_HANDLE  Handle;
  LIST_ENTRY  BootEntries;
  BOOLEAN     External;
  BOOLEAN     LoaderFs;
} OC_BOOT_FILESYSTEM;

typedef struct OC_BOOT_CONTEXT_ {
  UINTN          BootOptionCount;
  UINTN          FileSystemCount;
  LIST_ENTRY     FileSystems;
  EFI_GUID       *BootVariableGuid;
  OC_BOOT_ENTRY  *DefaultEntry;
} OC_BOOT_CONTEXT;

STATIC
EFI_STATUS
AddBootEntryFromBootOption (
  IN OUT OC_BOOT_CONTEXT     *BootContext,
  IN     UINT16              BootOption,
  OUT    OC_BOOT_ENTRY       **BootEntry  OPTIONAL
  )
{
  EFI_STATUS                 Status;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL   *RemainingDevicePath;
  EFI_DEVICE_PATH_PROTOCOL   *PrevDevicePath;
  EFI_DEVICE_PATH_PROTOCOL   *FullDevicePath;
  UINTN                      DevicePathSize;
  CHAR16                     *BootName;
  EFI_HANDLE                 DeviceHandle;
  INTN                       NumPatchedNodes;
  BOOLEAN                    IsAppleLegacy;

  //
  // Obtain original device path.
  //
  DevicePath = InternalGetBootOptionData (
    BootOption,
    BootContext->BootVariableGuid,
    &BootName,
    NULL,
    NULL
    );
  if (DevicePath == NULL) {
    return EFI_NOT_FOUND;
  }

  //
  // Get BootCamp device path stored in special variable.
  // BootCamp device path will point to disk instead of partition.
  //
  IsAppleLegacy = InternalIsAppleLegacyLoadApp (DevicePath);
  if (IsAppleLegacy) {
    FreePool (DevicePath);
    Status = GetVariable2 (
      APPLE_BOOT_CAMP_HD_VARIABLE_NAME,
      &gAppleBootVariableGuid,
      (VOID **) &DevicePath,
      &DevicePathSize
      );
    if (EFI_ERROR (Status) || !IsDevicePathValid (DevicePath, DevicePathSize)) {
      return EFI_NOT_FOUND;
    }
  }

  //
  // Fixup device path if necessary.
  //
  RemainingDevicePath = DevicePath;
  NumPatchedNodes = OcFixAppleBootDevicePath (&RemainingDevicePath);
  if (NumPatchedNodes == -1) {
    FreePool (DevicePath);
    return EFI_NOT_FOUND;
  }

  //
  // Expand BootCamp device path to EFI partition device path.
  //
  if (IsAppleLegacy) {
    RemainingDevicePath = DevicePath;
    DevicePath = OcDiskFindSystemPartitionPath (
      DevicePath,
      &DevicePathSize
      );

    FreePool (RemainingDevicePath);

    if (DevicePath == NULL) {
      return EFI_NOT_FOUND;
    }

    //
    // The Device Path must be entirely locatable as
    // OcDiskFindSystemPartitionPath() guarantees to only return valid paths.
    //
    ASSERT (DevicePathSize > END_DEVICE_PATH_LENGTH);
    DevicePathSize -= END_DEVICE_PATH_LENGTH;
    RemainingDevicePath = (EFI_DEVICE_PATH_PROTOCOL *) ((UINTN) DevicePath + DevicePathSize);
  }

  //
  // If the Device Path was not advanced (by OcFixAppleBootDevicePath) it can be a short-form.
  // Perform short to full form expansion.
  // TODO: Continue.
  //
  if (DevicePath == RemainingDevicePath) {
    PrevDevicePath = NULL;
    do {
      FullDevicePath = OcGetNextLoadOptionDevicePath (
                         DevicePath,
                         PrevDevicePath
                         );

      if (PrevDevicePath != NULL) {
        FreePool (PrevDevicePath);
      }

      if (FullDevicePath == NULL) {
        DEBUG ((DEBUG_INFO, "OCB: Short-form DP could not be expanded\n"));
        BootEntry = NULL;
        break;
      }

      PrevDevicePath = FullDevicePath;

      RemainingDevicePath = FullDevicePath;
      Status = gBS->LocateDevicePath (
        &gEfiDevicePathProtocolGuid,
        &RemainingDevicePath,
        &DeviceHandle
        );
      if (EFI_ERROR (Status)) {
        BootEntry = NULL;
        continue;
      }

      DebugPrintDevicePath (
        DEBUG_INFO,
        "OCB: Expanded DP remainder",
        UefiRemainingDevicePath
        );

      BootEntry = InternalGetBootEntryByDevicePath (
                    BootEntries,
                    NumBootEntries,
                    FullDevicePath,
                    UefiRemainingDevicePath,
                    IsBootNext
                    );
    } while (BootEntry == NULL);
  }

  RemainingDevicePath = DevicePath;
  Status = gBS->LocateDevicePath (
    &gEfiSimpleFileSystemProtocolGuid,
    &RemainingDevicePath,
    &DeviceHandle
    );


}

STATIC
VOID
FreeBootEntry (
  IN OC_BOOT_ENTRY        *BootEntry
  )
{
  // TODO: Implement
}

/**
  Allocate a new file system entry in boot entries
  in case it can be used according to current ScanPolicy.
**/
STATIC
EFI_STATUS
AddFileSystemEntry (
  IN OUT OC_BOOT_CONTEXT     *BootContext,
  IN     EFI_HANDLE          FileSystemHandle,
  IN     UINT32              ScanPolicy,
  IN     EFI_HANDLE          LoaderHandle,
     OUT OC_BOOT_FILESYSTEM  **FileSystemEntry  OPTIONAL
  )
{
  EFI_STATUS          Status;
  BOOLEAN             IsExternal;
  OC_BOOT_FILESYSTEM  *Entry;

  Status = InternalCheckScanPolicy (
    FileSystemHandle,
    ScanPolicy,
    &IsExternal
    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Entry = AllocatePool (sizeof (*Entry));
  if (Entry == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Entry->Handle = FileSystemHandle;
  InitializeListHead (&Entry->BootEntries);
  Entry->External = IsExternal;
  Entry->LoaderFs = LoaderHandle == FileSystemHandle;
  InsertTailList (&BootContext->FileSystems, &Entry->Link);
  ++BootContext->FileSystemCount;

  if (FileSystemEntry != NULL) {
    *FileSystemEntry = Entry;
  }

  return EFI_SUCCESS;
}

STATIC
VOID
FreeFileSystemEntry (
  IN OUT OC_BOOT_CONTEXT     *BootContext,
  IN     OC_BOOT_FILESYSTEM  *FileSystemEntry
  )
{
  LIST_ENTRY     *Link;
  OC_BOOT_ENTRY  *BootEntry;

  RemoveEntryList (&FileSystemEntry->Link);
  --BootContext->FileSystemCount;

  while (!IsListEmpty (&FileSystemEntry->BootEntries)) {
    Link = GetFirstNode (&FileSystemEntry->BootEntries);
    BootEntry = BASE_CR (Link, OC_BOOT_ENTRY, Link);
    RemoveEntryList (Link);
    FreeBootEntry (BootEntry);
  }

  FreePool (FileSystemEntry);
}

STATIC
OC_BOOT_CONTEXT *
BuildFileSystemList (
  IN UINT32      ScanPolicy,
  IN EFI_HANDLE  LoaderHandle,
  IN EFI_GUID    *BootVariableGuid,
  IN BOOLEAN     Empty
  )
{
  OC_BOOT_CONTEXT  *Context;
  EFI_STATUS       Status;
  UINTN            NoHandles;
  EFI_HANDLE       *Handles;
  UINTN            Index;

  Context = AllocatePool (sizeof (*Context));
  if (Context == NULL) {
    return NULL;
  }

  Context->BootOptionCount  = 0;
  Context->FileSystemCount  = 0;
  InitializeListHead (&Context->FileSystems);
  Context->BootVariableGuid = BootVariableGuid;
  Context->DefaultEntry     = NULL;

  if (Empty) {
    return Context;
  }

  Status = gBS->LocateHandleBuffer (
    ByProtocol,
    &gEfiSimpleFileSystemProtocolGuid,
    NULL,
    &NoHandles,
    &Handles
    );
  if (EFI_ERROR (Status)) {
    return Context;
  }

  for (Index = 0; Index < NoHandles; ++Index) {
    AddFileSystemEntry (
      Context,
      Handles[Index],
      ScanPolicy,
      LoaderHandle,
      NULL
      );
  }

  FreePool (Handles);
  return Context;
}

STATIC
VOID
FreeFileSystemList (
  IN OUT OC_BOOT_CONTEXT  *Context
  )
{
  LIST_ENTRY          *Link;
  OC_BOOT_FILESYSTEM  *FileSystem;

  while (!IsListEmpty (&Context->FileSystems)) {
    Link = GetFirstNode (&Context->FileSystems);
    FileSystem = BASE_CR (Link, OC_BOOT_FILESYSTEM, Link);
    FreeFileSystemEntry (Context, FileSystem);
  }

  FreePool (Context);
}


EFI_STATUS
HandleNew (
  IN  APPLE_BOOT_POLICY_PROTOCOL  *BootPolicy,
  IN  OC_PICKER_CONTEXT           *Context
  )
{
  EFI_STATUS                       Status;
  OC_BOOT_CONTEXT                  *BootContext;
  EFI_GUID                         *BootVariableGuid;
  UINT16                           *BootOrder;
  UINTN                            BootOrderCount;
  UINTN                            Index;
  BOOLEAN                          HasBootNext;

  BootContext = BuildFileSystemList (
    Context->ScanPolicy,
    Context->LoaderHandle,
    Context->CustomBootGuid
      ? &gOcVendorVariableGuid : &gEfiGlobalVariableGuid,
    FALSE
    );
  if (BootContext == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((DEBUG_INFO, "OCB: Found %u potentially bootable filesystems\n", (UINT32) BootContext->FileSystemCount));

  BootOrder = OcGetBootOrder (
    BootContext->BootVariableGuid,
    TRUE,
    &BootOrderCount,
    NULL,
    &HasBootNext
    );
  if (BootOrder != NULL) {
    DEBUG_CODE_BEGIN ();
    DEBUG ((
      DEBUG_INFO,
      "OCB: Found %u BootOrder entries with BootNext %a\n",
      (UINT32) BootOrderCount,
      HasBootNext ? "included" : "excluded"
      ));
    InternalDebugBootEnvironment (BootOrder, BootContext->BootVariableGuid, BootOrderCount);
    DEBUG_CODE_END ();

    if (HasBootNext) {
      gRT->SetVariable (
        EFI_BOOT_NEXT_VARIABLE_NAME,
        BootVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
        0,
        NULL
        );
    }

    for (Index = 0; Index < BootOrderCount; ++Index) {
      AddBootEntryFromBootOption (BootContext, BootOrder[Index], NULL);
    }

    FreePool (BootOrder);
  }
}

EFI_STATUS
OcScanForBootEntries (
  IN  APPLE_BOOT_POLICY_PROTOCOL  *BootPolicy,
  IN  OC_PICKER_CONTEXT           *Context,
  OUT OC_BOOT_ENTRY               **BootEntries,
  OUT UINTN                       *Count,
  OUT UINTN                       *AllocCount OPTIONAL,
  IN  BOOLEAN                     Describe
  )
{
  EFI_STATUS                       Status;
  BOOLEAN                          Result;

  UINTN                            NoHandles;
  EFI_HANDLE                       *Handles;
  UINTN                            Index;
  OC_BOOT_ENTRY                    *Entries;
  UINTN                            EntriesSize;
  UINTN                            EntryIndex;
  CHAR16                           *PathName;
  CHAR16                           *DevicePathText;

  UINTN                            DevPathScanInfoSize;
  INTERNAL_DEV_PATH_SCAN_INFO      *DevPathScanInfo;
  INTERNAL_DEV_PATH_SCAN_INFO      *DevPathScanInfos;
  EFI_DEVICE_PATH_PROTOCOL         *DevicePathWalker;
  CONST FILEPATH_DEVICE_PATH       *FilePath;

  Result = OcOverflowMulUN (Context->AllCustomEntryCount, sizeof (OC_BOOT_ENTRY), &EntriesSize);
  if (Result) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (Context->ShowNvramReset && !Context->HideAuxiliary) {
    Result = OcOverflowAddUN (EntriesSize, sizeof (OC_BOOT_ENTRY), &EntriesSize);
    if (Result) {
      return EFI_OUT_OF_RESOURCES;
    }
  }

  Status = gBS->LocateHandleBuffer (
    ByProtocol,
    &gEfiSimpleFileSystemProtocolGuid,
    NULL,
    &NoHandles,
    &Handles
    );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO, "OCB: Found %u potentially bootable filesystems\n", (UINT32) NoHandles));

  if (NoHandles == 0) {
    FreePool (Handles);
    return EFI_NOT_FOUND;
  }

  Result = OcOverflowMulUN (
             NoHandles,
             sizeof (*DevPathScanInfos),
             &DevPathScanInfoSize
             );
  if (Result) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  DevPathScanInfos = AllocateZeroPool (DevPathScanInfoSize);
  if (DevPathScanInfos == NULL) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < NoHandles; ++Index) {
    DevPathScanInfo = &DevPathScanInfos[Index];

    Status = InternalPrepareScanInfo (
      BootPolicy,
      Context,
      Handles,
      Index,
      DevPathScanInfo
      );

    if (EFI_ERROR (Status)) {
      continue;
    }

    ASSERT (DevPathScanInfo->NumBootInstances > 0);

    Result = OcOverflowMulAddUN (
      DevPathScanInfo->NumBootInstances,
      2 * sizeof (OC_BOOT_ENTRY),
      EntriesSize,
      &EntriesSize
      );
    if (Result) {
      FreePool (Handles);
      FreePool (DevPathScanInfos);
      return EFI_OUT_OF_RESOURCES;
    }
  }
  //
  // Errors from within the loop are not fatal.
  //
  Status = EFI_SUCCESS;

  FreePool (Handles);

  if (EntriesSize == 0) {
    FreePool (DevPathScanInfos);
    return EFI_NOT_FOUND;
  }

  Entries = AllocateZeroPool (EntriesSize);
  if (Entries == NULL) {
    FreePool (DevPathScanInfos);
    return EFI_OUT_OF_RESOURCES;
  }

  EntryIndex = 0;
  for (Index = 0; Index < NoHandles; ++Index) {
    DevPathScanInfo = &DevPathScanInfos[Index];

    DevicePathWalker = DevPathScanInfo->BootDevicePath;
    if (DevicePathWalker == NULL) {
      continue;
    }

    EntryIndex = InternalFillValidBootEntries (
      BootPolicy,
      Context,
      DevPathScanInfo,
      DevicePathWalker,
      Entries,
      EntryIndex
      );

    FreePool (DevPathScanInfo->BootDevicePath);
  }

  FreePool (DevPathScanInfos);

  if (Describe) {
    DEBUG ((DEBUG_INFO, "OCB: Scanning got %u entries\n", (UINT32) EntryIndex));

    for (Index = 0; Index < EntryIndex; ++Index) {
      Status = OcDescribeBootEntry (BootPolicy, &Entries[Index]);
      if (EFI_ERROR (Status)) {
        break;
      }

      DEBUG_CODE_BEGIN ();
      DEBUG ((
        DEBUG_INFO,
        "OCB: Entry %u is %s at %s (T:%d|F:%d)\n",
        (UINT32) Index,
        Entries[Index].Name,
        Entries[Index].PathName,
        Entries[Index].Type,
        Entries[Index].IsFolder
        ));

      DevicePathText = ConvertDevicePathToText (Entries[Index].DevicePath, FALSE, FALSE);
      if (DevicePathText != NULL) {
        DEBUG ((
          DEBUG_INFO,
          "OCB: Entry %u is %s at dp %s\n",
          (UINT32) Index,
          Entries[Index].Name,
          DevicePathText
          ));
        FreePool (DevicePathText);
      }
      DEBUG_CODE_END ();
    }

    if (EFI_ERROR (Status)) {
      OcFreeBootEntries (Entries, EntryIndex);
      return Status;
    }
  }

  for (Index = 0; Index < Context->AllCustomEntryCount; ++Index) {
    if (Context->CustomEntries[Index].Auxiliary && Context->HideAuxiliary) {
      continue;
    }

    Entries[EntryIndex].Name = AsciiStrCopyToUnicode (Context->CustomEntries[Index].Name, 0);
    PathName                 = AsciiStrCopyToUnicode (Context->CustomEntries[Index].Path, 0);
    if (Entries[EntryIndex].Name == NULL || PathName == NULL) {
      OcFreeBootEntries (Entries, EntryIndex + 1);
      return EFI_OUT_OF_RESOURCES;
    }

    if (Context->CustomEntries[Index].Tool) {
      Entries[EntryIndex].Type = OC_BOOT_EXTERNAL_TOOL;
    } else {
      Entries[EntryIndex].Type = OC_BOOT_EXTERNAL_OS;
    }

    if (Index < Context->AbsoluteEntryCount) {
      DEBUG ((
        DEBUG_INFO,
        "OCB: Custom entry %u is %s\n",
        (UINT32) EntryIndex,
        Entries[EntryIndex].Name
        ));

      Entries[EntryIndex].DevicePath = ConvertTextToDevicePath (PathName);
      FreePool (PathName);
      if (Entries[EntryIndex].DevicePath == NULL) {
        FreePool (Entries[EntryIndex].Name);
        continue;
      }

      FilePath = (FILEPATH_DEVICE_PATH *)(
                   FindDevicePathNodeWithType (
                     Entries[EntryIndex].DevicePath,
                     MEDIA_DEVICE_PATH,
                     MEDIA_FILEPATH_DP
                     )
                   );
      if (FilePath == NULL) {
        FreePool (Entries[EntryIndex].Name);
        FreePool (Entries[EntryIndex].DevicePath);
        continue;
      }

      Entries[EntryIndex].PathName = AllocateCopyPool (
                                       OcFileDevicePathNameSize (FilePath),
                                       FilePath->PathName
                                       );
      if (Entries[EntryIndex].PathName == NULL) {
        FreePool (Entries[EntryIndex].Name);
        FreePool (Entries[EntryIndex].DevicePath);
        continue;
      }
    } else {
      UnicodeUefiSlashes (PathName);
      Entries[EntryIndex].PathName = PathName;
    }

    Entries[EntryIndex].LoadOptionsSize = (UINT32) AsciiStrLen (Context->CustomEntries[Index].Arguments);
    if (Entries[EntryIndex].LoadOptionsSize > 0) {
      Entries[EntryIndex].LoadOptions = AllocateCopyPool (
        Entries[EntryIndex].LoadOptionsSize + 1,
        Context->CustomEntries[Index].Arguments
        );
      if (Entries[EntryIndex].LoadOptions == NULL) {
        Entries[EntryIndex].LoadOptionsSize = 0;
      }
    }

    ++EntryIndex;
  }

  if (Context->ShowNvramReset && !Context->HideAuxiliary) {
    Entries[EntryIndex].Name = AllocateCopyPool (
      L_STR_SIZE (OC_MENU_RESET_NVRAM_ENTRY),
      OC_MENU_RESET_NVRAM_ENTRY
      );
    if (Entries[EntryIndex].Name == NULL) {
      OcFreeBootEntries (Entries, EntryIndex + 1);
      return EFI_OUT_OF_RESOURCES;
    }

    Entries[EntryIndex].Type         = OC_BOOT_RESET_NVRAM;
    Entries[EntryIndex].SystemAction = InternalSystemActionResetNvram;
    ++EntryIndex;
  }

  *BootEntries = Entries;
  *Count       = EntryIndex;

  ASSERT (*Count <= EntriesSize / sizeof (OC_BOOT_ENTRY));

  if (AllocCount != NULL) {
    *AllocCount = EntriesSize / sizeof (OC_BOOT_ENTRY);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
OcLoadBootEntry (
  IN  APPLE_BOOT_POLICY_PROTOCOL  *BootPolicy,
  IN  OC_PICKER_CONTEXT           *Context,
  IN  OC_BOOT_ENTRY               *BootEntry,
  IN  EFI_HANDLE                  ParentHandle
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 EntryHandle;
  INTERNAL_DMG_LOAD_CONTEXT  DmgLoadContext;

  if ((BootEntry->Type & OC_BOOT_SYSTEM) != 0) {
    ASSERT (BootEntry->SystemAction != NULL);
    return BootEntry->SystemAction ();
  }

  Status = InternalLoadBootEntry (
    BootPolicy,
    Context,
    BootEntry,
    ParentHandle,
    &EntryHandle,
    &DmgLoadContext
    );
  if (!EFI_ERROR (Status)) {
    Status = Context->StartImage (BootEntry, EntryHandle, NULL, NULL);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "OCB: StartImage failed - %r\n", Status));
      //
      // Unload dmg if any.
      //
      InternalUnloadDmg (&DmgLoadContext);
    }
  } else {
    DEBUG ((DEBUG_ERROR, "OCB: LoadImage failed - %r\n", Status));
  }

  return Status;
}
