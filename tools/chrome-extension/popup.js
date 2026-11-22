// Custom (Chrome bridge): hand the current page URL to Veyra, with an explicit
// quality choice.
//
// Veyra listens on 127.0.0.1:18765 (see PlaybackServer.cpp) and runs the same
// 「播放地址」 path the player's own paste box uses, so Bilibili page URLs,
// direct media URLs and local files all behave identically.
//
// Quality is a direct Bilibili qn (0 = let Veyra pick the best rendition). It is
// manual on purpose: the page's own menu label lagged behind the <video> element
// and the player object is not global on current builds, so reading the page was
// unreliable. The choice is remembered in chrome.storage.
const ENDPOINT = 'http://127.0.0.1:18765';

const $url = document.getElementById('url');
const $quality = document.getElementById('quality');
const $go = document.getElementById('go');
const $status = document.getElementById('status');

// Bilibili quality menu label -> qn.
function labelToQn(label) {
  const t = (label || '').toLowerCase().replace(/\s+/g, '');
  if (t.includes('8k')) return 127;
  if (t.includes('杜比')) return 126;
  if (t.includes('hdr')) return 125;
  if (t.includes('4k')) return 120;
  if (t.includes('1080p')) return t.includes('高码率') ? 112 : t.includes('60') ? 116 : 80;
  if (t.includes('720p')) return t.includes('60') ? 74 : 64;
  if (t.includes('480p')) return 32;
  if (t.includes('360p')) return 16;
  return 0;
}

// Read the site's own quality: the selected menu item's label is what it streams.
// <video>.videoHeight lags behind the menu and player objects are not global on
// current builds, so this probes the menu first.
async function readPageQuality(tabId) {
  try {
    // 1) 最可靠：content.js 在页面里记下了播放器最近一次 playurl 请求的 qn，
    //    那是它真正在放的档位，不是猜的。
    try {
      const [hooked] = await chrome.scripting.executeScript({
        target: { tabId },
        world: 'MAIN',
        func: () => ({ qn: window.__veyraQn, at: window.__veyraQnAt })
      });
      const qn = hooked?.result?.qn;
      const at = hooked?.result?.at;
      if (Number.isFinite(qn) && qn > 0 && at && Date.now() - at < 120000) {
        return { qn, label: `播放器请求 qn=${qn}` };
      }
    } catch { /* 页面未注入时继续走下面的兜底 */ }

    // 2) 兜底：高度 + 菜单标签
    const [injection] = await chrome.scripting.executeScript({
      target: { tabId },
      func: () => {
        // Height: follows the user's selection (2160 Dolby Vision/4K, 1080 1080P...).
        let best = 0;
        for (const v of document.querySelectorAll('video')) {
          if (Number.isFinite(v.videoHeight) && v.videoHeight > best) best = v.videoHeight;
        }
        // Label: needed to tell apart renditions that share a height
        // (4K vs Dolby Vision vs HDR, 1080P vs 1080P60 vs 1080P high bitrate).
        let label = '';
        const active = document.querySelector(
          '.bpx-player-ctrl-quality-item.bpx-state-active, ' +
          '.bpx-player-ctrl-quality-menu-item.bpx-state-active, ' +
          '.bpx-player-ctrl-quality-menu .bpx-state-active, ' +
          '.bpx-player-ctrl-quality');
        if (active) {
          label = (active.textContent || '').trim();
          // `.bpx-player-ctrl-quality` 是容器，textContent 会把所有档位拼在一起
          // （"HDR 真彩 8K 超高清大会员 4K 超高清…"）。那种情况下取第一个档位关键字。
          if (label.length > 20) {
            const m = label.match(
              /(HDR\s*真彩|杜比视界|8K|4K\s*超高清|1080P\s*60帧|1080P\s*高码率|1080P|720P\s*60帧|720P|480P|360P)/);
            label = m ? m[1].replace(/\s+/g, ' ').trim() : '';
          }
        }
        return { height: best, label };
      }
    });
    const r = injection?.result || {};
    const h = r.height || 0;
    const fromLabel = r.label ? labelToQn(r.label) : 0;
    // The label refines the height only when it is consistent with it: a stale
    // "Dolby Vision" next to a 1080 height must not win.
    const consistent =
      fromLabel > 0 &&
      (h === 0 ||
       (h >= 2160 && fromLabel >= 120) ||
       (h >= 1080 && fromLabel >= 80) ||
       (h >= 720 && fromLabel >= 64) ||
       (h >= 480 && fromLabel >= 32) ||
       (h >= 360 && fromLabel >= 16));
    if (consistent) return { qn: fromLabel, label: r.label };
    return { qn: heightToQn(h), label: h ? `${h}px` : '' };
  } catch {
    return { qn: 0, label: '' };
  }
}

