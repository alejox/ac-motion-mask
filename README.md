# AC Motion Mask

Filtro para OBS Studio que une **máscaras** y **animación** en un solo lugar, pensado para configurarse en segundos.

A single OBS Studio filter that combines **masks** and **motion** — set up in seconds.

## Qué hace

- **Máscaras:** rectángulo redondeado, círculo, elipse, imagen (alfa o brillo) y degradado, con borde suave e inversión.
- **Animaciones de entrada y salida:** fundido, zoom, crecer máscara, deslizar (4 direcciones) y barrido (4 direcciones).
- **Estilos de movimiento:** lineal, suave, con rebase, elástico y rebote.
- **Disparo automático** cuando la fuente aparece, o manual con atajos de teclado (entrada, salida, alternar).
- Botones **Probar entrada / Probar salida** dentro del filtro.
- **Panel AC Motion:** guarda tantas posiciones como quieras para cualquier fuente (posición, tamaño, rotación y recorte) y muévela entre ellas en vivo con animación.

## Uso

1. Clic derecho en una fuente → **Filtros** → **+** en *Filtros de efecto* → **AC Motion Mask**.
2. Elige la **forma** de la máscara.
3. Elige la animación de **entrada** y **salida**. Listo.

Los atajos se asignan en *Ajustes → Atajos*, bajo el nombre de la fuente.

### Mover la fuente por la pantalla (panel AC Motion)

1. Abre el panel en *Paneles → AC Motion*.
2. Selecciona la fuente en la escena, colócala donde quieras y pulsa **Guardar posición**. Repite para cada lugar.
3. En vivo: doble clic en una posición, **Mover aquí**, ◀ ▶ o los atajos *AC Motion: ir a la posición 1–9 / siguiente / anterior*.

- **Fijar** mantiene el panel en esa fuente aunque selecciones otra.
- Cada escena guarda sus propias posiciones; en Modo Estudio se mueve la fuente en Programa usando el mismo nombre de posición.
- Clic derecho en una posición para renombrarla, actualizarla o eliminarla.

> Nota: la animación ocurre dentro del área de la propia fuente. La salida se dispara con atajo o botón (OBS no avisa a un filtro antes de ocultar la fuente).

## Instalación

Descarga el instalador desde [Releases](../../releases) (macOS `.pkg`, Windows `.exe`, Ubuntu `.deb`).

## Compilar

Basado en [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

```sh
cmake --preset macos
cmake --build --preset macos
```

## Licencia

GPL-2.0-or-later — por [alejoxcode](https://alejoxcode.com)
