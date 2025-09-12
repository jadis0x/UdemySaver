const $ = s => document.querySelector(s);
async function getJSON(url){
try{
const r = await fetch(url);
const txt = await r.text();
try{ return JSON.parse(txt) } catch { return { ok:false, raw:txt } }
}catch(err){ return { ok:false, error:String(err) } }
}

/* ===== i18n ===== */
let I18N = { lang: 'en', dict: {} };

function detectLang(){
const saved = localStorage.getItem('cf_lang');
if (saved) return saved;
const nav = (navigator.language||'en').toLowerCase();
return nav.startsWith('tr') ? 'tr' : 'en';
}

async function loadLang(lang){
try{
const res = await fetch(`/www/lang/${lang}.json`);
I18N.dict = await res.json();
I18N.lang = lang;
localStorage.setItem('cf_lang', lang);
}catch{ I18N.dict = {}; }
applyI18n();
}

function t(key, vars = {}){
let s = I18N.dict[key] || key;
return s.replace(/\{(\w+)\}/g, (_, k) => vars[k] ?? `{${k}}`);
}

function applyI18n(){
document.querySelectorAll('[data-i18n]').forEach(el => {
const key = el.getAttribute('data-i18n'); if(!key) return;
el.textContent = t(key);
});
const q = document.getElementById('q');
if(q) q.setAttribute('placeholder', t('search.placeholder', {}));
const sb = document.getElementById('searchBox');
if (sb) sb.setAttribute('title', t('search.title'));
const pageInfo = document.getElementById('pageInfo');
if(pageInfo && window.state){ pageInfo.textContent = t('pager.page_fmt', {page: state.page, total: Math.max(1, Math.ceil((state.total||0)/state.page_size))}); }
const qp = document.getElementById('qualityPill');
if(qp) qp.textContent = t('quality.label', {q: preferredQuality()});
}

/* ===== globals ===== */
const grid = $('#grid'), pager = $('#pager'), pageInfo = $('#pageInfo');
const prevBtn = $('#prevBtn'), nextBtn = $('#nextBtn'), refreshBtn = $('#refreshBtn');
const downloadSelectedBtn = $('#downloadSelectedBtn');
const authWarn = $('#authWarn'), authPill = $('#auth-pill');
const userbox = $('#userbox'), avatar = $('#avatar'), uname = $('#uname'), signOutBtn = $('#signOutBtn');
const countInfo = $('#countInfo'), qInput = $('#q');
const settingsBtn = $('#settingsBtn'), settingsPanel = $('#settingsPanel'), settingsCloseBtn = $('#settingsCloseBtn');
const qualityPill = $('#qualityPill');
const optSubs = $('#optSubs'), optAssets = $('#optAssets'), optQuality = $('#optQuality');
let userOpts = {subs:false, assets:false};
const selectedCourses = new Map();

function updateSelectedUI(){
    if(downloadSelectedBtn)
        downloadSelectedBtn.style.display = selectedCourses.size > 0 ? '' : 'none';
}

function toggleCourseSelection(course, checked){
    if(checked) selectedCourses.set(course.id, course);
    else selectedCourses.delete(course.id);
    updateSelectedUI();
}

async function downloadSelected(){
	const tasks = [];
	
    for(const c of selectedCourses.values()){
        tasks.push(queueWholeCourse(c, preferredQuality()));
    }
	
	await Promise.all(tasks);
	
    selectedCourses.clear();
    updateSelectedUI();
    renderGrid();
}

function loadOpts(){
  if(optQuality){
    optQuality.value = preferredQuality();
    updateQualityPill();
  }
}

async function saveOpts(){
	userOpts.subs = !!(optSubs?.checked);
	userOpts.assets = !!(optAssets?.checked);
    let quality = preferredQuality();

	if(optQuality){
		quality = optQuality.value;
		localStorage.setItem('cf_quality_pref', optQuality.value);
	
		updateQualityPill();
		toast(t('quality.label',{q:quality}));
	}
	
	 await fetch('/settings', {
                method:'POST',
                headers:{'Content-Type':'application/json'},
                body: JSON.stringify({
                        download_subtitles: userOpts.subs,
                        download_assets: userOpts.assets,
                        quality
                })
  }).catch(()=>({}));
}

