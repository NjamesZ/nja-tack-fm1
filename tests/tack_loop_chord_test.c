#define TACK_APP_REC_NO_MAIN 1
#include "tack_app_rec_test.c"

static int loop_gated(unsigned pitch) {
 unsigned i;for(i=0;i<NVOICE;i++)if(trk[0].v[i].gate && trk[0].v[i].note==pitch)return 1;return 0;
}
static void at(unsigned ms) {fm1_ms=ms;tack_app_input();events_block(CTL);}
int main(void) {
 setbuf(stdout,NULL);boot();tack_crec_tap();
 incoming(0,60,100,1010);incoming(0,64,100,1030);incoming(0,67,100,1050);
 incoming(0,60,0,1200);incoming(0,64,0,1220);incoming(0,67,0,1240);
 fm1_ms=1300;tack_crec_tap();tack_mode_idx=tack_state.mode=TACK_MODE_MINOR;tack_octave=2;
 tack_crec_loop_tap();at(1360);
 ck(loop_gated(60)&&loop_gated(64)&&loop_gated(67),"actual audio LOOP chord stays C E G in minor despite octave +2");
 tack_mode_idx=TACK_MODE_JAPANESE;tack_set_mode_keep(&tack_state,TACK_MODE_JAPANESE,tack_slot_owned);
 tack_set_root_keep(&tack_state,5,tack_slot_owned);at(1400);
 ck(loop_gated(60)&&loop_gated(64)&&loop_gated(67),"held chord keeps absolute audio gates across root/mode change");
 at(1501);ck(!loop_gated(60)&&loop_gated(64)&&loop_gated(67),"rolled chord releases C at its own raw note-off");
 at(1521);ck(!loop_gated(60)&&!loop_gated(64)&&loop_gated(67),"rolled chord releases E independently");
 at(1541);ck(!loop_gated(60)&&!loop_gated(64)&&!loop_gated(67),"rolled chord releases G without stuck gates");
 at(2000);ck(loop_gated(60)&&loop_gated(64)&&loop_gated(67),"next loop keeps chord pitches under changed scale/root");
 tack_crec_loop_tap();events_block(CTL);
 ck(!loop_gated(60)&&!loop_gated(64)&&!loop_gated(67),"LOOP OFF releases audio-owned chord gates");
 printf("AUDIO CHORD LOOP: %d failures\n",bad);return bad?1:0;
}
