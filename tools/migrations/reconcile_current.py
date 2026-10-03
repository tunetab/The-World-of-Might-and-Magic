from pathlib import Path
import json,re,hashlib
r=Path(__file__).resolve().parents[2];tick=chr(96)
manifest=json.loads((r/"10_Обслуживание/Миграция_v2.json").read_text(encoding="utf-8-sig"))
for name,heading,current in [
 ("Алиманджад","Текущие события","Алиманджад сохраняет значение двора Уриона и места коронации Вито. Прежнее предложение дочерней гильдии не стало договором: Вито отверг встречную формулу Михаэля без личной гарантии (DEC-135). Михаэль и Нилуфар уже покинули этот этап путешествия; их текущая точка ведётся в контексте ветки Михаэля. Предполагаемая беременность остаётся открытым Q-C3-056."),
 ("Цитадель_Вечного_Света","Текущая кампания","Цитадель остаётся столичным узлом Империи. Текущая основная линия Александроса развивается в Валантиде; публичная формула положения Михаэля и Харагорна ещё ожидает DEC-PENDING-132. Личная реакция Михаэля на весть Нилуфар уже зафиксирована как DEC-131; подтверждение беременности остаётся Q-C3-056.")
]:
 p=f"04_Локации/{name}.md";text=(r/p).read_text(encoding="utf-8-sig")
 hist=f"04_Локации/История/{name}_до_v2.md"
 if (r/hist).exists():continue
 (r/hist).parent.mkdir(parents=True,exist_ok=True)
 prefix=f"# История: {name}\n\n---\ntype: history_record\nstatus: active\ncanon_level: support\noriginal_path: {p}\n---\n\nИсторический срез; прежние текущие формулировки относятся к моменту записи.\n\n<!-- ORIGINAL DOCUMENT -->\n"
 (r/hist).write_text(prefix+text.rstrip()+"\n",encoding="utf-8",newline="\n")
 manifest["history"].append({"original_path":p,"history_path":hist,"text_sha256":hashlib.sha256(text.rstrip().encode()).hexdigest()})
 pattern=r"(?ms)^## "+re.escape(heading)+r"\s*\n.*?(?=^## |\Z)"
 text=re.sub(pattern,"## "+heading+"\n\n"+current+"\n\nПолная история: "+tick+hist+tick+".\n\n",text)
 text=text.replace("("+tick+"DEC-PENDING-133"+tick+", "+tick+"Q-C3-056"+tick+")","("+tick+"Q-C3-056"+tick+")")
 (r/p).write_text(text,encoding="utf-8",newline="\n")
(r/"10_Обслуживание/Миграция_v2.json").write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n",encoding="utf-8",newline="\n")
