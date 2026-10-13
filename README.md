# PitWall

**Aplicación de escritorio de telemetría e ingeniería de carrera para EA SPORTS™ F1® 25 y F1® 25: 2026 Season Pack.**

> 🚧 **Proyecto en desarrollo, en fase inicial.** Lo que se describe a continuación es el objetivo del proyecto: la mayor parte todavía no está implementada.

La idea: un único `PitWall.exe` con su propia ventana: núcleo en C++20/23 e
interfaz React + TypeScript alojada en WebView2, todo en el mismo proceso y todo en local.

> **Estado actual: fase F0 (cimientos) completada.** El repositorio compila, prueba y ejecuta un
> binario todavía vacío: `pw::core`, `pw::obs` y `pw::config` con su suite de pruebas. La recepción
> UDP llega en F1.1 y la ventana de escritorio en F1.6b.
> La especificación de ingeniería completa vive en [`docs/`](docs/).

---

## La experiencia (objetivo)

1. Instalas PitWall y abres `PitWall.exe`.
2. Se abre una ventana propia. **No un navegador.**
3. El núcleo ya está escuchando la telemetría UDP: **no hay nada que conectar**.
4. Abres F1 25 y PitWall lo detecta solo: formato 2025 o 2026, sesión, circuito y tu coche.
5. Si el juego no tiene UDP configurado, la propia ventana explica exactamente qué falta.

No hace falta navegador, terminal, Docker, ni saber qué es un puerto o un WebSocket.

---

## Qué hará

| | |
|---|---|
| **Live timing** | Clasificación, sectores, intervalos, neumáticos, paradas, penalizaciones, banderas |
| **Telemetría en vivo** | Velocidad, RPM, marcha, pedales, dirección, DRS, ERS, temperaturas, presiones, daños |
| **Mapa de pista** | Trazado **reconstruido desde la propia telemetría**, sin imágenes precargadas |
| **Análisis de vueltas** | Comparación alineada por distancia con delta acumulado sobre 8+ canales |
| **Grabación y replay** | Captura cruda de datagramas y reproducción determinista a seis velocidades |
| **Diagnóstico UDP** | Todo lo que llega por el socket, con detección temprana de cambios de protocolo |
| **2026** | 24 coches, Active Aero, Overtake Mode, zonas de activación, MADRING, Audi y Cadillac |

---

## Arquitectura en una pantalla

```
PitWall.exe  ── un solo proceso
├── Core C++20/23
│   ├── UDP Receiver      recepción sin pérdidas, arranca sola
│   ├── Protocol Decoder  formatos 2025 y 2026, detección automática
│   ├── Session State     sesiones, vueltas, stints, flashbacks
│   ├── Recorder          captura cruda .f1raw comprimida
│   ├── Replay            reproducción determinista
│   └── Analytics         métricas derivadas por vuelta
└── Desktop Shell
    └── ventana Win32
        └── WebView2
            └── React + TypeScript
```

El transporte entre núcleo e interfaz existe, pero es **infraestructura interna**: bucle local,
puerto efímero, token de un solo uso, y nunca visible para el usuario. No hay servidor HTTP, no hay
acceso desde la LAN a la interfaz y no hay multicliente: el único cliente posible es la propia ventana.

Decisiones clave en [`docs/15-ADR.md`](docs/15-ADR.md), en particular **ADR-002 v2** (aplicación de
escritorio con WebView2) y **ADR-016** (Docker no ejecuta ni distribuye el producto).

---

## Documentación

| Documento | Contenido |
|---|---|
| [`01-REQUIREMENTS`](docs/01-REQUIREMENTS.md) · [PDF](docs/01-REQUIREMENTS.pdf) | Requisitos de software completos: casos de uso, RF, RNF, reglas, riesgos |
| [`02-RESEARCH`](docs/02-RESEARCH.md) | Investigación: fuentes oficiales, ecosistema, comparativas de stack |
| [`03-UDP-PROTOCOL`](docs/03-UDP-PROTOCOL.md) | Referencia del protocolo 2025 y 2026 campo a campo |
| [`04-ARCHITECTURE`](docs/04-ARCHITECTURE.md) | Componentes, fronteras, concurrencia, flujo de eventos |
| [`05-DATA-MODEL`](docs/05-DATA-MODEL.md) | Entidades, series temporales, persistencia, índices, retención |
| [`06-REALTIME-PIPELINE`](docs/06-REALTIME-PIPELINE.md) | UDP → decode → normalize → state → persist → interfaz |
| [`07-RECORDING-AND-REPLAY`](docs/07-RECORDING-AND-REPLAY.md) | Formato `.f1raw`, grabador, motor de replay, fixtures |
| [`08-FRONTEND-PLAN`](docs/08-FRONTEND-PLAN.md) | Interfaz en WebView2: pantallas, gráficas, mapa, rendimiento |
| [`09-BACKEND-PLAN`](docs/09-BACKEND-PLAN.md) | Módulos C++, shell de escritorio, decoders, estado, transporte |
| [`10-TESTING-PLAN`](docs/10-TESTING-PLAN.md) | Protocolo, unitarias, integración, e2e en WebView2, fuzzing |
| [`11-IMPLEMENTATION-PLAN`](docs/11-IMPLEMENTATION-PLAN.md) | Fases, tareas y Definition of Done |
| [`12-MVP`](docs/12-MVP.md) | Alcance del MVP y sus 35 criterios de aceptación |
| [`13-BACKLOG`](docs/13-BACKLOG.md) | Epics, historias y tareas |
| [`14-ROADMAP`](docs/14-ROADMAP.md) | MVP → Alpha → Beta → V1 |
| [`15-ADR`](docs/15-ADR.md) | 16 decisiones arquitectónicas con sus consecuencias |
| [`16-DEVELOPMENT-GUIDE`](docs/16-DEVELOPMENT-GUIDE.md) | Cómo desarrollar el proyecto desde cero |
| [`17-PROJECT-STRUCTURE`](docs/17-PROJECT-STRUCTURE.md) | Árbol del repositorio explicado |
| [`18-GLOSSARY`](docs/18-GLOSSARY.md) | Glosario completo |

