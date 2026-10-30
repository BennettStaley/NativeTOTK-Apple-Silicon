/* Native keyboard and macOS gamepad backed full-key controller. State ABI follows libnx HID. */
#include <TargetConditionals.h>
#include "runtime.h"
#include "autoload.h"
#include "world_survey.h"
#include "actor_spawn.h"
#include "settings.h"
#include "gamepad_layout.h"
#include <dispatch/dispatch.h>
#include <math.h>
#include "keyboard_map.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
typedef struct{int64_t sampling;uint64_t buttons;int32_t lx,ly,rx,ry;uint32_t attributes,pad;} NpadState;
_Static_assert(sizeof(NpadState)==40,"Npad full-key ABI");
static pthread_mutex_t inputLock=PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[128];static uint64_t pending;
static uint32_t styles=1;static bool enabled,allowed=true;static int holdType;
static int64_t sample;static uint64_t padSeen;
/* Scripted and auto-load presses (runtime_input_key_synthetic: TOTK_INPUT_SCRIPT, TOTK_INPUT_FILE, autoload.c) keep this
   fixed layout, whatever the player's, so test scripts mean the same keys on every Mac: J A, K / Space B, I X, U Y, Q / E
   L / R, Option / Shift ZL / ZR, Return +, Tab / Esc -, F T H G the d-pad, Z / C the stick presses; W A S D and the arrow
   keys the sticks (pad_states). */
static uint64_t button(unsigned code){switch(code){case 38:return 1;case 40:case 49:return 2;case 34:return 4;case 32:return 8;case 6:return 16;case 8:return 32;case 12:return 64;case 14:return 128;case 58:return 256;case 56:return 512;case 36:return 1024;case 48:case 53:return 2048;case 3:return 4096;case 17:return 8192;case 4:return 16384;case 5:return 32768;default:return 0;}}
/* The player's keys and mouse buttons go through their layout (keyboard_map.h, keyboard.txt next to controls.txt, the
   settings panel's key bindings). held: inputs down (keys 0-127, mouse buttons 128-132); heldOut what they drive. */
static KeyMap keyMap;static int keyMapLoaded;
static unsigned char held[KEYMAP_INPUTS];static KeyMapOutput heldOut;
static void keymap_load_locked(void){
 if(keyMapLoaded)return;keyMapLoaded=1;keymap_default(&keyMap);
 char text[8192];if(!runtime_setting_word("keyboard.txt",text,sizeof text)){fprintf(stderr,"[totk-input] keyboard layout: default\n");return;}
 const char *error=NULL;int line=keymap_parse(text,&keyMap,&error);
 if(line)fprintf(stderr,"[totk-input] keyboard.txt line %d: %s; using the default keyboard layout\n",line,error);
 else fprintf(stderr,"[totk-input] keyboard layout: keyboard.txt\n");
}
/* The Switch buttons the held keys drive, for runtime_keyboard_buttons, which takes no lock (the mouse ticks under the
   gamepad's lock, which is taken inside inputLock): the script keys' fixed layout and the player's (after PR #1210). */
static _Atomic uint64_t heldButtons;
static void held_buttons_locked(void){uint64_t b=heldOut.buttons;for(unsigned i=0;i<128;i++)if(keys[i])b|=button(i);atomic_store(&heldButtons,b);}
uint64_t runtime_keyboard_buttons(void){return atomic_load(&heldButtons);}
/* inputLock held: what the held inputs drive now; a press shorter than one game read is latched in pending. */
static void held_changed_locked(void){keymap_load_locked();KeyMapOutput out=keymap_apply(&keyMap,held,runtime_mouse_enabled());pending|=out.buttons&~heldOut.buttons;heldOut=out;held_buttons_locked();}
void runtime_keymap_get(KeyMap *out){pthread_mutex_lock(&inputLock);keymap_load_locked();*out=keyMap;pthread_mutex_unlock(&inputLock);}
/* The settings panel's key bindings: in force at once and saved (keyboard.txt); reset: the default, the file removed. */
void runtime_keymap_set(const KeyMap *map){
 char text[8192];int n=keymap_format(map,text,sizeof text);
 pthread_mutex_lock(&inputLock);keyMapLoaded=1;keyMap=*map;held_changed_locked();pthread_mutex_unlock(&inputLock);
 if(n>0)runtime_setting_save("keyboard.txt",text,"keyboard layout");
}
void runtime_keymap_reset(void){
 KeyMap map;keymap_default(&map);
 pthread_mutex_lock(&inputLock);keyMapLoaded=1;keyMap=map;held_changed_locked();pthread_mutex_unlock(&inputLock);
 runtime_setting_remove("keyboard.txt");fprintf(stderr,"[totk-input] keyboard layout: default (reset)\n");
}
/* Real user input (a window key, a controller button, a stick pushed past half) stops TOTK_AUTOLOAD and is logged at most
   once a minute. In an agent's test session (TOTK_SLOT_OWNER other than user) the line says the user is playing it: its
   save is a copy, its scripted measurement is void, and its script must not close the game under the user
   (2026-10-15: the user played 12 minutes in a test window and the script closed it at its time limit). */
