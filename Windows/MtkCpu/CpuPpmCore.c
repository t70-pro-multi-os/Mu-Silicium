// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "CpuPpmCore.h"
#include <limits.h>

static int Digit(uint16_t C) { return C>='0'&&C<='9'; }
static uint16_t Upper(uint16_t C) { return C>='a'&&C<='z'?(uint16_t)(C-'a'+'A'):C; }
static int Match(const uint16_t *S,uint32_t N,uint32_t At,const char *Text,uint32_t Length) {
  if(At>N||Length>N-At)return 0;
  for(uint32_t I=0;I<Length;I++)if(Upper(S[At+I])!=(uint16_t)Text[I])return 0;
  return 1;
}
int T70CpuPpmBuild(const uint32_t FrequenciesKhz[T70_CPU_PPM_DOMAINS][T70_CPU_PPM_MAX_OPPS],
  const uint32_t Counts[T70_CPU_PPM_DOMAINS],const uint32_t CapacityDmipsMhz[T70_CPU_PPM_DOMAINS],
  T70_CPU_PPM_DOMAIN Output[T70_CPU_PPM_DOMAINS]) {
  if(!FrequenciesKhz||!Counts||!CapacityDmipsMhz||!Output)return 0;
  T70_CPU_PPM_DOMAIN Result[T70_CPU_PPM_DOMAINS]={{0}};
  for(uint32_t D=0;D<T70_CPU_PPM_DOMAINS;D++) {
    if(!Counts[D]||Counts[D]>T70_CPU_PPM_MAX_OPPS||!CapacityDmipsMhz[D])return 0;
    Result[D].Count=Counts[D];
    for(uint32_t I=0;I<Counts[D];I++) {
      uint32_t F=FrequenciesKhz[D][I];
      uint64_t Scaled;
      if(!F||(I&&F>=FrequenciesKhz[D][I-1]))return 0;
      Scaled=((uint64_t)F*CapacityDmipsMhz[D]+500U)/1000U;
      if(!Scaled||Scaled>UINT32_MAX||(I&&Scaled>=Result[D].Performance[I-1]))return 0;
      Result[D].FrequencyKhz[I]=F;
      Result[D].Performance[I]=(uint32_t)Scaled;
    }
    Result[D].HighestPerformance=Result[D].Performance[0];
    // The stock table exposes no separate boost or nonlinear thresholds.
    Result[D].NominalPerformance=Result[D].HighestPerformance;
    Result[D].LowestPerformance=Result[D].Performance[Counts[D]-1U];
    Result[D].LowestNonlinearPerformance=Result[D].LowestPerformance;
  }
  for(uint32_t D=0;D<T70_CPU_PPM_DOMAINS;D++)Output[D]=Result[D];
  return 1;
}
int T70CpuPpmSelect(const T70_CPU_PPM_DOMAIN *Domain,uint32_t Minimum,
  uint32_t Desired,uint32_t Maximum,uint32_t *FrequencyKhz) {
  uint32_t Best=UINT32_MAX,BestDistance=UINT32_MAX;
  if(!Domain||!FrequencyKhz||!Domain->Count||Domain->Count>T70_CPU_PPM_MAX_OPPS||
      Minimum>Maximum||Domain->LowestPerformance>Domain->HighestPerformance)return 0;
  if(Desired<Minimum)Desired=Minimum;
  if(Desired>Maximum)Desired=Maximum;

  for(uint32_t I=0;I<Domain->Count;I++) {
    uint32_t Perf=Domain->Performance[I];
    if(Perf<Minimum||Perf>Maximum)continue;
    uint32_t Distance=Perf>Desired?Perf-Desired:Desired-Perf;
    if(Best==UINT32_MAX || Distance<BestDistance ||
       (Distance==BestDistance&&Perf<Domain->Performance[Best])) {
      Best=I;BestDistance=Distance;
    }
  }
  if(Best==UINT32_MAX)return 0;
  *FrequencyKhz=Domain->FrequencyKhz[Best];
  return 1;
}

int T70CpuPpmUidFromDeviceId(const uint16_t *Id,uint32_t Characters,uint32_t *Uid) {
  uint32_t Found=UINT32_MAX,Length=Characters,LastSlash=UINT32_MAX;
  if(!Id||!Uid)return 0;
  while(Length&&Id[Length-1U]==0U)Length--;
  for(uint32_t I=0;I<Length;I++) {
    if(Id[I]=='\\')LastSlash=I;
    if(I+4U<=Length&&Match(Id,Length,I,"CPU",3U)&&Digit(Id[I+3U])) {
      uint32_t Value=0,J=I+3U;
      while(J<Length&&Digit(Id[J])) {
        Value=Value*10U+(uint32_t)(Id[J]-'0');
        if(Value>7U)return 0;
        J++;
      }
      if(J<Length&&Id[J]!=0U&&Id[J]!='\\'&&Id[J]!='.'&&Id[J]!='&'&&Id[J]!='#')continue;
      if(Found!=UINT32_MAX&&Found!=Value)return 0;
      Found=Value;
    }
  }
  if(Found==UINT32_MAX) {
    int HasHid=0;
    for(uint32_t I=0;I+8U<=Length;I++)if(Match(Id,Length,I,"ACPI0007",8U)){HasHid=1;break;}
    if(HasHid&&LastSlash!=UINT32_MAX&&LastSlash+1U<Length) {
      uint32_t Value=0;
      for(uint32_t I=LastSlash+1U;I<Length;I++) {
        if(!Digit(Id[I]))return 0;
        Value=Value*10U+(uint32_t)(Id[I]-'0');
        if(Value>7U)return 0;
      }
      Found=Value;
    }
  }
  if(Found>7U)return 0;
  *Uid=Found;return 1;
}
