#!/usr/bin/env python3
"""Verificador del grafo de dependencias de PitWall (F0-13, RNF-084).

Comprueba, leyendo los `#include` reales del codigo, que se cumplen las dos
reglas que se rompen las primeras cuando hay prisa:

  1. El grafo de modulos es UNIDIRECCIONAL y coincide con 09-BACKEND-PLAN.md seccion 2.
     En particular: `pw::domain` no puede incluir nada de `pw::proto`, y
     `pw::state` no puede conocer el protocolo.

  2. Solo `pw::shell` puede incluir cabeceras de Windows. Es lo que mantiene el
     nucleo compilando en Linux (RNF-091) y lo que hace que el shell sea
     sustituible (ADR-002 v2).

Uso:

    python tools/check_deps.py                 # desde la raiz del repositorio
    python tools/check_deps.py --root <ruta>
    python tools/check_deps.py --graph         # imprime el grafo permitido

Devuelve 0 si todo esta en orden y 1 con el detalle de cada infraccion.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# ── Reglas ───────────────────────────────────────────────────────────────────
#
# Para cada modulo, el conjunto de modulos del proyecto que PUEDE incluir.
# Lo que no esta permitido, esta prohibido. El orden refleja el flujo de
# dependencias de 17-PROJECT-STRUCTURE.md seccion 3.3, de lo mas basico a lo mas
# compuesto.
ALLOWED: dict[str, set[str]] = {
    # No depende de nada del proyecto.
    "core":        set(),
    # Infraestructura: solo core.
    "obs":         {"core"},
    "config":      {"core", "obs"},
    "mem":         {"core", "obs"},
    # Protocolo: solo structs, constantes y static_assert. No conoce el dominio.
    "proto":       {"core"},
    # Conoce proto. NO conoce domain.
    "decoder":     {"core", "obs", "mem", "proto"},
    # El modulo mas importante y el mas pequeno: no depende de nada salvo los
    # tipos base (Channel, tipos fuertes) que viven en core.
    "domain":      {"core"},
    # El UNICO que incluye a la vez proto y domain.
    "normalize":   {"core", "obs", "proto", "domain", "decoder"},
    # Conoce domain. NO conoce proto.
    "net":         {"core", "obs", "config", "mem", "recorder"},
    "state":       {"core", "obs", "domain", "normalize"},
    "recorder":    {"core", "obs", "config", "mem", "domain"},
    "persistence": {"core", "obs", "config", "domain", "state"},
    "transport":   {"core", "obs", "config", "domain", "state"},
    "processors":  {"core", "obs", "config", "domain", "state"},
    # Lo compone todo. El unico con vision global.
    "app": {
        "core", "obs", "config", "mem", "proto", "decoder", "domain",
        "normalize", "net", "state", "recorder", "persistence", "transport",
        "processors",
    },
    # El unico modulo especifico de Windows. Nadie depende de el.
    "shell": {
        "core", "obs", "config", "domain", "state", "transport", "app",
    },
}

# Modulos a los que se les permite incluir cabeceras de Windows (ADR-002 v2).
WINDOWS_ALLOWED: set[str] = {"shell"}

# Excepcion acotada para la capa de aislamiento de sockets (F1.1-04, RNF-091).
#
# La regla general dice «solo pw::shell incluye cabeceras de Windows», y sigue
# valiendo para Win32 y WebView2. Pero RNF-091 exige que el nucleo compile en
# Linux «sin API especificas de plataforma FUERA DE UNA CAPA DE AISLAMIENTO», y
# F1.1-04 pide exactamente esa capa: «Winsock e implementacion POSIX tras una
# interfaz comun». Alguien tiene que incluir winsock2.h, y ese alguien es la
# capa de aislamiento.
#
# La excepcion es lo mas estrecha posible:
#   - solo el modulo net;
#   - solo ficheros con sufijo de plataforma (_win32), que ademas solo se
#     compilan en Windows;
#   - solo las cabeceras de sockets. windows.h sigue prohibida en todo el
#     nucleo, tambien aqui.
WINDOWS_HEADER_EXCEPTIONS: dict[str, tuple[re.Pattern[str], set[str]]] = {
    "net": (re.compile(r"_win32\.(cpp|hpp)$"), {"winsock2.h", "ws2tcpip.h"}),
}

# Cabeceras que delatan dependencia de Windows.
WINDOWS_HEADERS = re.compile(
    r"^(windows\.h|windowsx\.h|winsock2?\.h|ws2tcpip\.h|shellapi\.h|shlobj\.h|"
    r"objbase\.h|combaseapi\.h|wrl/.*|wil/.*|webview2\.h|webview2.*\.h|"
    r"dwmapi\.h|shellscalingapi\.h|commctrl\.h|processthreadsapi\.h|"
    r"synchapi\.h|fileapi\.h|handleapi\.h|libloaderapi\.h)$",
    re.IGNORECASE,
)

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

# `#include "pw/<modulo>/..."` o `#include "<modulo>/..."` dentro de core/src.
PW_INCLUDE_RE = re.compile(r"^pw/([a-z_0-9]+)/")

SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".cc", ".cxx", ".ipp"}


class Violation:
    def __init__(self, path: Path, line: int, message: str) -> None:
        self.path = path
        self.line = line
        self.message = message

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.message}"


def module_of(path: Path, root: Path) -> str | None:
    """Modulo al que pertenece un fichero, o None si esta fuera del nucleo."""
    try:
        relative = path.relative_to(root)
    except ValueError:
        return None

    parts = relative.parts
    # core/src/<modulo>/...
    if len(parts) >= 3 and parts[0] == "core" and parts[1] == "src":
        return parts[2]
    # core/include/pw/<modulo>/...
    if len(parts) >= 4 and parts[0] == "core" and parts[1] == "include" and parts[2] == "pw":
        return parts[3]
    # core/apps/<lo que sea>/...  -> se comporta como app
    if len(parts) >= 2 and parts[0] == "core" and parts[1] == "apps":
        return "app"
    return None


def included_module(include: str) -> str | None:
    """Modulo de PitWall al que apunta un include, si apunta a alguno."""
    match = PW_INCLUDE_RE.match(include.replace("\\", "/"))
    if match:
        return match.group(1)
    return None


def _windows_exception(module: str, path: Path, header: str) -> bool:
    """Cierto si este fichero concreto puede incluir esta cabecera de Windows."""
    entry = WINDOWS_HEADER_EXCEPTIONS.get(module)
    if entry is None:
        return False
    pattern, allowed_headers = entry
    return bool(pattern.search(path.name)) and header.lower() in allowed_headers


def check_file(path: Path, root: Path) -> list[Violation]:
    module = module_of(path, root)
    if module is None:
        return []

    violations: list[Violation] = []
    allowed = ALLOWED.get(module)

    if allowed is None:
        violations.append(
            Violation(
                path,
                1,
                f"el modulo '{module}' no esta declarado en tools/check_deps.py. "
                f"Anada sus dependencias permitidas antes de escribir codigo en el.",
            )
        )
        allowed = set()

    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="latin-1")

    for number, line in enumerate(text.splitlines(), start=1):
        match = INCLUDE_RE.match(line)
        if not match:
            continue
        include = match.group(1)

        # Regla 2: cabeceras de Windows.
        header = include.replace("\\", "/").lstrip("./")
        if WINDOWS_HEADERS.match(header) and module not in WINDOWS_ALLOWED and not _windows_exception(
            module, path, header
        ):
            violations.append(
                Violation(
                    path,
                    number,
                    f"el modulo '{module}' incluye la cabecera de Windows '{include}'. "
                    f"Solo pw::shell puede hacerlo: el nucleo debe seguir compilando "
                    f"en Linux (RNF-091, ADR-002 v2).",
                )
            )

        # Regla 1: grafo de modulos.
        target = included_module(include)
        if target is None or target == module:
            continue
        if target not in ALLOWED:
            violations.append(
                Violation(path, number, f"include hacia un modulo desconocido: '{include}'")
            )
            continue
        if target not in allowed:
            violations.append(
                Violation(
                    path,
                    number,
                    f"'{module}' no puede depender de '{target}' (#include \"{include}\"). "
                    f"Dependencias permitidas de '{module}': "
                    f"{', '.join(sorted(allowed)) or 'ninguna'}.",
                )
            )

    return violations


def iter_sources(root: Path):
    for base in (root / "core" / "src", root / "core" / "include", root / "core" / "apps"):
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield path


def print_graph() -> None:
    print("Grafo de dependencias permitido (09-BACKEND-PLAN.md seccion 2):\n")
    for module in ALLOWED:
        dependencies = ", ".join(sorted(ALLOWED[module])) or "- (nada)"
        windows = "  [puede incluir Windows]" if module in WINDOWS_ALLOWED else ""
        print(f"  pw::{module:<12} -> {dependencies}{windows}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Verifica el grafo de modulos de PitWall.")
    parser.add_argument("--root", default=None, help="Raiz del repositorio")
    parser.add_argument("--graph", action="store_true", help="Imprime el grafo permitido y sale")
    arguments = parser.parse_args()

    if arguments.graph:
        print_graph()
        return 0

    root = Path(arguments.root).resolve() if arguments.root else Path(__file__).resolve().parents[1]

    if not (root / "core").is_dir():
        print(f"check_deps: no encuentro 'core/' bajo {root}", file=sys.stderr)
        return 1

    violations: list[Violation] = []
    inspected = 0
    for path in iter_sources(root):
        inspected += 1
        violations.extend(check_file(path, root))

    if violations:
        print(f"check_deps: {len(violations)} infraccion(es) del grafo de dependencias:\n")
        for violation in violations:
            print(f"  {violation}")
        print(
            "\nEl grafo esta en tools/check_deps.py y su justificacion en "
            "09-BACKEND-PLAN.md seccion 2 y 17-PROJECT-STRUCTURE.md seccion 3.3."
        )
        return 1

    print(f"check_deps: {inspected} fichero(s) inspeccionado(s); grafo de dependencias correcto.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
