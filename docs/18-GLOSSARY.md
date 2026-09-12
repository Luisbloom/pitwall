---
title: "PitWall — Glosario"
subtitle: "Términos del protocolo, del dominio, de la arquitectura y del proyecto"
version: "1.1"
status: "Vigente"
date: "2026-09-11"
author: "Luis Miguel"
docid: "PW-GLO-018"
---

# Glosario

Términos marcados con `[S1]` proceden de la especificación oficial `Data Output from F1® 25: 2026 Season Pack` v1.2, o de `Data Output from F1® 25 Game` v3 para lo específico de 2025.

---

## 1. Protocolo UDP

**Active Aero** — Sistema aerodinámico activo del reglamento 2026. Alterna entre *Corner mode* (0) y *Straight mode* (1). Su estado llega en `CarTelemetry2`; sus zonas, en el paquete `Session`. `[S1]`

**Active Aero Zone** — Tramo de la vuelta, expresado como fracción 0..1 de inicio y fin, donde el Active Aero puede usarse. Existen dos listas: completas (`Full`) y parciales (`Partial`), máximo 8 cada una. `[S1]`

**Broadcast Mode** — Modo de envío UDP que transmite a toda la subred. No requiere IP destino, sólo puerto. `[S1]`

**Car Telemetry 2** — Paquete con identificador 16, **exclusivo del formato 2026**. 269 bytes. Contiene Active Aero, Overtake Mode, el indicador de reglamento 2026 y la circulación en sentido contrario. `[S1]`

**Datagrama** — Unidad de transporte UDP. En este protocolo, exactamente un paquete completo, nunca fragmentado a nivel de aplicación.

**Event String Code** — Cadena de 4 caracteres que identifica el tipo de evento: `SSTA`, `FLBK`, `PENA`, `COLL`, `PMEN`… 21 códigos en 2025, **25 en 2026**. `[S1]`

**Frame Identifier** (`m_frameIdentifier`) — Identificador del fotograma en que se capturaron los datos. **Retrocede tras un flashback.** Sirve para agrupar paquetes del mismo fotograma, **nunca** como reloj. `[S1]`

**Little Endian** — Orden de bytes del protocolo: el menos significativo primero. Sin excepciones. `[S1]`

**MADRING** — Circuito de Madrid, `trackId = 42`. Introducido con el contenido 2026. `[S1]`

**Marshal Zone** — Sector de comisarios con su bandera. Máximo 21 por vuelta. `[S1]`

**Motion Ex** — Paquete 13. Física detallada (suspensión, deslizamiento, fuerzas, camber) **exclusiva del coche del jugador**. 273 bytes en ambos formatos. `[S1]`

**Overall Frame Identifier** (`m_overallFrameIdentifier`) — Identificador global del fotograma. **No retrocede tras un flashback.** Es el **único** ordenador temporal fiable del protocolo (RN-002). `[S1]`

**Overtake Mode** — Sistema de sobrepaso del reglamento 2026. Se reporta con disponibilidad, estado activo y distancia hasta la próxima activación. `[S1]`

**Packed** — Sin relleno entre campos. El tamaño de un struct es la suma exacta de sus campos. En C++ se consigue con `#pragma pack(1)`. `[S1]`

**Packet Format** (`m_packetFormat`) — Campo de la cabecera que vale `2025` o `2026`. Determina el layout completo del datagrama y **la elección del decoder**. `[S1]`

**Packet Header** — Cabecera común de **29 bytes**, idéntica en 2025 y 2026. Es la única parte que puede leerse con seguridad sin conocer el formato. `[S1]`

**Packet ID** (`m_packetId`) — Tipo de paquete. 0-15 en 2025, **0-16 en 2026**. `[S1]`

**Packet Version** (`m_packetVersion`) — Versión del layout de un tipo concreto de paquete. Empieza en 1. Actualmente **todos valen 1** en ambos formatos. `[S1]`

**Partial Mode** — Estado de pista en el que las zonas de Active Aero disponibles son las parciales. Señalizado por los eventos `PMEN` y `PMDI`, con razones: pista mojada, safety car o bandera roja. `[S1]`

**Player Car Index** (`m_playerCarIndex`) — Índice del coche del jugador dentro de los arrays. `[S1]`

**Secondary Player Car Index** — Índice del segundo jugador en pantalla partida. **255 si no hay.** `[S1]`

