# Cómo jugar online

## Antes de empezar

- **F1** abre y cierra las pantallas online. **F2** abre las opciones. En el menú principal del juego, la opción
  **ONLINE (F1)** te lo recuerda.
- Con el mando: **BACK + START** abre las opciones y **BACK + Y** (BACK + triángulo en PlayStation) las pantallas
  online. Adentro, el stick o la cruceta se mueven, **A** (cruz) elige, **LB / RB** cambian de pestaña, **LT / RT**
  suben y bajan de a una página en las listas largas (salas, chat, replays, opciones) y **B** (círculo) cierra. BACK es el botón chico de la izquierda (View en Xbox, Share o Create en PlayStation, - en Switch).
- La primera vez que abrís el juego, cuatro pasos te dejan listo: el **idioma** del juego, tu **nombre de usuario**
  para el online (de 2 a 20 letras: el que ven los demás en las salas y en el chat), la **calidad de los gráficos**
  (ya viene elegida para tu PC) y una **prueba del mando** (se encienden los botones que apretás). Se pueden saltar, y
  todo se cambia después en F1 y F2. Si saltaste el nombre, la ventana online se abre sola y te lo pide; se puede
  cambiar cuando quieras con "Cambiar".

## Encontrar rival

En la pestaña **Salas**:

- **Partida rápida**: te une a la sala con el rival más cercano (el menor ping estimado) y, entre las parecidas, a la
  que más tiempo lleva esperando. Si no hay ninguna, abre una tuya para el próximo que busque.
- **Crear sala pública**: aparece en la lista para que cualquiera entre.
- **Crear sala privada**: no aparece en la lista. Pasale el código de 6 letras a tu rival ("Copiar código").
- **Unirse con código**: escribí el código que te pasaron y tocá "Unirse".
- La lista de **salas abiertas** muestra quién espera, de qué país y un **ping aproximado** (calculado por la
  distancia entre los países); tocá "Entrar" en la que quieras. Las salas públicas que están jugando aparecen como
  "jugando", con el botón **Mirar** (ver "Mirar una partida").

Adentro de la sala tenés el **chat de la sala** y la bandera y el récord (victorias - derrotas) de tu rival. Si el juego
está minimizado, la barra de tareas parpadea con un sonido cuando alguien entra a tu sala o se encuentra partida. Cuando los dos tocan **"Estoy listo"**, el juego se vuelve a abrir solo,
ya conectado con tu rival. No toques nada mientras se vuelve a abrir.