static void user_input(const char *what){
 runtime_survey_cancel_user();
 runtime_autoload_user_input();
 static _Atomic uint64_t last;uint64_t now=runtime_now_ns(),prev=atomic_load(&last);
 if(prev&&now-prev<60000000000ull)return;if(!atomic_compare_exchange_strong(&last,&prev,now))return;
 const char *owner=getenv("TOTK_SLOT_OWNER");bool agent=owner&&*owner&&strcmp(owner,"user");
 fprintf(stderr,"[input] user input (%s)%s%s\n",what,agent?": the user is playing this test session of ":"",agent?owner:"");
}
/* Keys from the window are the user's; scripted and auto-load presses use the synthetic entry.
   Key 1 toggles the shortcut reminder in frontend.m. Key 2 (Mac key code 19) cycles MetalFX frame interpolation:
   off -> 30+30 -> half the refresh rate -> uncapped (totk_mfx.m). Key 3 (key code 20) cycles 2K modes:
   MetalFX 2K -> native 2K -> off (runtime_output_mode_cycle). Key 6 (key code 22) cycles the controller type: auto -> Xbox ->
   PlayStation -> Switch (runtime_gamepad_type_set, gamepad.m). Key 7 (key code 26) turns mouse mode on / off
   (runtime_mouse_set, gamepad.m; the window holds the pointer: frontend.m). Held-key repeats do not cycle. The settings
   menu (F1 / Command-comma, settings_panel_mac.h) holds the same choices; upstream's key-1 panel is not used here.
   The keyboard keeps this fixed layout (upstream's remappable keyboard layout, keyboard_map.h, would move keys 2-5 to the
   d-pad and clash with these shortcuts); only mouse mode's buttons go through the layout (runtime_input_mouse). */
enum { KEY_INTERP_TOGGLE = 19, KEY_OUTPUT_MODE_CYCLE = 20, KEY_ASPECT_TOGGLE = 21, KEY_PAD_TYPE_CYCLE = 22, KEY_TAA_TOGGLE = 23, KEY_MOUSE_TOGGLE = 26 };   /* 21: key 4, 16:9 -> fit to the window -> 21:9 (aspect_patch.h); 22: key 6; 23: key 5, MetalFX temporal anti-aliasing (totk_taa.m) */
/* Keys 5-7 on upstream's setters (its panel replaced the cycles). */
static void key_taa_cycle(void){extern int taa_enabled(void);extern void taa_set(int on);taa_set(!taa_enabled());}
static void key_pad_type_cycle(void){dispatch_async(dispatch_get_main_queue(),^{int detected;int t=runtime_gamepad_type(&detected);   /* main thread */
 int next=t>=0&&t<GAMEPAD_TYPE_COUNT-1?t+1:GAMEPAD_TYPE_AUTO;runtime_gamepad_type_set(next);});}   /* logs the new type */
static void key_mouse_toggle(void){runtime_mouse_set(!runtime_mouse_enabled());}
void runtime_input_key_synthetic(unsigned code,int down){if(code>=128)return;pthread_mutex_lock(&inputLock);int press=down&&!keys[code];if(down&&!keys[code])pending|=button(code);keys[code]=(unsigned char)!!down;held_buttons_locked();pthread_mutex_unlock(&inputLock);
 if(press&&code==18){extern void runtime_shortcut_help_toggle(void);runtime_shortcut_help_toggle();}
 if(press&&code==KEY_INTERP_TOGGLE){extern void dm_gfx_set_flag(const char *name,int v);dm_gfx_set_flag("mfx_interp",-1);}
 if(press&&code==KEY_OUTPUT_MODE_CYCLE)runtime_output_mode_cycle();
 if(press&&code==KEY_ASPECT_TOGGLE&&runtime_aspect_toggle())settings_aspect_follow(runtime_aspect_choice());   /* the menu shows the shape key 4 picked */
 if(press&&code==KEY_TAA_TOGGLE)key_taa_cycle();
 if(press&&code==KEY_PAD_TYPE_CYCLE)key_pad_type_cycle();
 if(press&&code==KEY_MOUSE_TOGGLE)key_mouse_toggle();}
/* Scripted pad ("ctl press | hold | release | stick" lines of the input file, ctl_command below): npad buttons and exact
   analog sticks for a script that plays the game, in sys-botbase's vocabulary (olliz0r/sys-botbase: click / press /
   release / setStick). A button or stick given a time lets go by itself then; a press is seen by the game at least once,
   however short. A scripted stick replaces the keyboard's (a controller pushed further still wins) and is not scaled by
   the camera-speed setting. Not the user's input: it never stops auto-load. Under inputLock. */
static uint64_t ctl_held,ctl_pending,ctl_until[16];
static struct{int on;int32_t x,y;uint64_t until;} ctl_stick[2];
static void ctl_expire_locked(void){uint64_t now=runtime_now_ns();for(int i=0;i<16;i++)if(ctl_until[i]&&now>=ctl_until[i]){ctl_until[i]=0;ctl_held&=~(1ull<<i);}
 for(int s=0;s<2;s++)if(ctl_stick[s].on&&ctl_stick[s].until&&now>=ctl_stick[s].until)ctl_stick[s].on=0;}
/* Leased keys (auto shrine, world_survey.c): a synthetic press that lets go by itself at its deadline, checked wherever
   the game or noclip reads input, so a key can never stay held when the code that pressed it stops running. ms 0 lets
   go at once. A key the player presses himself is his: its lease ends. */
static uint64_t lease_until[128];
static void lease_expire_locked(void){ctl_expire_locked();uint64_t now=0;for(unsigned i=0;i<128;i++)if(lease_until[i]){if(!now)now=runtime_now_ns();if(now>=lease_until[i]){lease_until[i]=0;keys[i]=0;}}}
void runtime_input_lease(unsigned code,unsigned ms){if(code>=128)return;pthread_mutex_lock(&inputLock);
 if(!ms){if(lease_until[code]){lease_until[code]=0;keys[code]=0;}}
 else{if(!keys[code])pending|=button(code);keys[code]=1;lease_until[code]=runtime_now_ns()+(uint64_t)ms*1000000ull;}
 pthread_mutex_unlock(&inputLock);}
