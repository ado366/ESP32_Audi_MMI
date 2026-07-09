// main_native.cpp — desktop emulator host.
// Runs the real App against native HAL stubs and serves a browser UI: a canvas
// rendering the 64x88 FIS, clickable console/steering buttons + rotary, and a
// scriptable fake-BC127 panel. Develop features here without the car (plan §7).
#include <cstdio>
#include <string>
#include <chrono>
#include "App.h"
#include "hal/native/EmulatorDisplay.h"
#include "hal/native/MemoryStorage.h"
#include "hal/native/FakeBluetooth.h"
#include "hal/native/FakeDiag.h"
#include "hal/native/ScriptedInputs.h"
#include "hal/native/HttpServer.h"

using namespace mmi;

#ifndef PIO_UNIT_TESTING

static Control controlFromName(const std::string& n) {
  if (n == "Menu") return Control::Menu;
  if (n == "Return") return Control::Return;
  if (n == "Nav") return Control::Nav;
  if (n == "Info") return Control::Info;
  if (n == "Traffic") return Control::Traffic;
  if (n == "EncoderCW") return Control::EncoderCW;
  if (n == "EncoderCCW") return Control::EncoderCCW;
  if (n == "EncoderClick") return Control::EncoderClick;
  if (n == "EncoderHold") return Control::EncoderHold;
  if (n == "SteerLeftPlus") return Control::SteerLeftPlus;
  if (n == "SteerLeftMinus") return Control::SteerLeftMinus;
  if (n == "SteerRightPlus") return Control::SteerRightPlus;
  if (n == "SteerRightMinus") return Control::SteerRightMinus;
  return Control::None;
}

