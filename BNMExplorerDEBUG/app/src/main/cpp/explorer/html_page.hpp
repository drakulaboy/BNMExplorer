#pragma once
#include <string>
inline std::string GetExplorerHTML(int asmCount) {
    std::string html = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,maximum-scale=1.0,user-scalable=no">
<title>BNM Explorer</title>
<style>
:root{--bg:#111;--bg2:#1e1e1e;--bg3:#252526;--bg4:#2d2d2d;--sel:#094771;--fg:#ccc;--kw:#569cd6;--type:#4ec9b0;--mth:#dcdcaa;--str:#d69d85;--num:#b5cea8;--brd:#3e3e42;--red:#5a1d1d;--red2:#8a2a2a;--grn:#1d4a1d;--grn2:#2a6a2a;--org:#664400;--org2:#885500;--blu:#1a2a4a;--blu2:#2a4a8a;}
*{box-sizing:border-box;margin:0;padding:0;font-family:"Segoe UI",sans-serif;font-size:12px;}
body{background:var(--bg);color:var(--fg);display:flex;flex-direction:column;height:100vh;overflow:hidden;}
.toolbar{background:var(--bg4);padding:8px 10px;border-bottom:1px solid var(--brd);display:flex;gap:10px;align-items:center;flex-wrap:wrap;}
.toolbar svg{width:18px;height:18px;fill:currentColor;}
.toolbar > span{font-size:14px;font-weight:bold;}
.srch{background:var(--bg2);border:1px solid var(--brd);color:#fff;padding:6px 8px;width:100%;max-width:300px;outline:none;flex:1;min-width:140px;border-radius:4px;}
.btn-dump{margin-left:auto;}
.workspace{display:flex;flex:1;overflow:hidden;padding:4px;gap:4px;flex-direction:row;}
.sidebar{width:280px;background:var(--bg2);border:1px solid var(--brd);display:flex;flex-direction:column;}
.side-hdr{background:var(--bg4);padding:10px;border-bottom:1px solid var(--brd);color:#fff;font-weight:bold;}
.tree-wrap{flex:1;overflow-y:auto;padding:4px 0;}
::-webkit-scrollbar{width:6px;height:6px;}
::-webkit-scrollbar-thumb{background:#555;border-radius:3px;}
::-webkit-scrollbar-track{background:transparent;}
.tree-node{list-style:none;padding-left:14px;}
.tree-nc{display:flex;align-items:center;padding:6px 4px;cursor:pointer;white-space:nowrap;user-select:none;min-height:28px;}
.tree-nc:hover{background:var(--bg4);}
.tree-nc.active{background:var(--sel);color:#fff;}
.arr{display:inline-block;width:18px;text-align:center;font-size:10px;transition:transform .1s;color:#888;}
.arr.open{transform:rotate(90deg);}
.ico{width:16px;height:16px;margin-right:6px;display:flex;align-items:center;justify-content:center;flex-shrink:0;}
.ico svg{width:100%;height:100%;fill:currentColor;}
.kids{display:none;}
.kids.open{display:block;}
.main-area{flex:1;display:flex;flex-direction:column;overflow:hidden;border:1px solid var(--brd);background:var(--bg2);}
.tabs{display:flex;background:var(--bg2);border-bottom:1px solid var(--brd);overflow-x:auto;-webkit-overflow-scrolling:touch;}
.tab{padding:12px 16px;background:var(--bg3);border-right:1px solid var(--brd);cursor:pointer;border-bottom:1px solid var(--brd);white-space:nowrap;flex-shrink:0;}
.tab.active{background:var(--bg2);border-bottom:none;color:#fff;border-top:2px solid var(--sel);font-weight:bold;}
.panel{flex:1;overflow:auto;background:var(--bg2);display:none;}
.panel.active{display:flex;flex-direction:column;}
.scene-mgr{display:flex;flex:1;overflow:hidden;flex-direction:row;}
.hier{width:340px;border-right:1px solid var(--brd);display:flex;flex-direction:column;flex-shrink:0;}
.hier-hdr{background:var(--bg4);padding:10px;border-bottom:1px solid var(--brd);display:flex;flex-direction:column;gap:8px;}
.insp{flex:1;overflow-y:auto;display:flex;flex-direction:column;}
.insp-hdr{padding:12px;border-bottom:1px solid var(--brd);display:flex;align-items:center;gap:8px;background:var(--bg3);flex-wrap:wrap;}
.insp-hdr .inp{flex:1;font-size:14px;font-weight:bold;background:transparent;border:1px solid transparent;min-width:120px;}
.insp-hdr .inp:focus{border:1px solid var(--brd);background:var(--bg2);}
.comp{border-bottom:1px solid var(--brd);}
.comp-hdr{background:var(--bg3);padding:10px;display:flex;align-items:center;cursor:pointer;user-select:none;gap:8px;}
.comp-hdr:hover{background:var(--bg4);}
.comp-body{display:none;}
.comp-body.open{display:block;}
.filter-inp{margin:6px 10px;width:calc(100% - 20px);padding:6px 8px;font-size:12px;background:var(--bg);border:1px solid var(--brd);color:#fff;border-radius:3px;outline:none;}
.filter-inp:focus{border-color:var(--sel);}
.row{display:flex;align-items:center;border-bottom:1px solid #1a1a1a;min-height:36px;flex-wrap:wrap;}
.lbl{width:35%;min-width:110px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;padding:8px 10px;color:var(--type);border-right:1px solid #1a1a1a;flex-shrink:0;}
.val{flex:1;display:flex;gap:6px;padding:8px 10px;align-items:center;flex-wrap:wrap;min-width:160px;}
.vl{color:#888;margin-right:2px;font-size:11px;}
.code-view{font-family:Consolas,'Courier New',monospace;font-size:12px;line-height:1.6;padding:12px 16px;white-space:pre;overflow-x:auto;}
.c-kw{color:var(--kw);}
.c-ty{color:var(--type);}
.c-mt{color:var(--mth);}
.c-st{color:var(--str);}
.c-cm{color:#608b4e;}
.c-nm{color:var(--num);}
.table{width:100%;border-collapse:collapse;}
.table th,.table td{padding:8px 10px;border:1px solid var(--brd);text-align:left;word-break:break-all;}
.table th{background:var(--bg4);font-weight:normal;}
.btn{background:#333;color:var(--fg);border:1px solid var(--brd);padding:8px 14px;cursor:pointer;display:inline-flex;align-items:center;justify-content:center;gap:6px;outline:none;flex-shrink:0;border-radius:4px;}
.btn svg{width:14px;height:14px;fill:currentColor;}
.btn:hover{background:#444;}
.btn-red{background:var(--red);border-color:var(--red2);}
.btn-red:hover{background:#7a2828;}
.btn-grn{background:var(--grn);border-color:var(--grn2);}
.btn-grn:hover{background:#286028;}
.btn-org{background:var(--org);border-color:var(--org2);}
.btn-org:hover{background:var(--org2);}
.btn-blu{background:var(--blu);border-color:var(--blu2);}
.btn-blu:hover{background:#2a4a8a;}
.inp{background:var(--bg2);border:1px solid var(--brd);color:#fff;padding:6px 10px;width:100%;outline:none;border-radius:3px;}
.inp:focus{border-color:#569cd6;}
.inp:disabled{opacity:.5;cursor:not-allowed;}
.ctrl-res{font-family:Consolas,monospace;background:var(--bg2);padding:6px 10px;border:1px solid var(--brd);flex:1;margin-left:6px;min-height:28px;display:flex;align-items:center;word-break:break-all;}
.loop-row{display:flex;align-items:center;gap:8px;margin-bottom:6px;background:var(--grn);padding:8px 12px;border:1px solid var(--grn2);flex-wrap:wrap;border-radius:4px;}
.loop-id{font-family:Consolas,monospace;opacity:.9;flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--type);min-width:130px;}
.script-hdr{background:#1a241a;}
.builtin-hdr{background:#1a1a2e;}
.catcher-entry{border-bottom:1px solid var(--brd);}
.catcher-hdr{background:var(--bg3);padding:10px;display:flex;align-items:center;gap:8px;cursor:pointer;user-select:none;flex-wrap:wrap;}
.catcher-hdr:hover{background:var(--bg4);}
.catcher-body{display:none;max-height:400px;overflow-y:auto;}
.catcher-body.open{display:block;}
.call-row{padding:8px 12px;border-bottom:1px solid #1a1a1a;font-family:Consolas,monospace;display:flex;flex-direction:column;gap:6px;}
.call-row:hover{background:var(--bg4);}
.call-ts{color:#666;font-size:11px;}
.call-inst{color:var(--type);word-break:break-all;}
.call-args{color:var(--fg);word-break:break-all;}
.add-comp-modal{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.8);display:none;z-index:100;align-items:center;justify-content:center;}
.add-comp-modal.open{display:flex;}
.add-comp-box{background:var(--bg2);border:1px solid var(--brd);width:90%;max-width:420px;max-height:80vh;display:flex;flex-direction:column;border-radius:6px;overflow:hidden;}
.add-comp-search{padding:12px;border-bottom:1px solid var(--brd);}
.add-comp-cats{flex:1;overflow-y:auto;}
.add-comp-cat-hdr{padding:10px 14px;background:var(--bg4);border-bottom:1px solid var(--brd);cursor:pointer;font-weight:bold;}
.add-comp-item{padding:10px 20px;cursor:pointer;border-bottom:1px solid #1a1a1a;display:flex;flex-direction:column;gap:4px;}
.add-comp-item span{color:#666;font-size:11px;}
.mono-tag{font-size:10px;background:#333;color:#aaa;padding:2px 6px;margin-left:6px;border-radius:3px;}
.addr-badge{background:var(--sel);color:#fff;padding:4px 8px;border-radius:4px;cursor:pointer;font-family:Consolas,monospace;font-size:11px;transition:0.2s;}
.addr-badge:hover{background:#0b5c91;}
.mth-row{display:flex;flex-direction:column;padding:12px;border-bottom:1px solid var(--brd);}
#pnl-lua .lua-container{display:flex;flex:1;overflow:hidden;}
@media(max-width:800px){
.workspace{flex-direction:column;overflow-y:auto;}
.sidebar{width:100%;height:35vh;min-height:220px;flex-shrink:0;}
.main-area{overflow:visible;flex-shrink:0;min-height:65vh;}
.scene-mgr{flex-direction:column;}
.hier{width:100%;height:35vh;min-height:220px;border-right:none;border-bottom:2px solid var(--brd);}
.insp{min-height:45vh;}
#pnl-ctrl .hier{width:100%;height:auto;}
.row{flex-direction:column;align-items:flex-start;padding:6px 0;}
.lbl{width:100%;border-right:none;border-bottom:1px dashed #333;padding-bottom:4px;font-weight:bold;}
.val{width:100%;padding-top:8px;}
.btn-dump{order:3;width:100%;margin-top:8px;justify-content:center;}
.srch{order:2;max-width:100%;}
#pnl-lua .lua-container{flex-direction:column;}
#pnl-lua .lua-container>div{border-right:none;border-bottom:2px solid var(--brd);height:50%;}
.catcher-hdr{flex-direction:column;align-items:flex-start;}
.catcher-hdr .btn{align-self:flex-end;margin-top:6px;}
}
</style>
</head>
<body>
<div class="toolbar">
<svg viewBox="0 0 24 24"><path d="M21 16.5c0 .38-.21.71-.53.88l-7.9 4.44c-.16.12-.36.18-.57.18s-.41-.06-.57-.18l-7.9-4.44A.991.991 0 0 1 3 16.5v-9c0-.38.21-.71.53-.88l7.9-4.44c.16-.12.36-.18.57-.18s.41.06.57.18l7.9 4.44c.32.17.53.5.53.88v9z"/></svg>
<span>BNM Explorer <span style="opacity:.4">()HTML";
    html += std::to_string(asmCount);
    html += R"HTML( asms)</span></span>
<input type="text" class="srch" id="gSearch" placeholder="Search classes..." onkeyup="doSearch(event)">
<button class="btn btn-dump" onclick="downloadDump()"><svg viewBox="0 0 24 24"><path d="M19 9h-4V3H9v6H5l7 7 7-7zM5 18v2h14v-2H5z"/></svg> Dump</button>
</div>
<div class="workspace">
<div class="sidebar">
<div class="side-hdr">Object Explorer</div>
<div class="tree-wrap" id="tree-root"></div>
</div>
<div class="main-area">
<div class="tabs">
<div class="tab active" onclick="switchTab('scene')" id="tab-scene">Scene</div>
<div class="tab" onclick="switchTab('code')" id="tab-code">Inspector</div>
<div class="tab" onclick="switchTab('inst')" id="tab-inst">Instances</div>
<div class="tab" onclick="switchTab('ctrl')" id="tab-ctrl">Controller</div>
<div class="tab" onclick="switchTab('loops')" id="tab-loops">Loops</div>
<div class="tab" onclick="switchTab('catcher')" id="tab-catcher">Catcher</div>
<div class="tab" onclick="switchTab('scanner')" id="tab-scanner">Scanner</div>
<div class="tab" onclick="switchTab('lua')" id="tab-lua">Console</div>
<div class="tab" onclick="switchTab('docs')" id="tab-docs">Docs</div>
</div>
<div class="panel active" id="pnl-scene">
<div class="scene-mgr">
<div class="hier">
<div class="hier-hdr">
<div style="display:flex;justify-content:space-between;align-items:center;">
<span>Hierarchy</span>
<button class="btn" onclick="loadScene()"><svg viewBox="0 0 24 24"><path d="M17.65 6.35C16.2 4.9 14.21 4 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08c-.82 2.33-3.04 4-5.65 4-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z"/></svg></button>
</div>
<input type="text" class="inp" id="sceneSearch" placeholder="Filter Hierarchy..." onkeyup="filterScene()">
</div>
<div class="tree-wrap" id="scene-tree"></div>
</div>
<div class="insp" id="scene-insp">
<div style="padding:20px;text-align:center;color:#555;">Select a GameObject</div>
</div>
</div>
</div>
<div class="panel code-view" id="pnl-code"></div>
<div class="panel" id="pnl-inst" style="padding:0">
<div class="scene-mgr">
<div class="hier" style="width:100%;border:none;">
<div class="hier-hdr"><span>Instances</span></div>
<div class="tree-wrap" id="inst-list" style="padding:10px;">Loading...</div>
</div>
</div>
</div>
<div class="panel" id="pnl-ctrl" style="padding:0">
<div class="scene-mgr">
<div class="hier">
<div class="hier-hdr"><span>Target</span></div>
<div style="padding:12px;display:flex;flex-direction:column;gap:10px;">
<div><label style="color:#aaa;display:block;margin-bottom:4px;">Address</label><input class="inp" id="ctrl-addr" value="0x0"></div>
<div style="display:flex;align-items:center;gap:8px;"><input type="checkbox" id="ctrl-static" onchange="document.getElementById('ctrl-addr').disabled=this.checked;if(this.checked)document.getElementById('ctrl-addr').value='0x0';"><label for="ctrl-static" style="color:#aaa;">Static (no instance)</label></div>
<div><label style="color:#aaa;display:block;margin-bottom:4px;">Assembly</label><input class="inp" id="ctrl-asm"></div>
<div><label style="color:#aaa;display:block;margin-bottom:4px;">Namespace</label><input class="inp" id="ctrl-ns"></div>
<div><label style="color:#aaa;display:block;margin-bottom:4px;">Class</label><input class="inp" id="ctrl-cls"></div>
<button class="btn btn-blu" style="justify-content:center;padding:10px;" onclick="loadController()">Fetch Target</button>
</div>
</div>
<div class="insp" id="ctrl-insp"><div style="padding:20px;text-align:center;color:#555;">Configure target</div></div>
</div>
</div>
<div class="panel" id="pnl-loops" style="padding:12px;flex-direction:column;gap:8px;">
<div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px;">
<span style="font-weight:bold;font-size:14px;">Active Loops</span>
<button class="btn btn-red" onclick="removeAllLoops()">Stop All</button>
</div>
<div id="loops-list" style="margin-top:6px;"></div>
</div>
<div class="panel" id="pnl-catcher" style="padding:0;flex-direction:column;">
<div style="background:var(--bg4);padding:10px;border-bottom:1px solid var(--brd);display:flex;gap:10px;align-items:center;flex-wrap:wrap;">
<span style="font-weight:bold;">Method Catcher</span>
<span style="opacity:.5;flex:1;min-width:150px;">Hook methods to record calls</span>
<button class="btn btn-red" onclick="unhookAll()">Unhook All</button>
</div>
<div id="catcher-list" style="flex:1;overflow-y:auto;"></div>
</div>
<!-- TAB SCANNER -->
<div class="panel" id="pnl-scanner" style="padding:16px;flex-direction:column;gap:12px;">
    <div style="background:var(--bg3);padding:16px;border-radius:4px;border:1px solid var(--brd);display:flex;gap:12px;align-items:flex-end;flex-wrap:wrap;">
        <div style="flex:1;min-width:200px;">
            <label style="color:#aaa;display:block;margin-bottom:6px;">Value to search</label>
            <input type="text" class="inp" id="scan-val" placeholder="e.g. 9999">
        </div>
        <div>
            <label style="color:#aaa;display:block;margin-bottom:6px;">Data Type</label>
            <select class="inp" id="scan-type" style="width:120px;">
                <option value="int32">Int32</option>
                <option value="float">Float</option>
                <option value="string">String</option>
            </select>
        </div>
        <button class="btn btn-blu" style="height:36px;" onclick="runMemoryScan()">New Scan</button>
    </div>
    <div style="background:var(--bg);flex:1;border:1px solid var(--brd);border-radius:4px;overflow-y:auto;padding:12px;" id="scan-results">
        <div style="color:#555;text-align:center;margin-top:20px;">Enter a value and click New Scan.</div>
    </div>
</div>

<!-- TAB LUA (Modificat pentru File Manager) -->
<div class="panel" id="pnl-lua" style="padding:0;flex-direction:column;">
    <div style="background:var(--bg4);padding:10px 16px;border-bottom:1px solid var(--brd);display:flex;gap:10px;align-items:center;flex-wrap:wrap;">
        <span style="font-weight:bold;">LuaBNM IDE</span>
        <input type="text" class="inp" id="lua-filename" placeholder="script_name.lua" style="width:180px;padding:4px 8px;">
        <button class="btn btn-org" style="padding:4px 10px;" onclick="saveLuaScript()"><svg><use href="#ic-save"/></svg> Save</button>
        <div style="display:flex;gap:8px;margin-left:auto;">
            <button class="btn btn-red" onclick="clearConsole()">Clear</button>
            <button class="btn btn-grn" onclick="runLua()"><svg><use href="#ic-play"/></svg> Run</button>
        </div>
    </div>
    <div class="lua-container">
        <div style="width:220px;background:var(--bg2);border-right:1px solid var(--brd);display:flex;flex-direction:column;">
            <div style="padding:8px 12px;background:var(--bg3);border-bottom:1px solid var(--brd);color:#888;">Saved Scripts</div>
            <div id="lua-files" style="flex:1;overflow-y:auto;padding:8px 0;"></div>
        </div>
        <div style="flex:1;display:flex;flex-direction:column;border-right:1px solid var(--brd);">
            <textarea id="lua-editor" spellcheck="false" style="flex:1;background:var(--bg);color:var(--fg);border:none;padding:12px;font-family:'Consolas','Courier New',monospace;font-size:14px;resize:none;outline:none;tab-size:2;" placeholder="-- Write LuaBNM code here..."></textarea>
        </div>
        <div style="flex:1;display:flex;flex-direction:column;">
            <pre id="lua-output" style="flex:1;background:#0a0a0a;color:#0f0;padding:12px;margin:0;overflow:auto;font-family:'Consolas','Courier New',monospace;font-size:13px;white-space:pre-wrap;"></pre>
        </div>
    </div>
</div>
<div class="panel" id="pnl-docs" style="padding:20px;flex-direction:column;overflow-y:auto;">
<div style="max-width:700px;">
<h2 style="color:var(--kw);margin-bottom:10px;">LuaBNM Documentation</h2>
<div style="color:#aaa;margin-bottom:20px;">Runtime Lua scripting for IL2CPP/Unity via BNM</div>
<h3 style="color:var(--type);margin:16px 0 8px;">Assembly &amp; Class Access</h3>
<pre style="background:var(--bg);padding:12px;border:1px solid var(--brd);margin-bottom:12px;color:var(--fg);white-space:pre-wrap;">local asm = Assembly("Assembly-CSharp.dll")
local ns = asm:NameSpace("MyGame")
local cls = ns:Class("PlayerController", 0x0)  -- 0x0 = static
print(cls.get_Health())
cls.set_Health(100)
local player = ns:Class("PlayerController", 0x7A3F00)
print(player.Health)
player.Speed = 10.5</pre>
<h3 style="color:var(--type);margin:16px 0 8px;">Scene Access</h3>
<pre style="background:var(--bg);padding:12px;border:1px solid var(--brd);margin-bottom:12px;color:var(--fg);white-space:pre-wrap;">local objs = scene.GetObjects()
for i,v in ipairs(objs) do print(v.Name) end
local go = scene.FindObject("Player")
print(go.Name, go.active)
local all = FindObjectsOfType("UnityEngine.Camera")</pre>
<h3 style="color:var(--type);margin:16px 0 8px;">GetComponent</h3>
<pre style="background:var(--bg);padding:12px;border:1px solid var(--brd);margin-bottom:12px;color:var(--fg);white-space:pre-wrap;">local go = scene.FindObject("Player")
local rb = go:GetComponent("Rigidbody")
print(rb.velocity)
local script = go:GetComponent("PlayerHealth")
script.maxHealth = 200</pre>
<h3 style="color:var(--type);margin:16px 0 8px;">API Reference</h3>
<div style="overflow-x:auto;">
<table class="table" style="width:100%;min-width:400px;">
<tr><th>Function</th><th>Description</th></tr>
<tr><td style="color:var(--mth);">Assembly(name)</td><td>Load assembly by name</td></tr>
<tr><td style="color:var(--mth);">asm:NameSpace(ns)</td><td>Get namespace from assembly</td></tr>
<tr><td style="color:var(--mth);">ns:Class(name, addr)</td><td>Get class. addr=0x0 for static</td></tr>
<tr><td style="color:var(--mth);">asm:Class(name, addr)</td><td>Get class directly</td></tr>
<tr><td style="color:var(--mth);">cls.fieldName</td><td>Read/Write field or property</td></tr>
<tr><td style="color:var(--mth);">cls.method(args...)</td><td>Call method</td></tr>
<tr><td style="color:var(--mth);">cls.addr</td><td>Get instance address</td></tr>
<tr><td style="color:var(--mth);">scene.GetObjects()</td><td>Get GameObjects as table</td></tr>
<tr><td style="color:var(--mth);">scene.FindObject(n)</td><td>Find GameObject by name</td></tr>
<tr><td style="color:var(--mth);">FindObjectsOfType(t)</td><td>Find all objects of IL2CPP type</td></tr>
<tr><td style="color:var(--mth);">go:GetComponent(n)</td><td>Get component as LuaClass</td></tr>
<tr><td style="color:var(--mth);">go.Name / go.active</td><td>Name and active state</td></tr>
<tr><td style="color:var(--mth);">wrap(addr)</td><td>Wrap pointer as LuaClass</td></tr>
</table>
</div>
</div>
</div>
</div>
</div>
<div class="add-comp-modal" id="addCompModal" onclick="closeAddComp(event)">
<div class="add-comp-box" onclick="event.stopPropagation()">
<div style="padding:12px 16px;background:var(--bg4);border-bottom:1px solid var(--brd);display:flex;justify-content:space-between;align-items:center;">
<span style="font-weight:bold;font-size:16px;">Add Component</span>
<button class="btn btn-red" style="padding:6px 12px;" onclick="closeAddComp()">X</button>
</div>
<div class="add-comp-search"><input class="inp" id="addCompSearch" placeholder="Search components..." oninput="filterAddComp()"></div>
<div class="add-comp-cats" id="addCompCats">Loading...</div>
</div>
</div>
<svg style="display:none">
<symbol id="ic-asm" viewBox="0 0 24 24"><path d="M4 6h16v2H4zm0 5h16v2H4zm0 5h16v2H4z"/></symbol>
<symbol id="ic-ns" viewBox="0 0 24 24"><path d="M10 4H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8z"/></symbol>
<symbol id="ic-cls" viewBox="0 0 24 24"><path d="M12 2L2 7l10 5 10-5zm0 10.5l-10-5v5.1l10 5 10-5v-5.1z"/></symbol>
<symbol id="ic-go" viewBox="0 0 24 24"><path d="M12 2L2 7l10 5 10-5zm0 10.5l-10-5v5.1l10 5 10-5v-5.1zM2 17l10 5 10-5"/></symbol>
<symbol id="ic-comp" viewBox="0 0 24 24"><path d="M14 2H6c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V8l-6-6zm2 16H8v-2h8v2zm0-4H8v-2h8v2zm-3-5V3.5L18.5 9H13z"/></symbol>
<symbol id="ic-play" viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></symbol>
<symbol id="ic-loop" viewBox="0 0 24 24"><path d="M12 4V1L8 5l4 4V6c3.31 0 6 2.69 6 6 0 1.01-.25 1.97-.7 2.8l1.46 1.46A7.93 7.93 0 0 0 20 12c0-4.42-3.58-8-8-8zm0 14c-3.31 0-6-2.69-6-6 0-1.01.25-1.97.7-2.8L5.24 7.74A7.93 7.93 0 0 0 4 12c0 4.42 3.58 8 8 8v3l4-4-4-4v3z"/></symbol>
<symbol id="ic-script" viewBox="0 0 24 24"><path d="M9.4 16.6L4.8 12l4.6-4.6L8 6l-6 6 6 6 1.4-1.4zm5.2 0l4.6-4.6-4.6-4.6L16 6l6 6-6 6-1.4-1.4z"/></symbol>
<symbol id="ic-del" viewBox="0 0 24 24"><path d="M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"/></symbol>
<symbol id="ic-add" viewBox="0 0 24 24"><path d="M19 3H5c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h14c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm-2 10h-4v4h-2v-4H7v-2h4V7h2v4h4v2z"/></symbol>
<symbol id="ic-catch" viewBox="0 0 24 24"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm0 18c-4.42 0-8-3.58-8-8s3.58-8 8-8 8 3.58 8 8-3.58 8-8 8zm3.5-9c.83 0 1.5-.67 1.5-1.5S16.33 8 15.5 8 14 8.67 14 9.5s.67 1.5 1.5 1.5zm-7 0c.83 0 1.5-.67 1.5-1.5S9.33 8 8.5 8 7 8.67 7 9.5 7.67 11 8.5 11zm3.5 6.5c2.33 0 4.31-1.46 5.11-3.5H6.89c.8 2.04 2.78 3.5 5.11 3.5z"/></symbol>
<symbol id="ic-save" viewBox="0 0 24 24"><path d="M17 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14c1.1 0 2-.9 2-2V7l-4-4zm-5 16c-1.66 0-3-1.34-3-3s1.34-3 3-3 3 1.34 3 3-1.34 3-3 3zm3-10H5V5h10v4z"/></symbol>
</svg>
<script>
let curAsm="",curNs="",curCls="",allClsCache=null,fullSceneData=[],sceneRoots=[],selectedGoAddr=null,addCompGoAddr=null,addCompData={};
function esc(s){return s?String(s).replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;").replace(/"/g,"&quot;").replace(/'/g,"&#039;"):"";}
function copyTxt(t){navigator.clipboard.writeText(t);let el=document.createElement('div');el.textContent='Copied '+t;el.style.cssText='position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:var(--sel);color:#fff;padding:8px 16px;border-radius:6px;z-index:9999;box-shadow:0 4px 12px rgba(0,0,0,0.5);font-weight:bold;';document.body.appendChild(el);setTimeout(()=>el.remove(),1500);}
function filterNode(inp,uid){let q=inp.value.toLowerCase();document.querySelectorAll(`#${uid}>.row, #${uid}>.mth-row`).forEach(r=>{r.style.display=r.innerText.toLowerCase().includes(q)?'':'none';});}
async function init(){let asms=await fetch('/api/assemblies').then(r=>r.json());document.getElementById('tree-root').innerHTML=asms.map(a=>`<ul class="tree-node"><div class="tree-nc" onclick="toggleAsm(this,'${esc(a)}')"><span class="arr">&#9654;</span><div class="ico" style="color:#dcdcaa"><svg><use href="#ic-asm"/></svg></div><span>${esc(a)}</span></div><div class="kids"></div></ul>`).join('');loadScene();}
async function toggleAsm(el,asm){let kids=el.nextElementSibling,arr=el.querySelector('.arr');if(kids.classList.contains('open')){kids.classList.remove('open');arr.classList.remove('open');return;}if(!kids.innerHTML.trim()){let classes=await fetch(`/api/classes?a=${encodeURIComponent(asm)}`).then(r=>r.json());let byNs={};for(let c of classes){let ns=c.ns||'';if(!byNs[ns])byNs[ns]=[];byNs[ns].push(c);}let h='';for(let ns of Object.keys(byNs).sort()){h+=`<ul class="tree-node"><div class="tree-nc" onclick="toggleNs(this)"><span class="arr">&#9654;</span><div class="ico" style="color:#daa520"><svg><use href="#ic-ns"/></svg></div><span>${esc(ns||'(global)')}</span></div><div class="kids">`;for(let c of byNs[ns].sort((a,b)=>a.name.localeCompare(b.name))){let badge=c.t!=='class'?`<span class="mono-tag">${c.t}</span>`:'';h+=`<div class="tree-nc" onclick="loadCls(this,'${esc(asm)}','${esc(c.ns||'')}','${esc(c.name)}')"><span class="arr"></span><div class="ico" style="color:var(--type)"><svg><use href="#ic-cls"/></svg></div><span>${esc(c.name)}</span>${badge}</div>`;}h+=`</div></ul>`;}kids.innerHTML=h;}kids.classList.add('open');arr.classList.add('open');}
function toggleNs(el){el.nextElementSibling.classList.toggle('open');el.querySelector('.arr').classList.toggle('open');}
function formatCode(d){let ns=d.ns||'',name=d.name||'',parent=d.parent||'',t=d.type||'class',ind=ns?'    ':'',lines=[];if(ns)lines.push(`<span class="c-kw">namespace</span> <span class="c-ty">${esc(ns)}</span> {`);let decl=`${ind}<span class="c-kw">public ${t}</span> <span class="c-ty">${esc(name)}</span>`;if(parent&&parent!=='Object'&&parent!=='ValueType')decl+=` : <span class="c-ty">${esc(parent)}</span>`;lines.push(decl+' {');for(let f of d.fields||[]){let isStatic=f.s?' <span class="c-kw">static</span>':'';lines.push(`${ind}    <span class="c-cm">// offset 0x${f.off?Number(f.off).toString(16):'?'}</span>`);lines.push(`${ind}    <span class="c-kw">public</span>${isStatic} <span class="c-ty">${esc(f.type)}</span> <span>${esc(f.name)}</span>;`);}for(let p of d.props||[]){let acc=(p.g?'get; ':'')+(p.s?'set; ':'');lines.push(`${ind}    <span class="c-kw">public</span> <span class="c-ty">${esc(p.type)}</span> <span class="c-mt">${esc(p.name)}</span> { ${acc}}`);}for(let m of d.methods||[]){let prms=(m.params||[]).map(p=>`<span class="c-ty">${esc(p.t)}</span> ${esc(p.n)}`).join(', ');let isStatic=m.s?' <span class="c-kw">static</span>':'';let addr=m.addr?`<span class="c-cm">// ${m.addr}</span>\n${ind}    `:'';lines.push(`${ind}    ${addr}<span class="c-kw">public</span>${isStatic} <span class="c-ty">${esc(m.ret)}</span> <span class="c-mt">${esc(m.name)}</span>(${prms}) {}`);}lines.push(ind+'}');if(ns)lines.push('}');return lines.join('\n');}
async function loadCls(el,asm,ns,name){document.querySelectorAll('#tree-root .tree-nc').forEach(e=>e.classList.remove('active'));if(el)el.classList.add('active');curAsm=asm;curNs=ns;curCls=name;let d=await fetch(`/api/class?a=${encodeURIComponent(asm)}&ns=${encodeURIComponent(ns)}&n=${encodeURIComponent(name)}`).then(r=>r.json());document.getElementById('pnl-code').innerHTML=formatCode(d);switchTab('code');loadInstances();}
async function loadInstances(){document.getElementById('inst-list').innerHTML='Loading...';let d=await fetch(`/api/instances?a=${encodeURIComponent(curAsm)}&ns=${encodeURIComponent(curNs)}&n=${encodeURIComponent(curCls)}`).then(r=>r.json());if(d.error){document.getElementById('inst-list').innerHTML=`<div style="color:#f66">${esc(d.error)}</div>`;return;}let h=`<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;"><span>Instances: ${d.instances.length}</span><button class="btn" onclick="openStaticController()">Open Static</button></div><table class="table"><tr><th>Address</th><th>Name</th><th></th></tr>`;for(let i of d.instances)h+=`<tr><td><span class="addr-badge" onclick="copyTxt('0x${i.addr}')">0x${i.addr}</span></td><td><span class="c-st">"${esc(i.name)}"</span></td><td><button class="btn" onclick="openController('0x${i.addr}')">Open</button></td></tr>`;document.getElementById('inst-list').innerHTML=h+'</table>';}
function openController(addr){document.getElementById('ctrl-addr').value=addr;document.getElementById('ctrl-asm').value=curAsm;document.getElementById('ctrl-ns').value=curNs;document.getElementById('ctrl-cls').value=curCls;document.getElementById('ctrl-static').checked=false;document.getElementById('ctrl-addr').disabled=false;switchTab('ctrl');loadController();}
function openStaticController(){document.getElementById('ctrl-addr').value='0x0';document.getElementById('ctrl-asm').value=curAsm;document.getElementById('ctrl-ns').value=curNs;document.getElementById('ctrl-cls').value=curCls;document.getElementById('ctrl-static').checked=true;document.getElementById('ctrl-addr').disabled=true;switchTab('ctrl');loadController();}
async function loadController(){let addr=document.getElementById('ctrl-addr').value,asm=document.getElementById('ctrl-asm').value,ns=document.getElementById('ctrl-ns').value,cls=document.getElementById('ctrl-cls').value;document.getElementById('ctrl-insp').innerHTML='<div style="padding:20px;text-align:center;">Loading...</div>';let d=await fetch(`/api/controller/inspect?addr=${addr}&asm=${encodeURIComponent(asm)}&ns=${encodeURIComponent(ns)}&cls=${encodeURIComponent(cls)}`).then(r=>r.json());if(!d.name){document.getElementById('ctrl-insp').innerHTML='<div style="padding:20px;color:#f66;">Not found</div>';return;}let h=`<div class="insp-hdr"><div class="ico" style="color:var(--type)"><svg><use href="#ic-cls"/></svg></div><span class="inp">${esc(d.name)}</span><span class="addr-badge" onclick="copyTxt('${addr}')">${addr}</span></div>`;if(d.fields?.length)h+=buildCompUI("Fields",false,d.fields,addr,'instance',true,false);if(d.methods?.length){let uid=uid8();h+=`<div class="comp"><div class="comp-hdr" onclick="toggleComp('${uid}')"><div class="ico"><svg><use href="#ic-play"/></svg></div><span>Methods</span></div><div class="comp-body open" id="${uid}"><input class="inp filter-inp" placeholder="Filter methods..." onkeyup="filterNode(this,'${uid}')">`;d.methods.forEach((m,idx)=>{h+=`<div class="mth-row"><div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:8px;flex-wrap:wrap;"><span style="color:var(--mth);font-weight:bold;font-size:13px;">${m.s?'[S] ':''}${esc(m.name)}</span><div style="display:flex;gap:8px;align-items:center;"><span style="color:var(--type);font-size:12px;">${esc(m.ret)}</span><button class="btn btn-blu" style="padding:6px 10px;" title="Add to Catcher" onclick="catchMethod('${esc(asm)}','${esc(ns)}','${esc(cls)}','${esc(m.name)}',this)"><svg><use href="#ic-catch"/></svg></button></div></div>`;if(m.params?.length){h+=`<div style="padding:8px 0 8px 14px;border-left:2px solid var(--sel);margin-bottom:10px;">`;m.params.forEach(p=>{let prim=['System.Int32','Int32','int','System.Single','Single','float','System.Double','Double','double','System.Boolean','Boolean','bool','System.String','String','string','System.Int64','Int64','long','System.UInt32','UInt32','uint','System.Byte','Byte','byte','System.Int16','Int16','short','UnityEngine.Vector3','Vector3','UnityEngine.Color','Color','UnityEngine.Vector2','Vector2'];let ph=prim.includes(p.t)?esc(p.n):'0x... (addr)';h+=`<div style="display:flex;gap:8px;margin-bottom:6px;align-items:center;"><span style="width:90px;color:#888;overflow:hidden;text-overflow:ellipsis;">${esc(p.t)}</span><input type="text" class="inp marg-${idx}" data-type="${esc(p.t)}" placeholder="${ph}" style="flex:1;"></div>`;});h+=`</div>`;}h+=`<div style="display:flex;align-items:center;flex-wrap:wrap;gap:8px;"><button class="btn" onclick="execMethod('${esc(asm)}','${esc(ns)}','${esc(cls)}','${esc(m.name)}',${m.s},'${addr}',${idx})"><svg><use href="#ic-play"/></svg> Run</button><div class="ctrl-res" id="mres-${idx}"></div></div></div>`;});h+=`</div></div>`;}document.getElementById('ctrl-insp').innerHTML=h;}
async function execMethod(asm,ns,cls,mName,isStatic,addr,idx){let args=Array.from(document.querySelectorAll(`.marg-${idx}`)).map(el=>({t:el.getAttribute('data-type'),v:el.value})),res=document.getElementById(`mres-${idx}`);res.innerHTML='<span style="color:#aaa">...</span>';let d=await fetch('/api/invoke',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({asm,ns,cls,method:mName,static:isStatic,instance:addr,args})}).then(r=>r.json());res.innerHTML=d.ok?`<span style="color:var(--str)">${esc(d.value)}</span>`:`<span style="color:#f66">${esc(d.error)}</span>`;}
async function catchMethod(asm,ns,cls,method,btn){btn.style.background='#2a4a8a';let d=await fetch('/api/catcher/hook',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({asm,ns,cls,method})}).then(r=>r.json());if(d.ok){btn.style.background='#1d4a1d';refreshCatcher();}else{btn.style.background='var(--red)';alert('Hook failed: '+(d.error||'unknown'));}}
function switchTab(id){document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));document.getElementById(`tab-${id}`).classList.add('active');document.getElementById(`pnl-${id}`).classList.add('active');if(id==='loops')refreshLoopsUI();if(id==='catcher')refreshCatcher();}
async function doSearch(e){let q=document.getElementById('gSearch').value.toLowerCase();if(!q){await init();return;}if(e.key!=='Enter')return;if(!allClsCache)allClsCache=await fetch('/api/allclasses').then(r=>r.json());let res=allClsCache.filter(c=>c.name.toLowerCase().includes(q)||c.ns?.toLowerCase().includes(q)),byAsm={};for(let c of res){if(!byAsm[c.a])byAsm[c.a]=[];byAsm[c.a].push(c);}let html='';for(let a of Object.keys(byAsm).sort()){html+=`<ul class="tree-node"><div class="tree-nc" onclick="toggleAsm(this,'${esc(a)}')"><span class="arr open">&#9654;</span><div class="ico" style="color:#dcdcaa"><svg><use href="#ic-asm"/></svg></div><span>${esc(a)}</span></div><div class="kids open">`;let byNs={};for(let c of byAsm[a]){let ns=c.ns||'';if(!byNs[ns])byNs[ns]=[];byNs[ns].push(c);}for(let ns of Object.keys(byNs).sort()){html+=`<ul class="tree-node"><div class="tree-nc" onclick="toggleNs(this)"><span class="arr open">&#9654;</span><div class="ico" style="color:#daa520"><svg><use href="#ic-ns"/></svg></div><span>${esc(ns||'(global)')}</span></div><div class="kids open">`;for(let c of byNs[ns].sort((x,y)=>x.name.localeCompare(y.name))){html+=`<div class="tree-nc" onclick="loadCls(this,'${esc(a)}','${esc(c.ns||'')}','${esc(c.name)}')"><span class="arr"></span><div class="ico" style="color:var(--type)"><svg><use href="#ic-cls"/></svg></div><span>${esc(c.name)}</span></div>`;}html+=`</div></ul>`;}html+=`</div></ul>`;}document.getElementById('tree-root').innerHTML=html;}
async function loadScene(){fullSceneData=await fetch('/api/scene').then(r=>r.json());filterScene();}
function filterScene(){let q=document.getElementById('sceneSearch').value.toLowerCase(),map={};sceneRoots=[];for(let g of fullSceneData){if(!map[g.addr])map[g.addr]={...g,children:[]};else Object.assign(map[g.addr],g);if(!g.parent||g.parent==="0")sceneRoots.push(map[g.addr]);else{if(!map[g.parent])map[g.parent]={children:[]};map[g.parent].children.push(map[g.addr]);}}document.getElementById('scene-tree').innerHTML=sceneRoots.map(n=>buildSceneNode(n,q)).filter(Boolean).join('');if(selectedGoAddr){let el=document.querySelector(`[data-go="${selectedGoAddr}"]`);if(el){el.classList.add('active');el.scrollIntoView({block:'nearest',behavior:'smooth'});}}}
function buildSceneNode(n,q){let matches=!q||n.name?.toLowerCase().includes(q),childHtml=(n.children||[]).map(c=>buildSceneNode(c,q)).filter(Boolean).join('');if(!matches&&!childHtml)return'';let hasKids=(n.children||[]).length>0;return`<ul class="tree-node"><div class="tree-nc" data-go="${n.addr}" onclick="inspectGO(this,'${n.addr}')"><span class="arr${hasKids?' open':''}" onclick="event.stopPropagation();toggleNs(this.parentElement)">${hasKids?'&#9654;':''}</span><div class="ico"><svg><use href="#ic-go"/></svg></div><span style="color:${n.active?'var(--fg)':'#555'}">${esc(n.name)}</span></div>${hasKids?`<div class="kids open">${childHtml}</div>`:''}</ul>`;}
async function inspectGO(el,addr){document.querySelectorAll('#scene-tree .tree-nc').forEach(e=>e.classList.remove('active'));if(el)el.classList.add('active');selectedGoAddr=addr;let d=await fetch(`/api/scene/inspect?addr=${addr}`).then(r=>r.json());if(d.stale||!d.addr){document.getElementById('scene-insp').innerHTML='<div style="padding:20px;text-align:center;color:#f66;">Object stale, reloading...</div>';selectedGoAddr=null;await loadScene();return;}let h=`<div class="insp-hdr"><input type="checkbox" ${d.active?'checked':''} onchange="updateGO('${d.addr}','gameobject','active',this.checked)"><div class="ico" style="color:var(--type)"><svg><use href="#ic-go"/></svg></div><input type="text" class="inp" value="${esc(d.name)}" onchange="updateGO('${d.addr}','gameobject','name',this.value)"><span class="addr-badge" onclick="copyTxt('0x${d.addr}')">0x${d.addr}</span><button class="btn btn-grn" style="padding:6px 10px;" onclick="openAddComp('${d.addr}')"><svg><use href="#ic-add"/></svg></button><button class="btn btn-red" style="padding:6px 10px;" onclick="deleteGO('${d.addr}')"><svg><use href="#ic-del"/></svg></button></div>`;if(d.transform)h+=buildCompUI("Transform",true,[{lbl:"Position",id:"p",val:d.transform.p,type:"UnityEngine.Vector3",canWrite:true},{lbl:"Rotation",id:"r",val:d.transform.r,type:"UnityEngine.Vector3",canWrite:true},{lbl:"Scale",id:"s",val:d.transform.s,type:"UnityEngine.Vector3",canWrite:true}],d.transform.addr,'transform',true);for(let sc of d.scripts||[])h+=buildScriptCompUI(sc,addr);document.getElementById('scene-insp').innerHTML=h;}
async function deleteGO(addr){if(!confirm('Delete GameObject?'))return;await fetch('/api/scene/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({addr})});document.getElementById('scene-insp').innerHTML='<div style="padding:20px;text-align:center;color:#555;">Deleted</div>';selectedGoAddr=null;await loadScene();}
async function openAddComp(goAddr){addCompGoAddr=goAddr;document.getElementById('addCompSearch').value='';document.getElementById('addCompCats').innerHTML='Loading...';document.getElementById('addCompModal').classList.add('open');if(!Object.keys(addCompData).length)addCompData=await fetch('/api/scene/addable').then(r=>r.json());renderAddCompCats('');}
function renderAddCompCats(q){let h='';for(let [cat,items] of Object.entries(addCompData)){let filtered=q?items.filter(i=>i.name.toLowerCase().includes(q)||i.full.toLowerCase().includes(q)):items;if(!filtered.length)continue;let uid=uid8();h+=`<div class="add-comp-cat-hdr" onclick="document.getElementById('cat-${uid}').classList.toggle('open')">${esc(cat)} <span style="opacity:.5">(${filtered.length})</span></div><div class="add-comp-items${q?' open':''}" id="cat-${uid}">`;for(let item of filtered)h+=`<div class="add-comp-item" onclick="addComponent('${esc(item.full)}')">${esc(item.name)}<span>${esc(item.full)}</span></div>`;h+=`</div>`;}document.getElementById('addCompCats').innerHTML=h||'<div style="padding:10px;color:#555;">No components</div>';}
function filterAddComp(){renderAddCompCats(document.getElementById('addCompSearch').value.toLowerCase());}
function closeAddComp(e){if(!e||e.target===document.getElementById('addCompModal'))document.getElementById('addCompModal').classList.remove('open');}
async function addComponent(fullType){document.getElementById('addCompModal').classList.remove('open');let d=await fetch('/api/scene/addcomponent',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({addr:addCompGoAddr,type:fullType})}).then(r=>r.json());if(d.ok){let el=document.querySelector(`[data-go="${addCompGoAddr}"]`);if(el)inspectGO(el,addCompGoAddr);}else alert('Failed: '+(d.error||'unknown'));}
async function removeComponent(compAddr,goAddr){if(!confirm('Remove component?'))return;await fetch('/api/scene/removecomponent',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({addr:compAddr})});let el=document.querySelector(`[data-go="${goAddr}"]`);if(el)inspectGO(el,goAddr);}
function buildScriptCompUI(sc,goAddr){let uid=uid8(),isBuiltin=sc.category==='builtin',hdrCls=isBuiltin?'builtin-hdr':'script-hdr',col=isBuiltin?'var(--kw)':'var(--mth)',h=`<div class="comp"><div class="comp-hdr ${hdrCls}" onclick="toggleComp('${uid}')" style="flex-wrap:wrap;"><input type="checkbox" ${sc.enabled?'checked':''} onchange="updateScriptEnabled('${sc.addr}',this.checked)" onclick="event.stopPropagation()" style="margin-right:6px;width:16px;height:16px;"><div class="ico" style="color:${col}"><svg><use href="#ic-${isBuiltin?'comp':'script'}"/></svg></div><span style="color:${col};font-weight:bold;">${esc(sc.name)}</span>${sc.ns?`<span style="opacity:.4;font-size:11px;margin-left:6px;">${esc(sc.ns)}</span>`:''}<span class="addr-badge" style="margin-left:auto;margin-right:8px;" onclick="event.stopPropagation();copyTxt('0x${sc.addr}')">0x${sc.addr}</span><button class="btn btn-red" style="padding:6px 10px;" title="Remove" onclick="event.stopPropagation();removeComponent('${sc.addr}','${goAddr}')"><svg><use href="#ic-del"/></svg></button></div><div class="comp-body open" id="${uid}">`;if(!sc.fields?.length){h+=`<div class="row" style="padding:10px;color:#555;">No readable fields</div>`;}else{h+=`<input class="inp filter-inp" placeholder="Filter fields..." onkeyup="filterNode(this,'${uid}')" onclick="event.stopPropagation()">`;for(let r of sc.fields){let da=`data-addr="${sc.addr}" data-tt="script" data-pid="${r.name}" data-vt="${r.type}" data-ip="${r.isProp?'true':'false'}" data-cns="${esc(sc.ns)}" data-ccls="${esc(sc.name)}" data-casm="${esc(sc.asm||'')}"`,dis=r.canWrite===false?'disabled':'';h+=buildFieldRow(r,da,dis,true,true);}}return h+'</div></div>';}
function buildCompUI(title,isTrans,rows,targetAddr,targetType,withSave,isScript){let uid=uid8(),h=`<div class="comp"><div class="comp-hdr" onclick="toggleComp('${uid}')"><div class="ico"><svg><use href="#ic-comp"/></svg></div><span style="font-weight:bold;">${esc(title)}</span></div><div class="comp-body open" id="${uid}">`;if(rows.length>4)h+=`<input class="inp filter-inp" placeholder="Filter fields..." onkeyup="filterNode(this,'${uid}')" onclick="event.stopPropagation()">`;for(let r of rows){let da=`data-addr="${targetAddr}" data-tt="${targetType}" data-pid="${r.id||r.name||r.lbl}" data-vt="${r.type}" data-ip="${r.isProp?'true':'false'}" data-static="${r.static?'true':'false'}"`,dis=r.canWrite===false?'disabled':'';h+=buildFieldRow(r,da,dis,withSave,targetType==='instance');}return h+'</div></div>';}
function buildFieldRow(r,da,dis,withSave,showLoop){let label=r.name||r.lbl||r.id||'',prefix=r.static?'<span style="color:#e8a0ff;font-size:11px;">[S] </span>':'',h=`<div class="row"><div class="lbl" title="${esc(label)}">${prefix}${esc(label)}</div><div class="val">`,t=r.type||'';if(t==="UnityEngine.Vector3"||t==="Vector3"){let v=Array.isArray(r.val)?r.val:tryParseVec3(r.val);['X','Y','Z'].forEach((l,i)=>{h+=`<div style="flex:1;display:flex;min-width:70px;"><span class="vl">${l}</span><input class="inp vec-input" type="number" step="any" value="${v[i]||0}" ${da} ${dis}></div>`;});}else if(t==="UnityEngine.Vector2"||t==="Vector2"){let v=Array.isArray(r.val)?r.val:tryParseVec2(r.val);['X','Y'].forEach((l,i)=>{h+=`<div style="flex:1;display:flex;min-width:70px;"><span class="vl">${l}</span><input class="inp vec-input" type="number" step="any" value="${v[i]||0}" ${da} ${dis}></div>`;});}else if(t==="UnityEngine.Quaternion"||t==="Quaternion"||t==="UnityEngine.Vector4"||t==="Vector4"){let v=r.val&&typeof r.val==='object'?r.val:{x:0,y:0,z:0,w:1};if(typeof r.val==='string'){try{v=JSON.parse(r.val);}catch(e){}}['X','Y','Z','W'].forEach((l,k)=>{h+=`<div style="flex:1;display:flex;min-width:70px;"><span class="vl">${l}</span><input class="inp vec-input" type="number" step="any" value="${v[l.toLowerCase()]||0}" ${da} ${dis}></div>`;});}else if(t==="UnityEngine.Color"||t==="Color"){let v=Array.isArray(r.val)?r.val:[1,1,1,1];if(typeof r.val==='string'){try{v=JSON.parse(r.val);}catch(e){}}let hex="#"+((1<<24)+(Math.round(v[0]*255)<<16)+(Math.round(v[1]*255)<<8)+Math.round(v[2]*255)).toString(16).slice(1).padStart(6,'0');h+=`<input type="color" class="inp sing-input" value="${hex}" style="padding:0;height:32px;width:60px;cursor:pointer;" ${da} ${dis}><span style="font-size:12px;color:#888;">rgba(${v.map(x=>Math.round(x*255)).join(',')})</span>`;}else if(t==="UnityEngine.Rect"||t==="Rect"){let v=r.val&&typeof r.val==='object'?r.val:{x:0,y:0,w:0,h:0};if(typeof r.val==='string'){try{v=JSON.parse(r.val);}catch(e){}}['x','y','w','h'].forEach(k=>{h+=`<div style="flex:1;display:flex;min-width:70px;"><span class="vl">${k}</span><input class="inp vec-input" type="number" step="any" value="${v[k]||0}" ${da} ${dis}></div>`;});}else if(t==="UnityEngine.LayerMask"||t==="LayerMask"){h+=`<input class="inp sing-input" type="number" step="1" value="${r.val||0}" ${da} ${dis} style="width:90px;"><span style="font-size:12px;color:#888;">Layer ${r.val||0}</span>`;}else if(t==="System.Boolean"||t==="Boolean"||t==="bool"){h+=`<input type="checkbox" class="sing-input" style="width:20px;height:20px;" ${(r.val===true||r.val==="true")?'checked':''} ${da} ${dis}>`;}else{let isNum=["System.Int32","Int32","int","System.Single","Single","float","System.Double","Double","double","System.Int64","Int64","long","System.UInt32","UInt32","uint","System.UInt64","UInt64","ulong","System.Byte","Byte","byte","System.Int16","Int16","short"].includes(t),sv=String(r.val||'').replace(/^"|"$/g,'');h+=`<input class="inp sing-input" ${isNum?'type="number" step="any"':'type="text"'} value="${esc(sv)}" ${da} ${dis} style="flex:1;min-width:120px;">`;}if(withSave&&!dis){h+=`<button class="btn" style="margin-left:auto;" onclick="handleSave(this)">Set</button>`;if(showLoop)h+=`<button class="btn btn-org" title="Loop" onclick="promptLoop(this)"><svg><use href="#ic-loop"/></svg></button>`;}h+=`</div></div>`;return h;}
function tryParseVec3(v){try{let a=JSON.parse(v);if(Array.isArray(a))return a;}catch(e){}return[0,0,0];}
function tryParseVec2(v){try{let a=JSON.parse(v);if(Array.isArray(a))return a;}catch(e){}return[0,0];}
function toggleComp(id){document.getElementById(id).classList.toggle('open');}
function uid8(){return Math.random().toString(36).substr(2,8);}
async function updateScriptEnabled(addr,val){await fetch('/api/scene/update',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({addr,type:'script',prop:'enabled',val:String(val)})});}
async function updateGO(addr,type,prop,val){let d=await fetch('/api/scene/update',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({addr,type,prop,val:String(val)})}).then(r=>r.json()).catch(()=>({}));if(d.ok===false){document.getElementById('scene-insp').innerHTML='<div style="padding:20px;text-align:center;color:#f66;">Stale object</div>';selectedGoAddr=null;await loadScene();}}
function handleSave(btn){let el=btn.parentElement.querySelector('.vec-input,.sing-input');if(el)processSave(el);}
function getValFromRow(el){if(el.classList.contains('vec-input')){let ins=el.parentElement.parentElement.querySelectorAll('input.vec-input'),vals=Array.from(ins).map(i=>i.value),vt=el.getAttribute('data-vt')||'';if(vt.includes('Quaternion')||vt.includes('Vector4'))return`{"x":${vals[0]},"y":${vals[1]},"z":${vals[2]},"w":${vals[3]||1}}`;if(vt.includes('Rect'))return`{"x":${vals[0]},"y":${vals[1]},"w":${vals[2]},"h":${vals[3]}}`;return`[${vals.join(',')}]`;}else if(el.type==='color'){let hx=el.value;return`[${parseInt(hx.substr(1,2),16)/255},${parseInt(hx.substr(3,2),16)/255},${parseInt(hx.substr(5,2),16)/255},1]`;}else if(el.type==='checkbox')return el.checked?'true':'false';return el.value;}
function processSave(el){let val=getValFromRow(el);if(el.classList.contains('vec-input'))el=el.parentElement.parentElement.querySelectorAll('input.vec-input')[0];dispatchUpdate(el,val);let orig=el.style.backgroundColor;el.style.backgroundColor='#094771';setTimeout(()=>{el.style.backgroundColor=orig;},300);}
function dispatchUpdate(el,val){let tt=el.getAttribute('data-tt'),addr=el.getAttribute('data-addr'),prop=el.getAttribute('data-pid'),vt=el.getAttribute('data-vt'),isProp=el.getAttribute('data-ip')==='true',isStatic=el.getAttribute('data-static')==='true',cns=el.getAttribute('data-cns')||'',ccls=el.getAttribute('data-ccls')||'',casm=el.getAttribute('data-casm')||'';if(tt==='transform')updateGO(addr,'transform',prop,val);else if(tt==='instance')updateInstField(addr,prop,vt,isProp,val,isStatic);else if(tt==='script')updateScriptField(addr,prop,vt,isProp,val,cns,ccls,casm);}
async function updateScriptField(addr,name,ftype,isProp,val,cns,ccls,casm){let d=await fetch('/api/scene/update',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({addr,type:'script',prop:name,prop2:name,ftype,isProp,val:String(val),compNs:cns,compCls:ccls,compAsm:casm||''})}).then(r=>r.json()).catch(()=>({}));if(d.ok===false){document.getElementById('scene-insp').innerHTML='<div style="padding:20px;text-align:center;color:#f66;">Stale object</div>';selectedGoAddr=null;await loadScene();}}
async function updateInstField(addr,name,ftype,isProp,val,isStatic){let asm=document.getElementById('ctrl-asm').value,ns=document.getElementById('ctrl-ns').value,cls=document.getElementById('ctrl-cls').value;await fetch('/api/instance/update',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({addr,asm,ns,cls,name,ftype,isProp,val:String(val),isStatic:isStatic?'true':'false'})});}
function promptLoop(btn){let el=btn.parentElement.querySelector('.vec-input,.sing-input');if(!el)return;let val=getValFromRow(el);if(el.classList.contains('vec-input'))el=el.parentElement.parentElement.querySelectorAll('input.vec-input')[0];let ms=prompt('Loop interval (ms)','100');if(!ms)return;let interval=parseInt(ms)||100,addr=el.getAttribute('data-addr'),tt=el.getAttribute('data-tt'),name=el.getAttribute('data-pid'),ftype=el.getAttribute('data-vt'),isProp=el.getAttribute('data-ip')==='true',cns=el.getAttribute('data-cns')||'',ccls=el.getAttribute('data-ccls')||'',casm=el.getAttribute('data-casm')||'',asm=document.getElementById('ctrl-asm')?.value||'',ns=document.getElementById('ctrl-ns')?.value||cns,cls=document.getElementById('ctrl-cls')?.value||ccls;if(tt==='script'){ns=cns;cls=ccls;asm=casm;}fetch('/api/loop/add',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({addr,asm,ns,cls,name,ftype,isProp,val,interval})}).then(r=>r.json()).then(d=>{if(d.ok){btn.style.background='var(--grn)';refreshLoopsUI();}});}
async function refreshLoopsUI(){let loops=await fetch('/api/loop/list').then(r=>r.json()),el=document.getElementById('loops-list');if(!loops.length){el.innerHTML='<div style="color:#555;padding:12px;">No active loops</div>';return;}el.innerHTML=loops.map(lp=>`<div class="loop-row"><svg style="width:16px;height:16px;fill:var(--mth)"><use href="#ic-loop"/></svg><span class="loop-id">${esc(lp.cls)}.${esc(lp.name)}</span><span style="color:var(--str);font-family:Consolas,monospace;font-size:13px;">${esc(lp.val)}</span><span style="color:#888;font-size:11px;">${lp.interval}ms</span><button class="btn btn-red" style="padding:6px 12px;margin-left:auto;" onclick="removeLoop('${esc(lp.id)}',this)">Stop</button></div>`).join('');}
async function removeLoop(id,btn){await fetch('/api/loop/remove',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})});btn?.closest('.loop-row')?.remove();}
async function removeAllLoops(){await fetch('/api/loop/removeall',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});refreshLoopsUI();}
async function refreshCatcher(){let entries=await fetch('/api/catcher/list').then(r=>r.json()),el=document.getElementById('catcher-list');if(!entries.length){el.innerHTML='<div style="padding:20px;text-align:center;color:#555;font-size:13px;line-height:1.6;">No hooked methods yet.<br>Go to Controller or Scene, click the <span style="color:var(--kw);">&#9679;</span> catch button.</div>';return;}el.innerHTML=entries.map(e=>{let uid=uid8(),path=`${e.ns?e.ns+'.':''}${e.cls}.${e.method}`,params=e.params?.map(p=>`<span style="color:var(--type)">${esc(p.t)}</span> ${esc(p.n)}`).join(', ')||'';return`<div class="catcher-entry"><div class="catcher-hdr" onclick="loadCatcherCalls('${esc(e.id)}','${uid}')"><div class="ico" style="color:var(--blu2)"><svg><use href="#ic-catch"/></svg></div><div style="display:flex;flex-direction:column;flex:1;"><div style="display:flex;align-items:center;flex-wrap:wrap;gap:6px;"><span style="color:var(--mth);font-weight:bold;font-size:13px;">${esc(path)}</span><span style="background:#333;padding:2px 8px;border-radius:4px;font-size:11px;">${e.count} calls</span></div><div style="color:var(--type);font-size:11px;margin-top:4px;">(${params}) -&gt; <span style="color:var(--str)">${esc(e.ret)}</span></div></div><div style="display:flex;gap:8px;"><button class="btn" style="padding:6px 10px;" onclick="event.stopPropagation();clearCatcher('${esc(e.id)}')">Clear</button><button class="btn btn-red" style="padding:6px 12px;" onclick="event.stopPropagation();unhookCatcher('${esc(e.id)}')">X</button></div></div><div class="catcher-body" id="${uid}"></div></div>`;}).join('');}
async function loadCatcherCalls(id,uid){let el=document.getElementById(uid);if(el.classList.contains('open')){el.classList.remove('open');return;}el.classList.add('open');let calls=await fetch(`/api/catcher/calls?id=${encodeURIComponent(id)}`).then(r=>r.json());if(!calls.length){el.innerHTML='<div class="call-row" style="color:#555;text-align:center;padding:12px;">No calls recorded yet</div>';return;}el.innerHTML=calls.slice().reverse().map(c=>{let t=new Date(c.ts*1000).toLocaleTimeString(),args=c.args?.map((a,i)=>{let v=typeof a.v==='object'?JSON.stringify(a.v):String(a.v),isObj=v.startsWith('{')&&v.includes('"name"');if(isObj){try{let o=JSON.parse(v);return`<span style="color:var(--type)">${esc(a.t)}</span>: <span style="color:var(--str)">"${esc(o.name)}"</span> <span style="color:#666;font-size:10px;">${esc(o.addr)}</span>`;}catch(e){}}return`<span style="color:var(--type)">${esc(a.t)}</span>: <span style="color:var(--num)">${esc(v)}</span>`;}).join('<br>')||'<span style="color:#555;">no args</span>';return`<div class="call-row"><div style="display:flex;justify-content:space-between;align-items:center;"><span class="call-inst" style="font-weight:bold;font-size:12px;">${esc(c.instance)}</span><span class="call-ts">${t}</span></div><div class="call-args" style="margin-top:6px;padding-left:10px;border-left:2px solid #333;font-size:12px;">${args}</div></div>`;}).join('');}
async function unhookCatcher(id){await fetch('/api/catcher/unhook',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})});refreshCatcher();}
async function clearCatcher(id){await fetch('/api/catcher/clear',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})});refreshCatcher();}
async function unhookAll(){let entries=await fetch('/api/catcher/list').then(r=>r.json());for(let e of entries)await fetch('/api/catcher/unhook',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:e.id})});refreshCatcher();}
function downloadDump(){let a=document.createElement('a');a.href='/api/dump';a.download='dump.cs';a.click();}
async function runLua(){let code=document.getElementById('lua-editor').value;if(!code.trim())return;let out=document.getElementById('lua-output');out.textContent+='> Running...\n';try{let d=await fetch('/api/lua/exec',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code})}).then(r=>r.json());out.textContent+=d.output+'\n';}catch(e){out.textContent+='Error: '+e.message+'\n';}out.scrollTop=out.scrollHeight;}
function clearConsole(){document.getElementById('lua-output').textContent='';}
// --- JS FILE MANAGER ---
async function loadLuaFiles() {
    let files = await fetch('/api/lua/list').then(r=>r.json());
    let h = '';
    for(let f of files) {
        h += `<div class="tree-nc" onclick="loadLuaScript('${esc(f)}')"><div class="ico" style="color:var(--org2)"><svg><use href="#ic-script"/></svg></div><span>${esc(f)}</span></div>`;
    }
    document.getElementById('lua-files').innerHTML = h || '<div style="padding:10px;color:#555;text-align:center;">No scripts found</div>';
}

async function saveLuaScript() {
    let name = document.getElementById('lua-filename').value.trim();
    if(!name) { name = "script_" + Math.floor(Math.random()*1000) + ".lua"; document.getElementById('lua-filename').value = name; }
    if(!name.endsWith('.lua')) name += '.lua';
    let code = document.getElementById('lua-editor').value;
    let d = await fetch('/api/lua/save', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({name, code}) }).then(r=>r.json());
    if(d.ok) { copyTxt('Saved!'); loadLuaFiles(); } else alert('Error saving script');
}

async function loadLuaScript(name) {
    let d = await fetch('/api/lua/load', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({name}) }).then(r=>r.json());
    if(d.code !== undefined) {
        document.getElementById('lua-editor').value = d.code;
        document.getElementById('lua-filename').value = name;
    }
}

// --- JS SCANNER ---
async function runMemoryScan() {
    let val = document.getElementById('scan-val').value;
    let type = document.getElementById('scan-type').value;
    if(!val) return alert('Enter a value to search!');
    
    document.getElementById('scan-results').innerHTML = '<div style="color:var(--sel);text-align:center;margin-top:20px;">Scanning memory...</div>';
    let d = await fetch('/api/scanner/search', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({value:val, type:type}) }).then(r=>r.json());
    
    if(d.ok && d.results.length > 0) {
        let h = `<div style="margin-bottom:12px;font-weight:bold;">Found ${d.count} results (showing up to 150):</div><table class="table"><tr><th>Address</th><th>Value</th></tr>`;
        for(let r of d.results) {
            h += `<tr>
                <td><span class="addr-badge" onclick="copyTxt('${r.addr}')">${r.addr}</span></td>
                <td><span class="c-nm">${esc(r.val)}</span></td>
            </tr>`;
        }
        h += '</table>';
        document.getElementById('scan-results').innerHTML = h;
    } else {
        document.getElementById('scan-results').innerHTML = `<div style="color:#f66;text-align:center;margin-top:20px;">${d.error || 'No results found.'}</div>`;
    }
}

document.addEventListener('keydown',function(e){if(e.ctrlKey&&e.key==='Enter'){let el=document.getElementById('lua-editor');if(document.activeElement===el)runLua();}});

// Modificăm init-ul original pentru a chema și loadLuaFiles
let origInit = init;
init = async function() {
    await origInit();
    loadLuaFiles();
}
init();
</script>
</body>
</html>)HTML";
    return html;
}