void runtime_input_lease_release_all(void){pthread_mutex_lock(&inputLock);for(unsigned i=0;i<128;i++)if(lease_until[i]){lease_until[i]=0;keys[i]=0;}pthread_mutex_unlock(&inputLock);}
void runtime_input_key(unsigned code,int down){if(down&&code<128)user_input("keyboard");if(code<128){pthread_mutex_lock(&inputLock);lease_until[code]=0;pthread_mutex_unlock(&inputLock);}runtime_input_key_synthetic(code,down);}
/* A mouse button in mouse mode (runtime/gamepad.m: 0 left, 1 right, 2 middle, 3 and 4 the side buttons), through the
   player's layout (keyboard_map.h; upstream PR #1210). */
void runtime_input_mouse(int button,int down){
 if(button<0||button>=KEYMAP_MOUSE_BUTTONS)return;if(down)user_input("mouse");
 pthread_mutex_lock(&inputLock);held[KEYMAP_MOUSE_FIRST+button]=(unsigned char)!!down;held_changed_locked();pthread_mutex_unlock(&inputLock);
}
/* While the settings menu is open the game reads no buttons and no keyboard: nothing the player presses there reaches it.
   The controller's two sticks still do (the menu navigates with the d-pad), so the camera and Link can move while a
   setting is tried. */
static _Atomic int inputHeld;
void runtime_input_hold(int on){atomic_store(&inputHeld,on?1:0);if(on)runtime_input_clear();}
/* Cheats (cheats.c): the buttons held now (keyboard and controller), in the npad bit order. */
uint64_t runtime_input_buttons(void){uint64_t b=0;pthread_mutex_lock(&inputLock);lease_expire_locked();for(unsigned i=0;i<128;i++)if(keys[i])b|=button(i);b|=ctl_held|heldOut.buttons;pthread_mutex_unlock(&inputLock);
 return b|runtime_gamepad_held();}
void runtime_input_left_stick(int32_t *x,int32_t *y){pthread_mutex_lock(&inputLock);lease_expire_locked();int32_t kx=32767*((int)keys[2]-(int)keys[0]),ky=32767*((int)keys[13]-(int)keys[1]);if(heldOut.lx)kx=heldOut.lx;if(heldOut.ly)ky=heldOut.ly;if(ctl_stick[0].on){kx=ctl_stick[0].x;ky=ctl_stick[0].y;}int menu_held=atomic_load(&inputHeld);pthread_mutex_unlock(&inputLock);
 int32_t px=0,py=0;runtime_gamepad_left_stick(&px,&py);if(menu_held){*x=*y=0;return;}*x=abs(px)>abs(kx)?px:kx;*y=abs(py)>abs(ky)?py:ky;}
void runtime_input_clear(void){pthread_mutex_lock(&inputLock);memset(keys,0,sizeof(keys));memset(lease_until,0,sizeof(lease_until));pending=0;
 memset(held,0,sizeof held);heldOut=(KeyMapOutput){0};
 ctl_held=ctl_pending=0;memset(ctl_until,0,sizeof(ctl_until));memset(ctl_stick,0,sizeof(ctl_stick));held_buttons_locked();pthread_mutex_unlock(&inputLock);runtime_gamepad_clear();}
static void initialize(void){pthread_mutex_lock(&inputLock);enabled=true;pthread_mutex_unlock(&inputLock);fprintf(stderr,"[totk] native keyboard controller initialized\n");}
/* Docked: one full-key controller as player 1 (id 0, style bit 0). Handheld (TOTK_OPERATION_MODE=handheld, the iPhone):
   the same input is the handheld controller (id 0x20, style bit 1, joy-cons attached) and player 1 is not connected;
   the game reads only the handheld controller in that mode. */
/* TOTK_INPUT_LOG=1: one line per new (state kind, controller id, style set) the game reads (which pad the game uses). */
static void input_log(const char *what,uint32_t id){static int on=-1;static uint64_t seen[64];static unsigned n;if(on<0)on=getenv("TOTK_INPUT_LOG")!=NULL;if(!on)return;
 uint64_t k=(uint64_t)(uintptr_t)what<<32^id^(uint64_t)styles<<48;for(unsigned i=0;i<n;i++)if(seen[i]==k)return;if(n<64)seen[n++]=k;
 fprintf(stderr,"[totk-input] game reads %s for controller %#x (supported styles %#x, ids allowed %d)\n",what,id,styles,allowed);}
static uint32_t pad_id(void){return runtime_handheld_mode()?0x20u:0u;}
static uint32_t pad_style(void){return runtime_handheld_mode()?2u:1u;}
static void supported_ids(const uint32_t *ids,uint64_t n){if(n>10||(!ids&&n))runtime_fail("invalid controller ids");
 if(getenv("TOTK_INPUT_LOG")){char b[128];size_t o=0;for(uint64_t i=0;i<n&&o<100;i++)o+=(size_t)snprintf(b+o,sizeof b-o," %#x",ids[i]);b[o]=0;fprintf(stderr,"[totk-input] game supports controller ids:%s\n",b);}pthread_mutex_lock(&inputLock);allowed=false;for(uint64_t i=0;i<n;i++)if(ids[i]==pad_id())allowed=true;pthread_mutex_unlock(&inputLock);}
static void supported_styles(uint32_t value){pthread_mutex_lock(&inputLock);styles=value;pthread_mutex_unlock(&inputLock);}
static uint32_t style(const uint32_t *id){if(!id)runtime_fail("controller id pointer");input_log("style set",*id);pthread_mutex_lock(&inputLock);uint32_t v=enabled&&allowed&&*id==pad_id()?(styles&pad_style()):0;pthread_mutex_unlock(&inputLock);return v;}
/* Camera speed (settings controls.camera_speed_x / _y, percent): the right stick as the game reads it, keyboard arrows and
   controller alike, scaled and kept within the stick's range. Above 100 % a half push turns the camera as a fuller one
   would; the game's own top speed (the stick all the way) stays. */
