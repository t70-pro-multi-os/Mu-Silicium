#include "CaptureBmp.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static UINTN Allocations, Live, FailAt, BltCalls;
static BOOLEAN FailBlt, ChangeMode;
static EFI_STATUS EFIAPI Alloc(EFI_MEMORY_TYPE Type,UINTN Size,VOID **Out) {
  assert(Type==EfiBootServicesData); Allocations++;
  if (Allocations==FailAt) return EFI_OUT_OF_RESOURCES;
  *Out=malloc(Size); assert(*Out); Live++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free(VOID *Ptr) { assert(Ptr && Live); free(Ptr); Live--; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels,EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,
 UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta) {
  BltCalls++; assert(Op==EfiBltVideoToBltBuffer && !Sx && !Sy && !Dx && !Dy);
  assert(Delta==W*sizeof(*Pixels));
  if (FailBlt) return EFI_DEVICE_ERROR;
  for (UINTN Y=0;Y<H;Y++) for (UINTN X=0;X<W;X++) {
    Pixels[Y*W+X].Blue=(UINT8)(10+X);
    Pixels[Y*W+X].Green=(UINT8)(20+Y);
    Pixels[Y*W+X].Red=(UINT8)(30+X+Y);
    Pixels[Y*W+X].Reserved=0xff;
  }
  if (ChangeMode) This->Mode->Info->HorizontalResolution++;
  return EFI_SUCCESS;
}
static EFI_GRAPHICS_OUTPUT_PROTOCOL *Candidates[3];
static UINTN CandidateCount;
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE Search,EFI_GUID *Guid,
    VOID *Key,UINTN *Count,EFI_HANDLE **Handles) {
  EFI_GUID Expected=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  assert(Search==ByProtocol && !memcmp(Guid,&Expected,sizeof(Expected)) && !Key);
  *Count=CandidateCount;
  *Handles=malloc(sizeof(EFI_HANDLE)*(CandidateCount?CandidateCount:1));
  assert(*Handles); Live++;
  for (UINTN I=0;I<CandidateCount;I++) (*Handles)[I]=(EFI_HANDLE)&Candidates[I];
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenGop(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Interface) {
  EFI_GUID Expected=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
  assert(!memcmp(Guid,&Expected,sizeof(Expected)));
  *Interface=*(EFI_GRAPHICS_OUTPUT_PROTOCOL **)Handle;
  return *Interface?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static void Fallback(EFI_BOOT_SERVICES *Bs) {
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION A={0},B={0};
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE AM={0},BM={0};
  EFI_GRAPHICS_OUTPUT_PROTOCOL G0={0},G1={0};
  VOID *Buffer;
  UINTN Size;
  Bs->LocateHandleBuffer=Locate; Bs->HandleProtocol=OpenGop;
  A.HorizontalResolution=B.HorizontalResolution=3;
  A.VerticalResolution=B.VerticalResolution=2;
  A.PixelsPerScanLine=B.PixelsPerScanLine=3;
  AM.Info=&A; BM.Info=&B;
  AM.SizeOfInfo=BM.SizeOfInfo=sizeof(A);
  AM.FrameBufferBase=BM.FrameBufferBase=0x5f0f0000;
  AM.FrameBufferSize=BM.FrameBufferSize=0x300000;
  G0.Blt=Blt; G1.Blt=Blt; G0.Mode=&AM; G1.Mode=&BM;
  Candidates[0]=&G0; Candidates[1]=&G1; Candidates[2]=NULL;
  CandidateCount=3; Allocations=BltCalls=FailAt=0; FailBlt=ChangeMode=FALSE;
  assert(CaptureBmp(Bs,NULL,&Buffer,&Size)==EFI_SUCCESS && Buffer && Size==78);
  assert(BltCalls==1 && Live==1); Free(Buffer);
  BM.FrameBufferBase+=0x400000;
  assert(CaptureBmp(Bs,NULL,&Buffer,&Size)==EFI_NO_MAPPING && !Buffer && !Size && !Live);
  BM.FrameBufferBase=AM.FrameBufferBase; B.PixelsPerScanLine++;
  assert(CaptureBmp(Bs,NULL,&Buffer,&Size)==EFI_NO_MAPPING && BltCalls==1 && !Live);
  CandidateCount=1; AM.FrameBufferBase=0; A.PixelFormat=PixelBltOnly;
  assert(CaptureBmp(Bs,NULL,&Buffer,&Size)==EFI_SUCCESS && BltCalls==2);
  Free(Buffer);
  CandidateCount=0;
  assert(CaptureBmp(Bs,NULL,&Buffer,&Size)==EFI_UNSUPPORTED && !Live);
}
static UINT32 Get32(UINT8 *P) {return (UINT32)P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
int main(void) {
  EFI_BOOT_SERVICES Bs={0}; EFI_GRAPHICS_OUTPUT_PROTOCOL Gop={0};
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode={0}; EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info={0};
  VOID *Buffer; UINTN Size;
  Bs.AllocatePool=Alloc; Bs.FreePool=Free; Gop.Mode=&Mode; Gop.Blt=Blt;
  Mode.Info=&Info; Mode.SizeOfInfo=sizeof(Info);
  /* PixelBltOnly requires BLT, not framebuffer reads; hardware stride irrelevant. */
  Info.PixelFormat=PixelBltOnly; Info.PixelsPerScanLine=999;
  for (UINTN W=1;W<=4;W++) {
    Info.HorizontalResolution=W; Info.VerticalResolution=2;
    Allocations=BltCalls=FailAt=0; FailBlt=ChangeMode=FALSE;
    assert(CaptureBmp(&Bs,&Gop,&Buffer,&Size)==EFI_SUCCESS);
    UINT8 *B=Buffer; UINTN Stride=(W*3+3)&~(UINTN)3;
    assert(Size==54+2*Stride && B[0]=='B' && B[1]=='M');
    assert(Get32(B+2)==Size && Get32(B+10)==54 && Get32(B+14)==40);
    assert(Get32(B+18)==W && Get32(B+22)==2 && B[26]==1 && B[28]==24);
    assert(Get32(B+30)==0 && Get32(B+34)==2*Stride);
    for (UINTN Y=0;Y<2;Y++) {
      UINT8 *Row=B+54+(1-Y)*Stride;
      for (UINTN X=0;X<W;X++) {
        assert(Row[X*3]==10+X && Row[X*3+1]==20+Y && Row[X*3+2]==30+X+Y);
      }
      for (UINTN I=W*3;I<Stride;I++) assert(Row[I]==0);
    }
    assert(BltCalls==1 && Live==1); Free(Buffer); assert(!Live);
  }
  for (UINTN Scenario=0;Scenario<5;Scenario++) {
    Info.HorizontalResolution=3; Info.VerticalResolution=2;
    Allocations=BltCalls=0; FailAt=Scenario<2 ? Scenario+1:0;
    FailBlt=Scenario==2; ChangeMode=Scenario==3;
    if (Scenario==4) Info.HorizontalResolution=4097;
    Buffer=(VOID *)1; Size=123;
    assert(EFI_ERROR(CaptureBmp(&Bs,&Gop,&Buffer,&Size)));
    assert(Buffer==NULL && Size==0 && Live==0);
    if (Scenario<2 || Scenario==4) assert(BltCalls==0);
  }
  assert(CaptureBmp(&Bs,&Gop,NULL,&Size)==EFI_INVALID_PARAMETER);
  Gop.Mode=NULL;
  assert(CaptureBmp(&Bs,&Gop,&Buffer,&Size)==EFI_UNSUPPORTED);
  Fallback(&Bs);
  puts("PASS: GOP fallback, BMP header, BGR, orientation, row padding, bounds and failure cleanup");
  return 0;
}
