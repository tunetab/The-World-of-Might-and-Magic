from pathlib import Path
import re,json
r=Path(__file__).resolve().parents[2]
graph=json.loads((r/"09_Реестры/Сущности.json").read_text(encoding="utf-8-sig"))
manifest=json.loads((r/"10_Обслуживание/Миграция_v2.json").read_text(encoding="utf-8-sig"))
def words(path):
 return len(re.findall(r"\S+",(r/path).read_text(encoding="utf-8-sig")))
metrics={
 "entity_ids":len(graph["entities"]),
 "relationships":len(graph["edges"]),
 "preserved_histories":len(manifest["history"]),
 "protected_closed_documents":len(manifest["protected_files"]),
 "old_startup_words":25504,
 "new_startup_words_excluding_agents":sum(words(p) for p in ["00_Инструкции_для_ИИ/01_Ведение_игры.md","01_Кампания/00_Текущий_контекст.md"]),
 "agents_words":words("AGENTS.md"),
 "branch_packets":{p.stem:words(p.relative_to(r)) for p in (r/"01_Кампания/Контекст").glob("*.md")},
 "facts":len(json.loads((r/"09_Реестры/Знания.json").read_text(encoding="utf-8-sig"))["facts"]),
 "unknown_source_links":sum(e["provenance"]=="unresolved" for e in graph["entities"]),
}
(r/"10_Обслуживание/Метрики_v2.json").write_text(json.dumps(metrics,ensure_ascii=False,indent=2)+"\n",encoding="utf-8",newline="\n")
print(json.dumps(metrics,ensure_ascii=False,indent=2))