static _Atomic int cameraX=100,cameraY=100;
void runtime_camera_speed_set(int axis,int percent){if(percent<25)percent=25;if(percent>300)percent=300;atomic_store(axis?&cameraY:&cameraX,percent);}
static int32_t camera_scale(int32_t v,int percent){if(percent==100)return v;int64_t s=(int64_t)v*percent/100;return (int32_t)(s>32767?32767:s<-32767?-32767:s);}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style);
/* D-pad diagnosis (2026-10-29; the user, Xbox controller over Bluetooth: at times the d-pad opens neither the weapon nor the
   bow wheel). When a d-pad direction goes down on the controller, one line with what the game gets in that read and what
   else holds buttons: the keys held (Mac key codes; a key whose keyUp never came stays held), scripted ctl buttons, leased
   keys, the settings menu's hold, noclip's buttons, text capture. A press only, at most one line a second. inputLock held. */
static void dpad_diagnose_locked(uint64_t controller,uint64_t sent,int menu_held){
 static uint64_t before,last;uint64_t down=controller&0xf000&~before;before=controller&0xf000;if(!down)return;
 uint64_t now=runtime_now_ns();if(last&&now-last<1000000000ull)return;last=now;
 static const char *const dir[4]={"left","up","right","down"};int d=0;while(!(down>>(12+d)&1))d++;
 char held_keys[48]="";size_t o=0;int leased=0;
 for(unsigned i=0;i<128;i++){if(lease_until[i])leased++;if(keys[i]&&o+6<sizeof held_keys)o+=(size_t)snprintf(held_keys+o,sizeof held_keys-o," %u",i);}
 fprintf(stderr,"[input] d-pad %s pressed: controller %#llx, game sent %#llx; keyboard %#llx (keys held:%s), scripted %#llx, leased keys %d, settings menu %s, noclip took %#llx, controller to the game %s\n",
  dir[d],(unsigned long long)controller,(unsigned long long)sent,(unsigned long long)atomic_load(&heldButtons),o?held_keys:" none",(unsigned long long)ctl_held,leased,
  menu_held?"holding input":"closed",(unsigned long long)runtime_noclip_taken(),runtime_gamepad_game_input()?"yes":"no (text capture)");
}
static int full_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,1);}
static int handheld_states(NpadState *out,int max,const uint32_t *id){return pad_states(out,max,id,2);}
static int pad_states(NpadState *out,int max,const uint32_t *id,uint32_t want_style){if(max<=0)return 0;if(id)input_log(want_style==2?"handheld state":"full-key state",*id);if(!out||!id)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);NpadState state={.sampling=++sample};lease_expire_locked();if(enabled&&allowed&&*id==pad_id()&&want_style==pad_style()&&(styles&want_style)){
 state.attributes=1;for(unsigned i=0;i<128;i++)if(keys[i])state.buttons|=button(i);state.buttons|=heldOut.buttons|pending;pending=0;state.buttons|=ctl_held|ctl_pending;ctl_pending=0;
 state.lx=32767*((int)keys[2]-(int)keys[0]);state.ly=32767*((int)keys[13]-(int)keys[1]);state.rx=32767*((int)keys[124]-(int)keys[123]);state.ry=32767*((int)keys[126]-(int)keys[125]);
 if(heldOut.lx)state.lx=heldOut.lx;if(heldOut.ly)state.ly=heldOut.ly;if(heldOut.rx)state.rx=heldOut.rx;if(heldOut.ry)state.ry=heldOut.ry;
 if(ctl_stick[0].on){state.lx=ctl_stick[0].x;state.ly=ctl_stick[0].y;}int ctl_camera=ctl_stick[1].on;if(ctl_camera){state.rx=ctl_stick[1].x;state.ry=ctl_stick[1].y;}
 RuntimeGamepadState pad;runtime_gamepad_read(&pad);if(atomic_load(&inputHeld)){pad.buttons=0;state.buttons=0;state.lx=state.ly=state.rx=state.ry=0;}state.buttons|=pad.buttons;state.buttons&=~runtime_noclip_taken();dpad_diagnose_locked(runtime_gamepad_held(),state.buttons,atomic_load(&inputHeld));
 if(pad.buttons&~padSeen||abs(pad.lx)>16384||abs(pad.ly)>16384||abs(pad.rx)>16384||abs(pad.ry)>16384)user_input("controller");padSeen=pad.buttons;
 if(abs(pad.lx)>abs(state.lx))state.lx=pad.lx;if(abs(pad.ly)>abs(state.ly))state.ly=pad.ly;
 if(abs(pad.rx)>abs(state.rx))state.rx=pad.rx;if(abs(pad.ry)>abs(state.ry))state.ry=pad.ry;
 if(!ctl_camera||abs(pad.rx)>abs(ctl_stick[1].x)||abs(pad.ry)>abs(ctl_stick[1].y)){state.rx=camera_scale(state.rx,atomic_load(&cameraX));state.ry=camera_scale(state.ry,atomic_load(&cameraY));}
 }*out=state;pthread_mutex_unlock(&inputLock);return 1;}
