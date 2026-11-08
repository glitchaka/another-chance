# Another Chance

Navegador web **nativo para Windows, escrito en C++20**, desarrollado sin Chromium, Blink, Gecko, Firefox, WebView2, Electron ni componentes de otros navegadores.

El proyecto reutiliza y adapta estructuras del código fuente de **DOOM 3 BFG Edition** (id Software), disponible en [DOOM-3-BFG-GES](https://github.com/glitchaka/DOOM-3-BFG-GES), pero implementa desde cero la obtención HTTPS, análisis HTML, estilos, layout y presentación gráfica.

> **Alcance**: es una primera implementación experimental de un motor web, no una alternativa completa a Chrome o Firefox. Sin motor JavaScript, DOM dinámico, CSS avanzado ni multimedia. Consulta los límites antes de usarlo para navegación cotidiana.

## Compilación (Windows)

Requiere Windows 10/11, Visual Studio 2022 con C++ Desktop y CMake 3.21+.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Ejecutable: `build/Release/AnotherChance.exe`.

## Arquitectura

- `src/tech/`: adaptación real de contenedores **idTech** (cola intrusiva).
- `src/net/`: acceso HTTP/HTTPS vía WinHTTP nativo de Windows (TLS mediante Schannel; no hay motor web externo).
- `src/html/`: parser HTML y árbol de nodos propios.
- `src/style/`: parser y aplicación de un subconjunto CSS.
- `src/layout/`: composición de texto y regiones clicables.
- `src/window/`: ventana, barra de navegación, historial, desplazamiento y rasterizado por GDI.
- `src/util/`: Unicode y resolución de URL.

## Estado funcional inicial

Direcciones HTTP/HTTPS, búsqueda desde la barra, navegación por enlaces, atrás/adelante/recargar, descarga asíncrona, HTML básico, un subconjunto de CSS en línea e interno, y renderizado de texto y separadores. No interpreta JavaScript ni promete renderizar correctamente todos los sitios modernos.

## Licencias

Código derivado de DOOM 3 BFG: GPLv3 con los términos adicionales y exclusiones detallados en `COPYING.txt`; se mantiene la atribución a id Software. El código nuevo del repositorio se distribuye bajo GPLv3. Los componentes de Windows se enlazan mediante sus API públicas y no se copian sus implementaciones.
