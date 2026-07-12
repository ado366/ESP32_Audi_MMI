// WebUi.h — shared browser control/debug UI: the HTML/canvas page plus the
// server-side helpers. Used by the native emulator AND the real device's OTA
// web server, so the whole UI can be driven from a computer with no car buttons.
#pragma once
#include "../hal/Types.h"
#include "../hal/IBluetooth.h"
#include <string>

namespace mmi {

// Map a control name (from the page) to a Control.
inline Control webControlFromName(const std::string& n) {
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
  // Steering + console chords (so the browser control page can exercise them)
  if (n == "SteerLPlusRPlus") return Control::SteerLPlusRPlus;
  if (n == "SteerLPlusRMinus") return Control::SteerLPlusRMinus;
  if (n == "SteerLMinusRPlus") return Control::SteerLMinusRPlus;
  if (n == "SteerLMinusRMinus") return Control::SteerLMinusRMinus;
  if (n == "SteerRightPlusHold") return Control::SteerRightPlusHold;
  if (n == "MenuReturn") return Control::MenuReturn;
  if (n == "MenuNav") return Control::MenuNav;
  if (n == "TrafficReverse") return Control::TrafficReverse;
  if (n == "InfoReverse") return Control::InfoReverse;
  return Control::None;
}

// Extract key=value from an x-www-form body.
inline std::string webField(const std::string& body, const std::string& key) {
  size_t p = body.find(key + "=");
  if (p == std::string::npos) return "";
  p += key.size() + 1;
  size_t e = body.find('&', p);
  return body.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

inline const char* webCtxName(Context c) {
  switch (c) {
    case Context::Menu: return "menu";
    case Context::IncomingCall: return "incoming";
    case Context::ActiveCall: return "active";
    case Context::Diagnostics: return "diag";
    default: return "nowplaying";
  }
}

inline std::string webJsonEsc(const std::string& in) {
  std::string o;
  for (char c : in) { if (c == '"' || c == '\\') o.push_back('\\'); if ((unsigned char)c >= 0x20) o.push_back(c); }
  return o;
}

inline std::string webBtJson(const BtStatus& s) {
  auto call = [](CallState c) {
    switch (c) { case CallState::Incoming: return "incoming"; case CallState::Outgoing: return "outgoing";
                 case CallState::Active: return "active"; default: return "idle"; }
  };
  std::string j = "{\"linked\":"; j += s.linked ? "true" : "false";
  j += ",\"device\":\"" + webJsonEsc(s.activeDeviceName) + "\"";
  j += ",\"playing\":"; j += s.playing ? "true" : "false";
  j += ",\"call\":\""; j += call(s.call); j += "\"";
  j += ",\"caller\":\"" + webJsonEsc(s.callerNumber) + "\",\"callerName\":\"" + webJsonEsc(s.callerName) + "\"}";
  return j;
}

// The control page. Renders the 64x88 FIS to a canvas (square red-on-black
// dot-matrix, compressed-font aware) and posts control presses to /input.
inline const char* kWebUiPage = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Audi MMI</title><style>
body{background:#111;color:#ddd;font-family:sans-serif;display:flex;flex-wrap:wrap;gap:20px;padding:16px}
canvas{background:#000;border:6px solid #222;image-rendering:pixelated}
button{margin:3px;padding:10px 12px;background:#333;color:#eee;border:1px solid #555;border-radius:4px;cursor:pointer}
button:active{background:#666}.grp{margin-bottom:12px}h3{margin:6px 0;color:#fa0}
.status{font-family:monospace;white-space:pre;background:#000;padding:8px;border-radius:4px}
</style></head><body>
<div><canvas id=fis width=256 height=352></canvas><div class=status id=st></div></div>
<div>
<div class=grp><h3>Console</h3>
<button onclick=k('Menu')>MENU</button><button onclick=k('Return')>RETURN</button>
<button onclick=k('Nav')>NAV</button><button onclick=k('Info')>INFO</button>
<button onclick=k('Traffic')>TRAFFIC</button></div>
<div class=grp><h3>Rotary</h3>
<button onclick=k('EncoderCCW',-1)>CCW</button><button onclick=k('EncoderClick')>CLICK</button>
<button onclick=k('EncoderCW',1)>CW</button><button onclick=k('EncoderHold')>HOLD</button></div>
<div class=grp><h3>Steering</h3>
<button onclick=k('SteerLeftPlus')>L+ vol</button><button onclick=k('SteerLeftMinus')>L- vol</button>
<button onclick=k('SteerRightPlus')>R+ trk/ans</button><button onclick=k('SteerRightMinus')>R- trk/end</button>
<button onclick=k('SteerRightPlusHold')>R+ hold (voice)</button></div>
</div>
<script>
const W=64,H=88,S=4;const cv=document.getElementById('fis'),cx=cv.getContext('2d');
const ON='#ff5a2a',BG='#0a0a08';
const G={
' ':['     ','     ','     ','     ','     ','     ','     '],
'0':[' ### ','#   #','#  ##','# # #','##  #','#   #',' ### '],'1':['  #  ',' ##  ','  #  ','  #  ','  #  ','  #  ',' ### '],
'2':[' ### ','#   #','    #','   # ','  #  ',' #   ','#####'],'3':['#####','   # ','  #  ','   # ','    #','#   #',' ### '],
'4':['   # ','  ## ',' # # ','#  # ','#####','   # ','   # '],'5':['#####','#    ','#### ','    #','    #','#   #',' ### '],
'6':['  ## ',' #   ','#    ','#### ','#   #','#   #',' ### '],'7':['#####','    #','   # ','  #  ',' #   ',' #   ',' #   '],
'8':[' ### ','#   #','#   #',' ### ','#   #','#   #',' ### '],'9':[' ### ','#   #','#   #',' ####','    #','   # ',' ##  '],
'A':[' ### ','#   #','#   #','#####','#   #','#   #','#   #'],'B':['#### ','#   #','#   #','#### ','#   #','#   #','#### '],
'C':[' ### ','#   #','#    ','#    ','#    ','#   #',' ### '],'D':['#### ','#   #','#   #','#   #','#   #','#   #','#### '],
'E':['#####','#    ','#    ','#### ','#    ','#    ','#####'],'F':['#####','#    ','#    ','#### ','#    ','#    ','#    '],
'G':[' ### ','#   #','#    ','# ###','#   #','#   #',' ####'],'H':['#   #','#   #','#   #','#####','#   #','#   #','#   #'],
'I':[' ### ','  #  ','  #  ','  #  ','  #  ','  #  ',' ### '],'J':['  ###','   # ','   # ','   # ','#  # ','#  # ',' ##  '],
'K':['#   #','#  # ','# #  ','##   ','# #  ','#  # ','#   #'],'L':['#    ','#    ','#    ','#    ','#    ','#    ','#####'],
'M':['#   #','## ##','# # #','#   #','#   #','#   #','#   #'],'N':['#   #','##  #','# # #','#  ##','#   #','#   #','#   #'],
'O':[' ### ','#   #','#   #','#   #','#   #','#   #',' ### '],'P':['#### ','#   #','#   #','#### ','#    ','#    ','#    '],
'Q':[' ### ','#   #','#   #','#   #','# # #','#  # ',' ## #'],'R':['#### ','#   #','#   #','#### ','# #  ','#  # ','#   #'],
'S':[' ####','#    ','#    ',' ### ','    #','    #','#### '],'T':['#####','  #  ','  #  ','  #  ','  #  ','  #  ','  #  '],
'U':['#   #','#   #','#   #','#   #','#   #','#   #',' ### '],'V':['#   #','#   #','#   #','#   #','#   #',' # # ','  #  '],
'W':['#   #','#   #','#   #','# # #','# # #','## ##','#   #'],'X':['#   #','#   #',' # # ','  #  ',' # # ','#   #','#   #'],
'Y':['#   #','#   #',' # # ','  #  ','  #  ','  #  ','  #  '],'Z':['#####','    #','   # ','  #  ',' #   ','#    ','#####'],
'.':['     ','     ','     ','     ','     ',' ##  ',' ##  '],',':['     ','     ','     ','     ',' ##  ',' ##  ',' #   '],
':':['     ',' ##  ',' ##  ','     ',' ##  ',' ##  ','     '],'-':['     ','     ','     ','#####','     ','     ','     '],
'+':['     ','  #  ','  #  ','#####','  #  ','  #  ','     '],'/':['    #','    #','   # ','  #  ',' #   ','#    ','#    '],
'>':['#    ',' #   ','  #  ','   # ','  #  ',' #   ','#    '],'<':['    #','   # ','  #  ',' #   ','  #  ','   # ','    #'],
'(':['  #  ',' #   ','#    ','#    ','#    ',' #   ','  #  '],')':['  #  ','   # ','    #','    #','    #','   # ','  #  '],
'%':['##  #','##  #','   # ','  #  ',' #   ','#  ##','#  ##'],'!':['  #  ','  #  ','  #  ','  #  ','  #  ','     ','  #  '],
'?':[' ### ','#   #','    #','   # ','  #  ','     ','  #  '],'*':['     ','# # #',' ### ','#####',' ### ','# # #','     '],
"'":['  #  ','  #  ','     ','     ','     ','     ','     '],'#':[' # # ','#####',' # # ','#####',' # # ','     ','     ']};
function k(c,d){fetch('/input',{method:'POST',body:'c='+c+'&d='+(d||0)}).then(poll).catch(()=>{})}
function text(str,dx,dy,c,cw){cw=cw||6;str=(str||'').toUpperCase();let x=dx;cx.fillStyle=c;
  for(const ch of str){const g=G[ch]||G['?'];
    for(let r=0;r<7;r++)for(let col=0;col<5;col++) if(g[r][col]=='#') cx.fillRect((x+col)*S,(dy+r)*S,S-1,S-1);
    x+=cw;} return (x-dx);}
function textW(str,cw){return (str||'').length*(cw||6);}
function topLine(s,y){s=(s||'');   // >=8 chars: left-aligned (matches FIS); else centred
  if(s.length>=8){text(s,0,y,ON);} else {s=s.trim();text(s,Math.max(0,(W-textW(s))/2),y,ON);}}
function draw(f){cx.fillStyle=BG;cx.fillRect(0,0,cv.width,cv.height);
  if(f.mode=='top'){topLine(f.top[0],1);topLine(f.top[1],10);return;}
  for(const o of f.ops){
    if(o.t=='text'){const hl=(o.f&0x40),cen=(o.f&0x20),cw=(o.f&0x04)?5:6;
      const w=textW(o.s,cw),x=cen?Math.max(0,(W-w)/2):o.x;
      if(hl){cx.fillStyle=ON;cx.fillRect(0,o.y*S,W*S,7*S);text(o.s,x,o.y,BG,cw);}
      else text(o.s,x,o.y,ON,cw);}
    else if(o.t=='bmp'){cx.fillStyle=ON;for(let yy=0;yy<o.h;yy++)for(let xx=0;xx<o.w;xx++){
      let bit=yy*o.w+xx,by=parseInt(o.d.substr((bit>>3)*2,2),16);
      if(by&(0x80>>(bit&7)))cx.fillRect((o.x+xx)*S,(o.y+yy)*S,S-1,S-1);}}}}
function poll(){fetch('/state').then(r=>r.json()).then(j=>{draw(j.display);
  document.getElementById('st').textContent='link: '+(j.bt.linked?j.bt.device:'-')+
    '\ncall: '+j.bt.call+'\nplay: '+j.bt.playing+'\nctx : '+j.ctx;}).catch(()=>{})}
setInterval(poll,150);poll();
</script></body></html>)HTML";

} // namespace mmi