static int disconnected_states(NpadState *out,int max,const uint32_t *id){if(id)input_log("joy-con state",*id);if(max<=0)return 0;if(!out)runtime_fail("controller output pointer");pthread_mutex_lock(&inputLock);*out=(NpadState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
static void hold_type(int type){if(type<0||type>1)runtime_fail("invalid joy hold type");holdType=type;(void)holdType;}
static void disconnect(const uint32_t *id){if(!id)runtime_fail("disconnect id");input_log("disconnect",*id);if(*id==pad_id()){   /* only the pad in use: in handheld mode TOTK disconnects player 1 */pthread_mutex_lock(&inputLock);allowed=false;pthread_mutex_unlock(&inputLock);runtime_input_clear();}}
/* Mouse and keyboard devices: the Mac keyboard drives the full-key controller, so the game sees no mouse
   (not connected) and a keyboard with no keys, avoiding duplicated input. Layouts follow the SDK (0x28 / 0x30 bytes). */
typedef struct{int64_t sampling;int32_t x,y,dx,dy,wx,wy;uint32_t buttons,attributes;} MouseState;
typedef struct{int64_t sampling;uint32_t modifiers,attributes;uint64_t keys[4];} KeyboardState;
_Static_assert(sizeof(MouseState)==40&&sizeof(KeyboardState)==48,"mouse/keyboard ABI");
static bool mouseReady,keyboardReady;
static void initialize_mouse(void){pthread_mutex_lock(&inputLock);mouseReady=true;pthread_mutex_unlock(&inputLock);}
static void initialize_keyboard(void){pthread_mutex_lock(&inputLock);keyboardReady=true;pthread_mutex_unlock(&inputLock);}
static int mouse_states(MouseState *out,int max){if(max<=0)return 0;if(!out)runtime_fail("mouse output pointer");pthread_mutex_lock(&inputLock);if(!mouseReady)runtime_fail("mouse used before InitializeMouse");*out=(MouseState){.sampling=++sample};pthread_mutex_unlock(&inputLock);return 1;}
static void keyboard_state(KeyboardState *out){if(!out)runtime_fail("keyboard output pointer");pthread_mutex_lock(&inputLock);if(!keyboardReady)runtime_fail("keyboard used before InitializeKeyboard");*out=(KeyboardState){.sampling=++sample};pthread_mutex_unlock(&inputLock);}
/* Controller support applet (the Switch's "connect controllers" screen). TOTK opens it in handheld mode; there is
   exactly one controller here, so it returns at once: one player, on the controller in use (ControllerSupportResultInfo:
   int8 player count, 3 bytes padding, uint32 selected id). */
typedef struct{int8_t players;uint8_t pad[3];uint32_t selected;} ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult)==8,"controller support result ABI");
static uint32_t show_controller_support(ControllerSupportResult *out,const void *arg){
 (void)arg;if(out)*out=(ControllerSupportResult){.players=1,.selected=pad_id()};
 static int once;if(!once++)fprintf(stderr,"[totk] controller support applet: one player on controller %#x\n",pad_id());
 return 0;
}
/* Vibration devices and the six-axis sensor: runtime/hid_feedback.c (none unless TOTK_RUMBLE=1 / TOTK_GYRO=1). */
#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md). The console's own seven-axis sensor (Labo VR) is absent on a Mac: it starts and
   stops without states. Gyroscope zero-drift mode and the dual Joy-Con assignment change nothing for the host controller.
   The one-argument controller applet returns at once, as the two-argument one does (one player). */
static uint32_t seven_axis_init(void *work,unsigned long size){(void)work;(void)size;fprintf(stderr,"[botw] seven-axis sensor: not present\n");return 0;}
static uint32_t seven_axis_ok(void){return 0;}
static void seven_axis_reset(void){}
static int seven_axis_states(void *out,int count){(void)out;(void)count;return 0;}
static void zero_drift_mode(const void *handle,const int *mode){(void)handle;(void)mode;}
static void joy_dual(const uint32_t *id){(void)id;}
static uint32_t show_controller_support_v1(const void *arg){return show_controller_support(NULL,arg);}
#endif
void *runtime_input_override(const char *name){
 void *feedback=runtime_hid_feedback_override(name);if(feedback)return feedback;
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn3hid14InitializeNpadEv",initialize},{"_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm",supported_ids},{"_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",supported_styles},
 {"_ZN2nn3hid15GetNpadStyleSetERKj",style},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj",full_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj",disconnected_states},
 {"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj",disconnected_states},{"_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj",handheld_states},
 {"_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE",hold_type},{"_ZN2nn3hid14DisconnectNpadERKj",disconnect},
 {"_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",show_controller_support},
 {"_ZN2nn3hid15InitializeMouseEv",initialize_mouse},{"_ZN2nn3hid18InitializeKeyboardEv",initialize_keyboard},
 {"_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi",mouse_states},{"_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE",keyboard_state}
#ifdef GAME_BOTW
 ,{"_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm",seven_axis_init},{"_ZN2nn3hid26FinalizeSevenSixAxisSensorEv",seven_axis_ok},
 {"_ZN2nn3hid23StartSevenSixAxisSensorEv",seven_axis_ok},{"_ZN2nn3hid22StopSevenSixAxisSensorEv",seven_axis_ok},
 {"_ZN2nn3hid32ResetSevenSixAxisSensorTimestampEv",seven_axis_reset},
 {"_ZN2nn3hid27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi",seven_axis_states},
 {"_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",zero_drift_mode},
 {"_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj",joy_dual},
 {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE",show_controller_support_v1}
#endif
 };
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

/* TOTK_INPUT_SCRIPT="sec:keycode:ms,...": press Mac key codes at given seconds after start (diagnostics without a
   picture: does the game react to a button). Key codes go through the fixed script layout (J=38 is A, Return=36 is Plus). */
