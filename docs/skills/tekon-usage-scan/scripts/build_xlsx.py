# -*- coding: utf-8 -*-
"""Build TEKON_usage_report.xlsx from a tekon-usage-scan output directory."""
import argparse, csv, json, os
from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.chart import BarChart, Reference

NAVY="1F3864"; BLUE="2F5597"; LIGHT="D9E2F3"; GREY="F2F2F2"; GREEN="E2EFDA"; RED="F8CBAD"
thin=Side(style="thin", color="BFBFBF")
BORDER=Border(left=thin,right=thin,top=thin,bottom=thin)
HF=Font(name="Microsoft YaHei",bold=True,color="FFFFFF",size=10)
BF=Font(name="Microsoft YaHei",size=10)
TF=Font(name="Microsoft YaHei",bold=True,size=16,color=NAVY)
SF=Font(name="Microsoft YaHei",size=10,color="595959")

def truth(v):
    return str(v).strip().lower() in ("yes","true","1")

def read_csv(path):
    if not os.path.exists(path): return []
    with open(path,encoding="utf-8-sig",newline="") as f: return list(csv.DictReader(f))

def read_json(path, default=None):
    if not os.path.exists(path): return default
    with open(path,encoding="utf-8") as f: return json.load(f)

def header(ws,row,n,height=28):
    for c in range(1,n+1):
        cell=ws.cell(row=row,column=c)
        cell.font=HF; cell.fill=PatternFill("solid",fgColor=BLUE)
        cell.alignment=Alignment(horizontal="center",vertical="center",wrap_text=True)
        cell.border=BORDER
    ws.row_dimensions[row].height=height

