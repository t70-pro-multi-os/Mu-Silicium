#include "CaptureBmp.h"

static VOID Put16(UINT8 *P, UINT16 V) { P[0]=(UINT8)V; P[1]=(UINT8)(V>>8); }
static VOID Put32(UINT8 *P, UINT32 V) {
  P[0]=(UINT8)V; P[1]=(UINT8)(V>>8); P[2]=(UINT8)(V>>16); P[3]=(UINT8)(V>>24);
}

static BOOLEAN UsableGop(EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop) {
  return Gop!=NULL && Gop->Blt!=NULL && Gop->Mode!=NULL && Gop->Mode->Info!=NULL &&
    Gop->Mode->SizeOfInfo>=sizeof(*Gop->Mode->Info);
}

static BOOLEAN SameDisplay(EFI_GRAPHICS_OUTPUT_PROTOCOL *Left, EFI_GRAPHICS_OUTPUT_PROTOCOL *Right) {
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *A=Left->Mode, *B=Right->Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *I=A->Info, *J=B->Info;
  if (Left==Right) return TRUE;
  if (!A->FrameBufferBase || !A->FrameBufferSize ||
      A->FrameBufferBase!=B->FrameBufferBase || A->FrameBufferSize!=B->FrameBufferSize ||
      I->HorizontalResolution!=J->HorizontalResolution || I->VerticalResolution!=J->VerticalResolution ||
      I->PixelsPerScanLine!=J->PixelsPerScanLine || I->PixelFormat!=J->PixelFormat ||
      I->PixelFormat>=PixelBltOnly) return FALSE;
  return I->PixelFormat!=PixelBitMask ||
    (I->PixelInformation.RedMask==J->PixelInformation.RedMask &&
     I->PixelInformation.GreenMask==J->PixelInformation.GreenMask &&
     I->PixelInformation.BlueMask==J->PixelInformation.BlueMask &&
     I->PixelInformation.ReservedMask==J->PixelInformation.ReservedMask);
}

static EFI_STATUS FindCaptureGop(EFI_BOOT_SERVICES *Bs, EFI_GRAPHICS_OUTPUT_PROTOCOL **Selected) {
  EFI_GUID Protocol=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  EFI_HANDLE *Handles=NULL;
  UINTN Count=0;
  EFI_STATUS Status;
  if (Bs->LocateHandleBuffer==NULL || Bs->HandleProtocol==NULL) return EFI_UNSUPPORTED;
  Status=Bs->LocateHandleBuffer(ByProtocol,&Protocol,NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return Status==EFI_NOT_FOUND?EFI_UNSUPPORTED:Status;
  if (Handles==NULL) return EFI_UNSUPPORTED;
  Status=Count>32?EFI_NO_MAPPING:EFI_UNSUPPORTED;
  for (UINTN I=0; I<Count && Count<=32; I++) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Candidate=NULL;
    if (EFI_ERROR(Bs->HandleProtocol(Handles[I],&Protocol,(VOID **)&Candidate)) ||
        !UsableGop(Candidate)) continue;
    if (*Selected!=NULL && !SameDisplay(*Selected,Candidate)) {
      *Selected=NULL;
      Status=EFI_NO_MAPPING;
      break;
    }
    *Selected=Candidate;
    Status=EFI_SUCCESS;
  }
  Bs->FreePool(Handles);
  return Status;
}

EFI_STATUS CaptureBmp(EFI_BOOT_SERVICES *Bs, EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
                      VOID **Buffer, UINTN *Size)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels=NULL;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  UINT8 *Bmp=NULL;
  UINTN Width, Height, Stride, PixelBytes, BmpBytes, X, Y, I;
  EFI_STATUS Status;
  if (Buffer==NULL || Size==NULL) return EFI_INVALID_PARAMETER;
  *Buffer=NULL; *Size=0;
  if (Bs==NULL || Bs->AllocatePool==NULL || Bs->FreePool==NULL) return EFI_UNSUPPORTED;
  // Prefer the caller's active console GOP. If ConOut exposes only text,
  // permit a single logical display, not an arbitrary choice across screens.
  if (Gop==NULL) {
    Status=FindCaptureGop(Bs,&Gop);
    if (EFI_ERROR(Status)) return Status;
  }
  if (!UsableGop(Gop)) return EFI_UNSUPPORTED;
  Mode=Gop->Mode; Info=Mode->Info;
  Width=Info->HorizontalResolution; Height=Info->VerticalResolution;
  if (Width==0 || Height==0 || Width>4096 || Height>4096) return EFI_BAD_BUFFER_SIZE;
  /* Bounds above make all arithmetic fit even a 32-bit UINTN. */
  Stride=(Width*3+3)&~(UINTN)3;
  PixelBytes=Width*Height*sizeof(*Pixels);
  BmpBytes=54+Stride*Height;
  if (PixelBytes+BmpBytes > 128U*1024U*1024U) return EFI_BAD_BUFFER_SIZE;
  Status=Bs->AllocatePool(EfiBootServicesData,PixelBytes,(VOID **)&Pixels);
  if (EFI_ERROR(Status)) return Status;
  Status=Bs->AllocatePool(EfiBootServicesData,BmpBytes,(VOID **)&Bmp);
  if (EFI_ERROR(Status)) { Bs->FreePool(Pixels); return Status; }
  /* GOP normalizes arbitrary framebuffer formats into B,G,R,Reserved pixels.
     Delta is our tightly packed capture buffer stride, not hardware scanline. */
  Status=Gop->Blt(Gop,Pixels,EfiBltVideoToBltBuffer,0,0,0,0,Width,Height,
                  Width*sizeof(*Pixels));
  if (!EFI_ERROR(Status) && (Gop->Mode!=Mode || Mode->Info!=Info ||
      Info->HorizontalResolution!=Width || Info->VerticalResolution!=Height)) {
    Status=EFI_ABORTED;
  }
  if (EFI_ERROR(Status)) { Bs->FreePool(Bmp); Bs->FreePool(Pixels); return Status; }
  for (I=0; I<BmpBytes; I++) Bmp[I]=0;
  Bmp[0]='B'; Bmp[1]='M';
  Put32(Bmp+2,(UINT32)BmpBytes); Put32(Bmp+10,54); Put32(Bmp+14,40);
  Put32(Bmp+18,(UINT32)Width); Put32(Bmp+22,(UINT32)Height);
  Put16(Bmp+26,1); Put16(Bmp+28,24); Put32(Bmp+34,(UINT32)(Stride*Height));
  for (Y=0; Y<Height; Y++) {
    UINT8 *Row=Bmp+54+(Height-1-Y)*Stride;
    for (X=0; X<Width; X++) {
      EFI_GRAPHICS_OUTPUT_BLT_PIXEL *P=&Pixels[Y*Width+X];
      Row[X*3]=P->Blue; Row[X*3+1]=P->Green; Row[X*3+2]=P->Red;
    }
  }
  Bs->FreePool(Pixels);
  *Buffer=Bmp; *Size=BmpBytes;
  return EFI_SUCCESS;
}
