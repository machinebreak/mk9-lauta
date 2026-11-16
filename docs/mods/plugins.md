# Plugins de código

Una forma oficial de cargar mods de código nativo (por ejemplo, un mod de accesibilidad que lee los menús en voz alta)
sin envolver las DLL del juego con una `version.dll` falsa, que los antivirus suelen borrar.

## Para jugadores

1. Creá una carpeta `plugins` al lado de `mk9.exe` (en Linux, al lado de `mk9`) y poné adentro el `.dll` del plugin
   (en Linux, el `.so`).
2. Abrí `mk9.toml` (al lado del ejecutable) con un editor de texto y agregá esta línea:

   ```toml
   mk9_plugins = true
   ```

3. Abrí el juego de nuevo.

Viene **apagado**. Un plugin es código de otra persona que corre con el mismo acceso que el juego: usá sólo plugins en
los que confíes. Cada plugin que se carga, y cada función del juego que reemplaza, queda anotado en el log
(`logs/mk9_*.log`), y **F2 > Acerca de** muestra los que están cargados.

En el online: un plugin que sólo lee (como un lector de pantalla) no cambia nada. Uno que cambie la pelea hace que los
dos juegos dejen de coincidir y la sesión se corta.

## Para quien hace plugins

Copiá [`app/src/rex_mod.h`](../../app/src/rex_mod.h) a tu proyecto. Es la misma interfaz que los plugins del port de
DBZ Budokai 3 HD Collection (también hecho con ReXGlue), así que un mismo plugin puede servir para los dos. Exportá,
con enlace C:

```c
void rex_mod_init(const RexModHost* host);  // obligatoria
void rex_mod_shutdown(void);                // opcional
```

- `rex_mod_init` se llama una vez, con la imagen del juego ya cargada y antes de su primera instrucción.
- Los plugins se cargan en orden alfabético.
- `rex_mod_shutdown` se llama cuando el juego se cierra normalmente.
- `host` vale toda la partida. Revisá `api_version` y `struct_size` antes de usar campos que se agreguen después: los
  campos nuevos sólo se suman al final.
- `guest_base + A` es la dirección `A` del juego. La memoria del juego es big-endian.
- `get_function(A)` devuelve la función recompilada de la dirección `A` del juego, o NULL. Para engancharte a todas
  las llamadas, desviá ese puntero con tu propia librería de hooks: el código recompilado se llama directamente.
- `set_function(A, f)` reemplaza la entrada de la tabla de funciones de `A`; esa tabla sólo la usan las llamadas
  indirectas (punteros a funciones y vtables).
- `log(plugin, mensaje)` escribe una línea en el log del juego.
- `region` es siempre `"eu"`: MK9 by Lauta usa el disco europeo.

Un ejemplo mínimo está en [`tools/plugins/plugin_example.cpp`](../../tools/plugins/plugin_example.cpp):

```
clang++ -shared -O1 -I app/src -o plugins/plugin_example.dll tools/plugins/plugin_example.cpp
```
