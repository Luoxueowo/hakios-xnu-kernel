/**
  XNU loader, stage 0: diagnostics only.

  Runs when BDS finds nothing to boot. If the payload behind the UEFI image
  carries "loader.efi", that loader is started from memory. Otherwise this
  collects what the real loader will need (exception level, CPU feature
  registers, framebuffer, memory map, payload), writes it as text to the raw
  "logdump" partition so it can be read back from Android over adb, and
  cold-resets. Nothing is shown to the user on purpose: the phone is driven
  unattended.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/PartitionInfo.h>

#include "XnuLoader.h"

#define LOG_SIZE  SIZE_256KB

STATIC CHAR8  *mLog;
STATIC UINTN   mLogLen;
STATIC BOOLEAN mLogOpen;

//
// Early trace: a line a stage, written by LBA to the first block of the logdump partition. FindPartition ()
// below needs the firmware to have published the partitions, which the vendor's DiskIoDxe does not do, so a
// log written through it never lands; this does not need them, and tells a run that never got as far as the
// loader (or whose screen stays empty) how far BDS got. The LBA is logdump's start on the phone's GPT.
//
#define TRACE_BYTE  (49152ULL * 4096)   // where logdump starts on the flash

STATIC CHAR8                 *mTrace;
STATIC UINTN                  mTraceLen;
STATIC EFI_BLOCK_IO_PROTOCOL *mTraceBio;
STATIC UINT64                 mTraceLba;     // TRACE_BYTE in the unit's own blocks

STATIC
VOID
Trace (
  IN CONST CHAR8 *Line)
{
  EFI_STATUS              Status;
  UINTN                   Index, Count;
  EFI_HANDLE              *Handles;
  EFI_BLOCK_IO_PROTOCOL   *Bio;

  if (mTrace == NULL) {
    mTrace = AllocateZeroPool (4096);
    if (mTrace == NULL) {
      return;
    }
  }

  if (mTraceBio == NULL) {
    Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
    if (EFI_ERROR (Status)) {
      return;
    }
    for (Index = 0; Index < Count; Index++) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&Bio);
      if (EFI_ERROR (Status) || Bio->Media == NULL) {
        continue;
      }
      // the whole unit (128 GB), not a partition
      if (Bio->Media->BlockSize == 0 || (TRACE_BYTE % Bio->Media->BlockSize) != 0 ||
          MultU64x32 (Bio->Media->LastBlock + 1, Bio->Media->BlockSize) < (64ULL << 30)) {
        continue;
      }
      mTraceBio = Bio;
      mTraceLba = TRACE_BYTE / Bio->Media->BlockSize;
      break;
    }
    gBS->FreePool (Handles);
    if (mTraceBio == NULL) {
      return;
    }
  }

  if (mTraceLen >= 4096 - 2) {
    mTraceLen = 0;
    ZeroMem (mTrace, 4096);
  }
  while (*Line != 0 && mTraceLen < 4096 - 2) {
    mTrace[mTraceLen++] = *Line++;
  }
  mTrace[mTraceLen++] = '\n';
  mTraceBio->WriteBlocks (mTraceBio, mTraceBio->Media->MediaId, mTraceLba, 4096, mTrace);
}

STATIC
VOID
EFIAPI
Log (
  IN CONST CHAR8 *Format,
  ...)
{
  VA_LIST Marker;

  if (mLog == NULL || mLogLen >= LOG_SIZE - 512) {
    return;
  }

  VA_START (Marker, Format);
  mLogLen += AsciiVSPrint (mLog + mLogLen, LOG_SIZE - mLogLen, Format, Marker);
  VA_END (Marker);
}

#define READ_SYSREG(Name) ({ UINT64 _Value; __asm__ volatile ("mrs %0, " #Name : "=r" (_Value)); _Value; })

STATIC
EFI_BLOCK_IO_PROTOCOL *
FindPartition (
  IN CONST CHAR16 *Name)
{
  EFI_STATUS  Status;
  EFI_HANDLE *Handles;
  UINTN       Count;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  for (UINTN i = 0; i < Count; i++) {
    EFI_PARTITION_INFO_PROTOCOL *Info;
    EFI_BLOCK_IO_PROTOCOL       *BlockIo;

    Status = gBS->HandleProtocol (Handles[i], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (EFI_ERROR (Status) || Info->Type != PARTITION_TYPE_GPT) {
      continue;
    }

    if (StrnCmp (Info->Info.Gpt.PartitionName, Name, 36) != 0) {
      continue;
    }

    Status = gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (!EFI_ERROR (Status)) {
      return BlockIo;
    }
  }

  return NULL;
}

STATIC
VOID
LogCpu ()
{
  Log ("[cpu]\n");
  Log ("CurrentEL=%lx\n", READ_SYSREG (CurrentEL) >> 2);
  Log ("MIDR_EL1=%016lx\n", READ_SYSREG (MIDR_EL1));
  Log ("ID_AA64MMFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR0_EL1));
  Log ("ID_AA64MMFR1_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR1_EL1));
  Log ("ID_AA64MMFR2_EL1=%016lx\n", READ_SYSREG (ID_AA64MMFR2_EL1));
  Log ("ID_AA64PFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64PFR0_EL1));
  Log ("ID_AA64PFR1_EL1=%016lx\n", READ_SYSREG (ID_AA64PFR1_EL1));
  Log ("ID_AA64ISAR0_EL1=%016lx\n", READ_SYSREG (ID_AA64ISAR0_EL1));
  Log ("ID_AA64ISAR1_EL1=%016lx\n", READ_SYSREG (ID_AA64ISAR1_EL1));
  Log ("ID_AA64DFR0_EL1=%016lx\n", READ_SYSREG (ID_AA64DFR0_EL1));
  Log ("SCTLR_EL1=%016lx\n", READ_SYSREG (SCTLR_EL1));
  Log ("TCR_EL1=%016lx\n", READ_SYSREG (TCR_EL1));
  Log ("MAIR_EL1=%016lx\n", READ_SYSREG (MAIR_EL1));
  Log ("TTBR0_EL1=%016lx\n", READ_SYSREG (TTBR0_EL1));
  Log ("VBAR_EL1=%016lx\n", READ_SYSREG (VBAR_EL1));
  Log ("CNTFRQ_EL0=%ld\n", READ_SYSREG (CNTFRQ_EL0));
  Log ("MPIDR_EL1=%016lx\n", READ_SYSREG (MPIDR_EL1));
  Log ("DAIF=%lx\n", READ_SYSREG (DAIF));
}

STATIC
VOID
LogDisplay ()
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;

  Log ("[display]\n");

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Log ("gop=missing status=%r\n", Status);
    return;
  }

  Log ("fb_base=%lx fb_size=%lx\n", Gop->Mode->FrameBufferBase, (UINT64)Gop->Mode->FrameBufferSize);
  Log ("width=%d height=%d stride=%d format=%d\n",
       Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
       Gop->Mode->Info->PixelsPerScanLine, Gop->Mode->Info->PixelFormat);
}

STATIC
VOID
LogMemoryMap ()
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR *Map;
  UINTN                  MapSize = 0;
  UINTN                  MapKey;
  UINTN                  DescSize;
  UINT32                 DescVersion;

  Log ("[memmap]\n");

  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescSize, &DescVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Log ("getmemorymap status=%r\n", Status);
    return;
  }

  MapSize += SIZE_4KB;
  Map      = AllocatePool (MapSize);
  if (Map == NULL) {
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescSize, &DescVersion);
  if (EFI_ERROR (Status)) {
    Log ("getmemorymap status=%r\n", Status);
    return;
  }

  for (UINTN Offset = 0; Offset < MapSize; Offset += DescSize) {
    EFI_MEMORY_DESCRIPTOR *Desc = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);

    Log ("%016lx %016lx type=%d attr=%lx\n", Desc->PhysicalStart,
         Desc->PhysicalStart + EFI_PAGES_TO_SIZE (Desc->NumberOfPages), Desc->Type, Desc->Attribute);
  }

  FreePool (Map);
}

STATIC
VOID
LogPayload ()
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;

  Log ("[payload]\n");
  Log ("base=%lx magic=%08x size=%x\n", (UINT64)XNU_PAYLOAD_BASE, Header->Magic, Header->Size);

}

STATIC
VOID
LogPersistence ()
{
  //
  // Does RAM written before a reset survive it? The kernel bring-up needs a
  // place for progress markers. Uses the tail of the reserved payload region.
  // (Device registers are deliberately not probed here: an MMIO read of the
  // IMEM / watchdog blocks took a synchronous exception on this platform.)
  //
  UINT32 *Ram = (UINT32 *)(UINTN)(XNU_PAYLOAD_BASE + XNU_PAYLOAD_MAX_SIZE - SIZE_4KB);

  Log ("[persistence]\n");
  Log ("ram_before=%08x %08x\n", Ram[0], Ram[1]);

  Ram[1] = (Ram[0] == XNU_MARKER_MAGIC) ? Ram[1] + 1 : 1;
  Ram[0] = XNU_MARKER_MAGIC;

  Log ("ram_after=%08x %08x\n", Ram[0], Ram[1]);
}

STATIC
VOID
FlushLog ()
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  UINTN                  BlockSize;
  UINTN                  WriteSize;

  BlockIo = FindPartition (XNU_LOG_PARTITION);
  if (BlockIo == NULL) {
    return;
  }

  BlockSize = BlockIo->Media->BlockSize;
  WriteSize = ALIGN_VALUE (mLogLen + 1, BlockSize);

  Status = BlockIo->WriteBlocks (BlockIo, BlockIo->Media->MediaId, 0, WriteSize, mLog);
  if (!EFI_ERROR (Status)) {
    BlockIo->FlushBlocks (BlockIo);
  }
}

STATIC
VOID
LogOpen ()
{
  EFI_TIME  Time;

  if (mLogOpen) {
    return;
  }

  mLog = AllocateZeroPool (LOG_SIZE);
  if (mLog == NULL) {
    return;
  }

  mLogOpen = TRUE;
  Log ("== XNULOADER LOG v1 ==\n");
  if (!EFI_ERROR (gRT->GetTime (&Time, NULL))) {
    Log ("time=%04d-%02d-%02d %02d:%02d:%02d\n", Time.Year, Time.Month, Time.Day, Time.Hour, Time.Minute, Time.Second);
  }
}

STATIC
VOID
LogPayloadEntries ()
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;
  UINT8              *Cursor;
  UINT8              *End;

  Log ("[entries]\n");
  if (Header->Magic != XNU_PAYLOAD_MAGIC || Header->Size > XNU_PAYLOAD_MAX_SIZE) {
    Log ("header rejected\n");
    return;
  }

  Cursor = (UINT8 *)(Header + 1);
  End    = Cursor + Header->Size;

  while (Cursor + sizeof (XNU_PAYLOAD_ENTRY) <= End) {
    XNU_PAYLOAD_ENTRY *Entry = (XNU_PAYLOAD_ENTRY *)Cursor;

    if (Entry->Name[0] == 0) {
      break;
    }

    Log ("%a %x\n", Entry->Name, Entry->Size);
    Cursor += sizeof (XNU_PAYLOAD_ENTRY) + ALIGN_VALUE (Entry->Size, 16);
  }
}

//
// The payload is a list of named blobs. If it carries "loader.efi", that is
// the real XNU loader: start it straight from memory, so the loader can be
// iterated on without rebuilding this firmware.
//
STATIC
EFI_STATUS
StartPayloadLoader ()
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)XNU_PAYLOAD_BASE;
  UINT8              *Cursor;
  UINT8              *End;

  if (Header->Magic != XNU_PAYLOAD_MAGIC || Header->Size > XNU_PAYLOAD_MAX_SIZE) {
    Trace ("bds: no payload in memory");
    return EFI_NOT_FOUND;
  }

  Cursor = (UINT8 *)(Header + 1);
  End    = Cursor + Header->Size;

  while (Cursor + sizeof (XNU_PAYLOAD_ENTRY) <= End) {
    XNU_PAYLOAD_ENTRY *Entry = (XNU_PAYLOAD_ENTRY *)Cursor;
    EFI_STATUS         Status;
    EFI_HANDLE         Handle;

    if (Entry->Name[0] == 0) {
      break;
    }

    if (AsciiStrnCmp (Entry->Name, "loader.efi", sizeof (Entry->Name)) == 0) {
      Trace ("bds: loader.efi found");
      Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Entry + 1, Entry->Size, &Handle);
      if (EFI_ERROR (Status)) {
        Trace (Status == EFI_SECURITY_VIOLATION ? "bds: LoadImage refused (security)" :
               "bds: LoadImage failed");
        return Status;
      }
      Trace ("bds: image loaded, starting it");
      Status = gBS->StartImage (Handle, NULL, NULL);
      Trace (Status == EFI_SECURITY_VIOLATION ? "bds: StartImage refused (security)" :
             "bds: StartImage returned");
      return Status;
    }

    Cursor += sizeof (XNU_PAYLOAD_ENTRY) + ALIGN_VALUE (Entry->Size, 16);
  }

  return EFI_NOT_FOUND;
}

VOID
XnuLoaderStartPayload ()
{
  EFI_STATUS  Status;

  //
  // BDS prints one line on the screen; the appending payload can only be
  // inspected from here. Everything the hand-over depends on goes to the log
  // partition first, so a run that never reaches the loader's own log is
  // still readable from recovery.
  //
  // This is called before BDS has connected the storage, so nothing here can be
  // written to a partition: EfiBootManagerConnectAll () at this point is what a
  // phone with the vendor's DiskIoDxe resets in -- and the log it was there for
  // comes from the loader itself at the end of its run anyway. The payload is
  // started first; only a payload that came back is worth writing about.
  //
  Trace ("bds: priority boot hook");
  LogOpen ();
  Log ("[stage] bds hook\n");
  LogPayload ();
  LogPayloadEntries ();
  LogDisplay ();
  FlushLog ();

  Status = StartPayloadLoader ();
  Log ("[loader] returned status=%r\n", Status);
  FlushLog ();
}

VOID
XnuLoaderMain ()
{
  EFI_STATUS  Status;

  Trace ("bds: unable-to-boot hook");
  LogOpen ();
  Log ("[stage] unable to boot\n");
  FlushLog ();

  // Normally does not return
  Status = StartPayloadLoader ();
  Log ("[loader] returned status=%r\n", Status);
  FlushLog ();

  if (mLogOpen) {
    LogCpu ();
    FlushLog ();
    LogDisplay ();
    FlushLog ();
    LogMemoryMap ();
    FlushLog ();
    LogPayload ();
    FlushLog ();
    LogPersistence ();
    Log ("== END ==\n");
    FlushLog ();
  }

  // The phone is unattended: always come back to Android.
  gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
}
