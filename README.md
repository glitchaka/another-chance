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
- `src/window/`: ventana Win32, pestañas funcionales (recargan al cambiar), historial independiente, barra de navegación y chrome vectorial original dibujado con GDI+.
- `src/util/`: Unicode y resolución de URL.

## Estado funcional inicial

Direcciones HTTP/HTTPS, introducción de direcciones en la barra, navegación por enlaces, atrás/adelante/recargar, descarga asíncrona, HTML básico, CSS en línea, interno y externo (subconjunto), imágenes PNG/JPEG/GIF y renderizado propio de texto y separadores. No interpreta JavaScript, no admite formularios interactivos y no promete renderizar correctamente todos los sitios modernos.

La compilación x64 y los tests de HTML, CSS y URL se verifican con GitHub Actions. Además, `BrowserEndToEnd` inicia la aplicación Windows real, visita un servidor HTTP local, comprueba la carga de HTML, CSS e imágenes, sigue un enlace y utiliza el historial. No implica compatibilidad con toda la web moderna.

El ejecutable Release utiliza la biblioteca de ejecución de MSVC enlazada estáticamente; no requiere instalar por separado Visual C++ Redistributable.

## Licencias

Código derivado de DOOM 3 BFG: GPLv3 con los términos adicionales y exclusiones detallados en `COPYING.txt`; se mantiene la atribución a id Software. El código nuevo del repositorio se distribuye bajo GPLv3. Los componentes de Windows se enlazan mediante sus API públicas y no se copian sus implementaciones.

## Interfaz

Diseño nativo renovado inspirado en las referencias proporcionadas: franja superior de pestañas, botones vectoriales minimalistas y barra de dirección redondeada sobre fondo claro. No usa HTML/CSS para la interfaz. Puede abrir hasta cinco pestañas independientes (las páginas se recargan al volver a una pestaña). El marcador de estrella se conserva durante la sesión, no en disco. Accesos: Ctrl+T, Ctrl+W, Ctrl+L, Alt+flechas, F5.

## Ventana y diseño propio

La ventana usa un marco Win32 dibujado íntegramente en el cliente, sin barra de título ni botones del sistema. Es redimensionable desde los bordes, arrastrable desde las zonas libres de su encabezado, maximizable y minimizable mediante controles propios. El encabezado de una sola franja distingue a Another Chance de las barras de pestañas de Chromium: identidad visual a la izquierda, búsqueda central y selector de vistas a la derecha. El selector permite alternar entre hasta cinco vistas.

## Estado real del motor y procedencia del código

- **Código adaptado de DOOM 3 BFG**: `neo/idlib/containers/Queue.h` → `src/tech/IdQueue.h` (cola de solicitudes), `neo/idlib/containers/Stack.h` → `src/tech/IdStack.h` (parser HTML), `neo/renderer/ScreenRect.h/.cpp` → `src/tech/IdScreenRect.h` (recorte y hit testing en el layout).
- **Todavía NO integrado**: el rasterizador GPU de idTech, su sistema de ventanas, comandos de renderizado, shaders y toda su infraestructura de materiales. El contenido de las páginas se sigue rasterizando con GDI de Windows, no con el motor gráfico de Doom. La integración completa requiere adaptar componentes de renderizado sin arrastrar dependencias del juego.
- **Implementación original limitada**: cliente de HTTP/HTTPS mediante WinHTTP, parser HTML parcial, estilos CSS parciales, layout de texto, imágenes mediante WIC y GUI Win32/GDI+.
- **Sin motores de navegador externos**: no se integra DuckDuckGo Browser ni Chromium, Firefox, Blink, Gecko o WebView2. Se ha eliminado la redirección automática a DuckDuckGo Lite; el texto que no sea una URL válida ahora se rechaza. **La búsqueda web sigue pendiente**, no se ha implementado un índice o proveedor propio.
- **No es un navegador de uso general**: carece de ejecución de JavaScript, CSS moderno, formularios operativos, almacenamiento de sesión, navegación segura suficientemente evaluada y compatibilidad web amplia. No utilizar para autenticación, operaciones bancarias ni información sensible.

Este estado es intencionadamente explícito para no confundir una prueba de compilación o una página HTML simple con un navegador completo.

## Repintado y desplazamiento

La rueda del ratón invalida exclusivamente el rectángulo del documento y conserva los incrementos parciales de dispositivos de alta resolución. La superficie afectada se dibuja primero en un bitmap compatible y se copia al cliente en una sola operación. La cabecera solo se vuelve a dibujar cuando cambia su propio estado o el tamaño de la ventana; esto elimina el repintado completo innecesario que producía parpadeo al desplazarse.

## Referencias de estilo verificadas

Se revisaron las fuentes de KDE Breeze (`kdecoration/breeze.h`), GNOME libadwaita (`src/stylesheet/widgets/_header-bar.scss`) y Linux Mint Mint-Y (`src/Mint-Y/gtk-3.0/gtk.css`). El encabezado propietario de Another Chance se redujo de 78 a 56 píxeles, eliminó el símbolo decorativo dentro de la barra de dirección, el monograma grande y los botones coloreados, y emplea márgenes compactos y colores neutros. Son referencias de diseño, no dependencias ni código copiado de dichos proyectos.