let state = { page:1, page_size:12, total:0, auth:false, items:[], filter:"" };

/* ===== utils ===== */
function imageOrPlaceholder(url){
const label = t('img.placeholder');
if(!url) return 'data:image/svg+xml;utf8,'+encodeURIComponent(
`<svg xmlns="http://www.w3.org/2000/svg" width="480" height="270">
<rect width="100%" height="100%" fill="#11151b"/>
<text x="50%" y="50%" fill="#445064" dominant-baseline="middle" text-anchor="middle" font-family="Arial" font-size="16">${label}</text>
</svg>`
);
return url;
}
function fmtBytes(b){ if(!isFinite(b)||b<=0) return ''; const KB=1024, MB=KB*1024, GB=MB*1024; if(b>=GB) return (b/GB).toFixed(2)+' GB'; if(b>=MB) return (b/MB).toFixed(1)+' MB'; if(b>=KB) return Math.round(b/KB)+' KB'; return b+' B'; }
function clamp01(x){ return Math.max(0, Math.min(1, x)); }
const pad3 = n => String(n).padStart(3,'0');
const safe = s => (s||"").toLowerCase().replace(/[^\p{L}\p{N}]+/gu,'-').replace(/-+/g,'-').replace(/^ -|-$/g,'').slice(0,60) || 'item';
function preferredQuality(){ return localStorage.getItem('cf_quality_pref') || 'Highest'; }
function updateQualityPill(){ if(qualityPill) qualityPill.textContent = t('quality.label', {q: preferredQuality()}); }

/* ===== token save ===== */
async function saveTokenFromUI(){
const inp = document.getElementById('tokenInput');
const msg = document.getElementById('authMsg');
if(!inp) return;
const token = (inp.value||"").trim();
msg.textContent = '';
if(!token){ msg.textContent = t('auth.token_empty'); return; }
const r = await fetch('/settings', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({ udemy_access_token: token }) }).catch(e=>({ok:false, statusText:String(e)}));
if(!r || !r.ok){ msg.textContent = t('auth.save_failed'); return; }
const j = await r.json().catch(()=>({ok:false}));
if(j && j.ok){ msg.textContent = t('auth.saved_reload'); setTimeout(()=> location.reload(), 1000); }
else { msg.textContent = t('auth.error_prefix') + (j && j.error ? j.error : 'unknown'); }
}

/* ===== session ===== */
async function loadSession(){
const data = await getJSON('/session');
state.auth = !!data.auth;
if (authPill) authPill.style.display = 'inline-flex';
if(data.opts){
  userOpts.subs = !!data.opts.subs;
  userOpts.assets = !!data.opts.assets;
  if(optSubs) optSubs.checked = userOpts.subs;
  if(optAssets) optAssets.checked = userOpts.assets;
}
if(data.auth && data.user){
if (authPill) authPill.textContent = t('auth.open');
if (userbox) userbox.style.display = 'flex';
if (avatar) avatar.src = data.user.image_50x50 || data.user.image_100x100 || '';
if (uname) uname.textContent = data.user.display_name || data.user.title || 'User';
if (authWarn) authWarn.style.display = 'none';
if (signOutBtn) signOutBtn.style.display = 'inline-flex';
} else {
if (authPill) authPill.textContent = t('auth.closed');
if (userbox) userbox.style.display = 'none';
if (authWarn) authWarn.style.display = 'block';
if (signOutBtn) signOutBtn.style.display = 'none';
}
}

async function signOut(){
const r = await fetch('/settings', { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({ udemy_access_token: '' }) }).catch(()=>({ok:false}));
if(r && r.ok){ location.reload(); }
}

async function setLang(lang){
await loadLang(lang);
await refreshUIAfterLang();
}

async function refreshUIAfterLang(){
applyI18n();

if (countInfo) {
countInfo.textContent = state.total ? `(${state.total} ${t('library.count_suffix')})` : '';
}

if (authPill) authPill.textContent = state.auth ? t('auth.open') : t('auth.closed');
if (authWarn) authWarn.style.display = state.auth ? 'none' : 'block';

renderGrid();     
renderPager();     
qTick(true);       
}

