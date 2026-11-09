# Стенд воспроизведения: wolf не отдаёт видео клиенту за VPN

Стенд воспроизводит жалобу «wolf лагает и рвёт соединение, когда в одной
сессии два клиента» и снимает артефакты, по которым это видно без
догадок: pcap видеопотока, лог wolf, метрики клиентов.

Живёт в форке `nhths/wolf` отдельной веткой, в каталоге `bench/`. Сборка
wolf'а стенд не затрагивает — это самостоятельный проект.

## Что проверяем

### 1. Размер RTP-пакета против path MTU (подтверждено)

Размер RTP-пакета выбирает клиент, и для «приватного» адреса сервера он
не каптится: `moonlight-common-c` считает 10/8, 172.16/12, 192.168/16
локальной сетью (`PlatformSockets.c:640`) и в этом случае не режет пакет
до 1024 байт (`Connection.c:406`). Адрес WireGuard-сервера приватный →
клиент просит 1392-байтные пакеты → wolf их так и режет
(`rtsp/commands.hpp:230`) → IP-пакет 1436 байт → **не влезает в path MTU
1420** → видео не идёт вообще, клиент рвёт соединение через 7 секунд.

Проверяется прогоном: `HOST=<приватный адрес> PACKET_SIZE=1392` +
`scripts/link-mtu.sh on 1420` → 0 кадров и обрыв соединения; без
`link-mtu.sh` или с `PACKET_SIZE=1024` — видео идёт штатно.

### 2. Лобби и общий producer (гипотеза не подтвердилась)

Раньше считалось, что лобби (один `waylanddisplaysrc` на всех) роняет
кадры, клиент видит пропуск в номерах и просит IDR. Механически это
неверно: номер кадра (`frameIndex`) присваивает payloader
(`rtpmoonlightpay->frame_num++` на каждый входной буфер), поэтому
просадка producer'а снижает fps, но **не создаёт пропусков в номерах
кадров**. Пропуск бывает только при потере уже пронумерованных пакетов —
в транспорте или при выбрасывании пакетов в appsink.

На стенде лобби (общий producer) и два отдельных producer'а при одинаковой
нагрузке дают одинаковый результат: пропусков нет, шторма IDR нет.

## Состав

| путь | что это |
|---|---|
| `client/` | bench-клиент на `moonlight-common-c`: настоящий протокол Moonlight (пейринг, RTSP, ENET), без декодирования видео |
| `wayland-client/` | минимальный Wayland-клиент: наполняет виртуальный дисплей wolf'а кадрами (градиент или шум) |
| `local/config.toml` | конфиг wolf для локального стенда (программный x264/x265, один профиль) |
| `scripts/run-local.sh` | локальный стенд целиком: поднять wolf в podman, поднять клиентов, создать лобби, собрать артефакты |
| `scripts/link-mtu.sh` | эмуляция path MTU: роняет UDP-пакеты длиннее порога |
| `scripts/link-rate.sh` | эмуляция узкого аплинка (tbf) |
| `scripts/link-loss.sh` | эмуляция потерь в канале (netem) |
| `scripts/capture-local.sh`, `scripts/capture.py` | захват pcap без tcpdump (сырой сокет AF_PACKET) |
| `scripts/bench.sh` | сценарий против удалённого wolf: pcap, два клиента, лобби, сбор артефактов |
| `scripts/analyze.py` | разбор pcap по потокам: пропуски кадров, размеры I-кадров, всплески, общие окна потерь |
| `scripts/build.sh`, `scripts/build-wayland-client.sh` | сборка клиента и wayland-клиента |
| `scripts/run-all.sh` | оркестратор: `build` / `run` / `summary` / `all` |
| `scripts/summarize.py` | сводная таблица по прогонам + подстановка в `TICKET.md` |
| `scripts/compare_config.py` | сверка конфига на машине с шаблоном из репозитория |
| `scripts/publish-to-fork.sh` | публикация стенда в форк отдельной веткой |

## Локальный стенд

wolf поднимается в podman в сетевом namespace хоста, клиенты ходят на
127.0.0.1 (или на приватный адрес машины — см. `HOST` ниже). Весь
трафик — на loopback, наружу ничего не ходит.

```sh
cd bench
./scripts/run-local.sh build          # собрать wayland-клиент и bench-клиент
./scripts/run-local.sh up             # поднять wolf (нужны права на podman)
./scripts/run-local.sh run            # два клиента (+ лобби по LOBBY=1)
./scripts/run-local.sh down           # снять контейнер
```

Ключевые переменные `run-local.sh`:

| переменная | по умолчанию | что это |
|---|---|---|
| `HOST` | `127.0.0.1` | адрес wolf для клиентов. **Важно:** клиент считает приватный адрес локальным и не каптит размер пакета; на loopback (127/8) он считает сервер удалённым и каптит до 1024 |
| `PACKET_SIZE` | `1024` | размер RTP-пакета, который просит клиент (1392 — дефолт Moonlight для LAN) |
| `LOBBY` | `1` | `0` — у каждого клиента свой producer, `1` — общий producer лобби |
| `NOISE` | `0` | `1` — приложение рисует шум: кодировщик упирается в CPU, конвейер отстаёт |
| `WIDTH`/`HEIGHT`/`FPS`/`BITRATE` | 1280/720/60/20000 | разрешение, частота и битрейт потока |
| `DURATION` | `60` | сколько секунд держать поток |
| `ARTIFACTS` | `artifacts/local` | куда складывать артефакты |

