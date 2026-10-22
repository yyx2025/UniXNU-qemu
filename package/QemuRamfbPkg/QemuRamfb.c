/** @file
  QEMU's ramfb at a size of the board's choosing, before DarwinBoot takes the frame buffer for the kernel.

  The firmware's ramfb driver (OvmfPkg QemuRamfbDxe) offers 640x480, 800x600 and 1024x768 only, but ramfb itself scans
  out any size: the guest writes the frame buffer's address, format, size and stride to the fw_cfg file "etc/ramfb".
  This driver reads the size from the fw_cfg item opt/org.unixnu/ramfb-size ("WIDTHxHEIGHT", given by run-qemu.py
  with -fw_cfg), allocates a reserved frame buffer of that size, points ramfb at it through fw_cfg's DMA interface and
  makes it the only mode of every Graphics Output Protocol instance, with its own Blt. UniXNU's menu and DarwinBoot then
  see the new size, and DarwinBoot hands it to the kernel. Without the item it changes nothing.

  QEMU virt's fw_cfg: MMIO at 0x09020000, data at +0, selector at +8, DMA address at +16. The selector, the DMA
  address, the file directory and the DMA structures are big-endian.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>

#define FW_CFG_DATA       0x09020000ULL
#define FW_CFG_SELECTOR   0x09020008ULL
#define FW_CFG_DMA        0x09020010ULL
#define FW_CFG_FILE_DIR   0x0019
#define DMA_CTL_ERROR     0x01
#define DMA_CTL_SELECT    0x08
#define DMA_CTL_WRITE     0x10
#define DRM_XRGB8888      0x34325258   // "XR24": blue, green, red, unused in memory, as the GOP's BGR mode

#pragma pack(1)
typedef struct {
  UINT64    Address;
  UINT32    FourCC;
  UINT32    Flags;
  UINT32    Width;
  UINT32    Height;
  UINT32    Stride;
} RAMFB_CONFIG;
#pragma pack()

typedef struct {
  UINT32    Control;
  UINT32    Length;
  UINT64    Address;
} FW_CFG_DMA_ACCESS;

STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mInfo;
STATIC UINT32                                *mFrame;

STATIC
UINT32
ReadBig32 (
  VOID
  )
{
  UINT32  Value = 0;
  UINTN   Index;

  for (Index = 0; Index < 4; Index++) {
    Value = (Value << 8) | MmioRead8 (FW_CFG_DATA);
  }

  return Value;
}

/**
  Select a fw_cfg file by name; its size in *Size. 0 when there is no such file.
**/
STATIC
UINT16
FindFile (
  IN  CONST CHAR8  *Name,
  OUT UINT32       *Size
  )
{
  UINT32  Count;
  UINT32  Index;
  UINT32  FileSize;
  UINT16  Selector;
  CHAR8   FileName[56];
  UINTN   Byte;

  MmioWrite16 (FW_CFG_SELECTOR, SwapBytes16 (FW_CFG_FILE_DIR));
  Count = ReadBig32 ();
  for (Index = 0; Index < Count && Index < 1024; Index++) {
    FileSize = ReadBig32 ();
    Selector = (UINT16)((MmioRead8 (FW_CFG_DATA) << 8) | MmioRead8 (FW_CFG_DATA));
    MmioRead8 (FW_CFG_DATA);
    MmioRead8 (FW_CFG_DATA);
    for (Byte = 0; Byte < sizeof (FileName); Byte++) {
      FileName[Byte] = (CHAR8)MmioRead8 (FW_CFG_DATA);
    }

    FileName[sizeof (FileName) - 1] = 0;
    if (AsciiStrCmp (FileName, Name) == 0) {
      *Size = FileSize;
      return Selector;
    }
  }

  return 0;
}

/**
  "WIDTHxHEIGHT" of opt/org.unixnu/ramfb-size.
**/
STATIC
BOOLEAN
ReadSize (
  OUT UINT32  *Width,
  OUT UINT32  *Height
  )
{
  UINT32  Size;
  UINT16  Selector;
  CHAR8   Text[16];
  UINTN   Index;
  UINT32  *Value;

  Selector = FindFile ("opt/org.unixnu/ramfb-size", &Size);
  if ((Selector == 0) || (Size == 0) || (Size >= sizeof (Text))) {
    return FALSE;
  }

  MmioWrite16 (FW_CFG_SELECTOR, SwapBytes16 (Selector));
  for (Index = 0; Index < Size; Index++) {
    Text[Index] = (CHAR8)MmioRead8 (FW_CFG_DATA);
  }

  *Width  = 0;
  *Height = 0;
  Value   = Width;
  for (Index = 0; Index < Size; Index++) {
    if ((Text[Index] >= '0') && (Text[Index] <= '9')) {
      *Value = *Value * 10 + (UINT32)(Text[Index] - '0');
    } else if ((Text[Index] == 'x') && (Value == Width)) {
      Value = Height;
    } else if ((Text[Index] != '\n') && (Text[Index] != 0)) {
      return FALSE;
    }
  }

  return *Width >= 320 && *Height >= 240 && *Width <= 4096 && *Height <= 4096;
}