// crude x-www-form / query value extractor: key=value&...
static std::string field(const std::string& body, const std::string& key) {
  size_t p = body.find(key + "=");
  if (p == std::string::npos) return "";
  p += key.size() + 1;
  size_t e = body.find('&', p);
  return body.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static const char* kPage = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<title>Audi MMI Emulator</title><style>
body{background:#111;color:#ddd;font-family:sans-serif;display:flex;gap:24px;padding:20px}
canvas{background:#000;border:6px solid #222;image-rendering:pixelated}
button{margin:3px;padding:8px 10px;background:#333;color:#eee;border:1px solid #555;border-radius:4px;cursor:pointer}
button:hover{background:#444}.grp{margin-bottom:14px}h3{margin:6px 0;color:#fa0}
.status{font-family:monospace;white-space:pre;background:#000;padding:8px;border-radius:4px}
</style></head><body>
<div><canvas id=fis width=256 height=352></canvas><div class=status id=st></div></div>
<div>
<div class=grp><h3>Console</h3>
<button onclick=k('Menu')>MENU</button><button onclick=k('Return')>RETURN</button>
<button onclick=k('Nav')>NAV</button><button onclick=k('Info')>INFO</button>
<button onclick=k('Traffic')>TRAFFIC (diag)</button></div>
<div class=grp><h3>Rotary</h3>
<button onclick=k('EncoderCCW',-1)>&#8630; CCW</button><button onclick=k('EncoderClick')>CLICK</button>
<button onclick=k('EncoderCW',1)>CW &#8631;</button><button onclick=k('EncoderHold')>HOLD</button></div>
<div class=grp><h3>Steering</h3>
<button onclick=k('SteerLeftPlus')>L+ vol</button><button onclick=k('SteerLeftMinus')>L- vol</button>
<button onclick=k('SteerRightPlus')>R+ trk/ans</button><button onclick=k('SteerRightMinus')>R- trk/end</button></div>
<div class=grp><h3>Fake BC127</h3>
<button onclick=b('connectA')>Connect A (Pixel)</button><button onclick=b('connectB')>Connect B while A</button>
<button onclick=b('play')>Play/Pause</button><button onclick=b('meta')>Track metadata</button><br>
<button onclick=b('call')>Incoming call</button><button onclick=b('end')>End call</button></div>
</div>
<script>
const W=64,H=88,S=4;const cv=document.getElementById('fis'),cx=cv.getContext('2d');
const ON='#ff5a2a',BG='#0a0a08';        // red-orange dots on near-black, per real FIS
// 5x7 dot-matrix font (7 rows of 5), uppercase — matches the Maxi-K screenshot.
const G={
' ':['     ','     ','     ','     ','     ','     ','     '],
'0':[' ### ','#   #','#  ##','# # #','##  #','#   #',' ### '],
'1':['  #  ',' ##  ','  #  ','  #  ','  #  ','  #  ',' ### '],
'2':[' ### ','#   #','    #','   # ','  #  ',' #   ','#####'],
'3':['#####','   # ','  #  ','   # ','    #','#   #',' ### '],
'4':['   # ','  ## ',' # # ','#  # ','#####','   # ','   # '],
'5':['#####','#    ','#### ','    #','    #','#   #',' ### '],
'6':['  ## ',' #   ','#    ','#### ','#   #','#   #',' ### '],
'7':['#####','    #','   # ','  #  ',' #   ',' #   ',' #   '],
'8':[' ### ','#   #','#   #',' ### ','#   #','#   #',' ### '],
'9':[' ### ','#   #','#   #',' ####','    #','   # ',' ##  '],
'A':[' ### ','#   #','#   #','#####','#   #','#   #','#   #'],
'B':['#### ','#   #','#   #','#### ','#   #','#   #','#### '],
'C':[' ### ','#   #','#    ','#    ','#    ','#   #',' ### '],
'D':['#### ','#   #','#   #','#   #','#   #','#   #','#### '],
'E':['#####','#    ','#    ','#### ','#    ','#    ','#####'],
'F':['#####','#    ','#    ','#### ','#    ','#    ','#    '],
'G':[' ### ','#   #','#    ','# ###','#   #','#   #',' ####'],
'H':['#   #','#   #','#   #','#####','#   #','#   #','#   #'],
'I':[' ### ','  #  ','  #  ','  #  ','  #  ','  #  ',' ### '],
'J':['  ###','   # ','   # ','   # ','#  # ','#  # ',' ##  '],
'K':['#   #','#  # ','# #  ','##   ','# #  ','#  # ','#   #'],
'L':['#    ','#    ','#    ','#    ','#    ','#    ','#####'],
'M':['#   #','## ##','# # #','#   #','#   #','#   #','#   #'],
'N':['#   #','##  #','# # #','#  ##','#   #','#   #','#   #'],
'O':[' ### ','#   #','#   #','#   #','#   #','#   #',' ### '],
'P':['#### ','#   #','#   #','#### ','#    ','#    ','#    '],
'Q':[' ### ','#   #','#   #','#   #','# # #','#  # ',' ## #'],
'R':['#### ','#   #','#   #','#### ','# #  ','#  # ','#   #'],
'S':[' ####','#    ','#    ',' ### ','    #','    #','#### '],
'T':['#####','  #  ','  #  ','  #  ','  #  ','  #  ','  #  '],
'U':['#   #','#   #','#   #','#   #','#   #','#   #',' ### '],
'V':['#   #','#   #','#   #','#   #','#   #',' # # ','  #  '],
'W':['#   #','#   #','#   #','# # #','# # #','## ##','#   #'],
'X':['#   #','#   #',' # # ','  #  ',' # # ','#   #','#   #'],
'Y':['#   #','#   #',' # # ','  #  ','  #  ','  #  ','  #  '],
'Z':['#####','    #','   # ','  #  ',' #   ','#    ','#####'],
'.':['     ','     ','     ','     ','     ',' ##  ',' ##  '],
',':['     ','     ','     ','     ',' ##  ',' ##  ',' #   '],
':':['     ',' ##  ',' ##  ','     ',' ##  ',' ##  ','     '],
'-':['     ','     ','     ','#####','     ','     ','     '],
'+':['     ','  #  ','  #  ','#####','  #  ','  #  ','     '],
'/':['    #','    #','   # ','  #  ',' #   ','#    ','#    '],
'>':['#    ',' #   ','  #  ','   # ','  #  ',' #   ','#    '],
'<':['    #','   # ','  #  ',' #   ','  #  ','   # ','    #'],
'(':['  #  ',' #   ','#    ','#    ','#    ',' #   ','  #  '],
')':['  #  ','   # ','    #','    #','    #','   # ','  #  '],
'%':['##  #','##  #','   # ','  #  ',' #   ','#  ##','#  ##'],
'!':['  #  ','  #  ','  #  ','  #  ','  #  ','     ','  #  '],
'?':[' ### ','#   #','    #','   # ','  #  ','     ','  #  '],
'*':['     ','# # #',' ### ','#####',' ### ','# # #','     '],
"'":['  #  ','  #  ','     ','     ','     ','     ','     '],
'#':[' # # ','#####',' # # ','#####',' # # ','     ','     ']};
function k(c,d){fetch('/input',{method:'POST',body:'c='+c+'&d='+(d||0)}).then(poll)}
function b(s){fetch('/bt',{method:'POST',body:'s='+s}).then(poll)}
// draw a string of dot-matrix glyphs starting at dot (dx,dy); colour c; returns width in dots
function text(str,dx,dy,c,cw){cw=cw||6;str=(str||'').toUpperCase();let x=dx;cx.fillStyle=c;
  for(const ch of str){const g=G[ch]||G['?'];
    for(let r=0;r<7;r++)for(let col=0;col<5;col++) if(g[r][col]=='#')
      cx.fillRect((x+col)*S,(dy+r)*S,S-1,S-1);       // square pixels w/ 1px grid gap
    x+=cw;} return (x-dx);}
function textW(str,cw){return (str||'').length*(cw||6);}
function draw(f){cx.fillStyle=BG;cx.fillRect(0,0,cv.width,cv.height);
  if(f.mode=='top'){const a=(f.top[0]||'').trim(),b2=(f.top[1]||'').trim();
    text(a,Math.max(0,(W-textW(a))/2),1,ON);text(b2,Math.max(0,(W-textW(b2))/2),10,ON);return;}
  for(const o of f.ops){
    if(o.t=='text'){const cen=(o.f&0x20),neg=!(o.f&0x01),cw=(o.f&0x04)?5:6;
      const w=textW(o.s,cw),x=cen?Math.max(0,(W-w)/2):o.x;
      if(neg){cx.fillStyle=ON;cx.fillRect(x*S-2,o.y*S-2,w*S+4,7*S+4);text(o.s,x,o.y,BG,cw);}
      else text(o.s,x,o.y,ON,cw);
    } else if(o.t=='bmp'){cx.fillStyle=ON;for(let yy=0;yy<o.h;yy++)for(let xx=0;xx<o.w;xx++){
      let bit=yy*o.w+xx,by=parseInt(o.d.substr((bit>>3)*2,2),16);
      if(by&(0x80>>(bit&7)))cx.fillRect((o.x+xx)*S,(o.y+yy)*S,S-1,S-1);}}}}
function poll(){fetch('/state').then(r=>r.json()).then(j=>{draw(j.display);
  document.getElementById('st').textContent=
    'link : '+(j.bt.linked?j.bt.device:'-')+'\ncall : '+j.bt.call+
    (j.bt.caller?(' '+ (j.bt.callerName||j.bt.caller)):'')+
    '\nplay : '+j.bt.playing+'\nctx  : '+j.ctx;})}
setInterval(poll,150);poll();
</script></body></html>)HTML";

static std::string btJson(const BtStatus& s, const char* ctx) {
  auto callName = [](CallState c){ switch(c){case CallState::Incoming:return "incoming";
    case CallState::Outgoing:return "outgoing";case CallState::Active:return "active";default:return "idle";}};
  std::string j = "{\"linked\":"; j += s.linked ? "true" : "false";
  j += ",\"device\":\"" + s.activeDeviceName + "\"";
  j += ",\"playing\":"; j += s.playing ? "true" : "false";
  j += ",\"call\":\""; j += callName(s.call); j += "\"";
  j += ",\"caller\":\"" + s.callerNumber + "\",\"callerName\":\"" + s.callerName + "\"}";
  return j;
}

static uint32_t nowMs() {
  using namespace std::chrono;
  static auto t0 = steady_clock::now();
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

static const char* ctxName(Context c) {
  switch (c) { case Context::Menu:return "menu"; case Context::IncomingCall:return "incoming";
    case Context::ActiveCall:return "active"; case Context::Diagnostics:return "diag"; default:return "nowplaying"; }
}

int main(int argc, char**) {
  EmulatorDisplay display;
  ScriptedInputs  inputs;
  FakeBluetooth   bt;
  MemoryStorage   storage;
  FakeDiag        diag;
  bt.setPhonebook({{"ALICE", "+41791234567"}, {"BOB", "+41797654321"}});

  App app(display, inputs, bt, storage, diag);
  // Seed paired devices + phonebook so the Switch-Device / Phonebook screens work.
  app.bluetooth().setPaired({
    {"AA:BB:CC:DD:EE:01", "PIXEL",  0, true},
    {"AA:BB:CC:DD:EE:02", "IPHONE", 0, true},
    {"AA:BB:CC:DD:EE:03", "GALAXY", 0, false}, // out of range -> fallback demo
  });
  for (auto& c : {Contact{"ALICE", "+41791234567"}, Contact{"BOB", "+41797654321"}})
    app.phonebook().add(c.name, c.number, 500);
  // Seed a few favourites showcasing each view mode.
  { Preset p; p.ecu = ecu::Engine; p.group = 2; p.valueIndex = 0; p.view = View::Graph;
    p.min = 0; p.max = 5000; p.guide1 = 4000; std::snprintf(p.label, 9, "RPM"); app.presets().add(p); }
  { Preset p; p.ecu = ecu::Engine; p.group = 115; p.valueIndex = 0; p.view = View::MultiValue;
    std::snprintf(p.label, 9, "BOOST"); app.presets().add(p); }
  { Preset p; p.ecu = ecu::Dashboard; p.group = 2; p.valueIndex = 3; p.view = View::TopLine;
    p.min = 60; p.max = 120; std::snprintf(p.label, 9, "COOLANT"); app.presets().add(p); }
  app.begin();
  app.tick(nowMs());

  HttpServer srv(8080);
  srv.onRequest([&](const HttpRequest& r) -> HttpResponse {
    if (r.path == "/" || r.path.rfind("/index", 0) == 0)
      return { "text/html", kPage };

    if (r.path == "/input") {
      std::string c = field(r.body, "c");
      int d = std::atoi(field(r.body, "d").c_str());
      Control ctl = controlFromName(c);
      if (ctl != Control::None) inputs.push(ctl, static_cast<int8_t>(d ? d : 0));
      app.tick(nowMs());
      return { "text/plain", "ok" };
    }
    if (r.path == "/bt") {
      std::string s = field(r.body, "s");
      if (s == "connectA") bt.scriptConnect("AA:BB:CC:DD:EE:01", "PIXEL");
      else if (s == "connectB") bt.scriptSecondConnects("AA:BB:CC:DD:EE:02", "IPHONE");
      else if (s == "play") bt.playPause();
      else if (s == "meta") bt.scriptMetadata("BOHEMIAN RHAPSODY", "QUEEN");
      else if (s == "call") bt.scriptIncomingCall("+41791234567");
      else if (s == "end") bt.callEnd();
      app.tick(nowMs());
      return { "text/plain", "ok" };
    }
    if (r.path == "/state") {
      app.tick(nowMs()); // advance time each poll so marquee/animation runs
      std::string j = "{\"display\":" + display.toJson() +
                      ",\"bt\":" + btJson(bt.status(), ctxName(app.context())) +
                      ",\"ctx\":\"" + ctxName(app.context()) + "\"}";
      return { "application/json", j };
    }
    return { "text/plain", "not found", 404 };
  });

  if (!srv.start()) { std::printf("emulator: failed to bind :8080\n"); return 1; }
  std::printf("Audi MMI emulator on http://localhost:8080  (Ctrl-C to stop)\n");
  (void)argc;
  srv.run();
  return 0;
}
#endif // PIO_UNIT_TESTING
