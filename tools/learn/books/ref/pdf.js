const {chromium}=require('playwright');
(async()=>{const b=await chromium.launch();const p=await b.newPage();
await p.goto('file:///tmp/claude-0/-home-user/3d438492-0fc4-54ce-af91-9a19580201ba/scratchpad/ref2/ref.html');
await p.pdf({path:'/tmp/claude-0/-home-user/3d438492-0fc4-54ce-af91-9a19580201ba/scratchpad/ref2/ref.pdf',format:'A4',printBackground:true,displayHeaderFooter:true,headerTemplate:'<span></span>',footerTemplate:'<div style="font-size:8px;width:100%;text-align:center;color:#888">Справочник Tachbit 1.3 · <span class="pageNumber"></span></div>',margin:{top:'16mm',bottom:'16mm',left:'15mm',right:'15mm'}});
await b.close();})();
