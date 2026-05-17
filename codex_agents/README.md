# Codex Scene Agents

---
type: codex_scene_agents_index
status: active
canon_level: support
updated_real_date: 2026-05-17
---

Эта папка описывает оркестратор и три специализированных агента для генерации сцен через Codex.

Используй их, когда нужно разделить работу на этапы:

1. сбор лора и ссылок на изображения;
2. написание промпта сцены;
3. генерация изображения по утвержденному промпту.

## Файлы

```text
lore_api/codex_agents/ORCHESTRATOR.md
lore_api/codex_agents/01_LORE_AND_REFERENCES_AGENT.md
lore_api/codex_agents/02_SCENE_PROMPT_AGENT.md
lore_api/codex_agents/03_IMAGE_GENERATION_AGENT.md
```

## Как запускать

В Codex можно написать:

```text
Используй lore_api/codex_agents/ORCHESTRATOR.md.

scene_name: Совет_в_Белой_Гавани

Сцена:
Принц Михаэль и Капитан Картос стоят над картой Белой Гавани перед ночным штурмом.
```

Оркестратор сам должен провести работу по этапам и не переходить к генерации до слова "Генерь".