/* ===== courses ===== */
async function loadPage(p=1){
state.page = Math.max(1, p|0);
const data = await getJSON(`/courses?page=${state.page}&page_size=${state.page_size}`);
state.total = data.total || 0;
state.auth = data.auth ?? state.auth;
state.items = Array.isArray(data.courses) ? data.courses : (data.results || []);
if (countInfo) countInfo.textContent = state.total ? `(${state.total} ${t('library.count_suffix')})` : '';
renderGrid(); renderPager();
}
function renderGrid(){
if(!grid) return; grid.innerHTML = '';
const f = (state.filter||'').trim().toLowerCase();
let items = state.items; if(f) items = items.filter(c => (c.title||'').toLowerCase().includes(f));
if(items.length===0){ grid.innerHTML=`<div class="empty">${t('grid.empty')}</div>`; return; }

for(const c of items){
const el = document.createElement('div');
el.className = 'card';
el.innerHTML = `
<div class="thumb-wrap">
<input type="checkbox" class="select-cb" data-id="${c.id}" ${selectedCourses.has(c.id)?'checked':''}>
<img class="thumb" src="${imageOrPlaceholder(c.image)}" alt="">
<span class="badge">#${c.id}</span>
</div>
<div class="body">
<h3 class="title" title="${c.title||t('misc.course')}">${c.title||t('misc.course')}</h3>
<div class="inst">${c.instructor||''}</div>
<div class="row">
<button class="btn primary" data-id="${c.id}">${t('card.download')}</button>
<a class="btn" href="${c.url||'#'}" target="_blank" rel="noopener">${t('card.open')}</a>
</div>
</div>`;
el.querySelector('.btn.primary').addEventListener('click', ()=> queueWholeCourse(c, preferredQuality()));
const cb = el.querySelector('.select-cb');
if(cb) cb.addEventListener('change', ()=> toggleCourseSelection(c, cb.checked));
grid.appendChild(el);
}
updateSelectedUI();
}
function renderPager(){
if(!pager||!pageInfo) return;
const totalPages = Math.max(1, Math.ceil((state.total||0)/state.page_size));
pager.style.display = totalPages>1 ? 'flex' : 'none';
pageInfo.textContent = t('pager.page_fmt', {page: state.page, total: totalPages});
if(prevBtn) prevBtn.disabled = state.page<=1;
if(nextBtn) nextBtn.disabled = state.page>=totalPages;
}

/* ===== busy pill (spinner only) ===== */
let __busy = 0; const qPill = $('#qPill');
function showBusy(msgKey='queue.collecting'){
    __busy++;
    if(qPill){
        qPill.style.display='';
        qPill.classList.add('loading');
    }
    const e=$('#qRunning');
    if(e) e.innerHTML = `<i class="spin"></i>${t(msgKey)}`;
}
function setBusyTextTextual(text){
    const e=$('#qRunning');
    if(e){
        if(qPill) qPill.style.display='';
        e.innerHTML = `<i class="spin"></i>${text}`;
    }
}
function setBusyText(msgKey){
    if(__busy<=0) return;
    const e=$('#qRunning');
    if(e){
        if(qPill) qPill.style.display='';
        e.innerHTML = `<i class="spin"></i>${t(msgKey)}`;
    }
}
function hideBusy(){
    __busy=Math.max(0,__busy-1);
    if(__busy===0){
        if(qPill){
            qPill.classList.remove('loading');
            qPill.style.display='none';
        }
        const e=$('#qRunning');
        if(e) e.textContent=t('queue.waiting');
    }
}

/* ===== quality toggle ===== */
document.addEventListener('keydown', e=>{
if(e.key.toLowerCase()==='f'){
const opts=['Highest','1080','720','480','360','Lowest'];
const cur=preferredQuality(); const i=(opts.indexOf(cur)+1)%opts.length;
localStorage.setItem('cf_quality_pref', opts[i]);
if(optQuality) optQuality.value = opts[i];
updateQualityPill();
toast(t('quality.label',{q:opts[i]}));
}
});

