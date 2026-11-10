#define UI_TEST_NO_MAIN 1
#include "ui_test.c"
#include "../firmware/src/tack/tack_data.c"
#include "../firmware/src/tack/tack_harmony.c"
#include "../firmware/src/tack/tack_phrases_user.c"
#include "../firmware/src/tack/tack_rec_policy.c"
static uint32_t tack_test_edges(uint32_t *r) { *r=0; return fm1_input_edges(0); }
#define fm1_input_edges tack_test_edges
#include "../firmware/src/tack_app.c"
#undef fm1_input_edges

static int bad;
static void ck(int ok, const char *msg) { printf("%s %s\n",ok?"PASS":"FAIL",msg); if(!ok) bad++; }
static void boot(void) {
 ui_power_on(); tack_app_init(); song.playing=0; fm1_ms=1000;
 tack_mode_idx=TACK_MODE_MAJOR; tack_state.mode=TACK_MODE_MAJOR; tack_state.root=0;
 tack_octave=0; tack_crec_mode=0; tack_crec_len=0; tack_pick_slot=0;
 memset(tack_bar_lit_n,0,sizeof tack_bar_lit_n);
}
static void incoming(unsigned ch,unsigned p,unsigned v,unsigned time) {
 fm1_ms=time; midi_note_event(ch,p,v);
 /* Producer ISR dispatch, then one main-loop drain. */
 tack_midi_rec_drain();
}
#ifndef TACK_APP_REC_NO_MAIN
int main(void) {
 setbuf(stdout, NULL);
 uint8_t notes[4]; unsigned i,j,found;
 ck(tack_rec_adapt_pitch(71,&TACK_SCALE_C_MINOR)==70,"nearest B4 in C minor is Bb4, not C4");
 boot(); tack_crec_tap();
 incoming(0,60,100,1010); incoming(0,64,100,1030); incoming(0,67,100,1050);
 incoming(0,60,0,1200); incoming(0,64,0,1220); incoming(0,67,0,1240);
 fm1_ms=1300; tack_crec_tap();
 ck(tack_crec_len==1 && tack_crec_lit_n[0]==3,"actual MIDI entry + REC stop stores exact triad");
 tack_crec_apply_to_seq(); tack_octave=1; tack_patterns(); found=0;
 for(i=0;i<16;i++) if(trk[0].step[i].n) {
  if(trk[0].step[i].n==3 && trk[0].step[i].note[0]==60 && trk[0].step[i].note[1]==64 && trk[0].step[i].note[2]==67) found=1;
 }
 ck(found,"actual pattern writer ignores octave for literal chord");
 tack_pick_slot=0; tack_pick_apply(0);
 ck(tack_bar_lit_n[0]==0,"harmony move clears stale literal metadata");
 boot(); tack_crec_tap();
 incoming(0,60,100,1010); incoming(1,60,100,1030);
 incoming(0,60,0,1100); incoming(1,60,0,1200);
 ck(tack_midi_sess.nraw==2 && tack_midi_sess.raw[0].onset_ms==10 && tack_midi_sess.raw[0].off_ms==100 && tack_midi_sess.raw[1].onset_ms==30 && tack_midi_sess.raw[1].off_ms==200,"same-pitch notes on two channels have independent releases");
 boot(); tack_crec_tap(); incoming(0,60,100,1010);
 tack_midi_self(60,100); incoming(0,60,0,1100);
 ck(tack_midi_sess.live_on[60]==0 && tack_midi_sess.nraw==1,"local echo marker cannot swallow external release");
 boot(); tack_crec_tap();
 for(i=0;i<5;i++) incoming(0,60+i*2,100,1010+i*5);
 for(i=0;i<5;i++) incoming(0,60+i*2,0,1200+i);
 fm1_ms=1300;tack_crec_tap();
 ck(tack_crec_len==0,"five-note chord rejected visibly rather than silently truncated to four");

 boot(); tack_crec_tap();
 incoming(0,64,100,1010); incoming(0,64,0,1110);
 fm1_ms=1200; tack_crec_tap(); tack_crec_loop_tap();
 fm1_ms=1210; tack_app_input(); events_block(CTL);
 found=0; for(i=0;i<NVOICE;i++) if(trk[0].v[i].gate && trk[0].v[i].note==64)found=1;
 ck(found,"actual LOOP sounds recorded note at performed onset");
 fm1_ms=1311; tack_app_input(); events_block(CTL);
 found=0; for(i=0;i<NVOICE;i++) if(trk[0].v[i].gate && trk[0].v[i].note==64)found=1;
 ck(!found,"actual LOOP releases note at performed duration");

 tack_crec_loop_tap();
 ck(!tack_rec_accept,"playback never re-arms recorder");
 ck(tack_midi_sess.nraw==0,"playback does not feed its notes back to recording");
 /* Pin mapped note-off through mode/root changes. */
 boot(); tack_mode_idx=TACK_MODE_MINOR; tack_state.mode=TACK_MODE_MINOR;
 tack_crec_tap(); incoming(0,64,100,1010); incoming(0,64,0,1210);
 fm1_ms=1300;tack_crec_tap();tack_crec_loop_tap();
 fm1_ms=1310;tack_app_input(); events_block(CTL);
 ck(tack_loop_active[0] && tack_loop_mapped[0]==63,"mono LOOP maps E4 to Eb4 in minor");
 tack_mode_idx=TACK_MODE_MAJOR; tack_set_mode_keep(&tack_state,TACK_MODE_MAJOR,tack_slot_owned);
 tack_set_root_keep(&tack_state,5,tack_slot_owned);
 fm1_ms=1340;tack_app_input(); events_block(CTL);
 ck(tack_loop_active[0] && tack_loop_mapped[0]==63,"mode/root changes leave held mapped identity pinned");
 fm1_ms=1511;tack_app_input(); events_block(CTL);
 found=0;for(i=0;i<NVOICE;i++)if(trk[0].v[i].gate && trk[0].v[i].note==63)found=1;
 ck(!found && !tack_loop_active[0],"release ends original mapped pitch after mode/root change");
 tack_crec_loop_tap();
 /* External same-pitch release must not cut an independent recorded owner. */
 boot();tack_crec_tap();incoming(0,60,100,1010);incoming(0,60,0,1210);
 fm1_ms=1300;tack_crec_tap();tack_crec_loop_tap();fm1_ms=1310;tack_app_input(); events_block(CTL);
 incoming(0,60,100,1320);incoming(0,60,0,1330);
 found=0;for(i=0;i<NVOICE;i++)if(trk[0].v[i].gate && trk[0].v[i].note==60)found=1;
 ck(found,"external release cannot cut playback-owned same pitch");
 tack_crec_clear(); events_block(CTL);
 found=0;for(i=0;i<NVOICE;i++)if(trk[0].v[i].gate && trk[0].v[i].note==60)found=1;
 ck(!found && !tack_rec_accept && tack_loop_raw_n==0,"CLEAR releases playback and clears recording generation");
 /* Same-time duplicates are idempotent; separate channels are not collapsed. */
 boot();tack_crec_tap();incoming(0,60,100,1010);incoming(0,60,100,1010);incoming(0,60,0,1100);
 ck(tack_midi_sess.nraw==1,"duplicate same-time callback does not manufacture a second raw note");
 fm1_ms=1200;tack_crec_tap();fm1_ms=1300;tack_crec_tap();
 ck(tack_midi_sess.nraw==0 && tack_midi_sess.active_n==0,"new REC starts with empty owned session");
 tack_crec_clear();
 /* Producer queue overflow fails the whole take rather than pretending success. */
 boot();tack_crec_tap();
 for(i=0;i<70;i++){fm1_ms=1010+i;tack_midi_note(0,(uint8_t)(40+i%40),100);}
 fm1_ms=1300;tack_crec_tap();
 ck(tack_rec_failed && tack_crec_len==0 && !tack_rec_accept,"queue overflow is explicit rejection, not partial recording");
 /* Mixed move/MIDI capacity is all-or-nothing, not move-first starvation. */
 boot();tack_crec_tap();incoming(0,61,100,1010);incoming(0,61,0,1020);
 for(i=0;i<16;i++){TackChord c={(uint8_t)(i%12),TACK_Q_MAJ};fm1_ms=1100+i*100;tack_crec_capture(&c);}
 fm1_ms=3000;tack_crec_tap();
 ck(tack_rec_failed && tack_crec_len==0,"mixed 17-event timeline rejected without starving earlier MIDI");
 /* Small mixed timeline retains both lanes, original MIDI timings. */
 boot();tack_crec_tap();incoming(0,61,100,1010);incoming(0,61,0,1100);
 {TackChord c={7,TACK_Q_DOM7};fm1_ms=1010;tack_crec_capture(&c);}
 fm1_ms=1300;tack_crec_tap();
 ck(tack_crec_len==2 && tack_loop_raw_n==1 && tack_loop_raw[0].onset_ms==10 && tack_loop_raw[0].off_ms==100,"mixed move+MIDI retains performed MIDI onset and duration");
 /* Keys 1-6 invoke the existing actual move-selection path, never note capture. */
 boot();tack_crec_tap();tack_last_notes=0;fm1_in.notes=1;fm1_ms=1100;tack_app_input(); events_block(CTL);
 ck(tack_pick_slot==1 && tack_crec_len==1 && tack_crec_src[0]==0 && tack_midi_sess.nraw==0,"actual physical key 1 stays a harmony move during REC");
 fm1_in.notes=0;tack_app_input(); events_block(CTL);tack_crec_clear();
 /* Metadata must round-trip with A/B and undo, and survive mode/style. */
 boot();tack_crec_tap();incoming(0,60,100,1010);incoming(0,64,100,1030);
 incoming(0,60,0,1200);incoming(0,64,0,1200);fm1_ms=1300;tack_crec_tap();tack_crec_apply_to_seq();
 tack_phrase_store(0);tack_pick_slot=0;tack_pick_apply(0);tack_do_undo();
 ck(tack_bar_lit_n[0]==2 && tack_bar_lit[0][0]==60 && tack_bar_lit[0][1]==64,"UNDO restores literal pitches and ownership, not stale token-only state");
 tack_phrase_load(1);ck(tack_bar_lit_n[0]==0,"other A/B phrase cannot inherit stale literal pitches");
 tack_phrase_load(0);ck(tack_bar_lit_n[0]==2 && tack_slot_owned[0],"A/B restore includes performed voicing ownership");
 tack_next_mode();tack_pick_style(TACK_STYLE_JAZZ);
 ck(tack_bar_lit_n[0]==2 && tack_bar_lit[0][1]==64 && tack_state.chords[0].root==0,"mode/style changes preserve owned performed chord");


 boot();tack_crec_tap();
 for(i=0;i<4;i++)incoming(0,60+2*i,100,1010+i*10);
 for(i=0;i<4;i++)incoming(0,60+2*i,0,1210+i*10);
 fm1_ms=1300;tack_crec_tap();
 ck(tack_crec_len==1 && tack_crec_lit_n[0]==4 && !tack_rec_failed,"supported four-voice chord is accepted intact");
 {uint32_t published=tack_rec_qwrite;fm1_ms=1400;tack_midi_note(0,72,100);
 ck(tack_rec_qwrite==published,"closed recording gate rejects late producer callback after STOP");}
 boot();tack_crec_tap();
 for(i=0;i<5;i++)incoming(0,60+i,100,1010+i*100);
 for(i=0;i<5;i++)incoming(0,60+i,0,1610+i);
 fm1_ms=1700;tack_crec_tap();
 ck(tack_rec_failed && !tack_crec_len,"five overlapping legato voices rejected even outside onset chord window");
 boot();tack_crec_tap();
 for(i=0;i<33;i++){incoming(0,60,100,1010+i*10);incoming(0,60,0,1015+i*10);}
 fm1_ms=1500;tack_crec_tap();
 ck(tack_rec_failed && !tack_crec_len && tack_midi_sess.active_n==0,"raw capacity overflow rejects take with no stuck session ownership");
 /* Every pattern mode/style and octave must leave a literal dyad absolute. */
 found=1;
 { unsigned m,st,slot;int oct;
#ifdef TACK_SANITIZE_FAST
  for(m=0;m<2;m++)for(st=0;st<2;st++)for(oct=0;oct<=0;oct++)for(slot=0;slot<1;slot++){
#else
  for(m=0;m<TACK_MODE_COUNT;m++)for(st=0;st<TACK_STYLE_COUNT;st++)for(oct=-2;oct<=2;oct++)for(slot=0;slot<4;slot++){
#endif
   tack_mode_idx=tack_state.mode=(uint8_t)m;tack_style_idx=tack_state.style=(uint8_t)st;tack_octave=oct;
   memset(tack_bar_lit_n,0,sizeof tack_bar_lit_n);
   tack_bar_lit_n[slot]=2;tack_bar_lit_kind[slot]=TACK_EV_CHORD;tack_bar_lit[slot][0]=60;tack_bar_lit[slot][1]=64;
   tack_move_rate=1;tack_move_variant=1;tack_fill=1;tack_patterns();
   for(i=slot*tack_bar_steps();i<(slot+1)*tack_bar_steps();i++)if(trk[0].step[i].n && (trk[0].step[i].n!=2 || trk[0].step[i].note[0]!=60 || trk[0].step[i].note[1]!=64))found=0;
  }
 }
 ck(found,"all modes/styles/octaves/fills keep literal comp pitches unchanged");
 /* Exhaustive nearest-scale property, including boundary notes 0 and 127. */
 found=1;
 for(i=0;i<128;i++){
  int actual=tack_rec_adapt_pitch((uint8_t)i,&TACK_SCALE_C_MINOR),best=128;
  for(j=0;j<128;j++)if(j%12==0 || j%12==2 || j%12==3 || j%12==5 || j%12==7 || j%12==8 || j%12==10){int d=abs((int)i-(int)j);if(d<best)best=d;}
  if(abs((int)i-actual)!=best)found=0;
 }
 ck(found,"all 128 MIDI pitches map to an absolutely nearest C-minor note");
 printf("APP REGRESSIONS: %d failures\n",bad);return bad?1:0;
}

#endif /* TACK_APP_REC_NO_MAIN */
