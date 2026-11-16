#pragma once
#include <Arduino.h>

// Dashboard served at http://focusdock.local — one self-contained page that
// polls /api/status and /api/history. No external assets, works offline.
static const char WEBPAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Focus Dock</title>
<style>
:root{--bg:#11151a;--card:#1a2028;--fg:#e8edf2;--dim:#8a96a3;--line:#2a333e;--acc:#ff6a3d}
*{box-sizing:border-box}
body{margin:0;padding:20px 16px 40px;background:var(--bg);color:var(--fg);
font:15px/1.4 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:720px;margin:0 auto}
h1{font-size:18px;margin:0 0 4px}
#mode{color:var(--dim);margin-bottom:18px}
#mode b{color:var(--acc);font-variant-numeric:tabular-nums}
.g{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}
.c{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px 14px}
.c small{display:block;color:var(--dim);font-size:12px;text-transform:uppercase;letter-spacing:.06em}
.c span{font-size:26px;font-weight:600;font-variant-numeric:tabular-nums}
.c i{font-style:normal;color:var(--dim);font-size:13px;margin-left:4px}
h2{font-size:13px;color:var(--dim);text-transform:uppercase;letter-spacing:.06em;margin:26px 0 8px}
svg{width:100%;height:auto;background:var(--card);border:1px solid var(--line);border-radius:8px}
svg text{fill:var(--dim);font-size:11px}
</style></head><body><main>
<h1>Focus Dock</h1>
<div id="mode">connecting&hellip;</div>
<div class="g">
<div class="c"><small>CO&#8322;</small><span id="co2">&ndash;</span><i>ppm</i></div>
<div class="c"><small>Temperature</small><span id="t">&ndash;</span><i>&deg;C</i></div>
<div class="c"><small>Humidity</small><span id="h">&ndash;</span><i>%</i></div>
<div class="c"><small>Sessions today</small><span id="s">&ndash;</span></div>
<div class="c"><small>Focused today</small><span id="m">&ndash;</span><i>min</i></div>
<div class="c"><small>Seated</small><span id="seat">&ndash;</span><i>min</i></div>
</div>
<h2>CO&#8322; &middot; last 24 hours</h2>
<svg id="chart" viewBox="0 0 600 180" role="img" aria-label="CO2 history"></svg>
</main><script>
const $=id=>document.getElementById(id);
const mmss=s=>String(Math.floor(s/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0');
async function status(){
  try{
    const d=await (await fetch('/api/status')).json();
    const label={idle:'Idle',focus:'Focusing',paused:'Paused',break:'On a break'}[d.mode];
    $('mode').innerHTML=d.mode=='idle'
      ? 'Idle &middot; next session '+d.focus_min+' min &middot; air: '+d.air
      : label+' &middot; <b>'+mmss(d.remaining_s)+'</b> left &middot; air: '+d.air;
    $('co2').textContent=d.air_valid?d.co2:'–';
    $('t').textContent=d.air_valid?d.temp_c.toFixed(1):'–';
    $('h').textContent=d.air_valid?Math.round(d.humidity):'–';
    $('s').textContent=d.sessions_today;
    $('m').textContent=d.focus_min_today;
    $('seat').textContent=d.present?d.seated_min:'away';
  }catch(e){$('mode').textContent='Device not reachable';}
}
async function history(){
  try{
    const d=await (await fetch('/api/history')).json(), v=d.co2, W=600,H=180,L=40,R=10,T=12,B=22;
    const hi=Math.max(1600,...v), lo=400, x=i=>L+(W-L-R)*(v.length<2?1:i/(v.length-1)),
          y=p=>T+(H-T-B)*(1-(p-lo)/(hi-lo));
    let s='';
    for(const p of [400,800,1000,1400]){
      s+='<line x1="'+L+'" x2="'+(W-R)+'" y1="'+y(p)+'" y2="'+y(p)+'" stroke="#2a333e"/>'+
         '<text x="'+(L-6)+'" y="'+(y(p)+4)+'" text-anchor="end">'+p+'</text>';
    }
    if(v.length){
      s+='<polyline fill="none" stroke="#ff6a3d" stroke-width="2" points="'+
         v.map((p,i)=>x(i).toFixed(1)+','+y(p).toFixed(1)).join(' ')+'"/>';
      const hrs=(v.length*d.interval_s/3600).toFixed(1);
      s+='<text x="'+L+'" y="'+(H-6)+'">'+hrs+' h ago</text>'+
         '<text x="'+(W-R)+'" y="'+(H-6)+'" text-anchor="end">now</text>';
    }else{
      s+='<text x="'+W/2+'" y="'+H/2+'" text-anchor="middle">First point appears after 5 minutes</text>';
    }
    $('chart').innerHTML=s;
  }catch(e){}
}
status();history();setInterval(status,2000);setInterval(history,60000);
</script></body></html>)HTML";