/**
  Write RAMFB_CONFIG to etc/ramfb with fw_cfg DMA (the file is not writable through the data register).
**/
STATIC
EFI_STATUS
ConfigureRamfb (
  IN UINT16                Selector,
  IN EFI_PHYSICAL_ADDRESS  Frame,
  IN UINT32                Width,
  IN UINT32                Height
  )
{
  volatile FW_CFG_DMA_ACCESS  *Access;
  RAMFB_CONFIG                *Config;
  UINT32                      Control;
  UINTN                       Wait;

  Access = AllocateZeroPool (sizeof (*Access) + sizeof (*Config));
  if (Access == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Config          = (RAMFB_CONFIG *)(Access + 1);
  Config->Address = SwapBytes64 (Frame);
  Config->FourCC  = SwapBytes32 (DRM_XRGB8888);
  Config->Flags   = 0;
  Config->Width   = SwapBytes32 (Width);
  Config->Height  = SwapBytes32 (Height);
  Config->Stride  = SwapBytes32 (Width * 4);
  Access->Length  = SwapBytes32 (sizeof (*Config));
  Access->Address = SwapBytes64 ((UINT64)(UINTN)Config);
  Access->Control = SwapBytes32 (((UINT32)Selector << 16) | DMA_CTL_SELECT | DMA_CTL_WRITE);
  MemoryFence ();
  MmioWrite32 (FW_CFG_DMA, SwapBytes32 ((UINT32)RShiftU64 ((UINT64)(UINTN)Access, 32)));
  MmioWrite32 (FW_CFG_DMA + 4, SwapBytes32 ((UINT32)(UINTN)Access));    // the low half starts the transfer
  for (Wait = 0; Wait < 100000; Wait++) {
    MemoryFence ();
    Control = SwapBytes32 (Access->Control);
    if ((Control & ~(UINT32)DMA_CTL_ERROR) == 0) {
      break;
    }

    gBS->Stall (10);
  }

  FreePool ((VOID *)Access);
  return (Control == 0) ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
EFIAPI
QueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  **Info
  )
{
  if ((ModeNumber != 0) || (SizeOfInfo == NULL) || (Info == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Info = AllocateCopyPool (sizeof (mInfo), &mInfo);
  if (*Info == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *SizeOfInfo = sizeof (mInfo);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
  IN UINT32                        ModeNumber
  )
{
  if (ModeNumber != 0) {
    return EFI_UNSUPPORTED;
  }

  ZeroMem (mFrame, (UINTN)mInfo.PixelsPerScanLine * mInfo.VerticalResolution * 4);
  return EFI_SUCCESS;
}

/**
  The four Blt operations on the frame buffer; its pixels have the BLT_PIXEL layout.
**/
STATIC
EFI_STATUS
EFIAPI
Blt (
  IN     EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer OPTIONAL,
  IN     EFI_GRAPHICS_OUTPUT_BLT_OPERATION  BltOperation,
  IN     UINTN                              SourceX,
  IN     UINTN                              SourceY,
  IN     UINTN                              DestinationX,
  IN     UINTN                              DestinationY,
  IN     UINTN                              Width,
  IN     UINTN                              Height,
  IN     UINTN                              Delta OPTIONAL
  )
{
  UINTN   Row;
  UINTN   Column;
  UINTN   Stride;
  UINT32  *Line;
  UINT8   *Buffer;

  if ((Width == 0) || (Height == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Stride = mInfo.PixelsPerScanLine;
  if (Delta == 0) {
    Delta = Width * sizeof (EFI_GRAPHICS_OUTPUT_BLT_PIXEL);
  }

  Buffer = (UINT8 *)BltBuffer;
  switch (BltOperation) {
    case EfiBltVideoFill:
      if ((BltBuffer == NULL) || (DestinationX + Width > mInfo.HorizontalResolution) ||
          (DestinationY + Height > mInfo.VerticalResolution))
      {
        return EFI_INVALID_PARAMETER;
      }

      for (Row = 0; Row < Height; Row++) {
        Line = mFrame + (DestinationY + Row) * Stride + DestinationX;
        for (Column = 0; Column < Width; Column++) {
          Line[Column] = *(UINT32 *)BltBuffer;
        }
      }

      return EFI_SUCCESS;

    case EfiBltVideoToBltBuffer:
      if ((BltBuffer == NULL) || (SourceX + Width > mInfo.HorizontalResolution) ||
          (SourceY + Height > mInfo.VerticalResolution))
      {
        return EFI_INVALID_PARAMETER;
      }

      for (Row = 0; Row < Height; Row++) {
        CopyMem (
          Buffer + (DestinationY + Row) * Delta + DestinationX * 4,
          mFrame + (SourceY + Row) * Stride + SourceX,
          Width * 4
          );
      }

      return EFI_SUCCESS;

    case EfiBltBufferToVideo:
      if ((BltBuffer == NULL) || (DestinationX + Width > mInfo.HorizontalResolution) ||
          (DestinationY + Height > mInfo.VerticalResolution))
      {
        return EFI_INVALID_PARAMETER;
      }

      for (Row = 0; Row < Height; Row++) {
        CopyMem (
          mFrame + (DestinationY + Row) * Stride + DestinationX,
          Buffer + (SourceY + Row) * Delta + SourceX * 4,
          Width * 4
          );
      }

      return EFI_SUCCESS;

    case EfiBltVideoToVideo:
      if ((SourceX + Width > mInfo.HorizontalResolution) || (SourceY + Height > mInfo.VerticalResolution) ||
          (DestinationX + Width > mInfo.HorizontalResolution) || (DestinationY + Height > mInfo.VerticalResolution))
      {
        return EFI_INVALID_PARAMETER;
      }

      // Rows in the order that never overwrites a row still to be read; CopyMem handles the overlap in a row.
      for (Row = 0; Row < Height; Row++) {
        UINTN  Index = (DestinationY > SourceY) ? Height - 1 - Row : Row;
        CopyMem (
          mFrame + (DestinationY + Index) * Stride + DestinationX,
          mFrame + (SourceY + Index) * Stride + SourceX,
          Width * 4
          );
      }

      return EFI_SUCCESS;

    default:
      return EFI_INVALID_PARAMETER;
  }
}

EFI_STATUS
EFIAPI
QemuRamfbEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_STATUS                    Status;
  UINT32                        Width;
  UINT32                        Height;
  UINT32                        Size;
  UINT16                        Selector;
  UINTN                         Bytes;
  EFI_HANDLE                    *Handles;
  UINTN                         Count;
  UINTN                         Index;

  if (!ReadSize (&Width, &Height)) {
    return EFI_SUCCESS;
  }

  // The ramfb driver's GOP and the console splitter's, which keeps its own copy of the mode and forwards Blt to the
  // first: UniXNU takes the console's, DarwinBoot the first it finds. Each one describes and draws the new frame buffer.
  Selector = FindFile ("etc/ramfb", &Size);
  Status   = gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &Count, &Handles);
  if ((Selector == 0) || EFI_ERROR (Status) || (Count == 0)) {
    return EFI_SUCCESS;
  }

  // Reserved memory: the kernel scans out from it and never takes it for itself.
  Bytes  = (UINTN)Width * Height * 4;
  mFrame = AllocateReservedPages (EFI_SIZE_TO_PAGES (Bytes));
  if (mFrame == NULL) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (mFrame, Bytes);
  Status = ConfigureRamfb (Selector, (EFI_PHYSICAL_ADDRESS)(UINTN)mFrame, Width, Height);
  if (EFI_ERROR (Status)) {
    FreePages (mFrame, EFI_SIZE_TO_PAGES (Bytes));
    FreePool (Handles);
    return Status;
  }

  mInfo.Version              = 0;
  mInfo.HorizontalResolution = Width;
  mInfo.VerticalResolution   = Height;
  mInfo.PixelFormat          = PixelBlueGreenRedReserved8BitPerColor;
  mInfo.PixelsPerScanLine    = Width;

  // QEMU now scans out from mFrame; every GOP describes and draws it from here on.
  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop)) ||
        (Gop->Mode == NULL))
    {
      continue;
    }

    Gop->QueryMode             = QueryMode;
    Gop->SetMode               = SetMode;
    Gop->Blt                   = Blt;
    Gop->Mode->MaxMode         = 1;
    Gop->Mode->Mode            = 0;
    Gop->Mode->Info            = &mInfo;
    Gop->Mode->SizeOfInfo      = sizeof (mInfo);
    Gop->Mode->FrameBufferBase = (EFI_PHYSICAL_ADDRESS)(UINTN)mFrame;
    Gop->Mode->FrameBufferSize = Bytes;
  }

  FreePool (Handles);
  return EFI_SUCCESS;
}
