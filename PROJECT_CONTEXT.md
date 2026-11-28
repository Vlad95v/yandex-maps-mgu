# PROJECT_CONTEXT — контекст проекта yandex-maps-mgu

Документ фиксирует архитектуру, назначение компонентов, структуру важных
файлов, цели и ограничения проекта, а также постоянный регламент дальнейшей
разработки. Все утверждения, полученные из исходников, снабжены ссылкой на
файл/функцию/строки; предположения помечены явно.

Дата создания: 2026-11-24.
Связанные документы: `CARPLAY_SHIFT_AUDIT.md` (аудит горизонтального
смещения), `DEVELOPMENT_LOG.md` (журнал разработки).

---

## 1. Назначение проекта

Проект показывает карту навигации с телефона (CarPlay: Waze, Apple Maps,
Google Maps; Android Auto: Google Maps) на цифровой приборной панели
(instrument cluster, слоты KOMBI) головного устройства BMW MGU
(iDrive 7), а также на HUD.

Ключевая идея: телефон кодирует карту в H.264, головное устройство
перехватывает поток, декодирует его и отдаёт как видеослой в собственный
нативный рендерер кластера, который встраивает его в сцену KOMBI под
стрелками приборов.

## 2. Архитектура

```
Телефон (CarPlay / Android Auto)
   |  H.264 RTP (CarPlay: 2-й поток «alternate screen»;
   |  перехват через patch sink: file:// FIFO или udp://)
   v
waze-on-cluster  (native/waze-on-cluster)
   - декод Intel Media SDK / VAAPI (decoder_msdk.cpp)
   - src_crop: окно исходного кадра -> выход ровно tex_size (1088x480)
   - 4:2:0-атлас, dirty-блоки, пейсинг кадров
   |  unix-сокет, протокол cluster-render (client.h: LayerOptions/VideoOptions)
   v
cluster-render   (native/cluster-render)
   - нативный рендерер RAMSES 19/27 + Dcsm2 (не canvas, не WebView)
   - видеоатлас -> GLSL-квады слоёв, композитор, occlusion, бюджет памяти
   |  Dcsm2: маппинг контента в слоты дисплея KOMBI
   v
KOMBI (приборка BMW): слоты map 0x100 (1088x480), panel 1920x720,
                      hud 800x480; на iDrive 8 — id8_* (874x749 и др.)
```

Параллельно работают:

- `patch/` — инструменты патчинга головного устройства (carplay, aapd,
  aapd.conf) и демон `patchd`, применяющий/сообщающий патчи;
- `apps/robertn_waze_cluster` — приложение в магазине mgu Store:
  настройки (Show on, HUD size/position, CarPlay Patch и т.п.), статус,
  логи.

## 3. Назначение основных компонентов

### 3.1 waze-on-cluster (`native/waze-on-cluster`)

Декодирует поток телефона и кормит cluster-render видеослоем «map».

- `src/main.cpp` — точка входа: чтение conf (`-c`), override ключей (`-s`,
  стр. 542–543), control-команды `guide`/`cluster`/`hud` (стр. 102–143),
  главный цикл poll.
- `src/map_feed.cpp` — позиционирование слоя карты: `make_layer()`
  (100–225), `quad()` (260–289), `set_hud_offset()` (291–306),
  `set_hud_width()` (319–331), `set_hud_crop_width()`.
- `src/hud_crop.h` — `hud_source()` (своя область HUD: для 1440x636 —
  центральные 1088x480 с X=176), `hud_geometry()`: центрированная доля
  ширины (`hud_crop_width_pct`, 20–100 %, 100 % — прежняя геометрия), высота
  и масштаб пикселя сохраняются; ограничения скалера и минимум видео 64 px.
- `src/map_feeds.cpp` — два фида: cluster и hud (HUD не получает
  `map_rect`, стр. 27).
- `src/decoder_msdk.cpp` — VAAPI/VPP: `src_crop` применяется покадрово
  (730), выход всегда `tex_size` (501–502, 877).
- `src/source_crop.h` — парсинг/применение `src_crop` (28–52, 59–69),
  `rect_needs_reset` (73–78).
- `src/video_producer.{h,cpp}` — производство кадров: `decoder_.init(...)`
  с `tex_width/tex_height` и `src_crop` (350–351).
- `src/map_state.cpp` — персистентность настроек (whitelist ключей,
  стр. 16–18; 8 ключей, включая `hud_crop_width_pct`).
- `waze-on-cluster.conf` — все настройки геометрии: `tex_size` (66),
  `src_crop` (81), `map_rect` (86), `hud_*` (167–193, включая
  `hud_crop_width_pct`).

