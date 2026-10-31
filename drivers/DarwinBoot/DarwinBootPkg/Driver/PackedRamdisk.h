// SPDX-License-Identifier: BSD-2-Clause-Patent
// Little-endian DBRDISK1: version, page size, expanded bytes, extent count;
// then sorted (destination offset, byte count) pairs and concatenated bytes.
// Omitted ranges are real zeroed RAM, not virtual/simulated disk storage.
#ifndef DARWIN_BOOT_PACKED_RAMDISK_H
#define DARWIN_BOOT_PACKED_RAMDISK_H

// A packed disk handed to the native sparse block driver keeps only its nonzero pages (KeepPacked), so its logical size is bounded by the
// disk format, not by memory; a dense expansion still has to fit the window, which the caller checks.
#define SIZE_MAX_LOGICAL  (1ULL << 40)

STATIC
BOOLEAN
RamdiskLayout (CONST VOID *Input, UINTN InputSize, UINTN *Expanded, BOOLEAN *Packed)
{
  CONST UINT8 *Bytes = Input;
  UINT64 Logical, Count, Table, Cursor, End, Index, Offset, Length;
  *Expanded = InputSize;
  *Packed = FALSE;
  if ((InputSize > SIZE_256MB) || ((Input == NULL) != (InputSize == 0))) { return FALSE; }
  if ((InputSize < 8) || (CompareMem (Input, "DBRDISK1", 8) != 0)) { return TRUE; }
  if ((InputSize < 32) || (ReadUnaligned32 ((CONST UINT32 *)(Bytes + 8)) != 1) ||
      (ReadUnaligned32 ((CONST UINT32 *)(Bytes + 12)) != EFI_PAGE_SIZE)) { return FALSE; }
  Logical = ReadUnaligned64 ((CONST UINT64 *)(Bytes + 16));
  Count = ReadUnaligned64 ((CONST UINT64 *)(Bytes + 24));
  if ((Logical == 0) || (Logical > SIZE_MAX_LOGICAL) || (Logical & (EFI_PAGE_SIZE - 1)) ||
      (Count > 65536) || (Count > (InputSize - 32) / 16)) { return FALSE; }
  Table = 32 + Count * 16;
  Cursor = Table;
  End = 0;
  for (Index = 0; Index < Count; Index++) {
    Offset = ReadUnaligned64 ((CONST UINT64 *)(Bytes + 32 + Index * 16));
    Length = ReadUnaligned64 ((CONST UINT64 *)(Bytes + 40 + Index * 16));
    if (!Length || ((Offset | Length) & (EFI_PAGE_SIZE - 1)) || (Offset < End) ||
        (Offset > Logical) || (Length > Logical - Offset) || (Length > InputSize - Cursor)) { return FALSE; }
    End = Offset + Length;
    Cursor += Length;
  }
  if (Cursor != InputSize) { return FALSE; }
  *Expanded = (UINTN)Logical;
  *Packed = TRUE;
  return TRUE;
}

STATIC
VOID
CopyRamdisk (VOID *Destination, CONST VOID *Input, UINTN InputSize, BOOLEAN Packed)
{
  CONST UINT8 *Bytes = Input;
  UINT64 Count, Cursor, Index, Offset, Length;
  // Caller has validated every range and zeroed the entire expanded allocation.
  if (!Packed) { CopyMem (Destination, Input, InputSize); return; }
  Count = ReadUnaligned64 ((CONST UINT64 *)(Bytes + 24));
  Cursor = 32 + Count * 16;
  for (Index = 0; Index < Count; Index++) {
    Offset = ReadUnaligned64 ((CONST UINT64 *)(Bytes + 32 + Index * 16));
    Length = ReadUnaligned64 ((CONST UINT64 *)(Bytes + 40 + Index * 16));
    CopyMem ((UINT8 *)Destination + Offset, Bytes + Cursor, (UINTN)Length);
    Cursor += Length;
  }
}
#endif