Los dos tienen que tener **la misma versión** de mk9-lauta (F2 > Acerca de). Si no, el juego lo avisa al conectar ("tiene
otra versión de mk9-lauta") y vuelve al lobby: actualicen los dos a la última.

En la pestaña **Chat global** se habla con todos los que están en línea; cada uno con la bandera de su país. Tocando un
nombre se abre un menú:

- **Ver perfil**: su foto, su récord (partidas, ganadas, perdidas, efectividad), sus luchadores más usados y sus
  últimas partidas, y cuántas jugaron ustedes dos. El perfil se lo pide tu juego al suyo en el momento (no hay un
  servidor que lo guarde): si está jugando en una sala o tiene una versión anterior, no contesta y la ventana lo dice.
- **Silenciar**: no ves más sus mensajes, sus salas ni su foto, y no puede entrar a tu sala ni mirar tus partidas. Es
  sólo en tu juego; la lista de silenciados está arriba del chat global, para sacar a alguien de ahí.

En la sala, tocando el nombre o la foto del rival también se ve su perfil.

En la pestaña **Perfil** está el tuyo: tu **foto**, tu récord online, tus luchadores más usados y las últimas 30
partidas (con quién, de dónde y con qué luchadores).

### Tu foto de perfil

En **F1 > Perfil**: **Elegir foto...** (cualquier imagen: JPG, PNG, la de tu celular; el juego toma el cuadrado del medio
y la achica) o **Usar mi foto de Discord** (con Discord abierto en la PC). La ven tus rivales en la sala, al lado de tu
nombre en la pelea y en tu perfil, y quienes miran tus partidas. Viaja de tu juego al del otro una sola vez (unos pocos
KB) y queda guardada en el suyo: la próxima vez que se crucen ya no viaja. No queda en ningún servidor y no le cuesta
nada a la pelea (se carga antes). En **F2 > Online** podés ocultar las fotos de los demás, o las fotos y los nombres
en la pelea.

## La pelea

- Los menús son los del juego: cualquiera de los dos puede elegir VERSUS, ONE VS ONE o TWO VS TWO. El otro se suma con
  START en la pantalla de 2 jugadores, y cada uno elige su luchador.
- En la selección de personajes y debajo de las barras de vida aparece la foto, la bandera y el nombre de cada
  jugador. En **F2 > Online** se ocultan las fotos o los nombres en la pelea.
- Arriba a la derecha (en la pelea, abajo al centro) ves contra quién jugás, unas barras con la calidad de la conexión,
  el ping y, en la pelea, el retraso ("vs Rival - 180 ms - retraso 2"). Si la conexión pierde paquetes o varía mucho
  lo dice, y también si va directa o por el relay. Si alguien mira tu partida, dice cuántos ("1 mirando"). En
  **F2 > Online > Cartel de la conexión en las peleas** se elige verlo completo, sólo la señal u oculto (los avisos
  aparecen siempre).
- Con un ping muy alto los menús responden con retraso (esperan los botones del otro): el cartel lo dice ("Conexión
  lenta"), no es que el juego se trabó.
- Si el otro deja de responder, a los 10 s aparece un aviso en el medio de la pantalla con la cuenta regresiva: si no
  vuelve en 20 s, la sesión termina y volvés al lobby.
- Las peleas usan **rollback**: si la conexión tiene un salto, el juego se corrige solo sin frenar. Los menús y la
  intro de cada pelea van sincronizados con un retraso según el ping, así no se traban.
- En la pelea online **no hay pausa** (START no pausa).
- Al terminar la partida, el que perdió elige en CONTINUE?: REMATCH, otro luchador o volver al menú.

## Si no te conecta al online

Si arriba de la ventana online dice **"Sin conexión con el lobby: reintentando"**, debajo explica la causa y qué
probar:

- **Falla el DNS** (tu conexión no encuentra el servidor; pasaba con algunos proveedores, como Claro): desde la 0.5.0
  el juego se conecta igual, directo a la dirección del servidor. Si aun así no conecta, cambiá el DNS de Windows a
  1.1.1.1 y 8.8.8.8, o probá con otra red (los datos del celular).
- **La conexión no llega** (firewall, antivirus o el proveedor de internet): probá con otra red o una VPN.
- **La conexión segura no se pudo verificar**: revisá la fecha y la hora de la PC y que Windows esté actualizado.

**"Copiar diagnóstico"** copia los datos del error para mandarlos en el Discord.

## Mirar una partida

En la lista de salas, las públicas que están jugando tienen el botón **Mirar**. El juego se vuelve a abrir mirando esa
partida desde el principio: la juega igual que los jugadores, con sus botones. Si llegás tarde se pone al día rápido
(sin imagen ni sonido un momento), en los menús y también en medio de una pelea, y después la seguís unos dos segundos
detrás. Mientras carga una pelea va a velocidad normal. Arriba a la derecha dice a quién mirás y cuánto vas detrás del
vivo. Para salir: F1, "Dejar de mirar". Cada sala admite hasta 4 espectadores.

Si la sesión ya lleva mucho tiempo, alcanzar el vivo tarda unos minutos (se juega todo desde el principio, más
rápido): mientras tanto la imagen avanza a saltos y el cartel dice cuánta partida falta y cuánto tarda más o menos. La
lista de salas muestra hace cuánto está abierta cada una.

## Replays

La grabación viene **apagada**: se activa en **F1 > Replays** ("Grabar mis sesiones online para verlas después") o
en **F2 > Online > Grabar replays**, y vale desde la próxima sesión. No graba video: guarda las entradas de los dos
jugadores (unos 3 KB por segundo, en la carpeta `logs`; las últimas 10 sesiones), así que no usa internet ni se nota
en el juego. En **F1 > Replays** están por fecha, con el rival, y **Ver** vuelve a abrir el juego jugando esa sesión
desde el principio: los menús pasan rápido (sin imagen ni sonido un momento) y las peleas se ven a su ritmo, con
sonido. Al terminar, "Volver al lobby". Un replay de otra versión de mk9-lauta no se puede ver (el juego lo avisa).

## Salir

- F1, pestaña **Sala**, "Terminar la sesión y volver al lobby".
- Si tu rival se va o se corta la conexión, el juego te avisa y vuelve solo al lobby en 10 segundos.
- Cerrar la ventana (la X o Alt+F4) pregunta antes: "Seguir jugando" o "Cerrar el juego".

## Controles

Se juega con mando o con teclado. Los mandos de Xbox, PlayStation y Switch Pro andan apenas se conectan; el primero que
se conecta es el Jugador 1 y el segundo el Jugador 2. Los mandos vibran como en la consola; la vibración se apaga en
**F2 > Controles**.

El teclado ya viene listo para pelear:

| Acción | Tecla | Botón del mando |
|---|---|---|
| Moverse, saltar, agacharse | W A S D o las flechas | stick izquierdo |
| Puño frontal (1) | U | X |
| Puño trasero (2) | I | Y |
| Patada frontal (3) / aceptar en los menús | J | A |
| Patada trasera (4) / volver en los menús | K o Retroceso | B |
| Agarre | O | RB |
| Cubrirse | L o Espacio | RT |
| Cambio de guardia | Ñ (la tecla a la derecha de la L) | LT |
| Relevo (peleas por equipos) | P | LB |
| Pausa / Start | Enter o Esc | Start |

El X-Ray es Cubrirse + Cambio de guardia (L + Ñ). Cada tecla se cambia en **F2 > Controles**: "Cambiar" y apretá la
tecla nueva. Para jugar un Versus en la misma PC con el teclado y un mando, poné "El teclado maneja al: Jugador 2". En
el online manejás a tu luchador con el teclado y con cualquier mando conectado.

Qué golpe hace cada botón se cambia en el menú del juego, **OPCIONES > MANDO**, y el teclado lo sigue.

Para practicar combos: **F2 > Controles > Mostrar las entradas en la pelea**. A la izquierda aparecen tus direcciones
(flechas) y botones, lo último arriba, con cuántos cuadros duró cada uno, en la notación de MK con los botones de
siempre: **1** puño frontal, **2** puño trasero, **3** patada frontal, **4** patada trasera, **BL** cubrirse, **TH**
agarre, **SS** cambio de guardia y **TAG** relevo. Sirve en cualquier pelea (Práctica, Versus, online: ahí, las tuyas);
el juego no se entera.

Los botones que muestra el juego (en los menús y en la lista de movimientos) son los de tu mando: el juego mira el
primer mando conectado al abrirse y muestra los botones de PlayStation (cruz, círculo, cuadrado, triángulo), de
Switch o de Xbox; sin mando, muestra tus teclas. En **F2 > Controles > Íconos de los botones** se fija uno; se aplica
al volver a abrir el juego.

## Opciones (F2)

- **Calidad**: un preset para todo de una vez. **Muy baja** (480p y, si la PC no llega a 60, se dibuja uno de cada dos
  cuadros), **Baja** (480p, para PCs con poca placa de video), **Consola** (720p, como en la Xbox 360), **Alta** (1080p)
  y **Ultra** (1440p). La primera vez que abrís el juego se elige solo según tu placa de video y tu memoria. Cada
  opción se puede ajustar sola; los cambios se aplican al momento y quedan guardados.
- **Gráficos**: resolución interna (480p a 4K), sombras, oclusión ambiental, filtrado de texturas, precisión de los
  efectos, antialiasing (MSAA), **resolución dinámica**, **cuadros dibujados** y **ahorro de memoria** (ver abajo).
- **Pantalla e idioma**: pantalla completa o ventana (también con **F11** o **Alt+Enter** en cualquier momento),
  **mostrar FPS** (arriba a la izquierda), la **API gráfica** (DirectX 12 o Vulkan: si el juego anda mal o se ve raro
  con una, probá la otra; se aplica al volver a abrir el juego) y el idioma del juego: español (con el Modo Historia
  doblado), inglés, portugués, francés, alemán o italiano. El idioma se aplica al volver a abrir el juego. También:
  el **monitor** donde se abre (si tenés más de uno), el **tamaño de la ventana** y el **tamaño de los menús F1 y
  F2** (Automático los agranda en pantallas 1440p o 4K y sigue la escala de Windows, por ejemplo 150 % en una
  notebook). En un monitor ultrapanorámico (21:9) el juego se ve en 16:9 con barras a los costados, como en la consola.
- **Controles**: las teclas, qué jugador maneja el teclado, la vibración del mando y los íconos de los botones.
- **Online / Retraso de entrada en las peleas**: **Automático** (recomendado) o de 0 a 7 cuadros. Cada jugador elige
  el suyo. En Automático es 1 cuadro con ping bajo y el juego lo sube solo con ping alto (unos 2 cuadros con 180 ms, 3
  con 220 ms, 4 con 260 ms, hasta 7 con más de 340 ms) para que los personajes no "salten" cuando llegan los botones
  del otro, y lo sigue ajustando toda la pelea si el ping cambia. Con un número fijo tus golpes salen siempre con ese
  retraso.
- **Kratos**: instalar o desinstalar a Kratos (ver abajo).
- **Acerca de**: tu versión, buscar una versión nueva, abrir la carpeta de logs y reportar un problema.

## Kratos

Kratos (el invitado de la versión de PS3) se puede agregar con el port de la comunidad para Xbox 360
([eLGatoRDV/mk9-kratos-xbox360](https://github.com/eLGatoRDV/mk9-kratos-xbox360)): modelo, combos, especiales, X-Ray,
las dos Fatalities, Babality, los dos trajes y su sonido. No viene con el juego:

1. Bajá **Kratos-MK9-360-Share.zip** de la página del port (**F2 > Kratos > Bajar el port** te lleva).
2. **F2 > Kratos > Instalar desde el zip…** y elegí ese zip. El juego lo instala en tus archivos del juego (guarda una
   copia de los dos que cambia) y te pide reiniciar.
3. Kratos aparece en la selección de personajes.

**Desinstalar** lo saca al reiniciar y deja los archivos como estaban. Para jugar online con Kratos tu rival también
tiene que tenerlo instalado (y si no lo tiene ninguno, igual): la sala avisa si no coinciden y la pelea no arranca. Lo
mismo para mirar una partida.

## Actualizaciones

Al abrirse, el juego se fija si hay una versión nueva en GitHub. Si la hay, aparece un aviso: **"Actualizar ahora"**
la baja (comprobando que sea la oficial, con su firma) y **"Reiniciar ahora"** la instala y vuelve a abrir el juego.
Tus partidas, tus opciones y los archivos del juego no se tocan. "Más tarde" la deja para otro momento. En una sesión
online el aviso no aparece. En **F2 > Acerca de** se busca a mano o se apaga la búsqueda al iniciar.

Si tenés la versión 0.1.0, bajá la 0.2.0 a mano una vez (la 0.1.0 no tenía actualizador): descomprimila en la misma
carpeta, reemplazando los archivos, y desde ahí se actualiza sola.

## Si algo falla

En **F2 > Acerca de > Abrir la carpeta de logs** está todo lo que pasó: mandá el último `mk9_*.log` (y el
`crash_*.dmp` de la misma hora si el juego se cerró de golpe) en "Reportar un problema". Si en una partida online
los dos vieron cosas distintas, cada uno tiene su `session_<fecha>_p1.mk9net` o `_p2.mk9net` de esa sesión: con los
dos archivos se ve en qué cuadro se separaron los juegos.

## PCs con poca placa de video o poca memoria

- Elegí la calidad **Muy baja** o **Baja** y activá **Mostrar FPS** para ver cómo va.
- **Resolución dinámica**: si la PC no llega a 60 (un X-Ray, muchos efectos), baja la resolución interna un escalón
  (1080p, 720p, 480p) y la vuelve a subir cuando sobra; nunca pasa de la que elegiste. Viene activada en Alta y
  Ultra. Con "Cuadros dibujados: Automático", primero baja la resolución y recién en 480p saltea cuadros.
- **Cuadros dibujados**: "Todos" dibuja los 60 cuadros por segundo. "Uno de cada dos" dibuja 30 y el juego sigue a 60
  (la mitad del trabajo de la placa de video; la pelea va a la misma velocidad). "Automático" dibuja todos mientras la
  PC llega y pasa a uno de cada dos cuando no, por ejemplo con muchos efectos o en un X-Ray. Con el contador de FPS
  activado, cuando se saltean cuadros ves "30 FPS (juego a 60)".
- **Ahorro de memoria**: para PCs con 4 a 6 GB de RAM; se activa solo en ellas. Libera las texturas que no se usan.
- Cerrá los programas que no uses mientras jugás (el navegador, sobre todo), y más en una notebook con gráficos
  integrados: comparten la placa de video con el juego.
- En el online, las peleas siguen a 60 aunque se dibujen menos cuadros: el rival no nota nada.
- Para jugar online conviene el cable de red o un Wi-Fi estable: con más de 250 ms de ping la pelea se corrige seguido
  y los personajes "saltan" aunque el juego suba el retraso.
