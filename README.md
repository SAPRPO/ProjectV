# tcp_project — TCP-клиент и сервер на C++17

Сервер принимает несколько одновременных подключений, отвечает на каждое
сообщение и логирует свою работу. Клиент подключается, отправляет строку
и печатает ответ.

## Протокол

Кадр: `[4 байта длины, big-endian][payload]`. Максимальный payload — 1 MiB
(`tcp::protocol::kMaxPayloadSize`). Ответ сервера: `OK <N> bytes: <ЗАПРОС В ВЕРХНЕМ РЕГИСТРЕ>`.

## Сборка

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Результат: `build/bin/tcp_server`, `build/bin/tcp_client`, тесты в `build/bin/test_*`.

## Запуск

```bash
# Терминал 1
./build/bin/tcp_server --port 5555 --log-level debug

# Терминал 2: одиночный запрос
./build/bin/tcp_client -m "Hello, server!"

# Терминал 2: интерактивный режим
./build/bin/tcp_client
```

Полезные опции сервера: `--bind`, `--max-connections`, `--io-timeout`, `--log-level`, `--log-file`.
Опции клиента: `--host`, `--port`, `--message`, `--interactive`, `--timeout`, `--log-level`, `--log-file`.

## Тесты

```bash
ctest --test-dir build --output-on-failure
# или по отдельности
./build/bin/test_protocol
./build/bin/test_logger
./build/bin/test_integration ./build/bin/tcp_server
```

## Особенности

* Многопоточная обработка: поток на соединение + реестр активных соединений и лимит подключений.
* Корректное завершение по `SIGINT`/`SIGTERM`: `poll`-цикл, `shutdown()` активных сокетов, join потоков.
* Логирование с уровнями, временем и идентификатором потока; дублирование в файл.
* Обрыв соединения, таймауты чтения/записи, битые кадры — обрабатываются и логируются.
* Windows-часть (Winsock) поддержана в коде; интеграционный тест работает на POSIX.