/* ===== lectures ===== */
async function fetchAllLectures(courseId){
const all=[]; let page=1, url=`/lectures?course_id=${courseId}&page=${page}`;
while(url){
const j = await getJSON(url);
const chunk = Array.isArray(j.results) ? j.results : [];
all.push(...chunk);
if(j.next){
try{ const u=new URL(j.next); page=parseInt(u.searchParams.get('page')||`${page+1}`,10); }
catch{ page=page+1; }
url = `/lectures?course_id=${courseId}&page=${page}`;
}else url=null;
}
return all;
}

/* ===== pick source ===== */
function pickVideoSource(asset, preference="Highest"){
if(!asset) return null;
if(asset.download_urls && asset.download_urls.Video && asset.download_urls.Video.length){
return {type:'mp4', label:'download', url:asset.download_urls.Video[0].file};
}
const list = (asset.stream_urls && asset.stream_urls.Video) ? asset.stream_urls.Video : [];
const mp4s = list.filter(x=>x.type==='video/mp4' && x.file);
const hls  = list.find(x=>x.type==='application/x-mpegURL' && x.file);
if(mp4s.length){
mp4s.sort((a,b)=>(parseInt(b.label,10)||0)-(parseInt(a.label,10)||0));
let chosen=null;
if(preference==='Lowest') chosen=mp4s[mp4s.length-1];
else if(/^\d+$/.test(preference)) chosen=mp4s.find(x=>String(x.label)===String(preference))||mp4s[0];
else chosen=mp4s[0];
return {type:'mp4', label:chosen.label, url:chosen.file};
}
if(hls) return {type:'hls', label:'Auto', url:hls.file};
if(asset.hls_url) return {type:'hls', label:'Auto', url:asset.hls_url};
if(Array.isArray(asset.media_sources)){ const m=asset.media_sources.find(x=>x&&x.src); if(m&&m.src) return {type:'mp4', label:'Auto', url:m.src}; }
return null;
}

/* ===== enqueue ===== */
async function enqueueLecture(course, lecture, idxInCourse, pref="Highest", opts={}){ 
const asset = lecture && lecture.asset; 
if(!asset || asset.asset_type!=='Video') return {skipped:true}; 
const picked = pickVideoSource(asset, pref); 
if(!picked) return {skipped:true}; 

const base = { 
course_id:course.id, course_title:course.title, 
section_index:lecture.section_index||0, section_title:lecture.section_title||'', 
lecture_index:idxInCourse, lecture_title:lecture.title||'', 
lecture_id: lecture.id 
}; 

const videoPayload = {...base, url:picked.url, filename:`${pad3(idxInCourse)} - ${safe(lecture.title)}-${picked.label}.mp4`}; 
let r = await fetch('/queue',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(videoPayload)}); 
let data = await r.json().catch(()=>({ok:false}));
if(data.skipped){
    if(data.reason==='exists') toast(t('toast.exists',{title: lecture.title}));
    else if(data.reason==='queued') toast(t('toast.queued',{title: lecture.title}));
}

if(opts.subs && Array.isArray(asset.captions)){ 
for(const cap of asset.captions){ 
const url = cap.url || cap.file || cap.src; if(!url) continue; 
const lang = safe(cap.language || cap.label || 'sub'); 
const ext = (url.split(/[#?]/)[0].split('.').pop()||'vtt'); 
const p = {...base, url, filename:`${pad3(idxInCourse)} - ${safe(lecture.title)}.${lang}.${ext}`}; 
await fetch('/queue',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)}); 
} 
} 

if(opts.assets && Array.isArray(lecture.supplementary_assets)){
for(const a of lecture.supplementary_assets){
const name = a.filename ? a.filename : `${safe(a.title||'asset')}`;
let url = '';
if(a.download_urls){
for(const k in a.download_urls){
const arr = a.download_urls[k];
if(Array.isArray(arr) && arr.length){
url = arr[0].file || arr[0].url || '';
if(url) break;
}
}
}
if(!url) continue;
const p = {...base, filename:`${pad3(idxInCourse)} - ${safe(lecture.title)} - ${safe(name)}`, asset_id:a.id, url};
await fetch('/queue',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});
}
}

return data; 
}

