# AC Motion Mask

Filtro para OBS Studio que une **máscaras** y **animación** en un solo lugar, pensado para configurarse en segundos.

A single OBS Studio filter that combines **masks** and **motion** — set up in seconds.

## Qué hace

- **Máscaras:** rectángulo redondeado, círculo, elipse, imagen (alfa o brillo) y degradado, con borde suave e inversión.
- **Animaciones de entrada y salida:** fundido, zoom, crecer máscara, deslizar (4 direcciones) y barrido (4 direcciones).
- **Estilos de movimiento:** lineal, suave, con rebase, elástico y rebote.
- **Disparo automático** cuando la fuente aparece, o manual con atajos de teclado (entrada, salida, alternar).
- Botones **Probar entrada / Probar salida** dentro del filtro.
- **Posiciones en pantalla:** guarda tantas posiciones como quieras (lugar, tamaño y forma) y mueve la fuente entre ellas con animación; la forma se transforma en el camino.

## Uso

1. Clic derecho en una fuente → **Filtros** → **+** en *Filtros de efecto* → **AC Motion Mask**.
2. Elige la **forma** de la máscara.
3. Elige la animación de **entrada** y **salida**. Listo.

Los atajos se asignan en *Ajustes → Atajos*, bajo el nombre de la fuente.

### Mover la fuente por la pantalla (posiciones)

Todo se hace en las propiedades del filtro, sección **2 · Posiciones en pantalla**:

1. Coloca la fuente en el lienzo y elige su forma en **1 · Forma**.
2. Pulsa **Guardar como posición nueva**. Repite en otro lugar, con otra forma si quieres.
3. Elige una posición y pulsa **Mover a esta posición**: la fuente viaja y cambia de forma.

En vivo usa los atajos *Ir a la posición 1–9* y *Posición siguiente / anterior* (Ajustes → Atajos, bajo el nombre de la fuente). Cada escena guarda sus propias posiciones.

## Actualizaciones

El plugin revisa al abrir OBS (y cada 12 horas) si hay una versión nueva. Si la hay, muestra sus novedades y ofrece **Actualizar ahora**: la descarga, comprueba su SHA-256 y la instala; solo hay que reiniciar OBS. También puedes buscar a mano en *Herramientas → AC Motion Mask: buscar actualizaciones*.

## Instalación

Descarga desde [Releases](../../releases):

- **macOS:** `ac-motion-mask-<versión>-macos.zip` → copia `ac-motion-mask.plugin` a `~/Library/Application Support/obs-studio/plugins/` (o usa el `.pkg`).
- **Windows:** `ac-motion-mask-<versión>-windows-x64.zip` → copia la carpeta `ac-motion-mask` a `C:\ProgramData\obs-studio\plugins\`.
- **Linux:** `ac-motion-mask-<versión>-linux-x86_64.tar.gz` → descomprímelo en `~/.config/obs-studio/plugins/` (o usa el `.deb`).

Si te sirve, [invítame un café en Ko-fi](https://ko-fi.com/alejoxcode) ☕

## Compilar

Basado en [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

```sh
cmake --preset macos
cmake --build --preset macos
```

## Licencia

GPL-2.0-or-later — por [alejoxcode](https://alejoxcode.com)
