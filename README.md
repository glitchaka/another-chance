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

- `src/tech/`: adaptación real de contenedores **idTech** (cola y pila intrusivas, usadas en el planificador de descargas y en el parser HTML).
- `src/net/`: acceso HTTP/HTTPS vía WinHTTP nativo de Windows (TLS mediante Schannel; no hay motor web externo).
- `src/html/`: parser HTML y árbol de nodos propios.
- `src/style/`: parser y aplicación de un subconjunto CSS.
- `src/layout/`: composición de texto, imágenes y regiones clicables.
- `src/image/`: decodificación PNG/JPEG/GIF mediante WIC nativo de Windows.
- `src/window/`: ventana, barra de navegación, historial, desplazamiento y rasterizado por GDI.
- `src/util/`: Unicode y resolución de URL.

## Estado funcional inicial

Direcciones HTTP/HTTPS, búsqueda desde la barra, navegación por enlaces, atrás/adelante/recargar, descarga asíncrona, HTML básico, CSS en línea, interno y externo (subconjunto), imágenes PNG/JPEG/GIF y renderizado propio de texto y separadores. No interpreta JavaScript, no admite formularios interactivos y no promete renderizar correctamente todos los sitios modernos.

La compilación x64 y los tests de HTML, CSS y URL se verifican con GitHub Actions. Además, `BrowserEndToEnd` inicia la aplicación Windows real, visita un servidor HTTP local, comprueba la carga de HTML, CSS e imágenes, sigue un enlace y utiliza el historial. No implica compatibilidad con toda la web moderna.

El ejecutable Release utiliza la biblioteca de ejecución de MSVC enlazada estáticamente; no requiere instalar por separado Visual C++ Redistributable.

## Licencias

Código derivado de DOOM 3 BFG: GPLv3 con los términos adicionales y exclusiones detallados en `COPYING.txt`; se mantiene la atribución a id Software. El código nuevo del repositorio se distribuye bajo GPLv3. Los componentes de Windows se enlazan mediante sus API públicas y no se copian sus implementaciones.
