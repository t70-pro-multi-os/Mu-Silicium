// SPDX-License-Identifier: BSD-2-Clause-Patent
// Link the actual kmscon PTY, XKB and Unifont code. No DRM/evdev device open.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <errno.h>
#include <libtsm.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "eloop.h"
#include "font.h"
#include "pty.h"
#include "uterm_input_internal.h"
#include "uterm_vt.h"
static unsigned Checks;
#define CHECK(X) do { ++Checks;if(!(X)){fprintf(stderr,"%u: %s\n",__LINE__,#X);abort();} } while(0)
static struct tsm_vte *Vte;
static struct kmscon_pty *Pty;
static char Typed[256],Output[8192];
static size_t TypedBytes,OutputBytes;
static int Eof,Activated;
static unsigned Mode;
static void Write(struct tsm_vte *V,const char *B,size_t N,void *User) {
  (void)V;(void)User;
  if (Mode==1) { CHECK(!kmscon_pty_write(Pty,B,N));return; }
  CHECK(N<=sizeof(Typed)-TypedBytes);
  memcpy(Typed+TypedBytes,B,N);TypedBytes+=N;
}
static void Read(struct kmscon_pty *P,const char *B,size_t N,void *User) {
  (void)P;(void)User;
  if (!N) { Eof=1;return; }
  CHECK(N<sizeof(Output)-OutputBytes);
  memcpy(Output+OutputBytes,B,N);OutputBytes+=N;Output[OutputBytes]=0;
  tsm_vte_input(Vte,B,N);
}
static void Key(struct uterm_input *I,struct uterm_input_key_event *E,void *User) {
  (void)I;(void)User;
  if (E->num_syms==1)
    E->handled=tsm_vte_handle_keyboard(Vte,E->keysyms[0],E->ascii,E->mods,E->codepoints[0]);
}
static void Press(struct uterm_input_dev *D,unsigned Keycode) {
  CHECK(!uxkb_dev_process(D,1,Keycode));
  CHECK(uxkb_dev_process(D,0,Keycode)==-ENOKEY);
}
static int VtEvent(struct uterm_vt *V,struct uterm_vt_event *E,void *User) {
  (void)V;(void)User;
  if (E->action==UTERM_VT_ACTIVATE) Activated++;
  return 0;
}
struct Cell { uint32_t Ch; unsigned Width; struct tsm_screen_attr Attr; };
static struct Cell Cells[8][80];
static int Draw(struct tsm_screen *S,uint64_t Id,const uint32_t *C,size_t N,
  unsigned W,unsigned X,unsigned Y,const struct tsm_screen_attr *A,tsm_age_t Age,void *User) {
  (void)S;(void)Id;(void)Age;(void)User;
  CHECK(X<80&&Y<8);
  Cells[Y][X]=(struct Cell){N?C[0]:0,W,*A};return 0;
}
static void ReleaseDevice(struct uterm_input_dev *D) {
  uxkb_dev_destroy(D);
  free(D->event.keysyms);free(D->event.codepoints);
  free(D->repeat_event.keysyms);free(D->repeat_event.codepoints);
}
int main(int Argc,char **Argv) {
  CHECK(Argc==2);
  FILE *F=fopen(Argv[1],"rb");CHECK(F);
  CHECK(!fseek(F,0,SEEK_END));long Length=ftell(F);CHECK(Length>0&&Length<262144);
  rewind(F);char *Keymap=calloc((size_t)Length+1,1);CHECK(Keymap);
  CHECK(fread(Keymap,1,(size_t)Length,F)==(size_t)Length);CHECK(!fclose(F));
  struct ev_eloop *Loop;struct uterm_input *Input;
  CHECK(!ev_eloop_new(&Loop,NULL,NULL));
  CHECK(!uterm_input_new(&Input,Loop,"","","","","C",Keymap,"",0,20,10,NULL,NULL));
  struct tsm_screen *Screen;CHECK(!tsm_screen_new(&Screen,NULL,NULL));
  CHECK(!tsm_screen_resize(Screen,80,8));tsm_screen_set_max_sb(Screen,32);
  CHECK(!tsm_vte_new(&Vte,Screen,Write,NULL,NULL,NULL));
  struct uterm_input_dev D={.input=Input,.rfd=-1};
  CHECK(!uxkb_dev_init(&D));CHECK(!uterm_input_register_key_cb(Input,Key,NULL));
  Press(&D,KEY_A);CHECK(TypedBytes==1&&Typed[0]=='a');
  CHECK(!uxkb_dev_process(&D,1,KEY_LEFTSHIFT));Press(&D,KEY_A);
  CHECK(uxkb_dev_process(&D,0,KEY_LEFTSHIFT)==-ENOKEY);
  CHECK(TypedBytes==2&&Typed[1]=='A');
  CHECK(!uxkb_dev_process(&D,1,KEY_LEFTCTRL));Press(&D,KEY_C);
  CHECK(uxkb_dev_process(&D,0,KEY_LEFTCTRL)==-ENOKEY);
  CHECK(TypedBytes==3&&Typed[2]==3);
  TypedBytes=0;Press(&D,KEY_UP);CHECK(TypedBytes==3&&!memcmp(Typed,"\033[A",3));
  TypedBytes=0;Press(&D,KEY_BACKSPACE);CHECK(TypedBytes==1&&(Typed[0]==127||Typed[0]==8));
  CHECK(uxkb_dev_process(&D,2,KEY_A)==-ENOKEY);
  TypedBytes=0;CHECK(!uxkb_dev_process(&D,1,KEY_B));
  CHECK(!ev_eloop_run(Loop,65));CHECK(TypedBytes>=3);
  CHECK(uxkb_dev_process(&D,0,KEY_B)==-ENOKEY);
  size_t Before=TypedBytes;CHECK(!ev_eloop_run(Loop,35));CHECK(TypedBytes==Before);
  struct uterm_vt_master *Master;struct uterm_vt *Vt;
  CHECK(!uterm_vt_master_new(&Master,Loop));
  CHECK(!uterm_vt_allocate(Master,&Vt,UTERM_VT_FAKE,"teclast-test-no-vt",Input,NULL,VtEvent,NULL));
  CHECK(uterm_vt_get_type(Vt)==UTERM_VT_FAKE&&uterm_vt_get_num(Vt)==0);
  CHECK(!uterm_vt_activate(Vt));CHECK(Activated==1);
  CHECK(!uterm_vt_deactivate(Vt));uterm_vt_unref(Vt);uterm_vt_master_unref(Master);
  const char Text[]="\033[2J\033[H\033[31mA\033[0m한글Z";
  // Split every UTF-8 sequence across calls to the actual VTE.
  for (size_t I=0;I<sizeof(Text)-1;I++) tsm_vte_input(Vte,Text+I,1);
  memset(Cells,0,sizeof(Cells));tsm_screen_draw(Screen,Draw,NULL);
  CHECK(Cells[0][0].Ch=='A'&&Cells[0][0].Attr.fccode==TSM_COLOR_RED);
  CHECK(Cells[0][1].Ch==0xd55c&&Cells[0][1].Width==2);
  CHECK(Cells[0][3].Ch==0xae00&&Cells[0][3].Width==2);
  CHECK(Cells[0][5].Ch=='Z');
  CHECK(!kmscon_font_register(&kmscon_font_unifont_ops));
  struct kmscon_font *Font;struct kmscon_font_attr Attr={.points=32};
  CHECK(!kmscon_font_find(&Font,&Attr,"unifont"));
  const uint32_t Glyphs[]={'A',0xd55c,0xae00};
  for (unsigned I=0;I<3;I++) {
    const struct kmscon_glyph *G;
    CHECK(!kmscon_font_render(Font,Glyphs[I],&Glyphs[I],1,&G));
    CHECK(G&&G->width==(I?2U:1U)&&G->buf.width&&G->buf.height&&G->buf.data);
    unsigned Nonzero=0;
    for (unsigned Y=0;Y<G->buf.height;Y++)
      for (unsigned X=0;X<G->buf.width;X++) Nonzero|=((unsigned char *)G->buf.data)[Y*G->buf.stride+X];
    CHECK(Nonzero);
  }
  kmscon_font_unref(Font);kmscon_font_unregister("unifont");
  // Actual controlling PTY, terminal size, line input and process lifetime.
  CHECK(!kmscon_pty_new(&Pty,Read,NULL));
  char *Command[]={"/bin/sh","-c","test -t 0 && test -t 1 || exit 41; stty size; printf READY; read line; printf '\\033[2J\\033[H[%s]한글' \"$line\"; exit 0",NULL};
  CHECK(!kmscon_pty_set_conf(Pty,"linux","kmscon",Command,"seat0",0,true,true));
  CHECK(!kmscon_pty_open(Pty,80,8,false));
  Mode=1;int Sent=0;
  for (unsigned I=0;I<300&&!Eof;I++) {
    struct pollfd Poll={kmscon_pty_get_fd(Pty),POLLIN,0};
    CHECK(poll(&Poll,1,10)>=0);kmscon_pty_dispatch(Pty);
    if (!Sent&&strstr(Output,"READY")) { Press(&D,KEY_A);Press(&D,KEY_B);Press(&D,KEY_ENTER);Sent=1; }
  }
  CHECK(Sent&&Eof);CHECK(strstr(Output,"8 80")&&strstr(Output,"[ab]한글"));
  memset(Cells,0,sizeof(Cells));tsm_screen_draw(Screen,Draw,NULL);
  CHECK(Cells[0][0].Ch=='['&&Cells[0][1].Ch=='a'&&Cells[0][2].Ch=='b'&&Cells[0][3].Ch==']');
  CHECK(Cells[0][4].Ch==0xd55c&&Cells[0][4].Width==2&&Cells[0][6].Ch==0xae00);
  kmscon_pty_close(Pty);kmscon_pty_unref(Pty);
  // The actual eloop already reaped the child before the EOF callback.
  errno=0;CHECK(waitpid(-1,NULL,WNOHANG)==-1&&errno==ECHILD);
  ReleaseDevice(&D);uterm_input_unref(Input);ev_eloop_unref(Loop);
  tsm_vte_unref(Vte);tsm_screen_unref(Screen);free(Keymap);
  printf("console: %u checks PASS; actual XKB/repeat, fake VT, UTF-8/wide glyphs and controlling PTY shell; no DRM/evdev device access\n",Checks);
  return 0;
}
