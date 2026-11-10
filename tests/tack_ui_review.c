/* SPDX-License-Identifier: GPL-3.0-only
 * Real firmware framebuffer review; no fabricated UI. */
#define TACK_APP_REC_NO_MAIN 1
#include "tack_app_rec_test.c"
static void dump(const char *path) {
 unsigned i;FILE *f=fopen(path,"wb");if(!f)exit(2);
 fprintf(f,"P6\n240 240\n255\n");
 for(i=0;i<240*240;i++) {uint16_t c=swap16(host_screen[i]);uint8_t rgb[3]={(c>>11)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};fwrite(rgb,1,3,f);}
 fclose(f);
}
int main(int argc,char **argv) {
 boot();tack_crec_tap();incoming(0,60,100,1010);incoming(0,64,100,1030);incoming(0,67,100,1050);
 incoming(0,60,0,1210);incoming(0,64,0,1230);incoming(0,67,0,1250);
 incoming(0,71,100,1400);incoming(0,71,0,1600);fm1_ms=1700;tack_crec_tap();
 tack_page=TACK_PG_LOOP;tack_loop_sel=0;tack_action_until=0;tack_drawn=0;tack_boot_until=0;tack_app_draw();
 dump(argc>1?argv[1]:"build/ui-review/loop.ppm");
 ck(text_w(&AF_S,"K1 ROW  K2 ACT  K4 APPLY")<=220,"LOOP control hint fits actual font width");
 ck(text_w(&AF_S,"MIDI notes / keys 1-6 moves")<=220,"MIDI/move distinction fits actual font width");
 ck(text_w(&AF_S,"EXPORT: HARMONY ONLY")<=220,"export limitation fits actual font width");
 { unsigned mode,sel; unsigned saved_len=tack_crec_len;
   for(mode=0;mode<3;mode++)for(sel=0;sel<5;sel++) {
     tack_crec_mode=mode;tack_loop_sel=sel;tack_drawn=0;tack_app_draw();
     if(tack_crec_len!=saved_len){puts("FAIL drawing changed captured phrase");return 1;}
   }
   ck(1,"15 actual LOOP row/mode frames preserve captured phrase length");
 }
 printf("rendered real tack_app_draw 240x240, %u recorded events\n",tack_crec_len);
 return bad?1:0;
}