#include <unistd.h>
static void *input_script(void *arg) {
    const char *p = arg; uint64_t t0 = runtime_now_ns();
    while (p && *p) {
        double at = strtod(p, (char **)&p); if (*p++ != ':') break;
        unsigned code = (unsigned)strtoul(p, (char **)&p, 10); if (*p++ != ':') break;
        unsigned ms = (unsigned)strtoul(p, (char **)&p, 10); if (*p == ',') p++;
        while ((runtime_now_ns() - t0) / 1e9 < at) usleep(10000);
        fprintf(stderr, "[totk-input] script: key %u down for %u ms at %.1f s\n", code, ms, at);
        runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
    }
    return NULL;
}
__attribute__((constructor)) static void input_script_start(void) {
    const char *e = getenv("TOTK_INPUT_SCRIPT"); if (!e) return;
    pthread_t t; pthread_create(&t, NULL, input_script, (void *)e); pthread_detach(t);
}

/* Optional local integration-test command stream. Appending complete lines lets tests inspect each
   actual frame before choosing the next input, instead of guessing load/menu timings. Disabled normally.
   Commands: key <Mac keycode> <milliseconds>, down/up <Mac keycode>, shot <path>, wshot <path>, passdump <frames>, querylog <frames>, anomdump,
   kbtest / kbtype <text> / kbenter / kbesc / kbshot <path> (the text entry bar), threads (dump every guest thread's wait), hletrace <0|1|2> (host calls the game makes), framestep <ms> (pin the frame step), render <w>x<h> (live scene size), mscan ... (memory scan and write watch, runtime/mem_scan.h), close;
   ctl ... (the scripted pad below; game state and control in world_survey.c). */
extern void dm_gfx_shot_at(long frames, const char *path);
extern void runtime_front_request_close(void);
/* "mscan ...": memory scan and write watch (runtime/mem_scan.h, in services.c); this default answers in the probes that
   link input.c alone, which have no game memory. */
__attribute__((weak)) int runtime_mem_scan_command(const char *line) { (void)line; return 0; }
/* "ctl press <buttons> [ms]" (down for ms, 100 by default, then up by itself), "ctl hold <buttons>" (down until released),
   "ctl release <buttons>|all", "ctl stick l|r <x> <y> [ms]" (x, y from -1 to 1, y up; for ms, or until the next stick
   line for that stick when absent or 0; "ctl stick l 0 0" without ms hands the stick back). Buttons: a b x y l r zl zr
   plus minus lstick rstick dleft dup dright ddown, several joined with '+' ("ctl press zl+a 300"). The other "ctl ..."
   lines (state, peek, face, walk, noclip, goto: world_survey.c) fall through. "[ctl-done] ..." when taken. */
static uint64_t ctl_buttons(const char *text){
 static const char *const names[16]={"a","b","x","y","lstick","rstick","l","r","zl","zr","plus","minus","dleft","dup","dright","ddown"};
 uint64_t mask=0;char word[16];
 while(*text){size_t n=strcspn(text,"+");if(!n||n>=sizeof word)return 0;memcpy(word,text,n);word[n]=0;text+=n+(text[n]=='+');
  int found=0;for(int i=0;i<16;i++)if(!strcmp(word,names[i])){mask|=1ull<<i;found=1;}if(!found)return 0;}
 return mask;}
static int ctl_command(const char *line){
 char op[16]="",what[96]="";int used=0;
 if(strncmp(line,"ctl ",4)||sscanf(line+4,"%15s %95s %n",op,what,&used)<2)return 0;
 const char *rest=line+4+used;
 if(!strcmp(op,"press")||!strcmp(op,"hold")||!strcmp(op,"release")){
  uint64_t mask=!strcmp(what,"all")&&op[0]=='r'?0xffffull:ctl_buttons(what);unsigned ms=op[0]=='p'?100:0;
  if(op[0]=='p'&&*rest&&(sscanf(rest,"%u",&ms)!=1||ms>60000))mask=0;
  if(!mask){fprintf(stderr,"[ctl] rejected: %s",line);return 1;}
  pthread_mutex_lock(&inputLock);uint64_t until=ms?runtime_now_ns()+(uint64_t)ms*1000000ull:0;
  for(int i=0;i<16;i++)if(mask>>i&1)ctl_until[i]=op[0]=='p'?until:0;
  if(op[0]=='r')ctl_held&=~mask;else{ctl_held|=mask;ctl_pending|=mask;}
  pthread_mutex_unlock(&inputLock);fprintf(stderr,"[ctl-done] %s %s %u\n",op,what,ms);return 1;}
 if(!strcmp(op,"stick")&&(!strcmp(what,"l")||!strcmp(what,"r"))){
  float x,y;unsigned ms=0;int n=sscanf(rest,"%f %f %u",&x,&y,&ms);
  if(n<2||!(x>=-1&&x<=1&&y>=-1&&y<=1)||ms>60000){fprintf(stderr,"[ctl] rejected: %s",line);return 1;}
  int s=what[0]=='r';pthread_mutex_lock(&inputLock);
  ctl_stick[s].x=(int32_t)lroundf(x*32767);ctl_stick[s].y=(int32_t)lroundf(y*32767);ctl_stick[s].until=ms?runtime_now_ns()+(uint64_t)ms*1000000ull:0;
  ctl_stick[s].on=ms||x!=0||y!=0;pthread_mutex_unlock(&inputLock);
  fprintf(stderr,"[ctl-done] stick %s %.3f %.3f %u\n",what,x,y,ms);return 1;}
 return 0;}
