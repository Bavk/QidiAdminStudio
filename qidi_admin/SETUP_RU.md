# Публикация Qidi Admin Studio

`QidiAdminStudioClean` — самостоятельная рабочая копия исходников OrcaSlicer
с интеграционным слоем Qidi Admin. Она не использует Flutter-сборку и не
собирает C++ на локальном ПК по умолчанию.

## Один раз на Windows

Откройте PowerShell и выполните:

```powershell
& 'C:\Program Files\GitHub CLI\gh.exe' auth login --web --git-protocol https
```

В браузере подтвердите вход в GitHub. Затем в папке этого исходника:

```powershell
.\qidi_admin\Publish-QidiAdminStudio.ps1
```

Сценарий создаёт **публичный** репозиторий `QidiAdminStudio` в текущем GitHub
аккаунте, добавляет официальный OrcaSlicer как `upstream` и отправляет ветку
`main` в `origin`. Публичность обязательна для распространяемого форка под
AGPL-3.0.

## Готовый Windows EXE без локальной сборки

В открытом GitHub-репозитории выберите:

`Actions → Qidi Admin Studio — Windows → Run workflow`.

Первый запуск с параметром `build_deps_only=true` заполняет кэш нативных
зависимостей. Затем запустите workflow с выключенным параметром — результат
появится в Artifacts. Последующие сборки используют кэш и не требуют сборки
Orca на вашем ПК.

Не добавляйте в Git ключ Raspberry, ключ Moonraker, конфигурации `.env` или
адреса приватной сети. Эти данные останутся в локальном защищённом профиле
Qidi Admin Studio.
