// web_ui.h - the single-page web interface served by the ESP32 (no external resources, works offline on the AP).
#pragma once
static const char INDEX_HTML[] = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Capacitor checker</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--tx:#1c1f24;--mut:#667;--ln:#dde0e6;--ac:#1f6feb;--g:#1a7f37;--w:#b26a00;--b:#c62828}
@media(prefers-color-scheme:dark){:root{--bg:#14161a;--card:#1d2026;--tx:#e8eaee;--mut:#9aa0ab;--ln:#2e323b;--ac:#58a6ff;--g:#3fb950;--w:#e3a008;--b:#ff6b6b}}
*{box-sizing:border-box}body{margin:0;font:15px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--tx)}
header{padding:12px 14px;background:var(--card);border-bottom:1px solid var(--ln);position:sticky;top:0;z-index:2}
h1{font-size:17px;margin:0 0 6px}.chips span{display:inline-block;font-size:12px;padding:2px 8px;margin:0 4px 4px 0;border-radius:99px;border:1px solid var(--ln);color:var(--mut)}
.chips .ok{color:var(--g);border-color:var(--g)}.chips .no{color:var(--b);border-color:var(--b)}
nav{display:flex;background:var(--card);border-bottom:1px solid var(--ln)}nav button{flex:1;padding:11px;border:0;background:none;color:var(--mut);font-size:15px;border-bottom:3px solid transparent}
nav button.on{color:var(--tx);border-color:var(--ac);font-weight:600}
main{padding:12px;max-width:640px;margin:auto}section{display:none}section.on{display:block}
.card{background:var(--card);border:1px solid var(--ln);border-radius:12px;padding:12px;margin-bottom:12px}
label{display:block;font-size:13px;color:var(--mut);margin:8px 0 3px}select,input{width:100%;padding:9px;font-size:16px;border-radius:8px;border:1px solid var(--ln);background:var(--bg);color:var(--tx)}
button.go{width:100%;margin-top:10px;padding:12px;font-size:16px;font-weight:600;border:0;border-radius:10px;background:var(--ac);color:#fff}
button.go:disabled{opacity:.45}button.sm{padding:6px 10px;border:1px solid var(--ln);background:none;color:var(--b);border-radius:8px}
.bar{height:8px;background:var(--ln);border-radius:4px;overflow:hidden;margin-top:10px;display:none}.bar i{display:block;height:100%;width:0;background:var(--ac);transition:width .3s}
.big{font-size:34px;font-weight:700;letter-spacing:.5px}.GOOD{color:var(--g)}.WEAK{color:var(--w)}.BAD,.SHORT{color:var(--b)}.OPEN,.PARALLEL{color:var(--w)}
.kv{display:flex;justify-content:space-between;padding:5px 0;border-bottom:1px solid var(--ln)}.kv b{font-variant-numeric:tabular-nums}
.mut{color:var(--mut);font-size:13px}.step{border-left:4px solid var(--ln);padding-left:10px}.step.done{border-color:var(--g)}
.msg{margin-top:8px;font-size:14px}.msg.err{color:var(--b)}table{width:100%;border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums}td,th{padding:3px 4px;text-align:right}th{color:var(--mut)}
</style></head><body>
<header><h1>Capacitor checker</h1><div class="chips" id="chips"></div></header>
<nav><button id="t0" class="on" onclick="tab(0)">Test</button><button id="t1" onclick="tab(1)">Calibrate</button><button id="t2" onclick="tab(2)">Good parts</button></nav>
<main>
<div class="card" id="startcard"><label>When does a measurement start?</label><select id="sm"><option value="0" selected>When the probes are on the part (recommended)</option><option value="1">After a 5 s countdown</option><option value="2">Immediately</option></select>
<div class="mut" style="margin-top:6px">Press the button, then clip on: the checker waits for firm contact. A part below about 0.16 &micro;F is not detected: use the countdown.</div>
<div id="live" style="display:none;margin-top:10px"><div class="msg" id="livemsg"></div><button class="sm" onclick="cancelJob()">Cancel</button></div></div>
<section id="s0" class="on">
 <div class="card"><label>Nominal value of the part (what is printed on it)</label><select id="nom"></select>
 <input id="nomc" type="number" step="any" min="0.01" placeholder="value in &micro;F" style="display:none;margin-top:6px">
 <label>Accuracy</label><select id="lvl"><option value="0">Quick (one reading per frequency)</option><option value="1" selected>Normal (repeats until precise)</option><option value="2">Precise (more repeats)</option></select>
 <button class="go" id="gotest" onclick="startTest()">TEST</button><div class="bar" id="bar0"><i></i></div><div class="msg" id="m0"></div></div>
 <div class="card" id="res" style="display:none"></div>
 <p class="mut">Power off and fully discharge the board and the capacitor first. Error bars are 1&sigma;. A verdict marked ? means the error bars cross a threshold: measure again with Precise, or out of circuit.</p>
</section>
<section id="s1">
 <div class="card step" id="st1"><b>1. Clips open</b><div class="mut">Clips apart, leads in their normal position, away from metal. Measures the empty-clip reference and the noise floor. Redoing this clears steps 2-4. Takes about a minute.</div><button class="go" onclick="startCal('open')">Run step 1</button><div class="bar"><i></i></div><div class="msg"></div></div>
 <div class="card step" id="st2"><b>2. Clips shorted</b><div class="mut">Clip the two clips firmly onto each other (or one piece of thick wire), at the contact points you will use. Removes the lead resistance from every reading.</div><button class="go" onclick="startCal('short')">Run step 2</button><div class="bar"><i></i></div><div class="msg"></div></div>
 <div class="card step" id="st3"><b>3. Known resistor (gain)</b><div class="mut">Optional, recommended. A 1 % resistor of 2.2 to 10 &Omega; (4.7 &Omega; is a good choice) across the clips. Improves ESR accuracy.</div><label>Resistor value (&Omega;)</label><input id="rv" type="number" step="any" value="4.7"><button class="go" onclick="startCal('res','rv')">Run step 3</button><div class="bar"><i></i></div><div class="msg"></div></div>
 <div class="card step" id="st4"><b>4. Known capacitor (C scale)</b><div class="mut">Optional. A capacitor whose value you trust (film or C0G, 1-5 %). Do this once for a small value (3 &micro;F or less) and once for a large one (above 3 &micro;F); in between the correction is interpolated.</div><label>Capacitor value (&micro;F)</label><input id="cv" type="number" step="any" value="1"><button class="go" onclick="startCal('cap','cv')">Run step 4</button><div class="bar"><i></i></div><div class="msg"></div></div>
 <div class="card"><b>Calibration state</b><div id="calinfo" class="mut"></div><button class="sm" style="margin-top:8px" onclick="resetCal()">Erase calibration</button></div>
</section>
<section id="s2">
 <div class="card"><b>Learn a known-good part</b><div class="mut">Measure a part you know is good. Later tests of the same nominal value are judged against it (ESR more than 1.5x = WEAK, 2.5x = BAD; C more than 10 % / 20 % lower = WEAK / BAD). Measuring several good parts averages them.</div>
 <label>Nominal value</label><select id="rnom"></select><input id="rnomc" type="number" step="any" min="0.01" placeholder="value in &micro;F" style="display:none;margin-top:6px">
 <button class="go" onclick="startLearn()">Measure and add as good part</button><div class="bar" id="bar2"><i></i></div><div class="msg" id="m2"></div></div>
 <div class="card"><b>Learned parts</b><div id="refs" class="mut">none yet</div></div>
 <div class="card"><b>Screen position</b><div class="mut">If the picture on the device is cut off, nudge it here until the border is fully visible.</div>
 <div style="display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:8px"><button class="sm" onclick="disp(0,-1,0)">Y &minus;</button><button class="sm" onclick="disp(0,1,0)">Y +</button><button class="sm" onclick="disp(0,0,1)">Flip</button><button class="sm" onclick="disp(-1,0,0)">X &minus;</button><button class="sm" onclick="disp(1,0,0)">X +</button><button class="sm" onclick="disp(0,0,2)">Reset</button></div><div class="mut" id="dispinfo" style="margin-top:6px"></div>
 <div class="mut">On the device itself: hold the button for 10 s.</div></div>
 <div class="card"><button class="sm" onclick="wifiOff()">Turn Wi-Fi off until reset</button><div class="mut" style="margin-top:6px">Wi-Fi radio bursts can add a little ADC noise. To compare, switch it off and test with the BOOT button.</div></div>
</section></main>
<script>
const NOM=[0,0.47,1,2.2,3.3,4.7,10,22,33,47,100,220,330,470,1000,2200,3300,4700,10000],$=id=>document.getElementById(id);
function fillNom(sel,auto){let h='';NOM.forEach(v=>{if(v===0&&!auto)return;h+='<option value="'+v+'">'+(v===0?'AUTO (guess from ESR x C)':(v>=1000?v/1000+' mF':v+' µF'))+'</option>'});h+='<option value="x">Other…</option>';sel.innerHTML=h}
fillNom($('nom'),true);fillNom($('rnom'),false);$('rnom').value='100';
$('nom').onchange=()=>{$('nomc').style.display=$('nom').value==='x'?'block':'none'};$('rnom').onchange=()=>{$('rnomc').style.display=$('rnom').value==='x'?'block':'none'};
function nomVal(s,c){return s.value==='x'?parseFloat(c.value)||0:parseFloat(s.value)}
function tab(i){for(let k=0;k<3;k++){$('s'+k).classList.toggle('on',k===i);$('t'+k).classList.toggle('on',k===i)}if(i>0)status()}
async function api(p,m){const r=await fetch(p,{method:m||'GET'});return r.json()}
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
function fmtC(u){if(u==null)return'?';return u>=1000?(u/1000).toPrecision(4)+' mF':u>=10?u.toPrecision(4)+' µF':u.toPrecision(3)+' µF'}
function fmtE(e){return e==null?'?':(Math.abs(e)<0.1?(e*1000).toPrecision(3)+' mΩ':e.toPrecision(3)+' Ω')}
let barEl=null,msgEl=null,busyBtns=[];
function setBusy(b){document.querySelectorAll('button.go').forEach(x=>x.disabled=b)}
function smq(){return '&sm='+$('sm').value+'&dl=5'}
function livemsg(j){const n=j.n>0?Math.round(100*Math.min(1,j.i/j.n)):0;
 if(j.phase===1)return j.good>0?'Probes detected: hold still… ('+j.good+'/'+j.need+')':'Waiting for the probes: clip them on the part…';
 if(j.phase===2)return 'Starting in '+j.cnt+' s: get ready…';
 if(j.phase===3)return 'Measuring… '+n+' %  (keep the probes on)';
 return 'Starting…'}
async function cancelJob(){try{await api('/api/cancel','POST')}catch(e){}}
async function run(path,bar,msg){setBusy(true);barEl=bar;msgEl=msg;bar.style.display='none';msg.className='msg';msg.textContent='Starting…';
 let j;try{j=await api(path+smq(),'POST')}catch(e){msg.textContent='No connection to the checker';msg.className='msg err';setBusy(false);return null}
 if(!j.ok){msg.textContent=j.msg;msg.className='msg err';setBusy(false);return null}
 $('live').style.display='block';
 for(;;){await sleep(400);try{j=await api('/api/job')}catch(e){msg.textContent='Connection lost, retrying…';continue}
  if(j.state==='running'){const t=livemsg(j);msg.textContent=t;$('livemsg').textContent=t;bar.style.display=j.phase===3?'block':'none';if(j.n>0)bar.firstElementChild.style.width=Math.min(100,100*j.i/j.n)+'%'}
  else break}
 $('live').style.display='none';setBusy(false);bar.style.display='none';return j}
async function disp(dx,dy,f){try{await api('/api/disp?dx='+dx+'&dy='+dy+'&flip='+(f===1?1:0)+'&reset='+(f===2?1:0),'POST')}catch(e){}setTimeout(status,600)}
async function startTest(){const nom=nomVal($('nom'),$('nomc'));const j=await run('/api/start?kind=test&nom='+nom+'&lvl='+$('lvl').value,$('bar0'),$('m0'));if(!j)return;
 if(!j.ok){$('m0').textContent=j.msg;$('m0').className='msg err';return}showRes(j.result);status()}
function showRes(r){const c=$('res');c.style.display='block';let h='<div class="big '+r.verdict.split(' ')[0]+'">'+r.verdict+(r.sure?'':' ?')+'</div>';
 if(!r.sure)h+='<div class="mut">Borderline: the error bars span '+r.best+' to '+r.worst+'.</div>';
 if(r.code<=2||r.code===5){h+='<div class="kv"><span>Capacitance</span><b>'+fmtC(r.c_uF)+' ± '+(r.c_rel*100).toFixed(1)+' %</b></div>';
  h+='<div class="kv"><span>ESR</span><b>'+fmtE(r.esr)+' ± '+fmtE(r.esr_unc)+'</b></div>';
  h+='<div class="kv"><span>ESR × C</span><b>'+Math.round(r.esrc)+' Ω·µF</b></div>';
  if(r.ref)h+='<div class="kv"><span>Good part ('+r.ref.nominal+' µF)</span><b>'+fmtC(r.ref.c_uF)+', '+fmtE(r.ref.esr)+'</b></div>';
  else if(r.nominal>0)h+='<div class="kv"><span>Nominal</span><b>'+r.nominal+' µF</b></div>'}
 h+='<div class="kv"><span>Readings</span><b>'+r.ncap+' captures, '+r.npts+' frequencies</b></div>';
 h+='<p class="mut">'+r.why+'</p>';
 if(r.chi2>4)h+='<p class="mut">The readings do not follow a simple ESR + capacitance model (chi2 '+r.chi2.toFixed(1)+'): something else is in parallel, or the contact is poor.</p>';
 if(r.pts.length){h+='<details><summary class="mut">Per frequency</summary><table><tr><th>Hz</th><th>Re Z (Ω)</th><th>Im Z (Ω)</th></tr>';r.pts.forEach(p=>{h+='<tr><td>'+p.f+'</td><td>'+p.re+'</td><td>'+p.im+'</td></tr>'});h+='</table></details>'}
 c.innerHTML=h}
async function startCal(kind,inp){let q='/api/start?kind='+kind;if(inp)q+='&val='+parseFloat($(inp).value);
 const id={open:'st1',short:'st2',res:'st3',cap:'st4'}[kind];const el=$(id);const j=await run(q,el.querySelector('.bar'),el.querySelector('.msg'));if(!j)return;
 const m=el.querySelector('.msg');m.textContent=j.msg;m.className='msg'+(j.ok?'':' err');status()}
async function startLearn(){const nom=nomVal($('rnom'),$('rnomc'));if(!(nom>0)){$('m2').textContent='Enter the nominal value';return}
 const j=await run('/api/start?kind=learn&nom='+nom,$('bar2'),$('m2'));if(!j)return;$('m2').textContent=j.msg;$('m2').className='msg'+(j.ok?'':' err');status()}
async function resetCal(){if(!confirm('Erase all calibration data? (learned parts are kept)'))return;await api('/api/reset','POST');status()}
async function delRef(i){await api('/api/ref/del?i='+i,'POST');status()}
async function wifiOff(){if(!confirm('Turn Wi-Fi off? Reset the checker to get it back.'))return;try{await api('/api/wifi/off','POST')}catch(e){}$('chips').innerHTML='<span class="no">Wi-Fi off</span>'}
async function status(){let s;try{s=await api('/api/status')}catch(e){return}
 const c=s.cal,ch=(ok,t)=>'<span class="'+(ok?'ok':'no')+'">'+t+'</span>';
 $('chips').innerHTML=ch(c.open,'open')+ch(c.short,'short')+ch(c.res,'resistor')+ch(c.cap0||c.cap1,'cap scale');
 ['st1','st2','st3','st4'].forEach((id,k)=>$(id).classList.toggle('done',[c.open,c.short,c.res,c.cap0||c.cap1][k]));
 $('calinfo').innerHTML='Leads: '+c.lead_r.toFixed(3)+' Ω + j'+c.lead_x.toFixed(3)+' Ω at 1 kHz<br>Gain trim: '+c.rtrim.toFixed(4)+(c.res?'':' (not calibrated)')+'<br>C scale: '+(c.cap0?c.ctrim0.toFixed(4)+' @ '+c.cref0+' µF':'—')+' / '+(c.cap1?c.ctrim1.toFixed(4)+' @ '+c.cref1+' µF':'—');
 let h='';s.refs.forEach(r=>{h+='<div class="kv"><span>'+(r.nominal>=1000?r.nominal/1000+' mF':r.nominal+' µF')+' · '+r.n+' part'+(r.n>1?'s':'')+'</span><b>'+fmtC(r.c_uF)+', '+fmtE(r.esr)+' <button class="sm" onclick="delRef('+r.i+')">✕</button></b></div>'});
 $('refs').innerHTML=h||'none yet';if(s.disp)$('dispinfo').textContent='X '+s.disp.x+', Y '+s.disp.y+(s.disp.flip?', flipped':'');if(s.busy)setBusy(true)}
status();
</script></body></html>)HTML";
