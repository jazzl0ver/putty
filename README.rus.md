# PuTTY 0.84k*

Это неофициальная сборка PuTTY, основанная на стандартном PuTTY `0.84`.

## Изменения

### Close+Restart и быстрый restart

В Windows-версии добавлено управление перезапуском сессии из меню окна.

- Во время активной сессии в системном меню и контекстном меню окна появляется пункт `Close+Restart`.

-  `Close+Restart` закрывает текущий backend, переводит окно в inactive-состояние и сразу запускает backend заново с тем же `Conf`.

- После закрытия сессии пункт меню меняется на `Restart Session`.

- Если сессия уже закрыта, обычный `Enter` без `Alt`, `Ctrl` и `Shift` запускает `Restart Session`.

В стандартном PuTTY 0.84 закрытую сессию нельзя быстро перезапустить из того же окна: обычно нужно открыть новое окно или заново выбрать saved session.

### Start WinSCP

Для SSH-сессий в системное и контекстное меню Windows добавлен пункт `Start WinSCP`. Он открывает SFTP-подключение с текущими host, port, username и private key. `WinSCP.exe` ищется рядом с `PuTTY.exe` или через стандартную регистрацию приложений Windows. Если программа не найдена, PuTTY открывает диалог выбора файла. Путь также можно указать в `Window -> Behaviour -> WinSCP integration`; он сохраняется вместе с сессией.

### Portable file storage в стиле KiTTY

Добавлен альтернативный Windows backend хранения настроек в файлах. Стандартный registry backend остается доступным и используется по умолчанию.

Файловое хранение включается одним из способов:

- создать файл `putty.ini` рядом с `putty.exe`;
- запустить процесс с `PUTTY_STORAGE=file`;
- запустить процесс с `PUTTY_STORAGE=ini`;
- запустить процесс с `PUTTY_STORAGE=files`.

Файловое хранение принудительно отключается через:

```text
PUTTY_STORAGE=registry
```
Корень file storage:

- если рядом с exe найден `putty.ini`, корнем становится каталог с exe;
- если file storage включен через переменную окружения без `putty.ini`, корнем становится `%APPDATA%\PuTTY`;
- если `%APPDATA%` недоступен, используется fallback на каталог с exe.

Структура файлов:

```text
putty.ini
sessions\<escaped>.ini
sshhostkeys.ini
hostcas\<escaped>.ini
randomseed
```

Что хранится в файлах:

- SSH host keys
- SSH host CAs
- random seed
- настройки сессий

Формат session-файлов:

