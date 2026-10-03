"""One-time, lossless migration. Python is used only for this migration, not normal play."""
from pathlib import Path
import json, re, hashlib, subprocess

ROOT = Path(__file__).resolve().parents[2]
T = chr(96)
TODAY = "2026-09-12"

def read(p):
    return (ROOT / p).read_text(encoding="utf-8-sig")
def write(p, text):
    path = ROOT / p
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text.rstrip() + "\n", encoding="utf-8")
def load(p):
    return json.loads(read(p))
def save(p, value):
    write(p, json.dumps(value, ensure_ascii=False, indent=2))
def meta(text, field):
    match = re.search(r"(?m)^" + re.escape(field) + r":\s*([^\n]*)", text)
    return match.group(1).strip() if match else ""
def setmeta(text, field, value):
    match = re.search(r"(?s)\A# [^\n]+\n\s*---\n(.*?)\n---", text)
    if not match:
        raise ValueError("No front matter")
    block = match.group(1)
    line = field + ": " + value
    if re.search(r"(?m)^" + re.escape(field) + ":", block):
        block = re.sub(r"(?m)^" + re.escape(field) + r":[^\n]*", lambda _: line, block)
    else:
        block += "\n" + line
    return text[:match.start(1)] + block + text[match.end(1):]
def section(text, title):
    m = re.search(r"(?ms)^## " + re.escape(title) + r"\s*\n(.*?)(?=^## |\Z)", text)
    return m.group(1).strip() if m else ""
def link(p):
    return T + p + T
def header(title, kind, extra=""):
    return f"# {title}\n\n---\ntype: {kind}\nstatus: active\ncanon_level: support\n{extra}---\n\n"

manifest_path = "10_Обслуживание/Миграция_v2.json"
if (ROOT / manifest_path).exists():
    raise SystemExit("Migration already applied. Use project checks; do not migrate twice.")
graph = load("09_Реестры/Сущности.json")
entities = graph["entities"]
by_path = {e["path"]: e for e in entities}
by_name = {e["name"]: e for e in entities if e["type"] == "character"}
def cid(name):
    return by_name[name]["id"]
def sid(branch, number):
    matches = [e for e in entities if e["type"] == "scene" and e["branch"] == branch and f"/Сцена_{number:03d}_" in e["path"]]
    assert len(matches) == 1, (branch, number)
    return matches[0]["id"]
def source(fragment):
    matches = [e for e in entities if e["type"].startswith("source") and fragment in e["path"]]
    assert len(matches) == 1, fragment
    return matches[0]["id"]
by_id = {e["id"]: e for e in entities}
manifest = {"schema_version": 2, "baseline_commit": subprocess.check_output(["git","rev-parse","HEAD"],cwd=ROOT,text=True).strip(), "migrated_on": TODAY, "protected_files": [], "history": []}
for folder in ["01_Кампания/Главы", "06_Архив_канона/Завершенные_главы"]:
    for path in (ROOT / folder).glob("*.md"):
        if meta(path.read_text(encoding="utf-8-sig"), "status") == "closed":
            manifest["protected_files"].append({"path": path.relative_to(ROOT).as_posix(), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})

def archive(path, destination):
    text = read(path)
    # The entire original document is retained byte-for-byte as UTF-8 content after the marker.
    prefix = header("История: " + Path(path).stem, "history_record", f"original_path: {path}\narchived_real_date: {TODAY}\n")
    prefix += "Исторический срез. Формулировки «сейчас» и незакрытые ссылки относятся к моменту записи. Для текущего положения используйте основную карточку и контекст ветки.\n\n<!-- ORIGINAL DOCUMENT -->\n"
    write(destination, prefix + text)
    manifest["history"].append({"original_path": path, "history_path": destination, "text_sha256": hashlib.sha256(text.rstrip().encode()).hexdigest()})
    return text

