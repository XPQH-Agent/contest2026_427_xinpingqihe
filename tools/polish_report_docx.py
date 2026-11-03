#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对 make_report_docx.py 生成的官方模板 docx 做两步润色：
   1) 删除模板自带的占位/提示段落（xx%…、可估算…、请填写…）；
   2) 追加「附录：真机证据图集」——按证据目录挑图并加图注（提高可读性与观感）。
   用法: python3 tools/polish_report_docx.py --docx docs/PhyWear_技术报告_官方模板.docx
"""
import argparse, glob, os, re
from docx import Document
from docx.shared import Inches, Pt
from docx.enum.text import WD_ALIGN_PARAGRAPH

HINT = re.compile(r'(xx%|可估算|请填写|请在此|示例[:：]|如是|（示例|待填)')
PICK = [
    ('docs/design/v8-01-home.png',                    'UI 原型（v8 设计稿，最终版按用户意见回退到重置前设计）'),
    ('docs/evidence/ui-v2-20260917',                  '真机 UI（重置前设计：主页/板块页/读数页）'),
    ('docs/evidence/btui-20260918',                   '蓝牙 UI：主页连接标识（灰/蓝）+ 手表版蓝牙串口页'),
    ('docs/evidence/bt-b3-20260918',                  '蓝牙 B3 自动验收（宿主作中心设备：设备 6/6 + 宿主 9/9）'),
    ('docs/evidence/accept-20260918',                  '一键设备侧验收 18/18（含句柄结构自检与 notify 演练）'),
    ('docs/evidence/imu-ahrs-20260915',                '姿态解算与标定（AHRS / 六面法 / 磁椭球）'),
    ('docs/evidence/proactive-20260915',               '「主动+执行」场景：晃表触发自动实验'),
    ('docs/evidence/realboard-20260912',               '真机页面截图（早期批次）'),
]

def strip_hints(doc):
    n = 0
    def scan(paras):
        nonlocal n
        for p in list(paras):
            t = (p.text or '').strip()
            if t and len(t) < 80 and HINT.search(t):
                p._element.getparent().remove(p._element); n += 1
    scan(doc.paragraphs)
    for tbl in doc.tables:                 # 模板提示文字藏在表格单元格里
        for row in tbl.rows:
            for cell in row.cells:
                scan(cell.paragraphs)
    return n

def add_figures(doc, per_dir=2):
    doc.add_page_break()
    _t = doc.add_paragraph(); _r = _t.add_run('附录：真机证据图集'); _r.bold = True; _r.font.size = Pt(16)
    doc.add_paragraph('以下均为真机（立创·黄山派 SF32LB52-MOD-1-N16R8）截图，完整证据与校验和见仓库 docs/evidence/ 对应目录。')
    got = 0
    for path, cap in PICK:
        files = [path] if os.path.isfile(path) else sorted(glob.glob(os.path.join(path, '*.png')))[:per_dir]
        for f in files:
            try:
                doc.add_picture(f, width=Inches(3.1))
            except Exception:
                continue
            doc.add_paragraph(f'图 {got+1}：{cap}（{os.path.basename(f)}）').alignment = WD_ALIGN_PARAGRAPH.CENTER
            got += 1
    return got

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--docx', required=True)
    ap.add_argument('--per-dir', type=int, default=2)
    a = ap.parse_args()
    doc = Document(a.docx)
    h = strip_hints(doc); g = add_figures(doc, a.per_dir)
    doc.save(a.docx)
    print(f'✅ 润色完成：删除提示段 {h} 处，插入配图 {g} 张 -> {a.docx}')

if __name__ == '__main__':
    main()
