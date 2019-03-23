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

#include <Uefi.h>

#include <IndustryStandard/AppleKmodInfo.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/OcAppleKernelLib.h>
#include <Library/OcMachoLib.h>
#include <Library/PrintLib.h>

#include "PrelinkedInternal.h"

STATIC
UINT64
PrelinkedFindLastLoadAddress (
  IN XML_NODE  *KextList
  )
{
  UINT32       KextCount;
  UINT32       FieldIndex;
  UINT32       FieldCount;
  XML_NODE     *LastKext;
  CONST CHAR8  *KextPlistKey;
  XML_NODE     *KextPlistValue;
  UINT64       LoadAddress;
  UINT64       LoadSize;

  KextCount = XmlNodeChildren (KextList);
  if (KextCount == 0) {
    return 0;
  }

  //
  // Here we make an assumption that last kext has the highest load address.
  //
  LastKext = PlistNodeCast (XmlNodeChild (KextList, KextCount - 1), PLIST_NODE_TYPE_DICT);
  if (LastKext == NULL) {
    return 0;
  }

  LoadAddress = 0;
  LoadSize = 0;

  FieldCount = PlistDictChildren (LastKext);
  for (FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex) {
    KextPlistKey = PlistKeyValue (PlistDictChild (LastKext, FieldIndex, &KextPlistValue));
    if (KextPlistKey == NULL) {
      continue;
    }

    if (LoadAddress == 0 && AsciiStrCmp (KextPlistKey, PRELINK_INFO_EXECUTABLE_LOAD_ADDR_KEY) == 0) {
      if (!PlistIntegerValue (KextPlistValue, &LoadAddress, sizeof (LoadAddress), TRUE)) {
        return 0;
      }
    } else if (LoadSize == 0 && AsciiStrCmp (KextPlistKey, PRELINK_INFO_EXECUTABLE_SIZE_KEY) == 0) {
      if (!PlistIntegerValue (KextPlistValue, &LoadSize, sizeof (LoadSize), TRUE)) {
        return 0;
      }
    }

    if (LoadSize != 0 && LoadAddress != 0) {
      break;
    }
  }

  if (OcOverflowAddU64 (LoadAddress, LoadSize, &LoadAddress)) {
    return 0;
  }

  return LoadAddress;
}

STATIC
UINT64
PrelinkedFindKmodAddress (
  IN OC_MACHO_CONTEXT  *ExecutableContext,
  IN UINT64            LoadAddress,
  IN UINT32            Size
  )
{
  MACH_NLIST_64            *Symbol;
  CONST CHAR8              *SymbolName;
  MACH_SEGMENT_COMMAND_64  *TextSegment;
  UINT64                   Address;
  UINT32                   Index;

  Index = 0;
  while (TRUE) {
    Symbol = MachoGetSymbolByIndex64 (ExecutableContext, Index);
    if (Symbol == NULL) {
      return 0;
    }

    SymbolName = MachoGetSymbolName64 (ExecutableContext, Symbol);
    if (SymbolName && AsciiStrCmp (SymbolName, "_kmod_info") == 0) {
      if (!MachoIsSymbolValueInRange64 (ExecutableContext, Symbol)) {
        return 0;
      }
      break;
    }

    Index++;
  }

  TextSegment = MachoGetSegmentByName64 (ExecutableContext, "__TEXT");
  if (TextSegment == NULL || TextSegment->FileOffset > TextSegment->VirtualAddress) {
    return 0;
  }

  Address = TextSegment->VirtualAddress - TextSegment->FileOffset;
  if (OcOverflowTriAddU64 (Address, LoadAddress, Symbol->Value, &Address)
    || Address > LoadAddress + Size - sizeof (KMOD_INFO_64_V1)) {
    return 0;
  }

  return Address;
}

