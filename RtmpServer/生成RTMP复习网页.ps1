$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'RTMP流程复习.md'
$target = Join-Path $PSScriptRoot 'RTMP流程复习.html'
$body = (ConvertFrom-Markdown -LiteralPath $source).Html
$parts = [regex]::Split($body, '(?=<h2(?:\s|>))')
$toc = [System.Text.StringBuilder]::new()
$content = [System.Text.StringBuilder]::new()
[void]$content.Append($parts[0])
for ($i = 1; $i -lt $parts.Length; $i++) {
    $match = [regex]::Match($parts[$i], '^<h2[^>]*>(.*?)</h2>', 'Singleline')
    $title = $match.Groups[1].Value
    $section = $parts[$i].Substring($match.Length)
    $id = 'section-' + ($i - 1)
    [void]$toc.Append("<a href='#$id'>$title</a>")
    [void]$content.Append("<details class='chapter' id='$id' open><summary>$title</summary><div class='chapter-body'>$section</div></details>")
}
$article = $content.ToString()
$article = [regex]::Replace($article, '<table>(.*?)</table>', '<div class="table-wrap" tabindex="0" role="region" aria-label="可横向滚动的表格"><table>$1</table></div>', 'Singleline')
$article = [regex]::Replace($article, '§(\d+)', '<a href="#section-$1">§$1</a>')
$template = @'
<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<meta name="description" content="RTMP 握手、分块、建流与音视频交换的离线复习笔记">
<title>RTMP 流程复习 · 手机阅读版</title>
<style>
:root{--bg:#f3f5f8;--panel:#fff;--text:#202c3b;--muted:#627184;--line:#dce3eb;--accent:#126567;--code:#edf2f5;font-size:16px;color-scheme:light dark}
*{box-sizing:border-box}html{scroll-behavior:smooth;scroll-padding-top:75px}body{margin:0;background:var(--bg);color:var(--text);font-family:system-ui,-apple-system,"Segoe UI","Microsoft YaHei",sans-serif;line-height:1.85;overflow-wrap:anywhere}header{background:#153e43;color:#fff;padding:34px max(20px,calc((100vw - 940px)/2));}header .label{font-size:12px;letter-spacing:2px;color:#b9ded9}header .title{font-size:clamp(25px,5vw,38px);font-weight:750;margin:6px 0}header p{margin:0;color:#d0e4e2;font-size:14px}.toolbar{position:sticky;top:0;z-index:5;display:flex;gap:8px;align-items:center;flex-wrap:wrap;padding:10px 16px;background:var(--panel);border-bottom:1px solid var(--line)}button,.toolbar a{font:inherit;font-size:14px;line-height:1.5;border:1px solid var(--line);border-radius:8px;background:var(--panel);color:var(--accent);padding:9px 12px;cursor:pointer;text-decoration:none}.toolbar a{margin-right:auto}main{max-width:980px;margin:24px auto;padding:0 20px 65px}nav{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:20px;margin-bottom:24px}nav summary{font-weight:700;cursor:pointer}nav .links{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:4px 20px;padding-top:12px}nav a{padding:6px 0;font-size:14px}a{color:var(--accent);text-underline-offset:3px}h1{font-size:25px;line-height:1.5}h3{font-size:19px;line-height:1.6;margin-top:26px}p{margin:14px 0}.chapter{background:var(--panel);border:1px solid var(--line);border-radius:14px;margin:16px 0;scroll-margin-top:80px}.chapter>summary{cursor:pointer;padding:18px 20px;font-size:19px;font-weight:700;line-height:1.6}.chapter[open]>summary{border-bottom:1px solid var(--line)}.chapter-body{padding:4px 20px 18px}.table-wrap{overflow-x:auto;max-width:100%;margin:18px 0;border:1px solid var(--line);border-radius:8px}table{border-collapse:collapse;width:100%;font-size:14px;min-width:510px}th,td{padding:11px 13px;text-align:left;vertical-align:top;border-bottom:1px solid var(--line)}th{background:var(--code);font-weight:650}tr:last-child td{border-bottom:0}pre{background:var(--code);border-radius:9px;padding:16px;overflow-x:auto;font-size:13px;line-height:1.8;white-space:pre;overflow-wrap:normal;tab-size:4}code{font-family:Consolas,"SFMono-Regular",monospace}p code,li code,td code{background:var(--code);border-radius:4px;padding:2px 4px;font-size:.9em}ul,ol{padding-left:24px}li{margin:8px 0}footer{color:var(--muted);font-size:13px;margin-top:26px}button:focus-visible,a:focus-visible,summary:focus-visible{outline:3px solid #e8ab48;outline-offset:3px}
@media(max-width:600px){main{padding:0 12px 45px;margin-top:14px}nav{padding:16px}nav .links{grid-template-columns:1fr}.chapter>summary{padding:16px;font-size:18px}.chapter-body{padding:2px 14px 14px}pre{padding:12px}h1{font-size:22px}body{line-height:1.9}.toolbar{gap:6px;padding:8px 12px}button,.toolbar a{font-size:13px;padding:9px 10px}}
@media(prefers-color-scheme:dark){:root{--bg:#111a22;--panel:#192630;--text:#e4edf4;--muted:#a5b4c2;--line:#34444f;--accent:#85d5cb;--code:#22343f}}
@media print{header,.toolbar,nav,footer{display:none}main{max-width:none;margin:0;padding:0}.chapter{break-inside:auto;border:none}.chapter-body{display:block!important}.table-wrap,pre{overflow:visible}pre{white-space:pre-wrap}body{background:white;color:black}a{color:inherit}table{min-width:0}}
</style>
</head>
<body>
<header id="top"><div class="label">NETWORK NOTES / OFFLINE EDITION</div><div class="title">RTMP 流程复习</div><p>握手 → 分块与重组 → 建流 → 推流与播放 → 音视频转发</p></header>
<div class="toolbar"><a href="#navigation">目录</a><button type="button" id="expand">展开全部</button><button type="button" id="collapse">收起全部</button><a href="#top">回顶部 ↑</a></div>
<main>
<nav id="navigation" aria-label="章节目录"><details open><summary>按问题快速查找</summary><div class="links">__TOC__</div></details><p style="font-size:13px;color:var(--muted)">点击章节标题可折叠。宽表格和字节示意可左右滑动。全部内容离线可读。</p></nav>
<article>__ARTICLE__</article>
<footer>配套来源：RTMP流程复习.md · 手机阅读版 · 无外部字体、脚本或样式依赖。规范链接需要联网打开。</footer>
</main>
<script>
const chapters=()=>document.querySelectorAll('details.chapter');
document.getElementById('expand').addEventListener('click',()=>chapters().forEach(x=>x.open=true));
document.getElementById('collapse').addEventListener('click',()=>chapters().forEach(x=>x.open=false));
function reveal(){const id=decodeURIComponent(location.hash.slice(1));const target=document.getElementById(id);if(target&&target.matches('details'))target.open=true;}
window.addEventListener('hashchange',reveal);reveal();
document.addEventListener('click',e=>{const a=e.target.closest('a[href^="#"]');if(!a)return;const target=document.getElementById(a.getAttribute('href').slice(1));if(target&&target.matches('details'))target.open=true;});
window.addEventListener('beforeprint',()=>{chapters().forEach(x=>{x.dataset.wasOpen=String(x.open);x.open=true;});});
window.addEventListener('afterprint',()=>chapters().forEach(x=>x.open=x.dataset.wasOpen!=='false'));
</script>
</body></html>
'@
$html = $template.Replace('__TOC__', $toc.ToString()).Replace('__ARTICLE__', $article)
[System.IO.File]::WriteAllText($target, $html, [System.Text.UTF8Encoding]::new($false))
Write-Output $target