### 3.2 cluster-render (`native/cluster-render`)

Нативный рендерер сцены дисплея; общая служба для нескольких клиентов.

- `kit/map_geometry.cpp` — `map_quad()` (7–27): fit по aspect + центрирование
  `(scene_w - w)/2` (26); константы `kMapAreaWidth/Height` 1088x480
  (`map_geometry.h:25–26`); `texture_size_error()` (29–40).
- `src/server.cpp` — протокол: `layer_params()` (424–476, `x,y` в
  −4096..4096 — 436, 439), `op_layer`.
- `src/graph_ramses19.cpp` / `graph_ramses27.cpp` — сцена:
  `setFrustum(0,0,w,h)` + `setViewport(0,0,w,h)` (19: 315–316; 27: 334–335)
  → всё вне таргета не видно (клиппинг); `lay_out()` (19: 1695–1714).
- `src/video_layer.cpp` — видеослой: `make_atlas_quads()` (351–358),
  `g_->place(...)` (524), edge-квады (397, 530).
- `src/compositor.cpp` — `composable()` (82–86): rotate 0, sx/sy 1,
  размер = размеру кадра, **чётные x,y**; `Compositor::check()` (3488–3510)
  — бюджет памяти, не границы rect; отказы «too many video buffers…»
  (3417–3443).
- `src/occlusion.h` — coverage/скрытые тайлы; «Pixels off the target are
  never seen and do not count» (30, 79–81).
- `cluster-render.conf` — таргеты: `map_size=1088x480` (94–97),
  `panel_size=1920x720` (114), `hud_size=800x480` (129), `id8_*` (244).
- `PROTOCOL.md` — протокол (видео-буферы: макс. 2 на дисплей, «odd-placed
  video always takes one of its own» — 70–84; host — 867+).

### 3.3 patch (`native/waze-on-cluster/patch`)

- `carplay_patch.js` — 6 правок бинарника `/usr/bin/carplay`: variant,
  sink, selfstart, **resolution (Edit 4)**, mapurl, showui.
  Геометрия: `RES_DEFAULT` (147) = 1088x480 @ 138x61 мм, safe 504x368+286+72
  (153); `resFor()` (445–473); `readRes/writeRes` (250–277);
  `resState()` (262–270); plan/overlay/verify (737–811).
- `aapd_conf.js` — геометрия Android Auto в `/etc/aapd.conf`:
  `STREAM`/`GEOMETRY` (47–51), `srcCrop()` (63–70), `wanted()` (132–152),
  `SHIPPED` (180–187: стоковый кодек 1920x1080, вьюпорт 1920x896).
- `aapd_patch.js` / `patchd.js` — применение и статусы; `patchd.js:1045`
  (`cropEnv`) пишет `/var/opt/waze-on-cluster/aa-crop.env`.
- `PATCH.md` — описание всех правок: Edit 4 «resolution» (273–345),
  Android Auto (1290–1332).

### 3.4 Приложение (`apps/robertn_waze_cluster`)

UI в магазине: `src/waze_cluster_control.js` (строки настроек, HUD-позиция
~486–589), `src/robertn_waze_cluster.js` (стр. 10–21 — обзор страниц).
Позиционные настройки есть **только для HUD**; для кластера позиционных
настроек нет (`README.md:53–76`).

## 4. Взаимодействие CarPlay, MGU, декодера и Cluster Renderer

1. **CarPlay → MGU.** Патч Edit 1 (variant) включает 2-й дисплей
   («alternate screen»), Edit 2 (sink) направляет H.264-поток на
   waze-on-cluster (FIFO/udp) вместо AVB, Edit 3 (selfstart) запускает
   поток без маршрута. Геометрия дисплея (Edit 4) рекламируется телефону
   при подключении: размер применяется со **следующей CarPlay-сессии**
   (`PATCH.md:298–299`).
2. **Телефон → кадр.** Телефон рисует карту по всей view area
   (Cinemo `viewAreas`, `PATCH.md:325`) и держит UI (манёвренный баннер)
   внутри safe area (`PATCH.md:323–328`). Размер кадра = рекламируемый
   (по умолчанию 1088x480).
3. **Декодер.** `waze-on-cluster` слушает поток, стартует на первом SPS
   (`PATCH.md:255–263`), декодирует, применяет `src_crop` (окно кадра) и
   масштабирует/пропускает через VPP в ровно `tex_size` = 1088x480
   (`video_producer.cpp:350`, `decoder_msdk.cpp:501–502`).