def body(cell,wrap=False):
    cell.font=BF; cell.border=BORDER
    cell.alignment=Alignment(vertical="top",wrap_text=wrap)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--dir",required=True,help="output directory of tekon-scan report")
    ap.add_argument("--out",default=None)
    a=ap.parse_args()
    base=os.path.abspath(a.dir)
    out=a.out or os.path.join(base,"TEKON_usage_report.xlsx")
    m=read_json(os.path.join(base,"manifest.json"),{})
    T=m.get("totals",{})
    summary=read_csv(os.path.join(base,"summary.csv"))
    sessions=read_csv(os.path.join(base,"sessions.csv"))
    audit=read_csv(os.path.join(base,"official_logs_audit.csv"))
    ledger=read_csv(os.path.join(base,"platform_ledger.csv"))

    wb=Workbook()
    ws=wb.active; ws.title="总览"; ws.sheet_view.showGridLines=False
    ws.merge_cells("A1:F1"); ws["A1"]="TEKON 用量统计 · "+str(m.get("project","project"))
    ws["A1"].font=TF; ws["A1"].alignment=Alignment(vertical="center"); ws.row_dimensions[1].height=34
    ws.merge_cells("A2:F2")
    ws["A2"]="生成 "+str(m.get("generated_at",""))+"　|　口径 "+str(m.get("accounting",{}).get("method",""))+"　|　落盘 "+base
    ws["A2"].font=SF; ws.row_dimensions[2].height=20

    kpis=[("计入总量（tokens）",T.get("total_tokens",0)),("输入 prompt",T.get("prompt_tokens",0)),
          ("其中缓存命中",T.get("cached_tokens",0)),("输出 completion",T.get("completion_tokens",T.get("output_tokens",0))),
          ("会话数",T.get("sessions",0)),("请求数",T.get("requests",0))]
    for i,(label,val) in enumerate(kpis):
        r=4+i//3; c=1+(i%3)*2
        ws.cell(row=r,column=c,value=label).font=Font(name="Microsoft YaHei",size=9,color="595959")
        cell=ws.cell(row=r,column=c+1,value=val)
        cell.font=Font(name="Microsoft YaHei",bold=True,size=15,color=NAVY); cell.number_format="#,##0"
        cell.alignment=Alignment(horizontal="left")
    ws.row_dimensions[4].height=24; ws.row_dimensions[5].height=24

    included=[s for s in summary if truth(s.get("included_in_total"))]
    hdrs=["Agent","会话","请求","total_tokens","占比","官方可采集"]
    for j,h in enumerate(hdrs,1): ws.cell(row=8,column=j,value=h)
    header(ws,8,len(hdrs))
    start=9
    for i,s in enumerate(included):
        r=start+i
        ws.cell(row=r,column=1,value=s.get("agent_name") or s.get("agent_id"))
        ws.cell(row=r,column=2,value=int(float(s.get("sessions") or 0)))
        ws.cell(row=r,column=3,value=int(float(s.get("requests") or 0)))
        ws.cell(row=r,column=4,value=int(float(s.get("total_tokens") or 0)))
        tot=int(float(T.get("total_tokens") or 0)) or 1
        ws.cell(row=r,column=5,value=int(float(s.get("total_tokens") or 0))/tot)
        ws.cell(row=r,column=6,value="是" if truth(s.get("official_collectable")) else "否")
        for c in range(1,7):
            body(ws.cell(row=r,column=c),wrap=(c==1))
            if c in (2,3,4): ws.cell(row=r,column=c).number_format="#,##0"
            if c==5: ws.cell(row=r,column=c).number_format="0.00%"
        if truth(s.get("official_collectable")):
            ws.cell(row=r,column=6).fill=PatternFill("solid",fgColor=GREEN)
    tr=start+len(included)
    ws.cell(row=tr,column=1,value="合计（计入）")
    ws.cell(row=tr,column=2,value=int(float(T.get("sessions") or 0)))
    ws.cell(row=tr,column=3,value=int(float(T.get("requests") or 0)))
    ws.cell(row=tr,column=4,value=int(float(T.get("total_tokens") or 0)))
    ws.cell(row=tr,column=5,value=1)
    for c in range(1,7):
        cell=ws.cell(row=tr,column=c); cell.font=Font(name="Microsoft YaHei",bold=True,size=10)
        cell.fill=PatternFill("solid",fgColor=LIGHT); cell.border=BORDER
    ws.cell(row=tr,column=4).number_format="#,##0"; ws.cell(row=tr,column=5).number_format="0.00%"

    if len(included)>0:
        ch=BarChart(); ch.type="bar"; ch.style=10
        ch.title="各 Agent total_tokens（计入合计）"; ch.y_axis.title="tokens"; ch.legend=None
        ch.add_data(Reference(ws,min_col=4,min_row=8,max_row=tr-1),titles_from_data=True)
        ch.set_categories(Reference(ws,min_col=1,min_row=start,max_row=tr-1))
        ch.height=9.5; ch.width=17; ws.add_chart(ch,"H8")

    note=("说明：官方 logs/ 仅自动采集 Claude Code（含 AIoT-IDE）、AIoT-IDE、OpenCode、Codex 四种工具。"
          "DSH、WorkBuddy、K3DSH、SUPERAGENT 等不在官方采集范围，本表是项目侧补充台账，不能计入官方有效工时。")
    nr=tr+2
    ws.merge_cells(start_row=nr,start_column=1,end_row=nr,end_column=6)
    ws.cell(row=nr,column=1,value=note).font=Font(name="Microsoft YaHei",size=9,color="C00000")
    ws.cell(row=nr,column=1).alignment=Alignment(wrap_text=True,vertical="top")
    ws.row_dimensions[nr].height=42
    for col,w in {"A":40,"B":10,"C":11,"D":18,"E":10,"F":13}.items(): ws.column_dimensions[col].width=w

    # 各端汇总
    ws2=wb.create_sheet("各端汇总"); ws2.sheet_view.showGridLines=False
    cols=["agent_id","agent_name","platform","type","official_tool","official_collectable","collection_mode",
          "included_in_total","sessions","requests","prompt_tokens","cached_tokens","input_tokens",
          "cache_write_tokens","output_tokens","total_tokens","first_at","last_at","notes"]
    labels=["agent_id","名称","平台","类型","官方工具","官方可采集","采集方式","计入","会话","请求",
            "prompt","cached","input","cache_write","output","total","起始","结束","备注"]
    for j,h in enumerate(labels,1): ws2.cell(row=1,column=j,value=h)
    header(ws2,1,len(labels))
    for i,row in enumerate(summary,2):
        for j,k in enumerate(cols,1):
            v=row.get(k,"")
            if k in ("sessions","requests","prompt_tokens","cached_tokens","input_tokens","cache_write_tokens","output_tokens","total_tokens"):
                try: v=int(float(v or 0))
                except Exception: v=0
            cell=ws2.cell(row=i,column=j,value=v); body(cell,wrap=(j in (2,3,19)))
            if isinstance(v,int): cell.number_format="#,##0"
        if row.get("agent_id")=="TOTAL":
            for j in range(1,len(labels)+1):
                ws2.cell(row=i,column=j).font=Font(name="Microsoft YaHei",bold=True,size=10)
                ws2.cell(row=i,column=j).fill=PatternFill("solid",fgColor=LIGHT)
        elif not truth(row.get("included_in_total")):
            for j in range(1,len(labels)+1): ws2.cell(row=i,column=j).fill=PatternFill("solid",fgColor=GREY)
    ws2.freeze_panes="C2"; ws2.auto_filter.ref="A1:S%d"%(len(summary)+1)
    widths={"A":18,"B":34,"C":14,"D":18,"E":9,"F":9,"G":22,"H":7,"I":8,"J":9,"K":14,"L":14,"M":12,"N":12,"O":12,"P":15,"R":26,"S":34}
    for col,w in widths.items(): ws2.column_dimensions[col].width=w

    # 会话明细
    ws3=wb.create_sheet("会话明细"); ws3.sheet_view.showGridLines=False
    scols=["session_key","agent_id","decision","included_in_total","session_id","title","cwd","started_at","last_event_at",
           "requests","duplicate_requests","prompt_tokens","cached_tokens","input_tokens","output_tokens","total_tokens","models","score","first_user"]
    slabels=["session_key","agent","决策","计入","session_id","标题","cwd","开始","最后事件","请求","去重丢弃",
             "prompt","cached","input","output","total","模型","评分","首条用户消息"]
    for j,h in enumerate(slabels,1): ws3.cell(row=1,column=j,value=h)
    header(ws3,1,len(slabels))
    for i,row in enumerate(sessions,2):
        for j,k in enumerate(scols,1):
            v=row.get(k,"")
            if k in ("requests","duplicate_requests","prompt_tokens","cached_tokens","input_tokens","output_tokens","total_tokens","score"):
                try: v=int(float(v or 0))
                except Exception: v=0
            cell=ws3.cell(row=i,column=j,value=v); body(cell,wrap=(j in (6,7,19)))
            if isinstance(v,int): cell.number_format="#,##0"
        if not truth(row.get("included_in_total")):
            for j in range(1,len(slabels)+1): ws3.cell(row=i,column=j).fill=PatternFill("solid",fgColor=GREY)
    ws3.freeze_panes="D2"; ws3.auto_filter.ref="A1:S%d"%(len(sessions)+1)
    for col,w in {"A":34,"B":14,"C":9,"D":7,"E":30,"F":36,"G":30,"H":21,"I":21,"J":8,"K":9,"L":14,"M":14,"N":12,"O":12,"P":15,"Q":34,"R":7,"S":48}.items():
        ws3.column_dimensions[col].width=w

    # 官方审计
    if audit:
        ws4=wb.create_sheet("官方日志审计"); ws4.sheet_view.showGridLines=False
        acols=list(audit[0].keys()); alabels=acols
        for j,h in enumerate(alabels,1): ws4.cell(row=1,column=j,value=h)
        header(ws4,1,len(alabels))
        for i,row in enumerate(audit,2):
            for j,k in enumerate(acols,1):
                v=row.get(k,"")
                if k in ("events","assistant_events","tokens_in","tokens_out","manifest_event_count","ts_delta_sec"):
                    try: v=int(float(v or 0))
                    except Exception: v=0
                cell=ws4.cell(row=i,column=j,value=v); body(cell,wrap=(j in (1,18)))
                if isinstance(v,int): cell.number_format="#,##0"
            if str(row.get("event_count_match")).strip().lower()=="no":
                for j in range(1,len(alabels)+1): ws4.cell(row=i,column=j).fill=PatternFill("solid",fgColor=RED)
        ws4.freeze_panes="B2"; ws4.auto_filter.ref="A1:R%d"%(len(audit)+1)
        for col,w in {"A":54,"B":13,"C":36,"D":9,"E":10,"F":12,"G":12,"H":26,"I":21,"J":21,"K":21,"L":21,"M":10,"N":10,"O":10,"P":8,"Q":14,"R":56}.items():
            ws4.column_dimensions[col].width=w

    # 平台台账
    if ledger:
        ws5=wb.create_sheet("平台台账"); ws5.sheet_view.showGridLines=False
        lcols=list(ledger[0].keys())
        for j,h in enumerate(lcols,1): ws5.cell(row=1,column=j,value=h)
        header(ws5,1,len(lcols))
        for i,row in enumerate(ledger,2):
            for j,k in enumerate(lcols,1):
                v=row.get(k,"")
                try:
                    if k not in ("agent_id","period_start","period_end","source","notes","platform","model","collection_mode"): v=int(float(v or 0))
                except Exception: pass
                cell=ws5.cell(row=i,column=j,value=v); body(cell,wrap=(k in ("source","notes")))
                if isinstance(v,int): cell.number_format="#,##0"
        ws5.freeze_panes="B2"; ws5.auto_filter.ref="A1:%s%d"%(chr(64+len(lcols)),len(ledger)+1)

    # 说明
    ws6=wb.create_sheet("说明"); ws6.sheet_view.showGridLines=False
    ws6.column_dimensions["A"].width=118
    lines=[
      ("TEKON 用量统计 · 说明",TF),
      ("",BF),
      ("1. 口径：prompt_tokens = 全部输入（含缓存）；cached_tokens 是 prompt 的子集；total = prompt + output。",BF),
      ("2. 去重：DSH 按 assistant message.id；WorkBuddy 按 providerData.messageId；官方 LOGS 按 manifest 文件。",BF),
      ("3. 崩溃恢复副本：同一对话的多份副本在 DSH 扫描阶段按 message id 归属，只计一次；merge 按 session_key 合并。",BF),
      ("4. 官方 LOGS：只采集 Claude Code / AIoT-IDE / OpenCode / Codex；included_in_total=no，只作证据。",BF),
      ("5. 平台账单：MiMo / DSH 平台 CSV 与本地会话是同一批用量的两个视图，不能相加。",BF),
      ("6. 快照：所有数字都有 generated_at；会话仍增长时，应重跑 scan/report 或标注快照时间。",BF),
      ("7. 隐私：报告不含 API key、token、内网 IP；WorkBuddy models.json 不会被读取。",BF),
      ("",BF),
      ("生成器：tekon-usage-scan / build_xlsx.py",SF),
      ("目录："+base,SF),
    ]
    for i,(text,font) in enumerate(lines,1):
        c=ws6.cell(row=i,column=1,value=text); c.font=font; c.alignment=Alignment(wrap_text=True,vertical="top")
        ws6.row_dimensions[i].height=22 if i>2 else 30

    wb.save(out)
    print(json.dumps({"ok":True,"out":out,"sheets":wb.sheetnames,"included_agents":len(included),"sessions":len(sessions),"audit":len(audit)},ensure_ascii=False))

if __name__=="__main__":
    main()