// Names mirror the site's own menu (screenshot of the Bilibili quality list).
const QUALITY_LABELS = {
  '-1': '当前网页画质',
  '0': '最高可用',
  '127': '8K',
  '126': '杜比视界',
  '125': 'HDR 真彩',
  '120': '4K 超高清',
  '116': '1080P 60帧',
  '112': '1080P 高码率',
  '80': '1080P 高清',
  '74': '720P 60帧',
  '64': '720P 准高清',
  '32': '480P 标清',
  '16': '360P 流畅'
};

let currentUrl = '';

function setStatus(text, ok) {
  $status.textContent = text;
  $status.className = ok ? 'ok' : 'err';
}

function isSupported(url) {
  try {
    const u = new URL(url);
    return u.protocol === 'http:' || u.protocol === 'https:';
  } catch {
    return false;
  }
}

function qualityName(qn) {
  return QUALITY_LABELS[String(qn)] || `qn=${qn}`;
}

// 选择记住，下次打开还是同一个档位。
// 默认「当前网页画质」(-1)；用户上次的选择优先。
chrome.storage.local.get({ quality: '-1' }).then(({ quality }) => {
  $quality.value = String(quality);
});
$quality.addEventListener('change', () => {
  chrome.storage.local.set({ quality: $quality.value });
});

// 从浏览器读 B 站会话，随 /play 一起发给 Veyra：这样不必在 Veyra 里再登录一次，
// 会员画质直接用浏览器里的登录状态。
async function readBilibiliCookie() {
  try {
    const cookies = await chrome.cookies.getAll({ domain: 'bilibili.com' });
    const wanted = new Set(['SESSDATA', 'bili_jct', 'DedeUserID', 'DedeUserID__ckMd5', 'sid']);
    return cookies
      .filter((c) => wanted.has(c.name) || c.name.startsWith('buvid'))
      .map((c) => `${c.name}=${c.value}`)
      .join('; ');
  } catch {
    return '';
  }
}

// 点击时先暂停网页播放，避免两边同时出声。
async function pausePageVideo(tabId) {  try {
    await chrome.scripting.executeScript({
      target: { tabId },
      func: () => {
        for (const v of document.querySelectorAll('video')) {
          try { v.pause(); } catch { /* detached */ }
        }
      }
    });
  } catch {
    /* chrome:// pages cannot inject; nothing to pause there */
  }
}