**Send Rate** — Frecuencia de emisión configurable en el juego, que gobierna los paquetes 0, 2, 6, 7, 13 y 16. Su valor por defecto en el XML es `20`. `[S1]`

**Session UID** (`m_sessionUID`) — Identificador único de sesión, `uint64`. Un cambio implica sesión nueva. `[S1]`

**Show Online ID / gamertags** — Ajuste del juego que permite que otros vean el identificador online del jugador. Requiere confirmación explícita. `[S1]`

**Vehicle Index** — Posición de un coche en los arrays. **Estable durante toda la sesión**, sin significado fuera de ella (RN-004). `[S1]`

**Your Telemetry** — Ajuste del juego con valores `Restricted` (por defecto) y `Public`. Determina si otros jugadores ven ciertos datos del coche. Es el origen de la regla RN-001. `[S1]`

---

## 2. Dominio de carreras

**Compuesto real** (`actualTyreCompound`) — El compuesto verdadero: C0-C6, intermedio, lluvia. Puede diferir del visual. `[S1]`

**Compuesto visual** (`visualTyreCompound`) — Lo que se muestra: blando, medio, duro, intermedio, lluvia. `[S1]`

**Delta** — Diferencia de tiempo acumulada entre dos vueltas, calculada **alineando por distancia recorrida**, nunca por tiempo transcurrido (RN-009).

**Degradación** — Pérdida de rendimiento del neumático con las vueltas. En PitWall se estima como la pendiente de una regresión lineal de tiempos de vuelta limpios dentro de un stint. Siempre `ESTIMATED`.

**DNF** — *Did Not Finish*. `resultStatus = 4`. `[S1]`

**DSQ** — Descalificado. `resultStatus = 5`. `[S1]`

**Flashback** — Función del juego que rebobina la sesión a un instante anterior. Genera el evento `FLBK` y produce el retroceso de `m_frameIdentifier` y `m_sessionTime`. Es el caso borde de mayor impacto arquitectónico del proyecto.

**Overcut** — Estrategia de retrasar la parada para ganar posición con vueltas rápidas en pista libre.

**Rama** (*branch*) — Concepto propio de PitWall: línea temporal creada por un flashback. Los datos de la rama superada se conservan marcados, no se borran (RN-006).

**Safety Car (SC)** — Coche de seguridad completo. `safetyCarStatus = 1`. `[S1]`

**Sector** — Cada uno de los tres tramos en que se divide una vuelta. En 2026 el paquete `Session` entrega las distancias de inicio de los sectores 2 y 3. `[S1]`

**Stint** — Conjunto de vueltas consecutivas con el mismo juego de neumáticos.

**Undercut** — Estrategia de adelantar la parada para ganar posición con neumáticos frescos.

**Virtual Safety Car (VSC)** — Coche de seguridad virtual. `safetyCarStatus = 2`. `[S1]`

**Vuelta inválida** — Vuelta con `m_currentLapInvalid = 1`. Se marca y **se excluye por defecto** de mejores tiempos y de la degradación, pero **no se borra** (RN-005).

---

## 3. Arquitectura de PitWall

**Backpressure (contrapresión)** — Mecanismo por el que un consumidor lento comunica su saturación. En PitWall la contrapresión **nunca sube**: cada etapa descarta según su política y lo contabiliza.

**BufferPool** — Conjunto de buffers preasignados al arrancar. Garantiza cero asignaciones por datagrama en el camino caliente (RNF-013).

**Bus** — Canal interno por el que el motor de estado publica instantáneas inmutables a los consumidores.

**Desktop Shell** — El módulo `pw::shell`: la ventana Win32 que aloja el WebView2, arranca el núcleo
y gestiona el ciclo de vida del proceso. **El único módulo específico de Windows** del proyecto.

**Endpoint efímero** — La dirección del transporte interno. El puerto lo asigna el sistema en cada
arranque; no es fijo, ni configurable, ni visible para el usuario. Junto con el token, es lo que
convierte el transporte en infraestructura interna en lugar de en una interfaz pública.

**Camino caliente** (*hot path*) — El recorrido recepción → decodificación → estado. Sujeto a las restricciones más estrictas: sin asignaciones, sin bloqueos, sin E/S.

**Canal** (transporte) — Cada uno de los seis flujos lógicos del transporte interno: `session`, `timing`, `telemetry`, `events`, `diagnostics`, `replay`. Cada uno con su garantía.