Порты wolf'а на стенде: HTTPS 47984, HTTP 47989, видео 47999,
**RTP-ping (и видеопоток) 48110**, аудио-ping 48210. Стандартные
48100/48200 на этой машине занимает NetworkManager. Видеопоток wolf идёт
на порту ping'а: `state/sessions.hpp` отдаёт клиенту
`video_stream_port = VIDEO_PING_PORT`.

Эмуляция канала:

```sh
bash scripts/link-mtu.sh on 1420 48110     # path MTU: роняем пакеты длиннее 1420
bash scripts/link-rate.sh on 4000 48110    # аплинк 4 Мбит/с
bash scripts/link-loss.sh on 1             # 1% потерь
bash scripts/capture-local.sh on out.pcap  # pcap (tcpdump может не стоять)
```

## Сборка клиента

Нужны: cmake, gcc, OpenSSL, libcurl.

```sh
cd bench
./scripts/build.sh
```

Зависимость `moonlight-common-c` (с сабмодулями `enet`/`nanors`)
подтягивается на лету через `FetchContent` — ровно так же, как wolf сам
тянет fmt/eventbus/nanors. `git submodule update` делать не нужно.

Клиент собирается на настоящем клиентском коде Moonlight, поэтому
поведение (пейринг, детект потери кадров, запрос IDR) совпадает с
Moonlight, а не с его имитацией.

## Сценарий против удалённого wolf

```sh
cd bench
WOLF_HOST=192.168.31.100 \
DURATION=90 \
PRODUCER_W=2560 PRODUCER_H=1440 PRODUCER_FPS=60 \
CLIENT_A="1920 1080 60 20000" \
CLIENT_B="1600 900 60 15000" \
./scripts/bench.sh
```

* `WOLF_HOST` — адрес wolf (нужен ssh без пароля);
* `PRODUCER_*` — разрешение и частота producer'а в лобби;
* `CLIENT_A` / `CLIENT_B` — `ширина высота fps битрейт_кбит` для каждого;
* `DURATION` — сколько секунд держать поток;
* `LOBBY=0` — базовый прогон: лобби не создаётся, у каждого клиента свой
  producer;
* `RFI=1` — клиент объявляет поддержку reference frame invalidation.

Скрипт сам пейрит клиентов: wolf публикует PIN в своём логе
(`Insert pin at http://ip:port/pin/#<secret>`), скрипт достаёт secret и
отправляет PIN через `POST /pin/`. Лобби создаётся через unix-сокет API
wolf (`/var/run/wolf/wolf.sock`, `POST /api/v1/lobbies/create`), клиенты
в неё входят через `POST /api/v1/lobbies/join`.

## Артефакты

| файл | что внутри |
|---|---|
| `video.pcap` | видеопоток wolf (UDP-диапазон 47980–48250) — видео не шифруется, в pcap видны номера кадров и payload |
| `wolf.log` | лог контейнера wolf за прогон (`Forcing IDR` пишется на уровне `debug`, нужен `WOLF_LOG_LEVEL=DEBUG`) |
| `client-A.json`, `client-B.json` | метрики клиента: кадры, I-кадры, пропуски, время сборки кадра |
| `client-*.log` | stderr клиента, включая «Network dropped N frames» от `moonlight-common-c` |
| `report.json`, `analysis.txt` | разбор pcap |

## Как читать результат

Каждый клиент получает **свой** RTP-поток (у каждого конвейера свой
`rtpmoonlightpay` и свой счётчик `frameIndex`), поэтому анализатор
разбирает потоки по паре портов.

Формат видеопакета wolf (совпадает с moonlight-common-c):

```
[0]      RTP-заголовок: 0x80 | 0x10 (версия 2 + бит расширения)
[1]      packetType
[2:4]    sequenceNumber (big-endian)
[4:8]    timestamp
[8:12]   ssrc
[12:16]  заголовок расширения RTP
[16:20]  streamPacketIndex (little-endian)
[20:24]  frameIndex (little-endian)
[24]     flags: 0x1 CONTAINS_PIC_DATA, 0x2 EOF, 0x4 SOF
[26]     multiFecFlags (всегда 0x10)
[28:32]  fecInfo: dataShards в старших битах, номер шарда в битах 12..21
[32:40]  VideoShortHeader: header_type=0x01, frame_type (2 = IDR), last_payload_len
[40:]    полезная нагрузка кадра
```

FEC-пакеты (parity shards) картинки не несут: их `fecInfo` даёт номер
шарда больше числа шардов с данными.

Признак шторма — совпадение трёх вещей:

1. в `analysis.txt` много I-кадров подряд (всплески) и/или крупные
   I-кадры (сотни килобайт);
2. в pcap видны пропуски номеров кадров (клиент их и замечает);
3. в `wolf.log` много `[GSTREAMER] Forcing IDR` — это wolf отвечает на
   запросы IDR от клиента.

## Проверка конфига

Конфиг wolf на машине полностью перекрывает встроенный шаблон: wolf грузит
пользовательский TOML (`rfl::toml::load<..., DefaultIfMissing>`) и шаблон
заново не подмешивает. Поэтому расхождение с шаблоном — это реально
работающие настройки.

```sh
ssh root@192.168.31.100 'cat /etc/wolf/cfg/config.toml' > /tmp/host-config.toml
./scripts/compare_config.py /tmp/host-config.toml \
  ../../src/moonlight-server/state/default/config.v7.toml --section gstreamer
```