A=cid("Император Александрос"); M=cid("Принц Михаэль"); N=cid("Лорд Никитосиос"); H=cid("Принц Харрингтон")
V=cid("Архимаг Вито"); L=cid("Линдольф"); NIL=cid("Принцесса Нилуфар"); AR=cid("Аругал")
S62=sid("Александрос",62); S63=sid("Александрос",63); S44=sid("Михаэль",44)
SW=sid("Общий мир",2); SN=sid("Никитосиос",9); S61=sid("Александрос",61)
# Explicit source matches were verified against the actual source content in the audit.
scene_sources = {
    S62: [source("2026-07-11_валантида,")],
    S63: [source("2026-07-11_храм_вечного")],
    S44: [source("2026-07-11_андрианаполис,")],
    SW: [source("2026-07-11_высадка_союза"), source("2026-07-11_жертва_утера")],
    SN: [source("2026-07-04_сюжет_-_никитосиос_-_свобода")],
}
for e in entities:
    path=e["path"]; text=read(path)
    if e["type"] == "character":
        aliases=[e["name"], Path(path).stem.replace("_"," ")]
        if e["id"] in [A,M,N,H]:
            aliases += [{A:"Александрос",M:"Михаэль",N:"Никитосиос",H:"Харрингтон"}[e["id"]]]
        text=setmeta(text,"aliases",json.dumps(sorted(set(aliases)),ensure_ascii=False))
    if e["type"] == "scene":
        participants=[]
        block=section(text,"Участники")
        # A role here is a participant of the recorded scene, not proof of physical presence.
        for name, char in by_name.items():
            if re.search(r"(?m)^-\s+" + re.escape(name) + r"(?:[.,; —–-]|$)",block):
                participants.append(char["id"])
        text=setmeta(text,"participant_ids",json.dumps(sorted(set(participants))))
        ids=scene_sources.get(e["id"],e.get("source_ids",[]))
        text=setmeta(text,"source_ids",json.dumps(ids))
        fronts=set(e.get("front_ids",[]))
        if e["id"] == S44: fronts.add("FRONT-WAR-TREASURY")
        text=setmeta(text,"front_ids",json.dumps(sorted(fronts)))
    write(path,text)

facts=[]
def fact(code,text,truth,evidence,subjects=(),known=(),visibility="restricted",reported_by=None):
    facts.append({"id":code,"text":text,"truth":truth,"subject_ids":list(subjects),"evidence_ids":list(evidence),"visibility":visibility,"known_to":list(known),"reported_by":reported_by,"story_time":"после аудиенции в Валантиде" if S62 in evidence or S63 in evidence or S44 in evidence else "см. событие-основание","recorded_on":TODAY})