async function queueWholeCourse(course, preference="Highest"){ 
const opts = {subs: userOpts.subs, assets: userOpts.assets};
showBusy('queue.collecting'); 
try{ 
toast(t('toast.collecting',{title: course.title||'Course'})); 
let page=1, knownTotal=null, seen=0, added=0, skipped=0, exists=0; 
while(true){ 
const j = await getJSON(`/lectures?course_id=${course.id}&page=${page}`); 
if(knownTotal==null) knownTotal = (j && Number.isFinite(j.count)) ? (j.count|0) : null; 
const chunk = Array.isArray(j.results) ? j.results : []; 
if(chunk.length===0) break; 
for(const lec of chunk){ 
seen++; setBusyTextTextual(`${seen}/${knownTotal ?? '…'}`); 
if(!lec || !lec.asset || lec.asset.asset_type!=='Video'){ skipped++; continue; } 
const res = await enqueueLecture(course, lec, seen, preference, opts); 
if(res && res.skipped && res.reason==='exists') exists++; 
else if(res && res.ok) added++; 
else skipped++; 
if(seen%8===0) qTick(true); 
} 
if(j.next) page++; else break; 
} 
qTick(true); 
toast(t('toast.added_summary', {added, seen, skipped, exists})); 
if(added===0 && exists>0 && skipped===0) toast(t('toast.course_done',{title: course.title})); 
ensureCourseSize(course.id, preference); 
} finally { hideBusy(); } 
}

/* ===== course total size (lazy) ===== */
const __courseSize = new Map(); // course_id -> {bytes, pending}
async function estimateCourseSize(courseId, quality){ return await getJSON(`/estimate?course_id=${courseId}&quality=${encodeURIComponent(quality||'Highest')}`); }
async function ensureCourseSize(courseId, quality){ const entry = __courseSize.get(courseId); if(entry && (entry.pending || entry.bytes>0)) return; __courseSize.set(courseId,{bytes:0,pending:true}); const j = await estimateCourseSize(courseId, quality); __courseSize.set(courseId,{bytes:(j && j.total_bytes)||0,pending:false}); }

/* ===== queue render ===== */
async function qTick(force=false){
if(!force && document.hidden) return;
const data = await getJSON('/queue');

const byCourse = new Map();
if(Array.isArray(data.courses)){
for(const c of data.courses){
const done=c.done|0, total=c.total|0;
byCourse.set(c.course_id,{ course_id:c.course_id, title:c.title||'Course', done, total, state:(total>0&&done>=total)?'done':'queued', pct: total>0 ? Math.round((done*100)/total) : 0, _r:-1 });
}
}
if(Array.isArray(data.items)){
const rank={downloading:4, failed:3, paused:2, queued:1, done:0};
for(const it of data.items){
const row = byCourse.get(it.course_id); if(!row) continue;
const st=String(it.state||'').toLowerCase(), r=rank[st]??0;
if(r>row._r){ row.state=st||'queued'; row._r=r; }
if(st==='downloading'){
const spd = (it.speed_bps||0) * 1024; // server reports KiB/s
row.speed = (row.speed||0) + spd;
}
}
}
for(const row of byCourse.values()){
if(!__courseSize.has(row.course_id)) ensureCourseSize(row.course_id, preferredQuality());
}

const qList = $('#qList'); if(!qList) return;
if(byCourse.size===0){ qList.innerHTML=`<div class="q-empty">${t('queue.empty')}</div>`; }
else {
const rows = [...byCourse.values()].map(row=>{
const pct = Math.round(clamp01(row.total? row.done/row.total : 0)*100) || row.pct || 0;
const sz = __courseSize.get(row.course_id);
const sizeTxt = sz && sz.bytes>0 ? ` • ${fmtBytes(sz.bytes)}` : '';
const speedTxt = row.speed>0 ? ` • ${fmtBytes(row.speed)}/s` : '';
const sub = `${row.done}/${row.total} • %${pct}${sizeTxt}${speedTxt}`;

const btn = row.state === 'paused'
    ? `<button class="btn sm" data-act="resume" data-cid="${row.course_id}">${t('actions.resume')}</button>`
    : `<button class="btn sm" data-act="pause" data-cid="${row.course_id}">${t('actions.pause')}</button>`;


return `
<div class="q-item${row.state==='done' ? ' done' : ''}" data-cid="${row.course_id}">
<div>
<div class="q-title">${row.title}</div>
<div class="q-sub">${sub}</div>
<div class="bar"><i style="width:${pct}%"></i></div>
</div>
<div class="q-ctrl">${btn}</div>
</div>`;
}).join('');
qList.innerHTML = rows;

qList.querySelectorAll('button[data-act]').forEach(btn=>{
  btn.addEventListener('click', async ()=>{
     const cid = btn.getAttribute('data-cid');
    const act = btn.getAttribute('data-act');
    fetch(`/queue/${act}`, {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({course_id:Number(cid)})
    }).catch(()=>{});
    qTick(true);
  });
});
}


if(__busy===0){
    const run = $('#qRunning');
    if(run && qPill){
        let statusKey = 'queue.waiting';
		let isDownloading = false;
        for(const row of byCourse.values()){
            if(row.state==='downloading'){ statusKey='queue.downloading'; isDownloading=true; break; }
            if(row.state==='failed'){ statusKey='queue.failed'; break; }
            if(row.state==='paused'){ statusKey='queue.paused'; }
        }
        run.innerHTML = isDownloading ? `<i class="spin"></i>${t(statusKey)}` : t(statusKey);
        qPill.style.display='';
    }
}
}