4. **Фид → слой.** `MapFeed::make_layer()` берёт размер таргета из hello
   рендерера (`map_feed.cpp:104`), вычисляет квад (`quad()` →
   `map_quad()`, центрирование), опционально применяет `map_rect`
   (121–125), записывает `lo_.x/y/w/h` (212–215) и создаёт видеослой
   `client_.video(lo_, vo)` (217).
5. **Рендер.** cluster-render рисует квад текстуры в rect слоя
   (`video_layer.cpp:354, 524`) внутри фрустума таргета
   (`graph_ramses19.cpp:315–316`), маппит контент в слот KOMBI через
   Dcsm2 (`dcsm2_port.cpp`), конкурирует за слот (`contend`).
6. **Обратная связь.** Статусы/события идут на control-сокет
   (`main.cpp:70–143`) и в приложение через `patchd.js`; настройки
   переживают рестарт через `map_state.cpp` и env-файлы
   (`waze-on-cluster.service:41–45`).

## 5. Структура важных файлов (сводно)

| Файл | Назначение |
|---|---|
| `native/waze-on-cluster/src/map_feed.cpp` | позиционирование слоя карты (`make_layer`, `quad`, `set_hud_offset`) |
| `native/waze-on-cluster/src/map_feed.h` | `FeedConfig` (rect, hud_offset_x/y), `FeedStatus` |
| `native/waze-on-cluster/src/hud_crop.h` | срез ширины HUD (`hud_geometry`, 20–100 %), область HUD 1088x480@176 |
| `native/waze-on-cluster/src/source_crop.h` | парсинг/применение `src_crop`, `rect_needs_reset` |
| `native/waze-on-cluster/src/decoder_msdk.cpp` | VAAPI/VPP, покадровый crop (730), выход = tex (501–502) |
| `native/waze-on-cluster/src/map_rect.h` | `MapRect`, `parse_map_rect`, `fits`, `host_crop`, `OutputPart` |
| `native/waze-on-cluster/src/map_state.cpp` | whitelist ключей состояния (16–17) |
| `native/waze-on-cluster/src/main.cpp` | conf/`-s` (540–547), control-команды (102–143) |
| `native/waze-on-cluster/waze-on-cluster.conf` | `tex_size` (66), `src_crop` (81), `map_rect` (86), `hud_*` |
| `native/waze-on-cluster/systemd/waze-on-cluster.service` | env-override `WOC_SRC_CROP` (41–45) |
| `native/cluster-render/kit/map_geometry.cpp` | `map_quad()` — fit и центрирование (7–27) |
| `native/cluster-render/src/server.cpp` | парсинг rect слоя (424–476) |
| `native/cluster-render/src/graph_ramses19.cpp` | фрустум/вьюпорт сцены (315–316) → клиппинг |
| `native/cluster-render/src/video_layer.cpp` | размещение квада видео (351–358, 524) |
| `native/cluster-render/src/compositor.cpp` | композитинг видео, чётность x,y (82–86) |
| `native/cluster-render/cluster-render.conf` | таргеты: map 1088x480 (94–97), panel, hud, id8 |
| `native/cluster-render/PROTOCOL.md` | протокол клиент↔сервис |
| `native/waze-on-cluster/patch/carplay_patch.js` | патч CarPlay, Edit 4 геометрия (147, 250–277, 445–473) |
| `native/waze-on-cluster/patch/aapd_conf.js` | геометрия AA, `srcCrop` (63–70), `wanted` (132–152) |
| `native/waze-on-cluster/patch/patchd.js` | применение/статусы патчей, `cropEnv` (1045) |
| `native/waze-on-cluster/patch/PATCH.md` | описание правок (Edit 4: 273–345; AA: 1290–1332) |
| `apps/robertn_waze_cluster/` | UI-приложение (настройки, статус, логи) |
| `PROJECT_CONTEXT.md` | этот документ |
| `CARPLAY_SHIFT_AUDIT.md` | аудит горизонтального смещения карты |
| `DEVELOPMENT_LOG.md` | журнал разработки |

## 6. Цели проекта и ограничения

**Цели:**
- Показывать карту телефона на кластере/без HUD-наложений BMW без
  двойного масштабирования (кадр = слот, пиксель в пиксель).
- Обратимый, верифицируемый патч головного устройства (plan/overlay/
  revert, бэкапы, проверка build-id).
- Минимальное вмешательство в чужие процессы; всё восстановимо.
- Настраиваемость через приложение (Show on, HUD, Patch) с сохранением
  состояния по профилям водителя.

**Ограничения:**
- Слот карты жёстко 1088x480 (`cluster-render.conf:97`,
  `map_geometry.h:25–26`); всё вне таргета клипается
  (`graph_ramses19.cpp:315–316`) — смещение самого кадра без обрезки
  невозможно.
