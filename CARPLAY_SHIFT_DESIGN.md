# CARPLAY_SHIFT_DESIGN: Independent Horizontal CarPlay Map Shift on BMW MGU Cluster

**Document Version:** 1.0  
**Date:** 2024-11-24  
**Status:** Design Analysis — Read-Only Investigation  

---

## 1. Цель и требования

### 1.1 Задача
Рассмотреть возможность независимого горизонтального смещения стрелки автомобиля (и карты) относительно карты Apple CarPlay на цифровой приборной панели (KOMBI) BMW iDrive 7 (MGU), при условии:
- Виртуальное разрешение CarPlay: **1404×480** (вместо текущих 1088×480)
- Физический размер дисплея: **178×61 мм** (вместо 138×61 мм)
- Декодер **всё равно выдаёт 1088×480** (tex_size неизменен)
- `src_crop` настроен на **1088×480** с возможностью горизонтального перемещения окна кропа

### 1.2 Требования
1. Стрелка автомобиля (центр карты) должна смещаться влево/вправо независимо от границ экрана
2. Карта CarPlay должна рендериться на телефоне с большим виртуальным разрешением
3. На кластере видимая область остаётся 1088×480 (через `src_crop`)
4. Нет изменения кода декодера/рендера — только конфигурация и/или патчи

---

## 2. Фактическая архитектура CarPlay на MGU

### 2.1 Общий путь данных
```
iPhone (CarPlay) 
    │
    ▼ (H.264 RTP stream, alternate screen)
CarPlay binary (patched by carplay_patch.js Edit 4)
    │
    ▼ (advertised geometry: 1088×480 px @ 138×61 mm)
waze-on-cluster capture (AVB/UDP/FIFO)
    │
    ▼ (H.264 Annex-B)
MSDK/VAAPI Decoder (decoder_msdk.cpp)
    │
    ├── VPP: crop (src_crop) → scale to tex_size (1088×480)
    │
    ▼ (YUV 4:2:0 atlas, 64×64 tiles)
VideoProducer → MapFeed
    │
    ▼ (quad positioning)
cluster-render (RAMSES 19/27)
    │
    ▼ (DCSM slot 0x100, scene 1088×480)
KOMBI display
```

### 2.2 Ключевые параметры в carplay_patch.js (Edit 4 — resolution)

| Поле | Stock | Patched (Default) | Что меняет |
|------|-------|-------------------|------------|
| **Physical size (mm)** | 234×110 | **138×61** | Физический размер дисплея, сообщаемый iPhone |
| **Pixel size (px)** | 1920×900 | **1088×480** | Разрешение, в котором iPhone рендерит карту |
| **View area** | (0,0) 1920×900 | (0,0) 1088×480 | Область просмотра = весь кадр |
| **Safe area** | 504×456 @ (708,153) | **504×368 @ (286,72)** | Где iPhone держит UI (маневровый баннер) |

**Критический вывод:** Физический размер (138×61 мм) **зашит в бинарнике CarPlay** через 4 `movabs` immediates. Изменение `--res` меняет только пиксельное разрешение, **НЕ** физический размер.

### 2.3 Safe Area и поведение iPhone
- **Safe area** = зона, где iPhone размещает навигационный UI (маневровый баннер, ETA)
- **Default safe area:** 504×368 px at (286, 72) в координатах 1088×480
- iPhone **масштабирует карту** так, чтобы safe area попала в физические границы
- При изменении `--res` safe area **масштабируется пропорционально** (см. `resFor()` в carplay_patch.js:445-473)

---

## 3. Путь входного видеопотока