**`Channel<T>`** (tipo) — Tipo de C++ que encapsula un valor **y su procedencia**, sin conversión implícita al valor. Convierte el principio P1 en una garantía del compilador (ADR-009).

**Coalescing** — Sustituir un mensaje pendiente por uno más reciente en lugar de encolar ambos. Mantiene la latencia acotada bajo un cliente lento (RF-304). Los eventos están **exentos**.

**Decimado mínimo-máximo** — Técnica de reducción de puntos para gráficas que conserva el mínimo y el máximo de cada columna de píxel, de modo que los picos nunca desaparecen.

**Decoder** — Componente que transforma bytes de un formato concreto en estructuras tipadas. Uno por formato, registrados en el `DecoderRegistry`.

**`.f1raw`** — Formato propio de captura de datagramas crudos: cabecera autodescriptiva, bloques comprimidos con zstd y registros autodelimitados con marca de tiempo monótona.

**Fixture** — Paquete individual capturado de una sesión real, acompañado de un `.json` con los valores esperados verificados a mano.

**Fotograma lógico** — Conjunto de paquetes de «rate del menú» que comparten `m_frameIdentifier`. Se agrupan para publicar instantáneas coherentes (RF-209).

**Instantánea** (*snapshot*) — Estructura **inmutable** con el estado en un momento dado, compartida por puntero contado. Elimina las carreras entre lectores y escritor.

**Normalizador** — El único componente que conoce a la vez `pw::proto` y `pw::domain`. Traduce del protocolo al dominio.

**Puente con el shell** — Las pocas operaciones que la interfaz sólo puede pedir al proceso
anfitrión: diálogos nativos de fichero, abrir un enlace en el navegador del sistema y fijar la
ventana siempre visible. Deliberadamente delgado: todo lo demás va por el transporte normal.

**Punto de sincronización** — Marca periódica en una captura (cada ~10 s) con el desplazamiento del bloque, el `sessionUid` y el `overallFrame`, que permite la búsqueda temporal sin leer todo el fichero.

**Reloj monótono** — Fuente de tiempo que nunca retrocede, inmune a cambios de la hora del sistema. Base de la fidelidad del replay y de la medición de latencias.

**Token de un solo uso** — Credencial generada al arrancar y entregada únicamente al WebView2 propio
mediante el script de inicialización. Toda conexión al transporte interno debe presentarla.

**WebView2** — Control de Microsoft, basado en el Chromium de Edge, que permite alojar contenido web
dentro de una aplicación nativa de Windows. Es donde vive la interfaz de PitWall. Requiere el
*WebView2 Runtime*, presente de serie en Windows 11 y comprobado al arrancar (RF-632).

**`SpscRing`** — Cola circular de un productor y un consumidor, sin bloqueos, con los índices en líneas de caché separadas.

---

## 4. Procedencia del dato

Las cinco etiquetas obligatorias (`01-REQUIREMENTS.md` §19.6). Toda magnitud expuesta lleva **exactamente una**.

**`RAW`** — Valor tal como lo emite el juego, con la única transformación de unidades o desescalado documentada en la especificación. *Ejemplo:* velocidad en km/h.

**`DERIVED`** — Calculado de forma determinista a partir de valores `RAW`, sin suposiciones sobre lo desconocido. *Ejemplo:* delta entre vueltas.

**`ESTIMATED`** — Resultado de un modelo con incertidumbre. Obliga a declarar entradas, fórmula, frecuencia, limitaciones y —cuando aplique— margen de error. *Ejemplo:* pendiente de degradación de un stint.

**`UNAVAILABLE`** — El juego no proporciona ese dato en este contexto. *Ejemplo:* `Motion Ex` de un rival; Active Aero en formato 2025.

**`RESTRICTED`** — Especialización de `UNAVAILABLE`: el dato existe, pero su propietario ha decidido no compartirlo mediante *Your Telemetry*. *Ejemplo:* desgaste de neumáticos de un rival.

---

## 5. Identificadores del proyecto