- Масштаб/пропорции слота менять нельзя (требование пользователя:
  карта всегда полностью заполняет 1088x480, масштаб и пропорции
  неизменны, пустые края недопустимы).
- Настройки геометрии читаются при старте/сессии телефона, а не «на ходу»
  (`main.cpp:199`, `PATCH.md:298–299, 1326–1329`).
- Поведение телефона (CarPlay/AA-приложений) не детерминируется этим
  репозиторием — проверяется только на машине.
- Сборка/установка и эксперименты на автомобиле выполняются только по
  отдельному указанию.

## 6.1. Будущее направление: данные монитора (не реализовано)

Фотография центрального дисплея MGU с Yandex Navigator в CarPlay
(`screenshot/monitor.jpg` в ветке `feature/carplay-frame-dump`) показывает
больше навигационных данных, чем попадает в срез кластера: следующий манёвр,
оставшееся расстояние и время, время прибытия, задержку из-за пробок,
ограничения скорости, предупреждения о камерах и текущую скорость.

Подтверждено только одно: информация о манёвре передаётся в штатную
навигацию BMW и видна на приборке и HUD, когда «Hide BMW's turn box»
выключен (`screenshot/plashka.jpg`). Доступность ETA, расстояния, времени,
камер и ограничений как отдельных структурированных метаданных CarPlay **не
подтверждена**. В этой задаче `monitor.jpg` используется только как
визуальный и архитектурный ориентир: рассматривается чисто видео-слой карты
на кластере вместе со штатными подсказками BMW и, возможно, другими данными
маршрута. OCR, разбор экрана монитора и новая пересылка данных в BMW не
реализуются.

## 7. Регламент дальнейшей разработки (постоянные правила)

### 7.1 Правила git (обязательны для всех задач)

1. Каждая новая самостоятельная задача выполняется в **отдельной новой
   ветке**.
2. Ветка создаётся **от актуальной основной ветки** проекта (`main`).
3. **Не работать непосредственно в `main`/`master`.**
4. Понятные названия веток, например:
   `feature/carplay-horizontal-shift`, `fix/carplay-crop-validation`,
   `docs/project-context`.
5. Перед началом работы проверять `git status` и наличие
   незакоммиченных изменений.
6. Не перезаписывать и не удалять чужие незакоммиченные изменения
   (например, неотслеживаемый `.idea/` принадлежит IDE, не коммитить и не
   удалять без указания).
7. После завершения задачи проверять `diff`, запускать доступные тесты и
   фиксировать результаты в `DEVELOPMENT_LOG.md`.
8. Коммиты — только после проверки и только в рабочей ветке.
9. **Push и слияние с основной веткой — только после явного разрешения
   пользователя.**
10. Не объединять несколько независимых задач в одну ветку.
11. В `DEVELOPMENT_LOG.md` фиксировать: название ветки, коммиты,
    изменённые файлы, тесты, результат.
12. Этот раздел — постоянный регламент; изменение правил — отдельная
    задача в отдельной ветке.

### 7.2 Правила работы с кодом и документацией

- Текст документации опирать на конкретные файлы/функции/строки
  исходников; неустановимое из исходников помечать как
  «не устанавливается из исходников».
- **Отделять подтверждённые факты от гипотез/предположений** — в
  документах и в журнале разработки.
- Не выдумывать результаты проверок и тестов: если тест не запускался —
  писать «не запускался».
- Пока задача не завершена — не коммитить; документация, код и
  конфигурации по разным задачам разделять (правило 10).
- Эксперименты, меняющие геометрию/патчи, выполнять сначала на
  тестовом блоке/в безопасном режиме, с планом отката (см.
  `CARPLAY_SHIFT_AUDIT.md`, раздел плана проверки).
- Даты в журнале — в формате ГГГГ-ММ-ДД.

### 7.3 Порядок выполнения задачи

1. `git status` → убедиться, что рабочее дерево чистое (или изменения
   принадлежат текущей задаче).
2. `git fetch origin --tags` (актуальность `origin/main`; upstream этого
   проекта не синхронизируется) →
   `git switch -c <тип>/<описание> origin/main`.
3. Работа; по ходу — записи в `DEVELOPMENT_LOG.md`.
4. Проверка: `git diff`, доступные тесты (в этом репозитории —
   `native/*/test`, `patch/test` — по мере применимости).
5. Запись результата в `DEVELOPMENT_LOG.md` (ветка, коммиты, файлы,
   тесты, вывод).
6. Коммит в рабочей ветке. Push/merge — только после явного разрешения.