## Resolución de la barra de direcciones

Se distingue entre una dirección y una búsqueda, sin incorporar motores de otros navegadores.
Las direcciones `https://sitio.cl`, `sitio.cl`, `sitio.cl/ruta`, `localhost:8080` y `127.0.0.1:8080` se normalizan automáticamente (HTTPS por defecto para dominios; HTTP para la red local).
Las palabras o frases sin dirección se envían al buscador configurado en el menú: Google (predeterminado), Bing o búsqueda desactivada. El servicio de búsqueda es externo y recibe los términos; **el motor de representación de páginas continúa siendo propio y experimental**, sin JavaScript ni compatibilidad CSS moderna. La selección del buscador no persiste tras cerrar.

## Icono de aplicación

El ejecutable incorpora el recurso ICON (ID 101) en `assets/anotherchance.ico`, con tamaños 16/24/32/48/64/128/256. Win32 lo carga como icono grande y pequeño de la clase; Windows Explorer y la barra de tareas reciben el icono incrustado en el .exe. El icono es una identidad original de Another Chance: dos rutas divergentes, sin monogramas ni letras, con una señal de dirección verde agua y un punto de partida cálido sobre fondo azul profundo. La prueba `BrowserEndToEnd` verifica que el EXE tenga un icono real y que la ventana registre su HICON.


## Pantalla de inicio y cabecera

La pantalla de inicio ya no muestra textos de demostración ni enlaces a páginas de prueba: abre un documento vacío, deja el foco en la barra de direcciones y permite navegar o buscar inmediatamente. La cabecera reserva el ancho necesario para escribir "Another Chance" sin truncarlo. El icono del encabezado se dibuja con el mismo recurso HICON del ejecutable, sin duplicar el diseño con trazos manuales.


## ScriptRuntime (etapa limitada)

Se añadió `src/script/` como módulo nativo independiente. Conserva el texto de scripts
inline y permite operaciones de cadenas literales sobre `document.title`,
`document.write`, `document.body.innerHTML/textContent`,
`document.getElementById(...).innerHTML/textContent` y selectores por ID.
Opera sobre el árbol HTML antes del layout y tiene límites de operaciones y bytes.
**No es un intérprete ECMAScript completo**: faltan ejecución de JS general,
`fetch`, promesas, event loop, módulos, scripts externos y un DOM estándar.
Por tanto, páginas modernas como Archive.org siguen sin compatibilidad completa.


## Recursos JavaScript externos (primera etapa)

`BrowserWindow` resuelve hasta cuatro etiquetas `script src` HTTP(S) y descarga
cada archivo con WinHTTP, con un máximo de 256 KiB por recurso; se ejecutan en
orden del documento las operaciones literales reconocidas por `ScriptRuntime`.
No es ejecución ECMAScript general ni implica compatibilidad con aplicaciones
web actuales basadas en grandes bundles JavaScript o con interacciones complejas.


## Interfaz de pestañas (noviembre 2026)

Se sustituyó la barra monolítica por dos filas compactas: una fila superior
de 34 px con pestañas reales (título, selección, cerrar y nueva pestaña)
y controles de ventana dibujados dentro de la aplicación; una fila de navegación
de 42 px con Atrás / Adelante / Recargar, dirección y acciones de página.
La fila superior permite arrastrar la ventana por las zonas libres.
No se usan decoraciones del sistema, Chromium, GTK ni Qt.


## Criterio de publicación y pruebas con sitios reales

La compilación no se considera un navegador funcional hasta que las pruebas
**Google Search** e **Internet Archive** puedan cargar, representar e interactuar
con controles reales, no solo recibir HTTP 200. `AnotherChanceSmoke --live`
abre ambas páginas y comprueba la existencia de controles interactivos, el
renderizado visible y el estado de red. El workflow publica el ejecutable
solo cuando superan esa validación. Las capturas de diagnóstico se conservan
también ante un fallo. La prueba requiere acceso a Internet desde el runner
de Windows; un bloqueo externo debe investigarse antes de declarar
compatibilidad, y nunca debe convertirse en un falso resultado positivo.


## Controles HTML representados en el layout

El layout crea una lista separada de ubicaciones de controles `<input>`, `<textarea>`, `<button>` y `<select>`; no vuelve a sustituirlos por textos entre corchetes. El administrador nativo `src/forms` se encarga de representar esos controles y conservar sus valores al cambiar el ancho o desplazar el documento. Las entradas ocultas siguen fuera de pantalla.


## Formularios HTML nativos

Las etiquetas `input`, `textarea`, `button` y `select` ahora se dibujan mediante controles Win32 propios y son editables. Se conservan sus valores al desplazar y recalcular el layout, se incluye la información de campos `hidden` al enviar GET y se implementan los botones submit/reset. Enter en un campo de texto también solicita el envío. El motor todavía no implementa POST, cookies de sesión completas, eventos de JavaScript ni todos los requisitos HTML5.