### 3.1 Источники захвата (VideoProducer)
- **AVB mode:** `media_transport.avb_streaming.15` (stock media-bus)
- **FIFO mode:** `/run/waze-on-cluster/alt.h264` (carplay writes file:// FIFO)
- **UDP mode:** `udp://127.0.0.1:20557`

Все источники конвергируют в **RTP depacketiser → MSDK decoder**.

### 3.2 Формат потока CarPlay
- **Разрешение:** 1088×480 (как анонсировано патчем Edit 4)
- **Кодек:** H.264 Baseline/Main, 4:2:0
- **Keyframes:** Только при старте стрима (SPS/PPS/IDR), далее только P-слайсы
- **FPS:** Вариабельно (зависит от телефона/навигатора), целевое `target_fps = 30`

### 3.3 Декодер (decoder_msdk.cpp)
```cpp
// State::start() — настройка VPP
mfxVideoParam vpp = {};
vpp.vpp.In.CropW = src_crop.w;  // 1088 (из src_crop)
vpp.vpp.In.CropH = src_crop.h;  // 480
vpp.vpp.Out.CropW = tex_width;  // 1088 (tex_size)
vpp.vpp.Out.CropH = tex_height; // 480
// VPP делает: crop → scale (1:1 в текущей конфигурации)
```

**Важно:** Выход декодера **всегда** `tex_size` (1088×480). `src_crop` применяется **на входе VPP** как crop-окно внутри исходного кадра.

---

## 4. Работа VPP и src_crop

### 4.1 SourceCrop структура (source_crop.h:19-25)
```cpp
struct SourceCrop {
    uint32_t seq_w = 0, seq_h = 0;  // 0 = no crop
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool enabled() const { return seq_w != 0; }
    bool applies(uint32_t sw, uint32_t sh) const { 
        return enabled() && sw == seq_w && sh == seq_h; 
    }
};
```

### 4.2 Формат конфига: `SWxSH:X,Y,W,H`
- **SW×SH** — размер исходного кадра, к которому применяется кроп
- **X,Y,W,H** — прямоугольник кропа (все числа чётные, 4:2:0)
- Минимум 16×16, внутри кадра

### 4.3 Применение на кадре (crop_picture, source_crop.h:59-69)
```cpp
// Вызывается ПЕР КАДРУ в decoder_msdk.cpp:730
if (crop_picture(src_crop, &x, &y, &w, &h)) {
    // x,y смещены на src_crop.x, src_crop.y
    // w,h заменены на src_crop.w, src_crop.h
}
```

### 4.4 Переключение VPP (rect_needs_reset, reset_vpp)
- Если `src_crop` меняет входной прямоугольник VPP — вызывается `reset_vpp()`
- `RectRestarts` класс защищает от лишних перезапусков декодера
- **Ограничение:** `src_crop` должен лежать внутри исходного кадра (SW×SH)

### 4.5 Текущая конфигурация (waze-on-cluster.conf:81)
```
src_crop = 1280x720:0,0,1280,564  # Android Auto default
```
Для CarPlay (уже 1088×480) `src_crop` **не применяется** — кадр проходит целиком.

---

## 5. Путь изображения до кластера

### 5.1 MapFeed — создание слоя (map_feed.cpp:100-225)
```cpp
// make_layer():
// 1. Получает target_size из crk::map_area_size(target) 
//    → cluster: 1088×480, HUD: 800×480
// 2. Вызывает crk::map_quad(tex_size, target_size, ...)
// 3. Создаёт video layer через client_.create_layer()
```

### 5.2 cluster-render: map_geometry.h/cpp — позиционирование квада

**Map Area Constants (map_geometry.h:25-26):**
```cpp
constexpr uint32_t kMapAreaWidth = 1088;
constexpr uint32_t kMapAreaHeight = 480;
```

**map_quad() (map_geometry.h:7-27):**
```cpp
// Для кластера (target = panel или cluster):
// - map_area = 1088×480 (центр сцены)
// - Текстура (tex_size) центрируется в map_area
// - fit_mode: сохраняет aspect ratio текстуры
```

**Критический момент:** Для кластера **нет offset-параметров**. Квад **всегда центрирован** в map_area (1088×480).

### 5.3 HUD — для сравнения (map_feed.cpp:281-306)
```cpp
// HUD ИМЕЕТ offset:
hud_offset_x = 0  // -800..800, в пикселях HUD сцены (800×480)
hud_offset_y = 0
// Применяется в quad():
quad.tx += hud_offset_x;
quad.ty += hud_offset_y;
// С клампингом внутри сцены
```

**Вывод:** Архитектура **уже поддерживает** смещение карты — но только для HUD. Для кластера этого нет.

### 5.4 RAMSES 19/27 — frustum culling (graph_ramses19.cpp:315-316)
```cpp
// Viewport clipping происходит на уровне RAMSES
// Если квад выходит за границы сцены — он обрезается
```

---

## 6. Сравнение вариантов A/B/C

### Вариант A: Extended Resolution + src_crop Shift
**Идея:** Патчить CarPlay на 1404×480, использовать `src_crop=1404x480:X,0,1088,480` с движущимся X.

| Критерий | Оценка |
|----------|--------|
| **Изменение кода** | Только carplay_patch.js (--res 1404x480) + config src_crop |
| **Масштаб карты на iPhone** | ⚠️ **ИЗМЕНИТСЯ** — iPhone рендерит в 1404×480 вместо 1088×480. Физический размер 138×61 мм фиксирован → **плотность пикселей вырастет 1.29×**. Карта станет "ближе" (больше деталей). |
| **Safe area** | Масштабируется пропорционально (504×368 → ~650×475). Манипуляция safe area может компенсировать. |
| **src_crop окно** | Может двигаться X=0..316 (1404-1088). Соответствует ±158 px смещению центра. |
| **Декодер/VPP** | Без изменений. VPP делает crop(1088×480) → scale(1088×480). |
| **cluster-render** | Без изменений. Видит 1088×480 текстуру. |
| **Риск UI iPhone** | **ВЫСОКИЙ** — кнопки, баннеры, текст станут меньше/плотнее. Может сломать UX. |
| **Тестирование** | Требует реального iPhone + CarPlay сессии. |

### Вариант B: cluster_offset_x/y (Quad Shift) — Аналог HUD
**Идея:** Добавить в cluster-render/map_feed поддержку `cluster_offset_x/y`, как `hud_offset_x/y`.

| Критерий | Оценка |
|----------|--------|
| **Изменение кода** | map_feed.cpp (quad + set_cluster_offset), map_geometry.h, waze-on-cluster.conf, control protocol |
| **Масштаб карты на iPhone** | **НЕ ИЗМЕНЯЕТСЯ** — iPhone всё так же рендерит 1088×480. |
| **Safe area** | Без изменений. |
| **Смещение** | Неограниченно в пределах сцены (кроме frustum culling). |
| **Декодер/VPP** | Без изменений. |
| **cluster-render** | Минимальные изменения — добавить offset в quad вычисление. |
| **Риск UI iPhone** | **НУЛЕВОЙ** — iPhone не знает о смещении. |
| **Тестирование** | Только кластер, iPhone не задействован. |
| **Архитектурная чистота** | **ЛУЧШАЯ** — разделение ответственности: телефон рендерит, кластер позиционирует. |

### Вариант C: Другие подходы

| Подход | Оценка |
|--------|--------|
| **VPP output crop** | MSDK VPP не поддерживает crop на выходе — только scale. |
| **RAMSES layer transform** | Сложнее, требует изменений в cluster-render core. |
| **map_rect config** | Существует (`map_rect=` в conf), но растягивает текстуру, не смещает. Не подходит. |
| **Два декодера** | Оверхед, избыточно. |

---

## 7. Анализ масштаба и поведения интерфейса iPhone

### 7.1 Как iPhone определяет масштаб карты
1. CarPlay анонсирует: **Physical Size (mm)** + **Pixel Size (px)** + **Safe Area (px)**
2. iPhone вычисляет: **DPI = px / mm**
3. iPhone рендерит карту так, чтобы **Safe Area** физически попала в заданные мм
4. Масштаб карты = функция от DPI и Safe Area

### 7.2 Вариант A (1404×480 @ 138×61 mm)
```
Stock DPI:     1088/138 = 7.88 px/mm  (horizontal)
New DPI:       1404/138 = 10.17 px/mm  (+29%)
```
**Результат:** Карта на телефоне станет **на 29% "увеличенной"** (больше деталей на тот же физический размер). Safe area пропорционально вырастет в пикселях.

**Риски:**
- Манипулятор/баннеры станут мельче физически
- Текст станет труднее читать
- Навигация может "ломаться" (непонятно, как Apple Maps/Google Maps/Waze реагируют на нестандартный DPI)

### 7.3 Вариант B (1088×480 @ 138×61 mm + cluster_offset)
```
DPI: 7.88 px/mm — НЕ МЕНЯЕТСЯ
```
**Результат:** iPhone рендерит **идентично текущему**. Смещение происходит на кластере пост-фактум.

---

## 8. Технические риски

| Риск | Вариант A | Вариант B |
|------|-----------|-----------|
| **Сломанный UX на iPhone** | ВЫСОКИЙ (DPI change) | НЕТ |
| **Нестабильность CarPlay** | СРЕДНИЙ (нестандартное разрешение) | НИЗКИЙ |
| **Frustum culling на кластере** | НЕТ (текстура 1088×480) | ЕСТЬ (квад может выйти за сцену) |
| **Патчинг carplay binary** | ОБЯЗАТЕЛЬЕН (--res 1404x480) | НЕ НУЖЕН |
| **Изменение cluster-render** | НЕТ | МИНИМАЛЬНОЕ |
| **Совместимость с HUD** | НЕ ЗАТРАГИВАЕТ | НУЖНО ПРОВЕРИТЬ независимость offset |
| **Rollback сложность** | Перезапуск CarPlay | Перезапуск waze-on-cluster |

### 8.1 Frustum Culling (RAMSES) — детально для Варианта B
- Сцена кластера: 1920×720 (panel) или 1088×480 (map slot 0x100)
- Map slot 0x100: сцена ровно 1088×480, карта заполняет её всю
- При смещении квада: **часть текстуры уйдёт за экран** → RAMSES обрежет
- **Решение:** Использовать panel slot (0x203, 1920×720) где есть запас по горизонтали
- ИЛИ: расширить map slot (требует cluster-render изменений)

---

## 9. Рекомендуемая архитектура: ВАРИАНТ B

### 9.1 Почему Вариант B
1. **Безопасность:** iPhone не задействован — нет риска сломать CarPlay UX
2. **Минимальные изменения:** Только cluster-render + map_feed + config
3. **Проверенная модель:** HUD уже делает то же самое (`hud_offset_x/y`)
4. **Откат:** Просто `cluster_offset_x=0` — мгновенно, без перезапуска CarPlay
5. **Архитектурно правильно:** Разделение — телефон рендерит, кластер композитит

### 9.2 Необходимые изменения (read-only анализ — НЕ ИМПЛЕМЕНТАЦИЯ)

**Файлы для модификации:**
1. `native/cluster-render/kit/map_geometry.h/cpp` — добавить `cluster_offset_x/y` в `map_quad()`
2. `native/waze-on-cluster/src/map_feed.cpp` — передавать offset в `map_quad()`, добавить `set_cluster_offset()`
3. `native/waze-on-cluster/waze-on-cluster.conf` — добавить `cluster_offset_x`, `cluster_offset_y`
4. Control protocol — добавить `{"cmd":"cluster","cluster_x":...,"cluster_y":...}`

**Логика quad() для кластера:**
```cpp
// В map_geometry.h — новый overload или параметры
Quad map_quad(TextureSize tex, TargetSize target, int cluster_offset_x = 0, int cluster_offset_y = 0) {
    Quad q = centered_quad(tex, target);
    q.tx += cluster_offset_x;
    q.ty += cluster_offset_y;
    // Clamping к границам сцены (как HUD)
    return q;
}
```

---

## 10. План минимального эксперимента на автомобиле

### 10.1 Подготовка (без изменений кода)
1. **Текущая конфигурация:** Проверить, что `src_crop` можно задать динамически через control socket
2. **HUD offset test:** Включить HUD, проверить `hud_offset_x` — убедиться, что смещение работает плавно
3. **Map slot探索:** Проверить, можно ли переключить карту на panel slot (0x203) вместо map slot (0x100)

### 10.2 Эксперимент Варианта A (только если нужно проверить DPI)
1. Применить `--res 1404x480` через carplay_patch.js
2. Перезапустить CarPlay (patchd activation)
3. Настроить `src_crop=1404x480:0,0,1088,480`
4. **Наблюдать:** масштаб карты на iPhone, читаемость UI, позиция баннеров
5. **Двигать src_crop X** через config reload — проверить следование стрелки

### 10.3 Эксперимент Варианта B (рекомендуемый первый)
1. Реализовать `cluster_offset_x` в map_feed (минимальный патч)
2. Запустить на авто, двигать offset через control socket
3. Проверить: frustum culling, плавность, взаимодействие с HUD
4. Определить рабочий диапазон offset (±пиксели)

---

## 11. План отката

| Сценарий | Вариант A | Вариант B |
|----------|-----------|-----------|
| **Мгновенный откат** | `src_crop=1088x480:0,0,1088,480` (config reload) | `cluster_offset_x=0` (control command) |
| **Полный откат** | `patchd revert` + CarPlay restart | Перезапуск waze-on-cluster service |
| **Время отката** | ~30 сек (CarPlay restart) | ~1 сек (service restart) или мгновенно (control) |
| **Риск зависания** | CarPlay может не подняться | Только map layer |

---

## 12. Список неподтверждённых гипотез

| # | Гипотеза | Как проверить |
|---|----------|---------------|
| 1 | iPhone масштабирует карту пропорционально DPI при `--res 1404x480` | Реальный тест на авто с патчем |
| 2 | Safe area масштабируется линейно в `resFor()` | Код carplay_patch.js:445-473 подтверждает, но нужен тест |
| 3 | Frustum culling в map slot 0x100 обрежет смещённый квад | Тест с HUD offset на кластере (если возможно) |
| 4 | Panel slot 0x203 доступен для map layer | Проверить cluster-render DCSM slots |
| 5 | `src_crop` можно менять динамически без перезапуска декодера | decoder_msdk.cpp:745 `rect_needs_reset` → `reset_vpp()` — есть логика, но нужен тест |
| 6 | Apple Maps / Google Maps / Waze одинаково реагируют на DPI change | Тест каждого навигатора |
| 7 | Cluster offset не конфликтует с `hide_bmw_guidance` | Тест с включённым/выключенным |

---

## 13. Вывод: какой вариант рекомендуется и почему

### 🏆 РЕКОМЕНДАЦИЯ: **ВАРИАНТ B (cluster_offset_x/y)**

**Обоснование:**
1. **Архитектурная правильность** — смещение viewпорта — задача композиттора (кластера), не телефона
2. **Нулевой риск для CarPlay/iPhone** — телефон продолжает рендерить 1088×480 @ 138×61 мм
3. **Минимальные изменения** — ~50 строк в map_feed.cpp + map_geometry.h + config
4. **Проверенная парадигма** — HUD offset уже работает годами
5. **Мгновенный откат** — control command, не требует CarPlay restart
6. **Гибкость** — offset можно анимировать, привязать к маневру, плавно двигать

### ⚠️ Вариант A — ТОЛЬКО если:
- Требуется **видеть больше карты** по бокам (peripheral vision) — но это меняет масштаб
- Принят риск нестандартного DPI для CarPlay
- Есть ресурсы на тестирование всех навигаторов

### 📋 Следующие шаги (для реализации):
1. Прототип `cluster_offset_x` в map_feed.cpp (1-2 дня)
2. Тест на авто: диапазон offset, frustum culling, взаимодействие с HUD
3. Если frustum culling в slot 0x100 — переключить на panel slot 0x203
4. Добавить в config + control protocol + app UI
5. Документировать в CHANGELOG

---

## Приложение: Справочные значения

### CarPlay Patch Geometry (carplay_patch.js)
```javascript
// RES_DEFAULT (line 147)
{w: 1088, h: 480, mmw: 138, mmh: 61}

// SAFE_DEFAULT (line 153)  
{x: 286, y: 72, w: 504, h: 368}

// --res 1404x480 вычислит (resFor):
// px: 1404×480, mm: 138×61 (fixed)
// safe: scaled proportionally ~650×475
```

### Config Keys (waze-on-cluster.conf)
```
tex_size = 1088x480          # Decoder output (fixed)
src_crop = 1404x480:X,0,1088,480  # Variant A only
cluster_offset_x = 0         # Variant B (NEW)
cluster_offset_y = 0         # Variant B (NEW)
hud_offset_x = 0             # Existing, works
hud_offset_y = 0             # Existing, works
```

### Map Slots (cluster-render)
```
0x100 — Central map (1088×480) — CURRENT
0x203 — Full panel (1920×720)  — ALTERNATIVE for offset
0x323 — HUD (800×480)          — Has offset support
```

---

*End of CARPLAY_SHIFT_DESIGN.md*