fact("FACT-001","Первый механизм Сар-Илама разрушен; два оставшихся известны в Элирии и Валантиде. Устройство и способы защиты не установлены.", "confirmed",[S62],[A,M,N],[A,M,N])
fact("FACT-002","Утер и Дариус погибли при первой высадке Союза. Закхерон Багровый убит, Клинок скорби уничтожен.", "confirmed",[SW],[A,N,H],[A,M,N,H])
fact("FACT-003","Вито при смерти после столкновения с Ур-Таликосом; обстоятельства боя и возможность спасения остаются открыты.", "confirmed",[S62],[V],[A,M,N])
fact("FACT-004","Демоны готовят удар по механизму скипетра в Элирии. Никитосиос отправляется добиваться перемирия с сумеречными эльфами.", "reported",[S62],[N],[A,M,N],reported_by=cid("Король Авалон"))
fact("FACT-005","Авалон поручил Лайонелу подготовить выживших лидеров Союза; подготовка ещё не означает готовности победить аватар.", "confirmed",[S62],[A,M,N],[A,M,N])
fact("FACT-006","Линдольф предложил Александросу поход к Храму Вечного Солнца. Согласие или отказ Императора ещё не зафиксированы.", "confirmed",[S63],[A,L],[A,L])
fact("FACT-007","Пламя вечности — местная легенда. Его существование, свойства и способность ранить аватар не подтверждены; рассказы о невозвращении искателей также не являются проверенным перечнем жертв.", "legend",[S63],[A,L],[A,L],reported_by=L)
fact("FACT-008","Торговая палата высших эльфов разрешила Михаэлю основать штаб в Андрианаполисе. Построенный штаб, монополия и условия доступа этим не подтверждены.", "confirmed",[S44],[M],[M,cid("Капитан Картос")])
fact("FACT-009","По докладу Аругала разведка гильдии действует, штаб в Цитадели Ночи открыт, склады в Феликии построены. Состав, бюджет и темпы расширения не установлены.", "reported",[S44],[M,AR],[M,AR],reported_by=AR)
fact("FACT-010","Вито отверг встречную формулу Михаэля и временный логистический контракт без личной гарантии. Новый договор не заключён.", "confirmed",[S44],[M,V],[M,V])
fact("FACT-011","Никитосиос передал Михаэлю официальное помилование Аразаила; основания и правовые пределы жеста неизвестны.", "confirmed",[S44],[M,N],[M,N])
fact("FACT-012","Михаэлю приснились женщина в алых шелках и Ур-Серагон. Сон не доказывает сделку, реальную помощь демону или личность женщины.", "dream",[S44],[M],[M])
fact("FACT-013","Михаэль остаётся сыном Александроса по воспитанию. Харагорн — его кровный сын; публичное оформление статусов ещё не решено.", "confirmed",[S61,cid("Харагорн")],[A,M,cid("Харагорн")],[A,M,cid("Харагорн")])
fact("FACT-014","Нилуфар сообщила Михаэлю о предполагаемой беременности; супруги сохраняют это в тайне до конфиденциального медицинского наблюдения.", "reported",[cid("Принцесса Нилуфар"),sid("Михаэль",43)],[M,NIL],[M,NIL],reported_by=NIL)
fact("FACT-015","Никитосиос свободен и признан внуком Аразаила. Приглашение в королевский клан остаётся ожидающим решением; автоматически считать его принятым нельзя.", "confirmed",[SN],[N],[N])
fact("FACT-016","Бейн не вернулся из лабиринта; его судьба неизвестна.", "unknown",[SN],[N],[N])
fact("FACT-017","Онсераг погиб после раскрытия обмана. Каролина сознательно приветствовала Ур-Серагона как господина; её вина не устанавливает вину всех Кланов Закона.", "confirmed",[source("2026-07-04_сюжет_-_эпилог")],[A,M,N],[])
save("09_Реестры/Знания.json",{"schema_version":2,"type":"knowledge_registry","facts":facts})
state={
 "schema_version":2,"type":"context_state","chapter_file":"01_Кампания/Главы/Глава_04_Врата_преисподней.md",
 "scene_ids":[S62,S63,S44],"fact_ids":["FACT-001","FACT-002","FACT-003","FACT-004","FACT-005","FACT-017"],
 "focus_ids":["FRONT-THREE-MOONS","Q-WORLD-044","Q-WORLD-046","Q-C4-001","DEC-PENDING-136","DEC-PENDING-137","Q-WORLD-043","Q-WORLD-045"],
 "branches":[
  {"name":"Александрос","character_id":A,"scene_ids":[S63,S62,S61],"fact_ids":["FACT-006","FACT-007","FACT-013"],"focus_ids":["DEC-PENDING-136","DEC-PENDING-132","FRONT-THREE-MOONS"],"situation":"Александрос в Валантиде выбирает между походом к храму с Линдольфом и подготовкой Лайонела.","history":"01_Кампания/История/Ветка_Александрос_до_v2.md"},
  {"name":"Михаэль","character_id":M,"scene_ids":[S44,sid("Михаэль",43),S62],"fact_ids":["FACT-008","FACT-009","FACT-010","FACT-011","FACT-012","FACT-013","FACT-014"],"focus_ids":["DEC-PENDING-137","Q-C4-001","FRONT-WAR-TREASURY","DEC-PENDING-132"],"situation":"Михаэль в Валантиде определяет распределение капитала после доступа к Андрианаполису; смысл сна не раскрыт.","history":"01_Кампания/История/Ветка_Михаэль_до_v2.md"},
  {"name":"Никитосиос","character_id":N,"scene_ids":[S62,S44,SN],"fact_ids":["FACT-015","FACT-016","FACT-011"],"focus_ids":["Q-WORLD-046","DEC-PENDING-131","FRONT-THREE-MOONS"],"situation":"Никитосиос отправляется в Элирию договариваться с сумеречными эльфами; вступление в клан Аразаила не подтверждено.","history":"01_Кампания/История/Ветка_Никитосиос_до_v2.md"},
  {"name":"Харрингтон","character_id":H,"scene_ids":[S62,SW],"fact_ids":[],"focus_ids":["Q-C3-037","FRONT-SUN-SPEAR"],"situation":"Положение Харрингтона сверяется с общими сценами Союза и его карточкой. Нового самостоятельного выбора не зафиксировано.","history":"01_Кампания/История/Ветка_Харрингтон_до_v2.md"}
 ]}
save("09_Реестры/Контекст.json",state)
save("09_Реестры/Хронология_связей.json",{"schema_version":2,"type":"event_relations","relations":[
 {"from":S62,"to":SW,"relation":"after","evidence_ids":[source("2026-07-11_валантида,")],"precision":"relative"},
 {"from":S63,"to":S62,"relation":"after","evidence_ids":[source("2026-07-11_храм_вечного")],"precision":"relative"},
 {"from":S44,"to":S62,"relation":"overlaps","evidence_ids":[source("2026-07-11_андрианаполис,")],"precision":"relative"}
]})

# Stable decision identity never depends on its pending/accepted display number.
decisions=load("09_Реестры/Решения.json")
decisions["schema_version"]=2
decisions["source_of_truth"]=True
decisions["imported_from"]=decisions.pop("generated_from",[])
for d in decisions["decisions"]:
    d.setdefault("uid","DUID-"+hashlib.sha256(d["id"].encode()).hexdigest()[:16])
    d.setdefault("transitions",[])
    d.setdefault("resolved_from",None)
    d["link_paths"]=re.findall(T+r"([^"+T+r"]+)"+T,d.get("links",""))
    d["scene_ids"]=[e["id"] for e in entities if e["type"]=="scene" and (e["path"] in d["link_paths"] or e["path"]==d.get("scene"))]
