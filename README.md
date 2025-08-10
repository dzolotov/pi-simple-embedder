# Flutter X11 Custom Embedder

Кастомный X11 эмбеддер для Flutter, предназначенный для запуска Flutter приложений на Raspberry Pi через XWayland.

## Описание

Этот эмбеддер реализует собственную интеграцию Flutter с X11 Window System, обеспечивая:
- Правильное сопоставление EGL visual с X11 visual
- Многопоточное управление OpenGL контекстами  
- Полную поддержку входных событий (мышь, клавиатура)
- Интеграцию с Flutter vsync callback
- Гибкость настройки через параметры командной строки

## Архитектура

### Основные компоненты

1. **X11 Window Management** - создание окна с корректным visual
2. **EGL Context Management** - управление основным и ресурсным контекстами
3. **Flutter Integration** - реализация Flutter Embedder API
4. **Event Handling** - обработка событий мыши и клавиатуры
5. **GL Function Resolution** - разрешение OpenGL функций

### Структура файлов

- `x11_embedder.cc` - основная реализация (546 строк)
- `CMakeLists.txt` - конфигурация сборки
- `build/` - директория с собранными бинарниками

## Зависимости

### Системные библиотеки
```bash
sudo apt install -y \
    libx11-dev \
    libegl1-mesa-dev \
    libgles2-mesa-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    libxext-dev
```

### Flutter Engine
Скачать готовые бинарники для ARM64:
```bash
# Release версия (рекомендуется)
wget https://github.com/ardera/flutter-engine-binaries-for-arm/releases/latest/download/engine_arm64_generic_linux_release.tar.xz

# Debug версия (для отладки)  
wget https://github.com/ardera/flutter-engine-binaries-for-arm/releases/latest/download/engine_arm64_generic_linux_debug.tar.xz

# Распаковать и использовать libflutter_engine.so
tar -xf engine_arm64_generic_linux_release.tar.xz
```

Необходимые файлы:
- `libflutter_engine.so` - Flutter Engine для Linux ARM64
- `flutter_embedder.h` - заголовочный файл Flutter Embedder API

## Сборка

```bash
# Создать директорию сборки
mkdir -p build && cd build

# Конфигурирование
cmake ..

# Сборка
make -j$(nproc)
```

### CMake конфигурация
CMakeLists.txt настроен для:
- C++17 стандарт
- Поиск необходимых системных библиотек через pkg-config
- Линковка с Flutter Engine
- Установка RPATH для поиска libflutter_engine.so

## Использование

```bash
./flutter_x11_embedder [OPTIONS]
```

### Параметры
- `--assets PATH` - путь к flutter_assets директории
- `--icu PATH` - путь к icudtl.dat файлу  
- `--help, -h` - справка

### Пример запуска
```bash
DISPLAY=:0 ./flutter_x11_embedder \
    --assets ../simple_demo \
    --icu ../simple_demo/icudtl.dat
```

## Технические детали

### EGL Context Management

```cpp
// Основной контекст для рендеринга
egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, ctx_attrs);

// Разделяемый контекст для загрузки ресурсов
egl_resource_context = eglCreateContext(egl_display, egl_config, egl_context, ctx_attrs);

// PBuffer для ресурсного контекста
egl_pbuffer = eglCreatePbufferSurface(egl_display, egl_config, pbuf_attrs);
```

### Visual Matching
Критически важное сопоставление EGL config с X11 visual:
```cpp
// Получить native visual ID из EGL config
EGLint vid = 0;
eglGetConfigAttrib(egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &vid);

// Найти соответствующий X11 visual
XVisualInfo tmpl = {};
tmpl.visualid = (VisualID)vid;
vinfo = XGetVisualInfo(display, VisualIDMask, &tmpl, &nvi);

// Создать colormap
colormap = XCreateColormap(display, RootWindow(display, vinfo->screen), 
                          vinfo->visual, AllocNone);
```

### Flutter Callbacks

#### OpenGL Context Management
- `make_current()` - активировать основной контекст для рендеринга
- `make_resource_current()` - активировать ресурсный контекст  
- `clear_current()` - очистить текущий контекст
- `present()` - представить кадр через eglSwapBuffers

#### GL Function Resolution
```cpp
static void* resolve_gl(const char* name) {
    // 1. Попробовать получить из libGLESv2
    if (libgles) p = dlsym(libgles, name);
    // 2. Попробовать через eglGetProcAddress для расширений
    if (!p) p = (void*)eglGetProcAddress(name);
    // 3. Глобальный поиск
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    return p;
}
```

### Event Handling

#### Поддерживаемые события
- `Expose` - перерисовка окна
- `ConfigureNotify` - изменение размера
- `MotionNotify` - движение мыши
- `ButtonPress/Release` - нажатия кнопок мыши
- `KeyPress/Release` - клавиатура (базовая поддержка)
- `ClientMessage` - закрытие окна

#### Конвертация событий
```cpp
// X11 кнопки мыши в Flutter
static FlutterPointerMouseButtons x11_button_to_flutter(unsigned int button) {
    switch (button) {
        case Button1: return kFlutterPointerButtonMousePrimary;
        case Button2: return kFlutterPointerButtonMouseMiddle;
        case Button3: return kFlutterPointerButtonMouseSecondary;
    }
}
```

### Vsync Integration
```cpp
static void vsync_callback(void* /*user_data*/, intptr_t baton) {
    uint64_t now = FlutterEngineGetCurrentTime();
    const uint64_t interval = 16666666ull; // ~60 FPS
    uint64_t frame_start = now;
    uint64_t frame_target = now + interval;
    
    FlutterEngineOnVsync(engine, baton, frame_start, frame_target);
}
```

## Отладка

### Логирование
Эмбеддер выводит подробные логи:
- EGL операции с кодами ошибок
- GL function resolution для отладки
- Контекстное переключение
- Входные события

### Типичные проблемы

#### EGL_BAD_ACCESS (0x3002)
- Проблема: Несоответствие visual между EGL и X11
- Решение: Используется автоматическое сопоставление через EGL_NATIVE_VISUAL_ID

#### GL Function Not Found
- Проблема: Flutter не может найти OpenGL функции
- Решение: Многоуровневый resolver (dlsym + eglGetProcAddress)

#### Context Switching Errors
- Проблема: Неправильное переключение контекстов между потоками
- Решение: Разделяемые контексты с pbuffer surface

### Диагностические команды
```bash
# Проверить EGL
pkg-config --exists egl && echo "EGL OK"

# Проверить X11
echo $DISPLAY
xdpyinfo | head

# Проверить OpenGL ES
glxinfo | grep "OpenGL ES"
```

## Ограничения

- Полная поддержка клавиатуры не реализована (требует key mapping)
- Фиксированный размер окна (1280x720)
- Работает только с OpenGL ES 2.0+
- Требует X11 сервер (XWayland)

## Расширения

### Добавление полной поддержки клавиатуры
1. Реализовать mapping X11 KeySym в Flutter key codes
2. Обрабатывать modifier keys (Shift, Ctrl, Alt)
3. Добавить поддержку текстового ввода

### Изменяемый размер окна
1. Обрабатывать ConfigureNotify события
2. Пересоздавать EGL surface при изменении размера
3. Уведомлять Flutter о новых метриках окна

### Fullscreen режим
1. Использовать EWMH hints для fullscreen
2. Скрывать системные панели
3. Обрабатывать переходы fullscreen ↔ windowed

---

**Flutter Pi Custom X11 Embedder**