Los `.md` son la **fuente única de verdad**. Los `.pdf` se generan con `node tools/build-all-pdfs.mjs`
y nunca se editan a mano.

---

## Requisitos

### Para usar PitWall

| | |
|---|---|
| Sistema | Windows 10 / 11 x64 |
| Motor de interfaz | **WebView2 Runtime** — de serie en Windows 11; en Windows 10 el instalador lo comprueba y ofrece instalarlo |
| Juego | EA SPORTS F1 25, con o sin el 2026 Season Pack |

Nada más. Sin navegador, sin Docker, sin servidor y sin privilegios de administrador.

### Para desarrollar

| Herramienta | Versión mínima |
|---|---|
| Visual Studio 2022 («Desarrollo para el escritorio con C++») | 17.10 |
| SDK de WebView2 | fijado por manifiesto |
| CMake | 3.28 |
| Node.js | 22 LTS |
| Python | 3.11 |

Paso a paso en [`16-DEVELOPMENT-GUIDE`](docs/16-DEVELOPMENT-GUIDE.md).

---

## Compilar y ejecutar

```bash
git clone <url> pitwall
cd pitwall

cmake --preset debug            # configura y resuelve dependencias
cmake --build --preset debug    # compila
ctest --preset debug            # ejecuta la suite completa
```

Las dependencias se declaran en [`vcpkg.json`](vcpkg.json) con versiones fijadas. Si hay una
instalación de vcpkg (`VCPKG_ROOT`), las provee el manifiesto; si no, CMake las descarga por commit
exacto. En ambos casos las versiones son las mismas.

### Presets

| Preset | Para qué | Requiere |
|---|---|---|
| `debug` | Desarrollo diario, con aserciones | MSVC o Clang |
| `release` | Medición de rendimiento y distribución | MSVC o Clang |
| `asan` | Desinfectante de direcciones | MSVC o Clang |
| `tsan` | Desinfectante de hilos | **Clang en Linux** — no existe en MSVC |
| `fuzz` | Objetivos de fuzzing (fase F1.2a) | **Clang en Linux** |

### Ejecutar

```bash
./build/debug/bin/pitwall-cli --config config/dev.toml --log-console
./build/debug/bin/pitwall-cli --help
```

`pitwall-cli` es el **núcleo sin interfaz**: lo usan las pruebas de integración, la reproducción por
lotes y la integración continua en Linux. El producto es `PitWall.exe` —una ventana propia con la
interfaz dentro— y se construye en la fase F1.6b.

### Comprobaciones del repositorio

```bash
python tools/check_deps.py          # el grafo de módulos sigue siendo válido (RNF-084)
python tools/check_deps.py --graph  # qué puede depender de qué, y por qué
```

Detalle completo en [`16-DEVELOPMENT-GUIDE`](docs/16-DEVELOPMENT-GUIDE.md).

---

## La especificación oficial de EA

Este repositorio **no incluye** la especificación UDP de EA, que se provee *«under license from EA for
reference purposes only»*. Debe descargarse del hilo oficial y colocarse en `spec/`, que está excluido
del control de versiones:

> EA Forums → F1 Games Game Info Hub → *EA SPORTS™ F1®25: 2026 Season Pack UDP SPECIFICATION*

Versión de referencia de este proyecto: **`Data Output from F1® 25: 2026 Season Pack` v1.2 (Season 8)**.

---

## Configuración del juego

`Game Options → Settings → UDP Telemetry Settings`

| Ajuste | Valor |
|---|---|
| UDP Telemetry | On |
| UDP IP Address | `127.0.0.1` (o la IP del PC si juegas en consola) |
| UDP Port | `20777` |
| UDP Format | **2026** con el Season Pack · **2025** con el juego base |

PitWall lee `hardware_settings_config.xml` para detectar estos valores y, si algo no cuadra, lo indica
concretamente en lugar de dar un error genérico.

Usar el formato 2025 con contenido 2026 degrada los datos: los coches se reportan con fórmula genérica
y se pierde por completo Active Aero, Overtake Mode y el límite de recuperación de ERS.

---

## Aviso legal

PitWall es un proyecto no oficial creado por aficionados, **sin afiliación, patrocinio ni respaldo** de
Electronic Arts Inc., Codemasters, la Fédération Internationale de l'Automobile ni Formula One World
Championship Limited.

«EA SPORTS», «F1», «FORMULA 1», «FIA» y los nombres de equipos, pilotos y circuitos son marcas de sus
respectivos titulares y se emplean aquí únicamente de forma descriptiva para identificar la
compatibilidad del software.

Este proyecto no distribuye ningún activo del juego ni la documentación de la especificación UDP de EA,
que debe obtenerse desde los canales oficiales.

---

## Licencia

Ver [`LICENSE`](LICENSE).