/* ===== tiny toast ===== */
let toastTimer=null; function toast(msg){ let t=document.getElementById('cf_toast'); if(!t){ t=document.createElement('div'); t.id='cf_toast'; t.style.cssText='position:fixed;left:50%;transform:translateX(-50%);bottom:24px;background:#111a22;border:1px solid rgba(255,255,255,.08);box-shadow:0 10px 30px rgba(0,0,0,.35);padding:10px 14px;border-radius:12px;color:#e8eef6;font-weight:700;z-index:9999'; document.body.appendChild(t); } t.textContent=msg; t.style.opacity='1'; clearTimeout(toastTimer); toastTimer=setTimeout(()=>{t.style.opacity='0'},1800); }

/* ===== events & boot ===== */
if(prevBtn) prevBtn.addEventListener('click',()=>loadPage(state.page-1));
if(nextBtn) nextBtn.addEventListener('click',()=>loadPage(state.page+1));
if(refreshBtn) refreshBtn.addEventListener('click',()=>loadPage(state.page));
if(downloadSelectedBtn) downloadSelectedBtn.addEventListener('click', downloadSelected);
if(qInput) qInput.addEventListener('input',()=>{ state.filter=qInput.value||''; renderGrid(); });
if(signOutBtn) signOutBtn.addEventListener('click', signOut);
if(settingsBtn && settingsPanel) settingsBtn.addEventListener('click', ()=>{ settingsPanel.style.display = (settingsPanel.style.display==='none' || settingsPanel.style.display==='') ? 'block' : 'none'; });
if(settingsCloseBtn && settingsPanel) settingsCloseBtn.addEventListener('click', ()=>{ settingsPanel.style.display='none'; });
if(optSubs) optSubs.addEventListener('change', saveOpts);
if(optAssets) optAssets.addEventListener('change', saveOpts);
if(optQuality) optQuality.addEventListener('change', saveOpts);

document.addEventListener('visibilitychange', ()=>{ if(!document.hidden) qTick(true); });
window.addEventListener('load', ()=>{ const btn = document.getElementById('saveTokenBtn'); if(btn) btn.addEventListener('click', saveTokenFromUI); });

// language buttons
const langSelect = document.getElementById('langSelect');
if (langSelect) {
  langSelect.addEventListener('change', e => {
    setLang(e.target.value);
  });
  // sayfa açıldığında mevcut dili seçili yap
  window.addEventListener('load', () => {
    langSelect.value = detectLang();
  });
}

// boot
window.addEventListener('load', async ()=>{
await loadLang(detectLang());
  await loadSession();        
  await loadPage(1);          
  await refreshUIAfterLang(); 
  qTick(true);
  setInterval(()=>qTick(false),1500);
});

// expose
window.queueWholeCourse = queueWholeCourse;