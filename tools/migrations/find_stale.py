from pathlib import Path
import re,json
r=Path(".")
d=json.loads((r/"09_Реестры/Решения.json").read_text(encoding="utf-8-sig"))
pending={x["id"] for x in d["decisions"] if x["state"]=="pending"}
for folder in ["03_Персонажи","04_Локации","05_Активы_персонажей","02_Лор"]:
 for p in (r/folder).rglob("*.md"):
  if "История" in p.parts: continue
  text=p.read_text(encoding="utf-8-sig")
  for m in re.finditer(r"(?ms)^## ([^\n]+)\n(.*?)(?=^## |\Z)",text):
   if not re.search("Текущее|Активные|Текущая|Текущие|Новые|Нынеш",m[1]):continue
   bad=set(re.findall(r"DEC-PENDING-\d{3}",m[2]))-pending
   if bad: print(str(p),m[1],",".join(sorted(bad)))
