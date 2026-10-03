from pathlib import Path
import json,re
r=Path(__file__).resolve().parents[2]
m=json.loads((r/"10_Обслуживание/Миграция_v2.json").read_text(encoding="utf-8-sig"))
protected={x["path"] for x in m["protected_files"]}
count=0
for directory in ["00_Инструкции_для_ИИ","01_Кампания","02_Лор","03_Персонажи","04_Локации","05_Активы_персонажей","07_Черновики_и_идеи","08_Источники","09_Реестры","09_Шаблоны","10_Обслуживание","tools"]:
 for p in (r/directory).rglob("*"):
  if not p.is_file() or p.suffix not in [".md",".ps1",".json",".py"]:continue
  if p.relative_to(r).as_posix() in protected:continue
  text=p.read_text(encoding="utf-8-sig")
  # Existing scripts specify CRLF; Markdown and JSON specify LF.
  normalized=text.replace("\r\n","\n")
  data=normalized.replace("\n","\r\n").encode("utf-8") if p.suffix==".ps1" else normalized.encode("utf-8")
  if p.read_bytes()!=data:p.write_bytes(data);count+=1
print("Normalized files:",count)