// B 站多 P（分 P）视频：从页面上读出每个分 P 的标题和链接。
//
// 读 DOM 而不是调 API，因为页面上的标题就是用户看到的那个标题（「1.1 映射」
// 这种），API 的 part 字段偶尔会带多余的空白。分 P 列表的两个版本都认：
//   新版：.video-pod__item      每个条目带 data-key(cid)，标题在 .title-txt
//   旧版：#multi_page .list-box  纯 <a href="?p=N">
// 返回 [{p, title, url}]，按 P 号升序；不是多 P 页面时返回 []。
async function readBilibiliParts(tabId, pageUrl) {
  let page;
  try { page = new URL(pageUrl); } catch { return []; }
  if (!/(^|\.)bilibili\.com$/i.test(page.hostname)) return [];
  const match = page.pathname.match(/\/video\/(BV[0-9A-Za-z]+|av\d+)/i);
  if (!match) return [];   // 番剧、直播、动态页没有分 P 列表

  try {
    const [{ result }] = await chrome.scripting.executeScript({
      target: { tabId },
      // 在页面里跑：拿到的就是渲染后的真实标题文本。
      func: (videoId) => {
        const parts = new Map();
        const origin = location.origin;
        const path = location.pathname;

        // 新版列表：每个条目带 data-key，标题在 .title-txt 里。
        //
        // data-key 有两种，含义完全不同：
        //   数字  = 同一 BV 下的第 N 个分 P，地址是 <当前页>?p=N
        //   BV... = 合集里的另一个视频（每个分集都是独立 BV），地址是 /video/<BV>/
        // 以前一律当成「?p=N」，所以合集被排成了一堆指向同一 BV 的 p=1..10，
        // 点第 6 条就报「视频只有 1 个分P」。这里按 data-key 的类型分别取地址。
        const items = [...document.querySelectorAll('.video-pod__item[data-key]')];
        const isMultiPart = items.some(el => /^\d+$/.test(el.getAttribute('data-key') || ''));
        for (const item of items) {
          const key = item.getAttribute('data-key') || '';
          const txt = item.querySelector('.title-txt')?.textContent?.trim() || '';
          const title = item.querySelector('.title')?.getAttribute('title')?.trim() || txt;
          if (!title) continue;
          if (/^\d+$/.test(key)) {
            // 分 P：列表顺序就是 P 号。
            parts.set('p' + (parts.size + 1), title);
          } else if (/^BV[0-9A-Za-z]{10}$/.test(key)) {
            // 合集分集：它自己的 BV 号就是地址。
            parts.set(key, title);
          }
        }
        const multiPart = isMultiPart && [...parts.keys()].some(k => k.startsWith('p'));

        // 旧版列表（页面结构较老时）：靠 <a href="?p=N"> 里的文字。
        if (parts.size === 0) {
          for (const a of document.querySelectorAll('a[href*="?p="]')) {
            let u;
            try { u = new URL(a.href, origin); } catch { continue; }
            if (!u.pathname.includes(videoId)) continue;
            const p = parseInt(u.searchParams.get('p') || '', 10);
            if (!Number.isFinite(p) || p < 1) continue;
            const title = (a.textContent || '').trim();
            if (title) parts.set('p' + p, title);
          }
        }

        if (parts.size < 2) return [];   // 单 P/单集，不需要队列
        // 每个条目的地址：分 P 换掉 p，合集分集换成它自己的 BV。
        // 其余查询参数（spm_id_from、vd_source 等）原样保留，这样列表里的地址和
        // 用户点「发送」时发来的地址一致，Veyra 才能认出正在播的那一条并高亮。
        const base = new URL(location.href);
        return [...parts.entries()]
          .map(([key, title], index) => {
            let u;
            if (key.startsWith('p')) {
              u = new URL(base.href);
              u.searchParams.set('p', String(index + 1));
            } else {
              u = new URL(origin + '/video/' + key + '/');
              // 合集分集保留跟踪参数没有意义，但保留 vd_source 能让播放器沿用会话。
              const vd = base.searchParams.get('vd_source');
              if (vd) u.searchParams.set('vd_source', vd);
            }
            return { p: index + 1, title, url: u.href };
          });
      },
      args: [match[1]]
    });
    return Array.isArray(result) ? result : [];
  } catch {
    return [];   // 页面不允许注入时静默退回单条发送
  }
}

(async () => {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  currentUrl = tab?.url || '';
  $url.textContent = currentUrl;
  $go.disabled = !isSupported(currentUrl);
  if (!isSupported(currentUrl)) setStatus('当前页面不是网页地址', false);
})();