EFI_STATUS
PrelinkedContextInit (
  IN OUT  PRELINKED_CONTEXT  *Context,
  IN OUT  UINT8              *Prelinked,
  IN      UINT32             PrelinkedSize,
  IN      UINT32             PrelinkedAllocSize
  )
{
  XML_NODE     *PrelinkedInfoRoot;
  CONST CHAR8  *PrelinkedInfoRootKey;
  UINT32       PrelinkedInfoRootIndex;
  UINT32       PrelinkedInfoRootCount;

  ZeroMem (Context, sizeof (*Context));

  Context->Prelinked          = Prelinked;
  Context->PrelinkedSize      = PRELINKED_ALIGN (PrelinkedSize);
  Context->PrelinkedAllocSize = PrelinkedAllocSize;

  //
  // Initialize kext list with kernel pseudo kext.
  //
  InitializeListHead (&Context->PrelinkedKexts);
  if (InternalCachedPrelinkedKernel (Context) == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Ensure that PrelinkedSize is always aligned.
  //
  if (Context->PrelinkedSize != PrelinkedSize) {
    if (Context->PrelinkedSize > PrelinkedAllocSize) {
      return EFI_BUFFER_TOO_SMALL;
    }

    ZeroMem (&Prelinked[PrelinkedSize], Context->PrelinkedSize - PrelinkedSize);
  }

  if (!MachoInitializeContext (&Context->PrelinkedMachContext, Prelinked, PrelinkedSize)) {
    return EFI_INVALID_PARAMETER;
  }

  Context->PrelinkedLastAddress = PRELINKED_ALIGN (MachoGetLastAddress64 (&Context->PrelinkedMachContext));
  if (Context->PrelinkedLastAddress == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Context->PrelinkedInfoSegment = MachoGetSegmentByName64 (
    &Context->PrelinkedMachContext,
    PRELINK_INFO_SEGMENT
    );
  if (Context->PrelinkedInfoSegment == NULL) {
    return EFI_NOT_FOUND;
  }
  if (Context->PrelinkedInfoSegment->FileOffset > MAX_UINT32) {
    return EFI_UNSUPPORTED;
  }

  Context->PrelinkedInfoSection = MachoGetSectionByName64 (
    &Context->PrelinkedMachContext,
    Context->PrelinkedInfoSegment,
    PRELINK_INFO_SECTION
    );
  if (Context->PrelinkedInfoSection == NULL) {
    return EFI_NOT_FOUND;
  }
  if (Context->PrelinkedInfoSection->Size > MAX_UINT32) {
    return EFI_UNSUPPORTED;
  }

  Context->PrelinkedTextSegment = MachoGetSegmentByName64 (
    &Context->PrelinkedMachContext,
    PRELINK_TEXT_SEGMENT
    );
  if (Context->PrelinkedTextSegment == NULL) {
    return EFI_NOT_FOUND;
  }

  Context->PrelinkedTextSection = MachoGetSectionByName64 (
    &Context->PrelinkedMachContext,
    Context->PrelinkedTextSegment,
    PRELINK_TEXT_SECTION
    );
  if (Context->PrelinkedTextSection == NULL) {
    return EFI_NOT_FOUND;
  }

  Context->PrelinkedInfo = AllocateCopyPool (
    Context->PrelinkedInfoSection->Size,
    &Context->Prelinked[Context->PrelinkedInfoSection->Offset]
    );
  if (Context->PrelinkedInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Context->PrelinkedInfoDocument = XmlDocumentParse (Context->PrelinkedInfo, (UINT32)Context->PrelinkedInfoSection->Size, TRUE);
  if (Context->PrelinkedInfoDocument == NULL) {
    PrelinkedContextFree (Context);
    return EFI_INVALID_PARAMETER;
  }

  PrelinkedInfoRoot = PlistNodeCast (XmlDocumentRoot (Context->PrelinkedInfoDocument), PLIST_NODE_TYPE_DICT);
  if (PrelinkedInfoRoot == NULL) {
    PrelinkedContextFree (Context);
    return EFI_INVALID_PARAMETER;
  }

  PrelinkedInfoRootCount = PlistDictChildren (PrelinkedInfoRoot);
  for (PrelinkedInfoRootIndex = 0; PrelinkedInfoRootIndex < PrelinkedInfoRootCount; ++PrelinkedInfoRootIndex) {
    PrelinkedInfoRootKey = PlistKeyValue (PlistDictChild (PrelinkedInfoRoot, PrelinkedInfoRootIndex, &Context->KextList));
    if (PrelinkedInfoRootKey == NULL) {
      continue;
    }

    if (AsciiStrCmp (PrelinkedInfoRootKey, PRELINK_INFO_DICTIONARY_KEY) == 0) {
      if (PlistNodeCast (Context->KextList, PLIST_NODE_TYPE_ARRAY) != NULL) {
        Context->PrelinkedLastLoadAddress = PrelinkedFindLastLoadAddress (Context->KextList);
        if (Context->PrelinkedLastLoadAddress != 0) {
          return EFI_SUCCESS;
        }
      }
      break;
    }
  }

  PrelinkedContextFree (Context);
  return EFI_INVALID_PARAMETER;
}

VOID
PrelinkedContextFree (
  IN OUT  PRELINKED_CONTEXT  *Context
  )
{
  UINT32          Index;
  LIST_ENTRY      *Link;
  PRELINKED_KEXT  *Kext;

  if (Context->PrelinkedInfoDocument != NULL) {
    XmlDocumentFree (Context->PrelinkedInfoDocument);
    Context->PrelinkedInfoDocument = NULL;
  }

  if (Context->PrelinkedInfo != NULL) {
    FreePool (Context->PrelinkedInfo);
    Context->PrelinkedInfo = NULL;
  }

  if (Context->PooledBuffers != NULL) {
    for (Index = 0; Index < Context->PooledBuffersCount; ++Index) {
      FreePool (Context->PooledBuffers[Index]);
    }
    FreePool (Context->PooledBuffers);
    Context->PooledBuffers = NULL;
  }

  if (Context->LinkBuffer != NULL) {
    FreePool (Context->LinkBuffer);
  }

  while (!IsListEmpty (&Context->PrelinkedKexts)) {
    Link = GetFirstNode (&Context->PrelinkedKexts);
    Kext = GET_PRELINKED_KEXT_FROM_LINK (Link);
    RemoveEntryList (Link);
    InternalFreePrelinkedKext (Kext);
  }

  ZeroMem (&Context->PrelinkedKexts, sizeof (Context->PrelinkedKexts));
}

EFI_STATUS
PrelinkedDependencyInsert (
  IN OUT  PRELINKED_CONTEXT  *Context,
  IN      VOID               *Buffer
  )
{
  VOID   **NewPooledBuffers;

  if (Context->PooledBuffersCount == Context->PooledBuffersAllocCount) {
    NewPooledBuffers = AllocatePool (
      2 * (Context->PooledBuffersAllocCount + 1) * sizeof (NewPooledBuffers[0])
      );
    if (NewPooledBuffers == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    if (Context->PooledBuffers != NULL) {
      CopyMem (
        &NewPooledBuffers[0],
        &Context->PooledBuffers[0],
        Context->PooledBuffersCount * sizeof (NewPooledBuffers[0])
        );
      FreePool (Context->PooledBuffers);
    }
    Context->PooledBuffers           = NewPooledBuffers;
    Context->PooledBuffersAllocCount = 2 * (Context->PooledBuffersAllocCount + 1);
  }

  Context->PooledBuffers[Context->PooledBuffersCount] = Buffer;
  Context->PooledBuffersCount++;

  return EFI_SUCCESS;
}

EFI_STATUS
PrelinkedInjectPrepare (
  IN OUT PRELINKED_CONTEXT  *Context
  )
{
  UINT64  SegmentEndOffset;

  //
  // Plist info is normally the last segment, so we may potentially save
  // some data by removing it and then appending new kexts over.
  //

  SegmentEndOffset = Context->PrelinkedInfoSegment->FileOffset + Context->PrelinkedInfoSegment->FileSize;

  if (PRELINKED_ALIGN (SegmentEndOffset) == Context->PrelinkedSize) {
    Context->PrelinkedSize = (UINT32)PRELINKED_ALIGN (Context->PrelinkedInfoSegment->FileOffset);
  }

  Context->PrelinkedInfoSegment->VirtualAddress = 0;
  Context->PrelinkedInfoSegment->Size           = 0;
  Context->PrelinkedInfoSegment->FileOffset     = 0;
  Context->PrelinkedInfoSegment->FileSize       = 0;
  Context->PrelinkedInfoSection->Address        = 0;
  Context->PrelinkedInfoSection->Size           = 0;
  Context->PrelinkedInfoSection->Offset         = 0;

  Context->PrelinkedLastAddress = PRELINKED_ALIGN (MachoGetLastAddress64 (&Context->PrelinkedMachContext));
  if (Context->PrelinkedLastAddress == 0) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Prior to plist there usually is prelinked text. 
  //

  SegmentEndOffset = Context->PrelinkedTextSegment->FileOffset + Context->PrelinkedTextSegment->FileSize;

  if (PRELINKED_ALIGN (SegmentEndOffset) != Context->PrelinkedSize) {
    //
    // TODO: Implement prelinked text relocation when it is not preceding prelinked info
    // and is not in the end of prelinked info.
    //
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
PrelinkedInjectComplete (
  IN OUT PRELINKED_CONTEXT  *Context
  )
{
  CHAR8       *ExportedInfo;
  UINT32      ExportedInfoSize;
  UINT32      NewSize;

  ExportedInfo = XmlDocumentExport (Context->PrelinkedInfoDocument, &ExportedInfoSize, 0);
  if (ExportedInfo == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Include \0 terminator.
  //
  ExportedInfoSize++;

  if (OcOverflowAddU32 (Context->PrelinkedSize, PRELINKED_ALIGN (ExportedInfoSize), &NewSize)
    || NewSize > Context->PrelinkedAllocSize) {
    FreePool (ExportedInfo);
    return EFI_BUFFER_TOO_SMALL;
  }

  Context->PrelinkedInfoSegment->VirtualAddress = Context->PrelinkedLastAddress;
  Context->PrelinkedInfoSegment->Size           = ExportedInfoSize;
  Context->PrelinkedInfoSegment->FileOffset     = Context->PrelinkedSize;
  Context->PrelinkedInfoSegment->FileSize       = ExportedInfoSize;
  Context->PrelinkedInfoSection->Address        = Context->PrelinkedLastAddress;
  Context->PrelinkedInfoSection->Size           = ExportedInfoSize;
  Context->PrelinkedInfoSection->Offset         = Context->PrelinkedSize;

  CopyMem (
    &Context->Prelinked[Context->PrelinkedSize],
    ExportedInfo,
    ExportedInfoSize
    );

  ZeroMem (
    &Context->Prelinked[Context->PrelinkedSize + ExportedInfoSize],
    PRELINKED_ALIGN (ExportedInfoSize) - ExportedInfoSize
    );

  Context->PrelinkedLastAddress += PRELINKED_ALIGN (ExportedInfoSize);
  Context->PrelinkedSize        += PRELINKED_ALIGN (ExportedInfoSize);

  FreePool (ExportedInfo);

  return EFI_SUCCESS;
}

EFI_STATUS
PrelinkedReserveKextSize (
  IN OUT UINT32       *ReservedSize,
  IN     UINT32       InfoPlistSize,
  IN     UINT32       ExecutableSize OPTIONAL
  )
{
  //
  // For new fields.
  //
  if (OcOverflowAddU32 (InfoPlistSize, 512, &InfoPlistSize)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // For __LINKEDIT segment.
  //
  if (OcOverflowAddU32 (ExecutableSize, 4096*2, &ExecutableSize)) {
    return EFI_INVALID_PARAMETER;
  }

  InfoPlistSize  = PRELINKED_ALIGN (InfoPlistSize);
  ExecutableSize = PRELINKED_ALIGN (ExecutableSize);

  if (OcOverflowTriAddU32 (*ReservedSize, InfoPlistSize, ExecutableSize, &ExecutableSize)) {
    return EFI_INVALID_PARAMETER;
  }

  *ReservedSize = ExecutableSize;
  return EFI_SUCCESS;
}

STATIC
VOID
AsciiIntegerToLowerHex (
  OUT CHAR8   *Buffer,
  IN  UINT32  BufferSize,
  IN  UINT64  Value
  )
{
  //
  // FIXME: This is utter crap, implement int to hex functions.
  //
  AsciiSPrint (Buffer, BufferSize, "0x%Lx", Value);
  while (Buffer[0] != 0) {
    if (Buffer[0] >= 'A' && Buffer[0] <= 'F') {
      Buffer[0] += ('a' - 'A');
    }
    Buffer++;
  }
}

EFI_STATUS
PrelinkedInjectKext (
  IN OUT PRELINKED_CONTEXT  *Context,
  IN     CONST CHAR8        *BundlePath,
  IN     CONST CHAR8        *InfoPlist,
  IN     UINT32             InfoPlistSize,
  IN     CONST CHAR8        *ExecutablePath OPTIONAL,
  IN     CONST UINT8        *Executable OPTIONAL,
  IN     UINT32             ExecutableSize OPTIONAL
  )
{
  EFI_STATUS        Status;
  XML_DOCUMENT      *InfoPlistDocument;
  XML_NODE          *InfoPlistRoot;
  CHAR8             *TmpInfoPlist;
  CHAR8             *NewInfoPlist;
  OC_MACHO_CONTEXT  ExecutableContext;
  UINT32            NewInfoPlistSize;
  UINT32            NewPrelinkedSize;
  UINT32            AlignedExecutableSize;
  UINT32            AlignedLoadSize;
  BOOLEAN           Failed;
  UINT64            KmodAddress;
  PRELINKED_KEXT    *PrelinkedKext;
  CHAR8             ExecutableSourceAddrStr[24];
  CHAR8             ExecutableSizeStr[24];
  CHAR8             ExecutableLoadAddrStr[24];
  CHAR8             KmodInfoStr[24];

  PrelinkedKext = NULL;

  //
  // Copy executable to prelinkedkernel.
  //
  if (Executable != NULL) {
    AlignedExecutableSize = PRELINKED_ALIGN (ExecutableSize);
    if (OcOverflowAddU32 (Context->PrelinkedSize, AlignedExecutableSize, &NewPrelinkedSize)
      || NewPrelinkedSize > Context->PrelinkedAllocSize) {
      return EFI_BUFFER_TOO_SMALL;
    }

    CopyMem (
      &Context->Prelinked[Context->PrelinkedSize],
      Executable,
      ExecutableSize
      );

    ZeroMem (
      &Context->Prelinked[Context->PrelinkedSize + ExecutableSize],
      AlignedExecutableSize - ExecutableSize
      );

    if (!MachoInitializeContext (&ExecutableContext, &Context->Prelinked[Context->PrelinkedSize], ExecutableSize)) {
      return EFI_INVALID_PARAMETER;
    }

    KmodAddress = PrelinkedFindKmodAddress (&ExecutableContext, Context->PrelinkedLastLoadAddress, ExecutableSize);
    if (KmodAddress == 0) {
      return EFI_INVALID_PARAMETER;
    }
  }

  //
  // Allocate Info.plist copy for XML_DOCUMENT.
  //
  TmpInfoPlist = AllocateCopyPool (InfoPlistSize, InfoPlist);
  if (TmpInfoPlist == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  InfoPlistDocument = XmlDocumentParse (TmpInfoPlist, InfoPlistSize, FALSE);
  if (InfoPlistDocument == NULL) {
    FreePool (TmpInfoPlist);
    return EFI_INVALID_PARAMETER;
  }

  InfoPlistRoot = PlistNodeCast (PlistDocumentRoot (InfoPlistDocument), PLIST_NODE_TYPE_DICT);
  if (InfoPlistRoot == NULL) {
    XmlDocumentFree (InfoPlistDocument);
    FreePool (TmpInfoPlist);
    return EFI_INVALID_PARAMETER;
  }

  Failed = FALSE;
  Failed |= XmlNodeAppend (InfoPlistRoot, "key", NULL, PRELINK_INFO_BUNDLE_PATH_KEY) == NULL;
  Failed |= XmlNodeAppend (InfoPlistRoot, "string", NULL, BundlePath) == NULL;
  if (Executable != NULL) {
    Failed |= XmlNodeAppend (InfoPlistRoot, "key", NULL, PRELINK_INFO_EXECUTABLE_RELATIVE_PATH_KEY) == NULL;
    Failed |= XmlNodeAppend (InfoPlistRoot, "string", NULL, ExecutablePath) == NULL;
    AsciiIntegerToLowerHex (ExecutableSourceAddrStr, sizeof (ExecutableSourceAddrStr), Context->PrelinkedLastAddress);
    Failed |= XmlNodeAppend (InfoPlistRoot, "key", NULL, PRELINK_INFO_EXECUTABLE_SOURCE_ADDR_KEY) == NULL;
    Failed |= XmlNodeAppend (InfoPlistRoot, "integer", PRELINK_INFO_INTEGER_ATTRIBUTES, ExecutableSourceAddrStr) == NULL;
    AsciiIntegerToLowerHex (ExecutableLoadAddrStr, sizeof (ExecutableLoadAddrStr), Context->PrelinkedLastLoadAddress);
    Failed |= XmlNodeAppend (InfoPlistRoot, "key", NULL, PRELINK_INFO_EXECUTABLE_LOAD_ADDR_KEY) == NULL;
    Failed |= XmlNodeAppend (InfoPlistRoot, "integer", PRELINK_INFO_INTEGER_ATTRIBUTES, ExecutableLoadAddrStr) == NULL;
    AsciiIntegerToLowerHex (KmodInfoStr, sizeof (KmodInfoStr), KmodAddress);
    Failed |= XmlNodeAppend (InfoPlistRoot, "key", NULL, PRELINK_INFO_KMOD_INFO_KEY) == NULL;
    Failed |= XmlNodeAppend (InfoPlistRoot, "integer", PRELINK_INFO_INTEGER_ATTRIBUTES, KmodInfoStr) == NULL;  
  }

  if (Failed) {
    XmlDocumentFree (InfoPlistDocument);
    FreePool (TmpInfoPlist);
    return EFI_OUT_OF_RESOURCES;
  }

  if (Executable != NULL) {
    AlignedLoadSize = Context->PrelinkedAllocSize - Context->PrelinkedSize;
    PrelinkedKext = InternalLinkPrelinkedKext (
      Context,
      &ExecutableContext,
      InfoPlistRoot,
      Context->PrelinkedLastLoadAddress,
      KmodAddress,
      &AlignedLoadSize
      );

    if (PrelinkedKext != NULL) {
      if (AlignedLoadSize < AlignedExecutableSize) {
        AlignedLoadSize = AlignedExecutableSize;
      }

      AsciiIntegerToLowerHex (ExecutableSizeStr, sizeof (ExecutableSizeStr), AlignedLoadSize);
      Failed |= XmlNodeAppend (InfoPlistRoot, "key", NULL, PRELINK_INFO_EXECUTABLE_SIZE_KEY) == NULL;
      Failed |= XmlNodeAppend (InfoPlistRoot, "integer", PRELINK_INFO_INTEGER_ATTRIBUTES, ExecutableSizeStr) == NULL;
    }

    if (PrelinkedKext == NULL || Failed) {
      XmlDocumentFree (InfoPlistDocument);
      FreePool (TmpInfoPlist);
      return EFI_INVALID_PARAMETER;
    }

    ZeroMem (
      &Context->Prelinked[Context->PrelinkedSize + AlignedExecutableSize],
      AlignedLoadSize - AlignedExecutableSize
      );

    //
    // XNU assumes that load size and source size are same, so we should append
    // whatever is bigger to all sizes.
    //
    Context->PrelinkedSize                  += AlignedLoadSize;
    Context->PrelinkedLastAddress           += AlignedLoadSize;
    Context->PrelinkedLastLoadAddress       += AlignedLoadSize;
    Context->PrelinkedTextSegment->Size     += AlignedLoadSize;
    Context->PrelinkedTextSegment->FileSize += AlignedLoadSize;
    Context->PrelinkedTextSection->Size     += AlignedLoadSize;
  }

  //
  // Strip outer plist & dict.
  //
  NewInfoPlist = XmlDocumentExport (InfoPlistDocument, &NewInfoPlistSize, 2);

  XmlDocumentFree (InfoPlistDocument);
  FreePool (TmpInfoPlist);

  if (NewInfoPlist == NULL) {
    if (PrelinkedKext != NULL) {
      InternalFreePrelinkedKext (PrelinkedKext);
    }
    return EFI_OUT_OF_RESOURCES;
  }

  Status = PrelinkedDependencyInsert (Context, NewInfoPlist);
  if (EFI_ERROR (Status)) {
    FreePool (NewInfoPlist);
    if (PrelinkedKext != NULL) {
      InternalFreePrelinkedKext (PrelinkedKext);
    }
    return Status;
  }

  if (XmlNodeAppend (Context->KextList, "dict", NULL, NewInfoPlist) == NULL) {
    if (PrelinkedKext != NULL) {
      InternalFreePrelinkedKext (PrelinkedKext);
    }
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // Let other kexts depend on this one.
  //
  if (PrelinkedKext != NULL) {
    InsertTailList (&Context->PrelinkedKexts, &PrelinkedKext->Link);
  }

  return EFI_SUCCESS;
}