static void *input_file(void *arg) {
    FILE *f = fopen((const char *)arg, "r"); if (!f) runtime_fail("cannot open TOTK_INPUT_FILE");
    char line[1200];
    for (;;) {
        if (!fgets(line, sizeof line, f)) {
            if (ferror(f)) runtime_fail("diagnostic input read");
            clearerr(f); usleep(100000);
            /* A file replaced instead of appended to (devicectl device copy to an iPhone's Documents writes a new file, so
               this stream would stay on the old one and never see another line): open the new one. The writer copies the
               whole command list again, so it is read on from where the old one ended; a shorter file is a new list, read
               from its start. */
            struct stat now, held; FILE *n;
            if (!stat((const char *)arg, &now) && !fstat(fileno(f), &held) && now.st_ino != held.st_ino && (n = fopen((const char *)arg, "r"))) {
                long at = ftell(f); fclose(f); f = n;
                if (at > 0 && now.st_size >= at) fseek(f, at, SEEK_SET);
            }
            continue;
        }
        if (ctl_command(line)) continue;   /* "ctl press | hold | release | stick": the scripted pad */
        if (runtime_survey_command(line) || runtime_spawn_command(line) || runtime_autoshrine_command(line)) continue;   /* "autoshrine ..." / "ctl ...": world_survey.c */
        unsigned code, ms; int used = 0;
        if (sscanf(line, "down %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[totk-input] file: key %u down\n",code);runtime_input_key_synthetic(code,1);
        } else if (sscanf(line, "up %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[totk-input] file: key %u up\n",code);runtime_input_key_synthetic(code,0);
        } else if (sscanf(line, "key %u %u %n", &code, &ms, &used) == 2 && used && !line[used] && code < 128 && ms > 0 && ms <= 5000) {
            fprintf(stderr, "[totk-input] file: key %u for %u ms\n", code, ms);
            runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
        } else if (sscanf(line, "passdump %u %n", &code, &used) == 1 && used && !line[used] && code > 0 && code < 16) {
            extern void dm_gfx_passdump_next(long n); dm_gfx_passdump_next((long)code);
        } else if (!strncmp(line, "anomdump", 8) && (line[8] == '\n' || line[8] == '\r' || !line[8])) {   /* TOTK_ANOMALY=<dir>: write the per-draw records (with GPU sample counts) of the last frames now */
            extern void dm_gfx_anomdump(void); dm_gfx_anomdump();
        } else if (sscanf(line, "querylog %u %n", &code, &used) == 1 && used && !line[used] && code > 0 && code <= 600) {
            extern void dm_gfx_querylog_next(long n); dm_gfx_querylog_next((long)code);   /* occlusion query results of the next n frames */
        } else if (!strncmp(line, "noclip goto ", 12)) {   /* "noclip goto X Y Z": Tools › Teleport */
            float xyz[3]; if (sscanf(line + 12, "%f %f %f", &xyz[0], &xyz[1], &xyz[2]) == 3) runtime_noclip_goto(xyz);
        } else if (!strcmp(line, "noclip on\n") || !strcmp(line, "noclip off\n")) {   /* the Cheats tab's noclip switch (world_survey.c) */
            runtime_noclip_set(line[8] == 'n');
        } else if (!strncmp(line, "flag ", 5)) {
            /* "flag <name> <0|1>": switch a graphics behaviour while the game runs (A/B without restarting); rumble / gyro mute and unmute those */
            char name[64]; int v; if (sscanf(line + 5, "%63s %d", name, &v) != 2) runtime_fail("diagnostic flag needs <name> <0|1>");
            extern void dm_gfx_set_flag(const char *name, int v); if (!runtime_hid_feedback_flag(name, v)) dm_gfx_set_flag(name, v);
        } else if (!strncmp(line, "texdump ", 8)) {
            extern void dm_gfx_texdump_next(const char *path);
            line[strcspn(line, "\r\n")] = 0; if (!line[8] || !strstr(line + 8, "%d")) runtime_fail("diagnostic texdump path needs %d");
            dm_gfx_texdump_next(line + 8);
        } else if (sscanf(line, "shotn %u %n", &code, &used) == 1 && used && code > 0 && code <= 60) {
            /* "shotn <n> <path with %d>": screenshots of n consecutive game frames (frame flicker); %d = 0..n-1 */
            line[strcspn(line, "\r\n")] = 0; const char *fmt = line + used;
            if (!*fmt || !strstr(fmt, "%d") || strchr(strstr(fmt, "%d") + 2, '%')) runtime_fail("diagnostic shotn path needs one %d");
            for (unsigned i = 0; i < code; i++) { char p[1200]; snprintf(p, sizeof p, fmt, (int)i); dm_gfx_shot_at(2 + (long)i, p); }
        } else if (!strncmp(line, "shot ", 5)) {
            line[strcspn(line, "\r\n")] = 0; if (!line[5]) runtime_fail("diagnostic screenshot path");
            dm_gfx_shot_at(2, line + 5);
        } else if (!strncmp(line, "wshot ", 6)) {   /* "wshot <path>": the next shown picture as the window shows it (FPS box and the bottom-left box included) */
            line[strcspn(line, "\r\n")] = 0; if (!line[6]) runtime_fail("diagnostic window screenshot path");
            extern void dm_gfx_window_shot(const char *path); dm_gfx_window_shot(line + 6);
#ifdef GAME_BOTW
        } else if (!strncmp(line, "ashot ", 6)) {   /* "ashot <path>": the next game frame, written in the background, its present time logged (botw.md) */
            line[strcspn(line, "\r\n")] = 0; if (!line[6]) runtime_fail("diagnostic ashot path");
            extern void dm_gfx_ashot(const char *path); dm_gfx_ashot(line + 6);
#endif
        } else if (!strcmp(line, "kbtest\n")) {   /* text entry bar the way the game names a horse (runtime/swkbd_host.c) */
            runtime_swkbd_selftest();
        } else if (!strncmp(line, "kbtype ", 7)) {   /* "kbtype <text>": typed into the open bar */
            line[strcspn(line, "\r\n")] = 0; runtime_front_text_test("type", line + 7);
        } else if (!strncmp(line, "kbshot ", 7)) {   /* "kbshot <path>": the open bar as a PNG */
            line[strcspn(line, "\r\n")] = 0; runtime_front_text_test("shot", line + 7);
        } else if (!strcmp(line, "kbenter\n") || !strcmp(line, "kbesc\n")) {
            runtime_front_text_test(line[2] == 'e' && line[3] == 'n' ? "enter" : "esc", NULL);
        } else if (sscanf(line, "hletrace %u %n", &code, &used) == 1 && used && !line[used] && code <= 2) {
            runtime_hle_trace_set((int)code);   /* 1 count host calls, 2 also log the first call of each, 0 stop and print the counts */
        } else if (!strncmp(line, "framestep ", 10)) {   /* "framestep <ms>": pin the real-time frame step, 0 = back to real time */
            char *end; double ms = strtod(line + 10, &end); if (end == line + 10 || ms < 0 || ms > 100) runtime_fail("framestep takes milliseconds (0 = real time)");
            runtime_frame_step_pin(ms);
        } else if (sscanf(line, "fov %u %n", &code, &used) == 1 && used && !line[used] && code >= 70 && code <= 140) {
            runtime_fov_set((int)code);   /* "fov <percent>": field of view 70..140 %, 100 = the game's own (aspect_patch.h) */
        } else if (!strncmp(line, "render ", 7)) {   /* "render <w>x<h>": the live scene size (runtime_render_request), as the settings menu asks */
            unsigned w, h; char rest = 0; if (sscanf(line + 7, "%ux%u%c", &w, &h, &rest) < 2 || (rest && rest != '\n' && rest != '\r')) runtime_fail("render takes <width>x<height>");
            runtime_render_request(w, h);
        } else if (!strncmp(line, "memreport", 9) && (line[9] == ' ' || line[9] == '\n' || line[9] == '\r' || !line[9])) {   /* "memreport [tag]": GPU memory breakdown in the log (nvn_mtl.m mtl_mem_report) */
            extern void mtl_mem_report(const char *tag); char *tag = line + 9; tag += *tag == ' '; tag[strcspn(tag, "\r\n")] = 0; mtl_mem_report(tag);
        } else if (!strncmp(line, "set ", 4)) {   /* "set <key> <value>": a setting changed the way the settings menu changes it (settings_set) */
            char key[64], word[64]; int v;
            if (sscanf(line + 4, "%63s %63s", key, word) != 2 || runtime_setting_test(key, word, &v)) runtime_fail("set takes <settings key> <value as settings.txt writes it>");
        } else if (sscanf(line, "menukey %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            runtime_front_settings_key(code);   /* "menukey <Mac keycode>": a key pressed in the open settings menu */
        } else if (!strcmp(line, "restart\n")) {   /* "restart": the menu's Restart now (closes the game, opens TOTK_APP_BUNDLE again) */
            runtime_front_restart();
        } else if (!strcmp(line, "menu\n")) {   /* "menu": open or close the settings menu, as Command-comma does */
            runtime_front_settings_toggle();
        } else if (!strncmp(line, "settings-shot ", 14)) {   /* "settings-shot <png>": the Mac settings panel as a picture, without showing it */
            char *path = line + 14; path[strcspn(path, "\r\n")] = 0;
            runtime_front_settings_shot(path);
#if defined(TOTK_GAME_V143) && !TARGET_OS_IPHONE   /* high-frame-rate diagnostics (game_patches_v143.c) */
        } else if (!strncmp(line, "frmwatch ", 9) || !strncmp(line, "frmwatchx ", 10)) {   /* "frmwatch[x] <seconds> [hex offset]": who reads a time value */
            extern void runtime_diag_frm_watch(double secs, unsigned off, int in_x);
            int in_x = line[8] == 'x'; double secs; unsigned off = 0x10;
            if (sscanf(line + 9 + in_x, "%lf %x", &secs, &off) < 1 || secs <= 0 || off > 0x400 || off % 4) runtime_fail("frmwatch[x] <seconds> [hex offset, a multiple of 4]");
            runtime_diag_frm_watch(secs, off, in_x);
        } else if (!strncmp(line, "xpoke ", 6)) {   /* "xpoke <hex offset> <value>": a float into the game's time object */
            extern void runtime_diag_xpoke(unsigned off, float value);
            unsigned off; float value; if (sscanf(line + 6, "%x %f", &off, &value) != 2 || off > 0x400 || off % 4) runtime_fail("xpoke <hex offset, a multiple of 4> <value>");
            runtime_diag_xpoke(off, value);
#endif
        } else if (!strncmp(line, "mscan ", 6) && runtime_mem_scan_command(line + 6)) {   /* results in memscan.txt */
        } else if (!strcmp(line, "threads\n")) {   /* every guest thread's wait state and guest return chain, now (a hang: what is each thread waiting for) */
            runtime_dump_threads();
        } else if (!strcmp(line, "close\n")) { runtime_front_request_close(); break; }
        else runtime_fail("invalid diagnostic input command");
    }
    fclose(f); return NULL;
}
__attribute__((constructor)) static void input_file_start(void) {
    const char *e = getenv("TOTK_INPUT_FILE"); if (!e || !*e) return;
    pthread_t t; if (pthread_create(&t, NULL, input_file, (void *)e)) runtime_fail("diagnostic input thread"); pthread_detach(t);
}