- основной формат совместим с KiTTY portable files: `Item\Value\`;
- значения percent-encoded;
- для плавной миграции также читается legacy-формат `key=value`.

Имена сессий с `/` превращаются в подпапки внутри `sessions`. Например:

```text
Network/Routers/Core01
```
будет храниться как файл сессии внутри подпапок `sessions\Network\Routers\`.

Команда `-load` в file storage умеет открывать:

- обычное имя сохраненной сессии;
- имя `.ini` файла;
- относительный или абсолютный путь к `.ini` файлу.

Примеры:

```text
putty.exe -load "Network/Routers/Core01"
putty.exe -load "Core01.ini"
putty.exe -load "D:\PuTTY\sessions\Core01.ini"
```

### Иерархия Saved Sessions

Добавлен folder-like режим:

- папки показываются перед сессиями;
- внутри подпапки отображается название сессии;
- load/save/delete работают с полным именем сессии;
- при вводе существующего имени сессии список переключается в соответствующую папку.

Это особенно полезно вместе с file storage, где `/` в имени реально отражается в структуре каталогов.

### putty-launcher.exe

В Windows-сборку добавлен отдельное singleton-приложение tray launcher: `putty-launcher.exe`.

 Левый клик по tray icon открывает основное окно launcher рядом с курсором:
- поле поиска;
- список сессий;
- кнопка `Load`;
- кнопка `New`.

Поведение основного окна:

- без поискового текста работает explorer mode с папками сохраненных сессий;
- при вводе текста работает плоский поиск по всем сессиям;
-  `Enter` и double click запускает выбранную сессию;
-  `Load` запускает `putty.exe -load "<session>" -edit`;
-  `New` запускает `putty.exe` без аргументов;
-  `Backspace` в списке поднимает на папку выше;
-  `Up` / `Down` переключают фокус между поиском и списком;
-  `Esc` скрывает окно launcher.

Launcher собирает список сессий из двух источников:

- Windows Registry: `HKCU\Software\SimonTatham\PuTTY\Sessions`;
- file storage: каталог `sessions`.

Если одна и та же сессия есть и в registry, и в file storage, launcher учитывает оба источника при построении списка.

### GitHub Actions build/release workflow

Добавлен workflow `.github/workflows/build.yml`.

Он срабатывает на push tag, собирает Windows x64 через `dockcross/windows-static-x64`, берет exe из `build/shipped.txt` и создает GitHub Release через `softprops/action-gh-release`.

### Исправление Ctrl/Shift/Alt стрелок

Изменено поведение `ShiftedArrowKeys`.

В стандартном PuTTY 0.84 default mode был `Ctrl toggles app mode`. В этой ветке default изменен на `xterm-style bitmap`.

Теперь Ctrl/Shift/Alt со стрелками генерируют xterm-style CSI sequences:
```text
ESC [ 1 ; <mod> A
ESC [ 1 ; <mod> B
ESC [ 1 ; <mod> C
ESC [ 1 ; <mod> D
```

Где `A/B/C/D` соответствуют Up/Down/Right/Left, а `<mod>` кодирует набор модификаторов.

Это исправляет сценарий, где в application cursor mode модификаторы Ctrl/Shift терялись. Практическая цель изменения: совместимость с Midnight Commander и похожими terminal-приложениями, ожидающими xterm-style modified arrow keys.

### Шаблоны заголовка окна

В поле `Window/Behaviour -> Window title` добавлены KiTTY-style placeholders. Синтаксис использует двойной `%`.

| Placeholder | Значение |
| --- | --- |
| `%%f` | название папки сессии, т.е. часть имени сессии до последнего `/` или `\` |
| `%%h` | host name |
| `%%p` | port number |
| `%%P` | protocol name |
| `%%s` | название сессии |
| `%%u` | username |
| `%%l` | список local forwarded ports |
| `%%d` | список dynamic forwarded ports |
| `%%` | literal `%` |

Пример:
```text
%%s [%%u@%%h:%%p]
```
Для session `Network/Core01`, пользователя `admin`, host `core01.example.net` и port `22` это даст заголовок вида:

```text
Network/Core01 [admin@core01.example.net:22]
```
Если у сессии нет названия, используется стандартная логика PuTTY: hostname + app name.

### Сохранение позиции и размера окна

Добавлена настройка:
```text
SaveWindowPos
```
В UI она отображается как:
```text
Window/Behaviour -> Save position and size on exit
```
Default: enabled.

При закрытии окна PuTTY сохраняет в загруженную session:

-  `TermWidth`;
-  `TermHeight`;
-  `TermXPos`;
-  `TermYPos`.

При следующем запуске сессии окно открывается в сохраненной позиции, если `TermXPos` и `TermYPos` не равны `-1`.
Если session не была загружена или это `Default Settings`, сохранение идет в default settings.

### Расширение mouse selection

Добавлена возможность быстро расширить выделение до строки с текущим положением курсора, нажав `Enter`
 
### Ctrl+MouseWheel меняет размер шрифта

В Windows terminal window добавлена поддержка:
```text
Ctrl + MouseWheel Up -> увеличить размер шрифта на 1
Ctrl + MouseWheel Down -> уменьшить размер шрифта на 1
```

### Minimize to Tray через putty-launcher

В системное и контекстное меню добавлен пункт:
```text
Minimize to Tray
```
Правый клик по tray icon launcher открывает окно:
```text
Minimized PuTTY Sessions
```
и позволяет:
- искать по названию сессии;
- видеть список минимизированных сессий;
- минимизировать все открытые окна сессий (`Minimize all to tray`);
- восстановить окна всех сессий (`Restore all`);
- закрыть все сессии (`Close all...`);

Операции со свернутыми сессиями:
- left click / double click / `Enter` восстанавливает выбранную сессию;
- right click открывает context menu;
- context menu умеет `Restore`, `Close session`, `Copy title`, `Copy session name`;

### Direct connect из launcher search

В поиск launcher добавлена строка прямого подключения, если введенный текст выглядит как хост и не совпадает с названиями существующих сессий.

Поддерживаются:
- IPv4;
- IPv6;
- FQDN;
-  `user@host`.

Примеры:  

```text
192.0.2.10
2001:db8::10
server.example.net
admin@server.example.net
```
Для `host` launcher запускает:
```text
putty.exe "<host>"
```
Для `user@host` launcher запускает:
```text
putty.exe -l "<user>" "<host>"
```
### Открытие URL по Ctrl+Shift+Left Click

- если URL найден, он открывается системным URL handler;
- если URL не найден, действие тихо игнорируется.

Windows frontend открывает URL через:
```text
ShellExecuteA(..., "open", url, ...)
```
GTK/Unix frontend открывает URL через:
-  `gtk_show_uri_on_window`, если доступен GTK 3.22+;
-  `gtk_show_uri`, если доступен GTK 2.14+;
- fallback на `xdg-open`;
- fallback на `open` для macOS GTK.

URL распознается по префиксам:

```text
https://
http://
www.
```
Для `www.` автоматически добавляется `https://`.
Пример:
```text
www.example.com/path
```
открывается как:
```text
https://www.example.com/path
```

```text
visit https://example.com/path), now
```
открывает:
```text
https://example.com/path
```

## Сравнение со стандартным PuTTY 0.84
 
| Область | Стандартный PuTTY 0.84 | 0.84k-r2 |
| --- | --- | --- |
| Saved sessions | Windows Registry, плоский список | Registry по умолчанию, optional file storage, folder-like UI |
| Portable mode | Нет встроенного KiTTY-style file storage | `putty.ini` рядом с exe или `PUTTY_STORAGE=file` |
| Host keys / host CAs | Registry | Registry или files, в зависимости от storage backend |
| Random seed | Стандартные Windows paths | Registry mode как upstream, file mode хранит `randomseed` |
| Launcher | Нет отдельного tray launcher | `putty-launcher.exe` с поиском sessions |
| Быстрый connect | Через main PuTTY dialog / command line | Launcher search умеет direct host connect |
| Restart session | Нет `Close+Restart` / Enter restart | `Close+Restart`, `Restart Session`, Enter после закрытия |
| Minimize to tray | Нет | Через `putty-launcher`, со списком minimized sessions |
| Ctrl+MouseWheel font size | Нет | Есть для Windows terminal window |
| URL click | Нет встроенного Ctrl+Shift URL open | `Ctrl+Shift+Left Click`, hover underline, hand cursor |
| Modified arrow keys | Default `Ctrl toggles app mode` | Default `xterm-style bitmap`, лучше для MC |
| Window title | Статический title или remote title | KiTTY-style placeholders в initial title |
| Window position | Нет session-level `SaveWindowPos` | Сохраняет position и terminal size on exit |
| Mouse selection + Enter | Нет | Enter расширяет active drag-selection до низа |
| Build automation | Нет этого repo workflow | Tag-based Windows x64 release workflow |

## Ограничения и важные замечания

 - File storage реализован для Windows storage backend.
- В обычном режиме PuTTY продолжает использовать Registry, пока не найден `putty.ini` или не задан `PUTTY_STORAGE=file|ini|files`.
- File storage не импортирует registry sessions автоматически. Launcher может видеть оба источника, но основной PuTTY storage backend выбирается для процесса целиком.
-  `putty-launcher.exe` запускает `putty.exe` из своего каталога.
-  `Minimize to Tray` требует запущенный `putty-launcher.exe`; без него PuTTY выполняет обычное minimize.
- URL opening не имеет отдельной настройки включения/выключения.
-  `Ctrl+Shift+Left Click` зарезервирован под открытие URL и не отправляется как обычный mouse click/selection gesture.
-  `SaveWindowPos` включен по умолчанию, поэтому закрытие сессии обновляет saved settings.

## Основные файлы реализации

| Файл | Назначение |
| --- | --- |
| `windows/filestore.c`, `windows/filestore.h` | File storage backend для Windows |
| `windows/storage.c` | Runtime switch между Registry и file storage |
| `config.c`, `conf.h`, `settings.c` | Новые настройки, foldered Saved Sessions, SaveWindowPos |
| `terminal/terminal.c`, `terminal/terminal.h` | Title placeholders, Enter selection, modified arrows, URL scanner/hover/open |
| `windows/window.c`, `windows/win-gui-seat.h`, `windows/platform.h` | Windows UI: restart, minimize-to-tray, Ctrl+Wheel, URL click/hover |
| `unix/window.c` | GTK/Unix URL click/hover/open |
| `windows/putty-launcher.c`, `windows/putty-launcher.rc`, `windows/launcher_stubs.c` | Tray launcher |
| `windows/CMakeLists.txt` | Build target `putty-launcher` и file storage linkage |
| `.github/workflows/build.yml` | Tag-based Windows x64 release workflow |
| `test/test_terminal.c`, `test/test_conf.c` | Regression tests for URL and config defaults |