$go.addEventListener('click', async () => {
  $go.disabled = true;
  setStatus('正在连接 Veyra...', true);
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    let qn = Number($quality.value);
    let qualityLabel = qualityName(qn);
    if (qn === -1) {
      // 「当前网页画质」: read the site's own selection now and send that qn.
      // Falling back to 0 keeps the click working when the page is not Bilibili
      // or the menu cannot be found.
      const info = await readPageQuality(tab.id);
      if (info.qn > 0) {
        qn = info.qn;
        qualityLabel = info.label ? `${info.label}` : qualityName(qn);
      } else {
        qn = 0;
        qualityLabel = '最高可用（网页画质读不到）';
      }
    } else if (qn < 0) {
      qn = 0;
      qualityLabel = qualityName(qn);
    }
    await pausePageVideo(tab.id);

    let running = false;
    try {
      const ping = await fetch(ENDPOINT + '/ping', { cache: 'no-store' });
      running = ping.ok;
    } catch {
      running = false;
    }
    if (!running) {
      // Veyra not running: hand off through the veyra:// URL protocol, which the
      // registered handler starts with the URL and quality as arguments. The part
      // list has to travel too - without it the player has no titles and the list
      // shows bare addresses (this was the "video name does not appear" report).
      const parts = await readBilibiliParts(tab.id, currentUrl);
      let link = `veyra://play?url=${encodeURIComponent(currentUrl)}&quality=${qn}`;
      if (parts.length > 1) {
        // Compact form: one record per part as "<p>|<title>|<url>", records joined by
        // newlines. The whole thing is URL-encoded inside a single parameter so the
        // protocol handler can hand it to the same /play path the HTTP route uses.
        const packed = parts.map(x => `${x.p}|${x.title}|${x.url}`).join('\n');
        // 协议启动走的是命令行，Windows 上限 32767 字符。中文标题编码后每字 9 字符，
        // 实测 195 集约 38000 字符——超限会让协议启动直接失败（没有报错提示）。
        // 超限时干脆不带 parts：Veyra 会自己走 resolveBilibiliStream 的 data.pages
        // 兜底，标题退化成站内 part 字段，但分集和播放都正常。
        // The protocol launch passes through a command line, capped at 32767 chars on
        // Windows. A CJK title costs 9 chars once encoded, and 195 parts measured ~38000 -
        // over the limit, which makes the launch fail with no error shown. When it does not
        // fit, drop `parts`: Veyra falls back to data.pages from its own resolver, the
        // titles degrade to the site's `part` field, but the parts and playback still work.
        const withParts = link + `&parts=${encodeURIComponent(packed)}`;
        link = withParts.length <= 8000 ? withParts : link;
      }
      setStatus(parts.length > 1
        ? `Veyra 未运行，正在通过协议启动…（${parts.length} 个分 P）`
        : `Veyra 未运行，正在通过协议启动…（${qualityLabel}）`, true);
      window.location.href = link;
      setTimeout(() => window.close(), 1500);
      return;
    }

    const cookie = await readBilibiliCookie();
    // 多 P 视频：把整页分 P 一起发过去，Veyra 会排进播放列表。
    const parts = await readBilibiliParts(tab.id, currentUrl);
    const payload = { url: currentUrl, quality: qn, cookie };
    if (parts.length > 1) {
      payload.parts = parts;
      setStatus(`正在发送 ${parts.length} 个分 P…`, true);
    }
    const r = await fetch(ENDPOINT + '/play', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    const data = await r.json();
    if (!data.ok) throw new Error(data.error || 'Veyra 拒绝了请求');
    setStatus(parts.length > 1 ? `已发送 ${parts.length} 个分 P，${qualityLabel}` : `已发送，${qualityLabel}`, true);
    setTimeout(() => window.close(), 1200);
  } catch (e) {
    setStatus('失败：' + (e.message || e) + '（Veyra 是否在运行？）', false);
    $go.disabled = false;
  }
});