save("09_Реестры/Решения.json",decisions)
questions=load("09_Реестры/Вопросы.json")
questions["schema_version"]=2
questions["source_of_truth"]=True
questions["imported_from"]=questions.pop("generated_from",[])
focus=set(state["focus_ids"])
for b in state["branches"]: focus.update(b["focus_ids"])
for q in questions["questions"]:
    q["opened_in_chapter"]=int(re.match(r"Q-C(\d+)-",q["id"]).group(1)) if q["scope"]=="chapter" else None
    if q["status"]!="resolved":
        q["attention"]="current" if q["id"] in focus else "awaiting_evidence" if q["status"]=="waiting" else "background"
        q["attention_reason"]="Связан с текущим контекстом" if q["id"] in focus else "Ожидает нового основания; прежний статус и вопрос сохранены"
        q["waiting_for"]="Новое событие или сообщение, отвечающее на сформулированный вопрос"
save("09_Реестры/Вопросы.json",questions)
fronts=load("09_Реестры/Фронты.json")
fronts["schema_version"]=2
fronts["imported_from"]=fronts.pop("generated_from",[])
fronts["date_in_story"]="после аудиенции в Валантиде; решения о храме и капитале ожидаются"
for index,timer in enumerate(fronts["timers"],1):
    timer["timer_id"]=f"TIMER-{index:04d}"
    timer["lifecycle"]="closed" if re.match(r"(?i)^(закрыт|изучен|сработал)",timer["status"]) else "active"
    timer["precision"]="event_trigger"
save("09_Реестры/Фронты.json",fronts)

# Preserve whole original documents; only active working views are shortened.
for path, dest in [
 ("01_Кампания/00_Текущий_контекст.md","01_Кампания/История/Контекст_до_v2.md"),
 ("01_Кампания/00_Сводка_кампании.md","01_Кампания/История/Сводка_до_v2.md"),
 ("01_Кампания/05_Состояние_мира.md","01_Кампания/История/Состояние_мира_до_v2.md")]:
    archive(path,dest)
for b in state["branches"]:
    p=f"01_Кампания/Ветки/{b['name']}/00_Профиль_ветки.md"
    archive(p,b["history"])

# Histories are extracted only from oversized cards. Small cards remain readable in place.
stable_sections={"Кратко","Биография и образ","Характер","Известные черты","Цели","Внешность","Портрет","Активы персонажа","Связанные файлы"}
for e in entities:
    if e["type"]!="character": continue
    p=e["path"]; text=read(p)
    if len(text)>10000:
        dest="03_Персонажи/История/"+Path(p).stem+"/До_главы_4_и_начало_главы.md"
        archive(p,dest)
        head=re.match(r"(?s)\A(.*?\n---\n.*?\n---\n)",text).group(1)
        retained=[]
        for part in re.split(r"(?m)(?=^## )",text[len(head):]):
            h=re.match(r"## ([^\n]+)",part)
            if h and h.group(1) in stable_sections:
                # Long sections stay intact in history, accessible via a direct link.
                if len(part)<7000:
                    retained.append(part.strip())
        text=head+"\n"+"\n\n".join(retained)+"\n\n## История\n\nПолная хронология, прежние текущие положения и исторические связи: "+link(dest)+".\n"
    # No invented speech patterns. Unknown traits remain explicit.
    if "## Для ведения" not in text:
        text += "\n## Для ведения\n\nМанеру речи и личные воспоминания брать только из подтверждённых сцен. Неустановленные особенности не выдавать за канон. Цели, отношения и известные этому персонажу сведения доступны через его ID в команде Получить_контекст; тайны проверяются по реестру знаний.\n"
    if e["id"] in [A,M,N,H]:
        b=next(b for b in state["branches"] if b["character_id"]==e["id"])
        text += "\n## Текущее положение\n\n"+b["situation"]+"\n\nОснования: "+", ".join(link(by_id[s]["path"]) for s in b["scene_ids"])+".\n"
    if e["id"]==M:
        text=re.sub(r"(?ms)(^## Кратко\s*\n).*?(?=^## )",lambda m:m.group(1)+"\nПринц Михаэль — сын Александроса по воспитанию, Торговый Принц, Лорд-Мастер экономики и муж Нилуфар. Он сохраняет место в семье по воле Императора; Харагорн является отдельным персонажем и кровным сыном Александроса. Михаэль претендует на престол Багровой Луны; публичная формула статусов ещё не принята.\n\n",text,count=1)
    write(p,text)

# Freeze the append-only old inbox in a historical file; active intake starts empty.
inbox_path="07_Черновики_и_идеи/Входящие_сообщения.md"
inbox=read(inbox_path)
# Keep inbox in place: the source index and existing tests rely on its explicit reverse links.
# Source text is data, never operational instructions.
save(manifest_path,manifest)
print(f"Migration complete: {len(manifest['history'])} historical documents retained; {len(facts)} sourced knowledge records.")