| Prefijo | Significado | Documento |
|---|---|---|
| `RF-nnn` | Requisito funcional (`RF-62x` = shell de escritorio) | `01-REQUIREMENTS.md` §16 |
| `RNF-nnn` | Requisito no funcional | §17 |
| `RN-nnn` | Regla de negocio | §18 |
| `CU-nn` | Caso de uso | §15 |
| `ACT-nn` | Actor | §9 |
| `OBJ-nn` / `OBJ-Tn` | Objetivo de producto / técnico | §5 |
| `OUT-nn` | Fuera de alcance | §6 |
| `LIM-nn` | Limitación del protocolo | §14.1 |
| `D-nn` | Diferencia 2025/2026 | §13.1 |
| `AS-nn` | Suposición | §33 |
| `RSK-nn` | Riesgo | §32 |
| `Q-nn` | Pregunta abierta | §35 |
| `A-nn` | Criterio de aceptación del MVP | §31 |
| `SEC-nn` | Requisito de seguridad | §25 |
| `PRV-nn` | Compromiso de privacidad | §26.2 |
| `PER-nn` | Requisito de persistencia | §22.4 |
| `RT-nn` | Requisito de tiempo real | §23 |
| `RP-nn` | Requisito de replay | §24 |
| `OBS-nn` | Requisito de observabilidad | §27 |
| `TST-nn` | Requisito de pruebas | §30.4 |
| `UX-nn` / `A11Y-nn` | Interacción / accesibilidad | §29.4, `08-FRONTEND-PLAN.md` §9 |
| `ADR-nnn` | Decisión arquitectónica | `15-ADR.md` |
| `EPIC-` / `STORY-` / `TASK-` | Backlog | `13-BACKLOG.md` |
| `F<n>.<m>-<k>` | Tarea del plan de implementación | `11-IMPLEMENTATION-PLAN.md` |
| `T0` … `T6` | Hilos del sistema (`T0` = interfaz, hilo principal) | `04-ARCHITECTURE.md` §4 |
| `L0` … `L3` | Capas de almacenamiento | `05-DATA-MODEL.md` §4 |
| `P1` … `P6` | Principios de producto | `01-REQUIREMENTS.md` §4.1 |
| `F1` … `F6` | Fuerzas arquitectónicas | `04-ARCHITECTURE.md` §1 |
| `[Sn]` | Fuente | `01-REQUIREMENTS.md` §37 |

---

## 6. Términos que este proyecto usa con un sentido preciso

Palabras habituales cuyo significado aquí es más estrecho de lo normal. Merece la pena tenerlas presentes.

| Término | En este proyecto significa |
|---|---|
| **Sesión** | Una sesión del juego identificada por `sessionUID`, no una sesión de usuario ni una conexión |
| **Evento** | Un `Event Packet` del protocolo, no un mensaje interno cualquiera |
| **Canal** | Ambas cosas, según el contexto: un flujo del transporte interno, o un `Channel<T>` del dominio. El contexto siempre lo desambigua |
| **Frame** | Fotograma del juego, no una trama de red |
| **Captura** | Un fichero `.f1raw`, no una captura de pantalla |
| **Formato** | El formato del **protocolo UDP** (2025 o 2026), no un formato de fichero |
| **Restringido** | Específicamente, oculto por el ajuste *Your Telemetry* de otro jugador |
| **Shell** | La capa de aplicación de escritorio: ventana más WebView2. **No** un intérprete de órdenes |
| **Ventana** | La ventana de la aplicación. **No** la «ventana de parada» de la estrategia |
| **Transporte** | El cable interno entre núcleo e interfaz. **No** una API pública |
| **Rama** | Línea temporal creada por un flashback, no una rama de Git |
| **Trazado** | La geometría del circuito reconstruida desde telemetría |
| **Trazada** | La línea concreta que recorre un piloto en una vuelta |
| **Derivado** | Calculado de forma determinista. **No** es sinónimo de estimado |
| **Estimado** | Resultado de un modelo con incertidumbre. **Siempre** lleva su margen o su calidad de ajuste |

La distinción entre **trazado** y **trazada**, y entre **derivado** y **estimado**, aparece constantemente en la documentación y en la interfaz. Confundirlas cambia el significado de lo que se muestra al usuario.

---

## Sources

### Official

| Título | Uso |
|---|---|
| `Data Output from F1® 25: 2026 Season Pack` v1.2 | Definiciones de todos los términos del protocolo marcados `[S1]` |
| `Data Output from F1® 25 Game` v3 | Definiciones específicas del formato 2025 |

Consultadas el 11 de septiembre de 2026.

---

*Fin de `PW-GLO-018` v1.1*
