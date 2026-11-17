// mk9-lauta (2026-11-17): the texts of the windows besides the online ones, in English and Portuguese (i18n.cpp
// includes this file in its table): F2, the first start's installer, the updater, Kratos' download, the game's
// messages (a damaged file, the graphics API, the close question), the keys' and the moves' names. The Spanish text
// is the key: it is the one in the sources, exactly (escapes and all).

      // --- F2: the window and its tabs
      {"Opciones  (F2 para mostrar u ocultar)", {"Options  (F2 to show or hide)", "Opções  (F2 para mostrar ou ocultar)"}},
      {"Gráficos", {"Graphics", "Gráficos"}},
      {"Controles", {"Controls", "Controles"}},
      {"Pantalla e idioma", {"Display and language", "Tela e idioma"}},
      {"Acerca de", {"About", "Sobre"}},
      {"Los cambios se aplican al momento y quedan guardados en mk9.toml.",
       {"Changes apply at once and are saved in mk9.toml.", "As mudanças valem na hora e ficam salvas no mk9.toml."}},
      {"No se pudo guardar en mk9.toml", {"Could not save to mk9.toml", "Não foi possível salvar no mk9.toml"}},
      {"{}: guardado (se aplica al volver a abrir el juego)",
       {"{}: saved (applies when the game is opened again)", "{}: salvo (vale ao abrir o jogo de novo)"}},
      {"{}: guardado", {"{}: saved", "{}: salvo"}},
      {"Calidad:", {"Quality:", "Qualidade:"}},
      {"Calidad {}", {"Quality {}", "Qualidade {}"}},
      {"Para netbooks y PCs con gráficos integrados viejos o 4 GB de RAM: 480p, todo como en la consola y, si la PC no "
       "llega a 60, se dibuja uno de cada dos cuadros (el juego sigue a su velocidad).",
       {"For netbooks and PCs with old integrated graphics or 4 GB of RAM: 480p, everything as on the console and, if "
        "the PC cannot reach 60, one of every two frames is drawn (the game keeps its speed).",
        "Para netbooks e PCs com gráficos integrados antigos ou 4 GB de RAM: 480p, tudo como no console e, se o PC não "
        "chegar a 60, se desenha um a cada dois quadros (o jogo segue na sua velocidade)."}},
      {"Para PCs con poca placa de video: 480p, sombras y efectos como en la consola.",
       {"For PCs with a weak graphics card: 480p, shadows and effects as on the console.",
        "Para PCs com placa de vídeo fraca: 480p, sombras e efeitos como no console."}},

      // --- F2 > Gráficos
      {"Resolución interna", {"Internal resolution", "Resolução interna"}},
      {"480p (la más liviana)", {"480p (the lightest)", "480p (a mais leve)"}},
      {"720p (la de la consola)", {"720p (the console's)", "720p (a do console)"}},
      {"La resolución a la que se dibuja el juego. Bajarla es lo que más ayuda en una PC con poca placa de video.",
       {"The resolution the game is drawn at. Lowering it helps the most on a PC with a weak graphics card.",
        "A resolução em que o jogo é desenhado. Baixá-la é o que mais ajuda num PC com placa de vídeo fraca."}},
      {"Sombras", {"Shadows", "Sombras"}},
      {"Según la resolución", {"By the resolution", "Conforme a resolução"}},
      {"Consola (1x)", {"Console (1x)", "Console (1x)"}},
      {"Nítidas (2x)", {"Sharp (2x)", "Nítidas (2x)"}},
      {"Muy nítidas (3x)", {"Very sharp (3x)", "Muito nítidas (3x)"}},
      {"Máximas (4x)", {"Maximum (4x)", "Máximas (4x)"}},
      {"La resolución de las sombras. \"Consola\" es la más liviana.",
       {"The shadows' resolution. \"Console\" is the lightest.", "A resolução das sombras. \"Console\" é a mais leve."}},
      {"Oclusión ambiental", {"Ambient occlusion", "Oclusão ambiente"}},
      {"Media resolución (consola)", {"Half resolution (console)", "Meia resolução (console)"}},
      {"Resolución completa", {"Full resolution", "Resolução completa"}},
      {"Las sombras de contacto entre objetos.",
       {"The contact shadows between objects.", "As sombras de contato entre objetos."}},
      {"Filtrado de texturas", {"Texture filtering", "Filtragem de texturas"}},
      {"El del juego", {"The game's", "O do jogo"}},
      {"Texturas más nítidas vistas de costado (el piso). Cuesta poco.",
       {"Sharper textures seen at an angle (the floor). It costs little.",
        "Texturas mais nítidas vistas de lado (o chão). Custa pouco."}},
      {"Brillos y desenfoque", {"Glow and blur", "Brilho e desfoque"}},
      {"Como en la consola", {"As on the console", "Como no console"}},
      {"Más precisos", {"More precise", "Mais precisos"}},
      {"La precisión del brillo (bloom), la profundidad de campo y la oclusión ambiental.",
       {"The precision of the glow (bloom), the depth of field and the ambient occlusion.",
        "A precisão do brilho (bloom), da profundidade de campo e da oclusão ambiente."}},
      {"Cuadros dibujados", {"Frames drawn", "Quadros desenhados"}},
      {"Todos (60 por segundo)", {"All (60 a second)", "Todos (60 por segundo)"}},
      {"Automático (la mitad si la PC no llega)",
       {"Automatic (half if the PC cannot keep up)", "Automático (a metade se o PC não der conta)"}},
      {"Uno de cada dos (30 por segundo)", {"One of every two (30 a second)", "Um a cada dois (30 por segundo)"}},
      {"Con poca placa de video: el juego sigue a su velocidad (la pelea a 60 cuadros por segundo) y se dibuja uno de "
       "cada dos, la mitad de trabajo para la placa. \"Automático\" lo hace sólo mientras la PC no llega a 60.",
       {"With a weak graphics card: the game keeps its speed (the fight at 60 frames a second) and one of every two "
        "frames is drawn, half the work for the card. \"Automatic\" does it only while the PC cannot reach 60.",
        "Com placa de vídeo fraca: o jogo segue na sua velocidade (a luta a 60 quadros por segundo) e se desenha um a "
        "cada dois, metade do trabalho para a placa. \"Automático\" faz isso só enquanto o PC não chega a 60."}},
      {"Sincronización vertical", {"Vertical sync", "Sincronização vertical"}},
      {"Activada (sin cortes de imagen)", {"On (no tearing)", "Ligada (sem cortes na imagem)"}},
      {"Desactivada", {"Off", "Desligada"}},
      {"Activada, la imagen no se corta. Desactivada puede bajar un poco la latencia en monitores de más de 60 Hz.",
       {"On, the picture does not tear. Off can lower the latency a little on monitors above 60 Hz.",
        "Ligada, a imagem não corta. Desligada pode baixar um pouco a latência em monitores de mais de 60 Hz."}},
      {"Resolución dinámica", {"Dynamic resolution", "Resolução dinâmica"}},
      {"Apagada", {"Off", "Desligada"}},
      {"Activada", {"On", "Ligada"}},
      {"Si la PC no llega a 60 (un X-Ray, muchos efectos), baja la resolución interna un escalón (1080p, 720p, 480p) y "
       "la vuelve a subir cuando sobra tiempo; nunca pasa de la que elegiste arriba.",
       {"If the PC cannot reach 60 (an X-Ray, many effects), it lowers the internal resolution one step (1080p, 720p, "
        "480p) and raises it again when there is time to spare; it never goes above the one you chose above.",
        "Se o PC não chegar a 60 (um X-Ray, muitos efeitos), baixa a resolução interna um degrau (1080p, 720p, 480p) e "
        "volta a subir quando sobra tempo; nunca passa da que você escolheu acima."}},
      {"Ahorro de memoria", {"Memory saving", "Economia de memória"}},
      {"Apagado", {"Off", "Desligado"}},
      {"Activado (PCs con 4-6 GB de RAM)", {"On (PCs with 4-6 GB of RAM)", "Ligado (PCs com 4-6 GB de RAM)"}},
      {"Suelta las texturas que no se usan y guarda menos copias: menos memoria a cambio de volver a armarlas cuando "
       "hacen falta. Se activa solo en PCs con 6 GB o menos.",
       {"Drops the textures not in use and keeps fewer copies: less memory in exchange for building them again when "
        "they are needed. It turns on by itself on PCs with 6 GB or less.",
        "Libera as texturas que não estão em uso e guarda menos cópias: menos memória em troca de montá-las de novo "
        "quando forem necessárias. Liga sozinho em PCs com 6 GB ou menos."}},
      {"Apagado (el del juego)", {"Off (the game's)", "Desligado (o do jogo)"}},
      {"Bordes más suaves; cada paso cuesta más placa de video. Se aplica al volver a abrir el juego.",
       {"Smoother edges; each step costs more of the graphics card. Applies when the game is opened again.",
        "Bordas mais suaves; cada passo custa mais placa de vídeo. Vale ao abrir o jogo de novo."}},
      {"Placa de video", {"Graphics card", "Placa de vídeo"}},
      {"Automática usa la que tiene más memoria (en una notebook, la dedicada). Se aplica al volver a abrir el juego.",
       {"Automatic uses the one with the most memory (on a laptop, the dedicated one). Applies when the game is opened "
        "again.",
        "Automática usa a que tem mais memória (num notebook, a dedicada). Vale ao abrir o jogo de novo."}},
      {"Automática", {"Automatic", "Automática"}},
      {"Automático", {"Automatic", "Automático"}},

      // --- F2 > Pantalla e idioma
      {"Pantalla", {"Display", "Tela"}},
      {"Pantalla completa", {"Fullscreen", "Tela cheia"}},
      {"Ventana", {"Window", "Janela"}},
      {"F11 o Alt+Enter cambian entre pantalla completa y ventana en cualquier momento.",
       {"F11 or Alt+Enter switch between fullscreen and window at any time.",
        "F11 ou Alt+Enter alternam entre tela cheia e janela a qualquer momento."}},
      {"DirectX 11 (placas o drivers viejos)", {"DirectX 11 (old cards or drivers)", "DirectX 11 (placas ou drivers antigos)"}},
      {"Si el juego anda mal o se ve raro con una, probá otra (algunas placas y drivers van mejor con Vulkan). DirectX "
       "11 es para placas o drivers viejos donde las otras no arrancan o se cierran. Se aplica al volver a abrir el "
       "juego.",
       {"If the game runs badly or looks wrong with one, try another (some cards and drivers do better with Vulkan). "
        "DirectX 11 is for old cards or drivers where the others do not start or close. Applies when the game is "
        "opened again.",
        "Se o jogo roda mal ou fica estranho com uma, teste outra (algumas placas e drivers vão melhor com Vulkan). "
        "DirectX 11 é para placas ou drivers antigos onde as outras não iniciam ou fecham. Vale ao abrir o jogo de "
        "novo."}},
      {"Mostrar FPS", {"Show FPS", "Mostrar FPS"}},
      {"Sí, arriba a la izquierda", {"Yes, top left", "Sim, no canto superior esquerdo"}},
      {"Los cuadros por segundo que se ven y, si se saltean, los que se dibujan.",
       {"The frames a second shown and, if some are skipped, the ones drawn.",
        "Os quadros por segundo exibidos e, se alguns forem pulados, os desenhados."}},
      {"%u FPS (juego a %u)", {"%u FPS (game at %u)", "%u FPS (jogo a %u)"}},
      {"Tamaño de los menús F1 y F2", {"Size of the F1 and F2 menus", "Tamanho dos menus F1 e F2"}},
      {"Automático (según la pantalla)", {"Automatic (by the screen)", "Automático (conforme a tela)"}},
      {"El tamaño del texto de estas ventanas. Automático lo agranda en pantallas de más de 1080 líneas (1440p, 4K) y "
       "sigue la escala de Windows (125 %, 150 %).",
       {"The text size of these windows. Automatic makes it bigger on screens above 1080 lines (1440p, 4K) and follows "
        "Windows' scale (125 %, 150 %).",
        "O tamanho do texto destas janelas. Automático o aumenta em telas de mais de 1080 linhas (1440p, 4K) e segue a "
        "escala do Windows (125 %, 150 %)."}},
      {"El tamaño del texto de estas ventanas. Automático lo agranda en pantallas de más de 1080 líneas (1440p, 4K) y "
       "sigue la escala del escritorio (125 %, 150 %).",
       {"The text size of these windows. Automatic makes it bigger on screens above 1080 lines (1440p, 4K) and follows "
        "the desktop's scale (125 %, 150 %).",
        "O tamanho do texto destas janelas. Automático o aumenta em telas de mais de 1080 linhas (1440p, 4K) e segue a "
        "escala da área de trabalho (125 %, 150 %)."}},
      {"Redes en el menú principal", {"Socials in the main menu", "Redes no menu principal"}},
      {"Mostrar", {"Show", "Mostrar"}},
      {"Ocultar", {"Hide", "Ocultar"}},
      {"MK9 Online by Lauta y los botones de Discord, WhatsApp, Instagram, Kick, YouTube y X, abajo a la derecha del "
       "menú principal.",
       {"MK9 Online by Lauta and the Discord, WhatsApp, Instagram, Kick, YouTube and X buttons, at the bottom right of "
        "the main menu.",
        "MK9 Online by Lauta e os botões de Discord, WhatsApp, Instagram, Kick, YouTube e X, no canto inferior direito "
        "do menu principal."}},
      {"Idioma del juego", {"Game language", "Idioma do jogo"}},
      {"Los textos del juego y de estas ventanas (estas cambian al momento; el juego, al volver a abrirlo). Si no "
       "elegís uno, es el idioma de tu PC. En español el Modo Historia está doblado.",
       {"The texts of the game and of these windows (these change at once; the game, when it is opened again). Unless "
        "you choose one, it is your PC's language. In Spanish, Story Mode is dubbed.",
        "Os textos do jogo e destas janelas (estas mudam na hora; o jogo, ao abri-lo de novo). Se você não escolher "
        "um, é o idioma do seu PC. Em espanhol o Modo História é dublado."}},
      {"Automático (el principal)", {"Automatic (the main one)", "Automático (o principal)"}},
      {"Monitor", {"Monitor", "Monitor"}},
      {"En qué monitor se abre el juego (en pantalla completa, ese). Se aplica al volver a abrir el juego.",
       {"Which monitor the game opens on (in fullscreen, that one). Applies when the game is opened again.",
        "Em qual monitor o jogo abre (em tela cheia, esse). Vale ao abrir o jogo de novo."}},
      {"Automático (1280x720)", {"Automatic (1280x720)", "Automático (1280x720)"}},
      {"Tamaño de la ventana", {"Window size", "Tamanho da janela"}},
      {"El tamaño con el que abre en ventana (también se puede arrastrar el borde). Se aplica al volver a abrir el "
       "juego.",
       {"The size it opens with in a window (you can also drag the edge). Applies when the game is opened again.",
        "O tamanho com que abre em janela (também dá para arrastar a borda). Vale ao abrir o jogo de novo."}},

      // --- F2 > Controles
      {"Íconos de los botones", {"Button icons", "Ícones dos botões"}},
      {"Automático (según el mando)", {"Automatic (by the controller)", "Automático (conforme o controle)"}},
      {"Los botones que muestra el juego en sus menús y en la lista de movimientos. Automático: los del primer mando "
       "conectado al abrir el juego, o las teclas si no hay ninguno. Se aplica al volver a abrir el juego.",
       {"The buttons the game shows in its menus and in the move list. Automatic: those of the first controller "
        "connected when the game opens, or the keys if there is none. Applies when the game is opened again.",
        "Os botões que o jogo mostra nos menus e na lista de golpes. Automático: os do primeiro controle conectado ao "
        "abrir o jogo, ou as teclas se não houver nenhum. Vale ao abrir o jogo de novo."}},
      {"Jugar con el teclado", {"Play with the keyboard", "Jogar com o teclado"}},
      {"Teclado activado", {"Keyboard on", "Teclado ligado"}},
      {"Teclado desactivado", {"Keyboard off", "Teclado desligado"}},
      {"El teclado maneja al", {"The keyboard controls", "O teclado controla o"}},
      {"Jugador 1", {"Player 1", "Jogador 1"}},
      {"Jugador 2", {"Player 2", "Jogador 2"}},
      {"El teclado maneja al Jugador 1", {"The keyboard controls Player 1", "O teclado controla o Jogador 1"}},
      {"El teclado maneja al Jugador 2", {"The keyboard controls Player 2", "O teclado controla o Jogador 2"}},
      {"Jugador 2: para un Versus en esta PC con el teclado y un mando (el mando es el Jugador 1). Online manejás a tu "
       "luchador con el teclado y con cualquier mando conectado.",
       {"Player 2: for a Versus on this PC with the keyboard and a controller (the controller is Player 1). Online you "
        "control your fighter with the keyboard and with any connected controller.",
        "Jogador 2: para um Versus neste PC com o teclado e um controle (o controle é o Jogador 1). Online você "
        "controla seu lutador com o teclado e com qualquer controle conectado."}},
      {"Vibración del mando", {"Controller vibration", "Vibração do controle"}},
      {"Vibración activada", {"Vibration on", "Vibração ligada"}},
      {"Vibración desactivada", {"Vibration off", "Vibração desligada"}},
      {"Mostrar las entradas en la pelea (para practicar combos)",
       {"Show the inputs in the fight (to practice combos)", "Mostrar as entradas na luta (para treinar combos)"}},
      {"Entradas en pantalla activadas", {"Inputs on screen: on", "Entradas na tela: ligadas"}},
      {"Entradas en pantalla desactivadas", {"Inputs on screen: off", "Entradas na tela: desligadas"}},
      {"Mostrar la frame data en Práctica (arranque y ventaja de cada golpe)",
       {"Show frame data in Practice (each attack's startup and advantage)",
        "Mostrar frame data na Prática (início e vantagem de cada golpe)"}},
      {"Frame data activada", {"Frame data on", "Frame data ligada"}},
      {"Frame data desactivada", {"Frame data off", "Frame data desligada"}},
      // the frame data's panel in Practice (mk9_frame_data.cpp)
      {"FRAME DATA · RIVAL", {"FRAME DATA · OPPONENT", "FRAME DATA · RIVAL"}},
      {"Pegá un golpe", {"Throw an attack", "Dê um golpe"}},
      {"Arranque", {"Startup", "Início"}},
      {"Total", {"Total", "Total"}},
      {"No tocó", {"Whiffed", "Errou"}},
      {"Al golpear", {"On hit", "No acerto"}},
      {"Al bloquear", {"On block", "Na defesa"}},
      {"Mandos conectados:", {"Connected controllers:", "Controles conectados:"}},
      {"%s (sin jugador: ya hay cuatro)", {"%s (no player: there are four already)", "%s (sem jogador: já há quatro)"}},
      {"Ninguno. Los de Xbox, PlayStation y Switch Pro andan apenas se conectan; el primero que se conecta es el "
       "Jugador 1.",
       {"None. Xbox, PlayStation and Switch Pro controllers work as soon as they connect; the first one connected is "
        "Player 1.",
        "Nenhum. Os de Xbox, PlayStation e Switch Pro funcionam assim que se conectam; o primeiro a se conectar é o "
        "Jogador 1."}},
      {"Qué golpe hace cada botón del mando se cambia en el menú del juego: OPCIONES > MANDO (para las peleas online, "
       "acá abajo). El teclado lo sigue: cada tecla aprieta el botón del mando que dice la tabla.",
       {"Which move each controller button does is changed in the game's menu: OPTIONS > CONTROLLER (for online "
        "fights, below). The keyboard follows it: each key presses the controller button the table says.",
        "Qual golpe cada botão do controle faz se muda no menu do jogo: OPÇÕES > CONTROLE (para as lutas online, aqui "
        "embaixo). O teclado segue isso: cada tecla aperta o botão do controle que a tabela diz."}},
      {"Botones en el online", {"Online buttons", "Botões no online"}},
      {"En las peleas online el juego usa siempre los botones de fábrica (cada partida online arranca con un perfil "
       "nuevo para que las dos PC jueguen igual), aunque los hayas cambiado en OPCIONES > MANDO. Elegí acá qué botón "
       "hace cada golpe en tus peleas online: tu rival no se entera y no cambia nada para él.",
       {"In online fights the game always uses the default buttons (each online match starts with a new profile so "
        "both PCs play the same), even if you changed them in OPTIONS > CONTROLLER. Choose here which button does each "
        "move in your online fights: your opponent does not know and nothing changes for them.",
        "Nas lutas online o jogo usa sempre os botões de fábrica (cada partida online começa com um perfil novo para "
        "que os dois PCs joguem igual), mesmo que você os tenha mudado em OPÇÕES > CONTROLE. Escolha aqui qual botão "
        "faz cada golpe nas suas lutas online: seu rival não fica sabendo e nada muda para ele."}},
      {"Golpe", {"Move", "Golpe"}},
      {"Botón", {"Button", "Botão"}},
      {"Volver a los botones de fábrica", {"Back to the default buttons", "Voltar aos botões de fábrica"}},
      {"Botones en el online: los de fábrica", {"Online buttons: the defaults", "Botões no online: os de fábrica"}},
      {"Botones en el online: {}", {"Online buttons: {}", "Botões no online: {}"}},
      {"X (Cuadrado)", {"X (Square)", "X (Quadrado)"}},
      {"Y (Triángulo)", {"Y (Triangle)", "Y (Triângulo)"}},
      {"A (Cruz)", {"A (Cross)", "A (Xis)"}},
      {"B (Círculo)", {"B (Circle)", "B (Círculo)"}},
      {"Golpe frontal (Front Punch)", {"Front Punch", "Soco frontal (Front Punch)"}},
      {"Golpe trasero (Back Punch)", {"Back Punch", "Soco traseiro (Back Punch)"}},
      {"Patada frontal (Front Kick)", {"Front Kick", "Chute frontal (Front Kick)"}},
      {"Patada trasera (Back Kick)", {"Back Kick", "Chute traseiro (Back Kick)"}},
      {"Agarre (Throw)", {"Throw", "Agarrão (Throw)"}},
      {"Relevo (Tag)", {"Tag", "Troca (Tag)"}},
      {"Bloquear (Block)", {"Block", "Bloquear (Block)"}},
      {"Cambiar de postura (Flip Stance)", {"Flip Stance", "Trocar de postura (Flip Stance)"}},
      {"Acción", {"Action", "Ação"}},
      {"Mando", {"Controller", "Controle"}},
      {"Teclas", {"Keys", "Teclas"}},
      {"Teclado", {"Keyboard", "Teclado"}},
      {"Apretá una tecla...", {"Press a key...", "Aperte uma tecla..."}},
      {"(ninguna)", {"(none)", "(nenhuma)"}},
      {"Volver a las teclas de fábrica", {"Back to the default keys", "Voltar às teclas de fábrica"}},
      {"Teclas de fábrica", {"Default keys", "Teclas de fábrica"}},
      {"Esa tecla no se puede usar: elegí otra", {"That key cannot be used: choose another", "Essa tecla não pode ser usada: escolha outra"}},
      {" (se la saqué a {})", {" (taken from {})", " (tirada de {})"}},
      // the actions (mk9_controls.cpp) and the keys' names
      {"Arriba (saltar)", {"Up (jump)", "Cima (pular)"}},
      {"Abajo (agacharse)", {"Down (crouch)", "Baixo (agachar)"}},
      {"Izquierda", {"Left", "Esquerda"}},
      {"Derecha", {"Right", "Direita"}},
      {"Stick arriba", {"Stick up", "Analógico para cima"}},
      {"Stick abajo", {"Stick down", "Analógico para baixo"}},
      {"Stick izquierda", {"Stick left", "Analógico para a esquerda"}},
      {"Stick derecha", {"Stick right", "Analógico para a direita"}},
      {"Puño frontal (1)", {"Front Punch (1)", "Soco frontal (1)"}},
      {"Puño trasero (2)", {"Back Punch (2)", "Soco traseiro (2)"}},
      {"Patada frontal (3) / aceptar", {"Front Kick (3) / accept", "Chute frontal (3) / aceitar"}},
      {"Patada trasera (4) / volver", {"Back Kick (4) / back", "Chute traseiro (4) / voltar"}},
      {"Agarre", {"Throw", "Agarrão"}},
      {"Cubrirse", {"Block", "Defender"}},
      {"Cambio de guardia", {"Flip Stance", "Inverter postura"}},
      {"Relevo (peleas por equipos)", {"Tag (tag team fights)", "Troca (lutas em dupla)"}},
      {"Pausa (Start)", {"Pause (Start)", "Pausa (Start)"}},
      {"Espacio", {"Space", "Espaço"}},
      {"Retroceso", {"Backspace", "Backspace"}},
      {"Flecha arriba", {"Up arrow", "Seta para cima"}},
      {"Flecha abajo", {"Down arrow", "Seta para baixo"}},
      {"Flecha izquierda", {"Left arrow", "Seta para a esquerda"}},
      {"Flecha derecha", {"Right arrow", "Seta para a direita"}},
      {"Supr", {"Delete", "Delete"}},
      {"Inicio", {"Home", "Home"}},
      {"Fin", {"End", "End"}},
      {"Re Pág", {"Page Up", "Page Up"}},
      {"Av Pág", {"Page Down", "Page Down"}},
      {"Bloq Mayús", {"Caps Lock", "Caps Lock"}},
      {"Clic izquierdo", {"Left click", "Clique esquerdo"}},
      {"Clic derecho", {"Right click", "Clique direito"}},
      {"Clic de la rueda", {"Wheel click", "Clique da rodinha"}},

      // --- F2 > Online
      {"Retraso de entrada en las peleas", {"Input delay in fights", "Atraso de entrada nas lutas"}},
      {"Automático (recomendado)", {"Automatic (recommended)", "Automático (recomendado)"}},
      {"Ninguno", {"None", "Nenhum"}},
      {"1 cuadro", {"1 frame", "1 quadro"}},
      {"2 cuadros", {"2 frames", "2 quadros"}},
      {"3 cuadros", {"3 frames", "3 quadros"}},
      {"4 cuadros", {"4 frames", "4 quadros"}},
      {"5 cuadros", {"5 frames", "5 quadros"}},
      {"6 cuadros", {"6 frames", "6 quadros"}},
      {"7 cuadros", {"7 frames", "7 quadros"}},
      {"Cuadros que espera cada golpe tuyo antes de verse. Con menos retraso tus golpes salen antes pero el rollback "
       "corrige más (los personajes \"saltan\"); con más, al revés. Automático: 1 cuadro con ping bajo y más con ping "
       "alto, ajustándose durante toda la pelea. Cada jugador elige el suyo.",
       {"Frames each of your hits waits before it shows. With less delay your hits come out sooner but rollback "
        "corrects more (the characters \"jump\"); with more, the other way around. Automatic: 1 frame with low ping and "
        "more with high ping, adjusting through the whole fight. Each player chooses their own.",
        "Quadros que cada golpe seu espera antes de aparecer. Com menos atraso seus golpes saem antes, mas o rollback "
        "corrige mais (os personagens \"pulam\"); com mais, o contrário. Automático: 1 quadro com ping baixo e mais "
        "com ping alto, ajustando-se durante toda a luta. Cada jogador escolhe o seu."}},
      {"Cartel de la conexión en las peleas", {"Connection banner in fights", "Painel da conexão nas lutas"}},
      {"Completo (rival, señal, ping y retraso)", {"Full (opponent, signal, ping and delay)", "Completo (rival, sinal, ping e atraso)"}},
      {"Sólo la señal", {"Signal only", "Só o sinal"}},
      {"Oculto", {"Hidden", "Oculto"}},
      {"Lo que se ve abajo durante una pelea online. Los avisos (conexión inestable, esperando al rival) aparecen "
       "siempre; en los menús se ve completo.",
       {"What shows at the bottom during an online fight. The warnings (unstable connection, waiting for the opponent) "
        "always show; in the menus it is shown in full.",
        "O que aparece embaixo durante uma luta online. Os avisos (conexão instável, esperando o rival) aparecem "
        "sempre; nos menus aparece completo."}},
      {"Nombres en la pelea", {"Names in the fight", "Nomes na luta"}},
      {"El nombre y la bandera de cada jugador debajo de su barra de vida en las peleas online (y en la selección de "
       "personajes).",
       {"Each player's name and flag under their health bar in online fights (and in the character select).",
        "O nome e a bandeira de cada jogador embaixo da barra de vida nas lutas online (e na seleção de personagens)."}},
      {"Fotos de perfil en la pelea", {"Profile pictures in the fight", "Fotos de perfil na luta"}},
      {"La foto de cada jugador al lado de su nombre. Se cargan antes de la pelea: no cuestan nada mientras se juega.",
       {"Each player's picture next to their name. They load before the fight: they cost nothing while playing.",
        "A foto de cada jogador ao lado do nome. Carregam antes da luta: não custam nada enquanto se joga."}},
      {"Récord y racha en la pelea", {"Record and streak in the fight", "Recorde e sequência na luta"}},
      {"Al lado del nombre de cada jugador, sus victorias y derrotas online y, si viene ganando, cuántas seguidas.",
       {"Next to each player's name, their online wins and losses and, if they are on a run, how many in a row.",
        "Ao lado do nome de cada jogador, suas vitórias e derrotas online e, se estiver ganhando, quantas seguidas."}},
      {"Fotos de los otros jugadores", {"Other players' pictures", "Fotos dos outros jogadores"}},
      {"No mostrar ninguna", {"Show none", "Não mostrar nenhuma"}},
      {"Las fotos de perfil de los demás, en la sala, en la pelea y en los perfiles. Las de los jugadores que "
       "silenciaste no se ven nunca.",
       {"The others' profile pictures, in the room, in the fight and in the profiles. Those of the players you muted "
        "never show.",
        "As fotos de perfil dos outros, na sala, na luta e nos perfis. As dos jogadores que você silenciou nunca "
        "aparecem."}},
      {"Grabar replays", {"Record replays", "Gravar replays"}},
      {"Sí, mis sesiones online", {"Yes, my online sessions", "Sim, minhas sessões online"}},
      {"Guarda las entradas de cada sesión online (unos 3 KB por segundo, en la carpeta logs; las últimas 10) para "
       "verlas en F1 > Replays. No usa internet ni se nota en el juego. Vale desde la próxima sesión.",
       {"Keeps the inputs of each online session (about 3 KB a second, in the logs folder; the last 10) to watch them "
        "in F1 > Replays. It uses no internet and does not show in the game. Applies from the next session.",
        "Guarda as entradas de cada sessão online (uns 3 KB por segundo, na pasta logs; as últimas 10) para vê-las em "
        "F1 > Replays. Não usa internet nem pesa no jogo. Vale a partir da próxima sessão."}},

      // --- F2 > Acerca de
      {"Mortal Kombat Komplete Edition para PC, con online con rollback. Hecho por y para la comunidad.",
       {"Mortal Kombat Komplete Edition for PC, with rollback online. Made by and for the community.",
        "Mortal Kombat Komplete Edition para PC, com online com rollback. Feito pela e para a comunidade."}},
      {"Plugins de código cargados (mk9_plugins en mk9.toml): %s",
       {"Code plugins loaded (mk9_plugins in mk9.toml): %s", "Plugins de código carregados (mk9_plugins no mk9.toml): %s"}},
      {"Buscar una versión nueva al iniciar", {"Look for a new version at startup", "Procurar uma versão nova ao iniciar"}},
      {"Buscar versiones nuevas: sí", {"Look for new versions: yes", "Procurar versões novas: sim"}},
      {"Buscar versiones nuevas: no", {"Look for new versions: no", "Procurar versões novas: não"}},
      {"Buscar ahora", {"Check now", "Procurar agora"}},
      {"Si algo falla, la carpeta logs tiene lo que pasó: mandá el último mk9_*.log (y el crash_*.dmp de la misma hora "
       "si el juego se cerró de golpe).",
       {"If something fails, the logs folder has what happened: send the latest mk9_*.log (and the crash_*.dmp of the "
        "same time if the game closed suddenly).",
        "Se algo falhar, a pasta logs tem o que aconteceu: mande o último mk9_*.log (e o crash_*.dmp do mesmo horário "
        "se o jogo fechou de repente)."}},
      {"Abrir la carpeta de logs", {"Open the logs folder", "Abrir a pasta de logs"}},
      {"Reportar un problema", {"Report a problem", "Relatar um problema"}},
      {"Página del proyecto", {"Project page", "Página do projeto"}},
      {"Comunidad y redes", {"Community and socials", "Comunidade e redes"}},
      {"Apoyar el proyecto", {"Support the project", "Apoiar o projeto"}},

      // --- the updater (F2 > Acerca de, and its banner)
      {"dirección inválida", {"invalid address", "endereço inválido"}},
      {"sin conexión", {"no connection", "sem conexão"}},
      {"la descarga se cortó", {"the download was cut off", "o download foi interrompido"}},
      {"sin conexión con GitHub", {"no connection to GitHub", "sem conexão com o GitHub"}},
      {"Buscando una versión nueva...", {"Looking for a new version...", "Procurando uma versão nova..."}},
      {"No se pudo buscar una versión nueva ({}).", {"Could not look for a new version ({}).", "Não foi possível procurar uma versão nova ({})."}},
      {"GitHub no dio la última versión.", {"GitHub did not give the latest version.", "O GitHub não informou a última versão."}},
      {"Tenés la última versión.", {"You have the latest version.", "Você tem a última versão."}},
      {"La v{} ya está bajada: se instala al reiniciar.",
       {"v{} is already downloaded: it installs at the restart.", "A v{} já foi baixada: instala ao reiniciar."}},
      {"Hay una versión nueva, pero sin el archivo firmado: bajala a mano.",
       {"There is a new version, but without the signed file: download it by hand.",
        "Há uma versão nova, mas sem o arquivo assinado: baixe-a manualmente."}},
      {"Hay una versión nueva: v{}.", {"A new version is out: v{}.", "Há uma versão nova: v{}."}},
      {"No se pudo bajar la firma ({}).", {"Could not download the signature ({}).", "Não foi possível baixar a assinatura ({})."}},
      {"Bajando la v%s... %.0f MB de %.0f MB", {"Downloading v%s... %.0f MB of %.0f MB", "Baixando a v%s... %.0f MB de %.0f MB"}},
      {"No se pudo bajar la v{} ({}).", {"Could not download v{} ({}).", "Não foi possível baixar a v{} ({})."}},
      {"Verificando la firma...", {"Checking the signature...", "Verificando a assinatura..."}},
      {"La descarga no tiene una firma válida: no se instala.",
       {"The download does not have a valid signature: it is not installed.",
        "O download não tem uma assinatura válida: não será instalado."}},
      {"Preparando los archivos...", {"Preparing the files...", "Preparando os arquivos..."}},
      {"No se pudo descomprimir la v{}.", {"Could not unpack v{}.", "Não foi possível descompactar a v{}."}},
      {"La v{} está lista. Se instala al reiniciar.", {"v{} is ready. It installs at the restart.", "A v{} está pronta. Instala ao reiniciar."}},
      {"(tenés la v%s)", {"(you have v%s)", "(você tem a v%s)"}},
      {"Actualizar ahora", {"Update now", "Atualizar agora"}},
      {"Tus partidas, opciones y archivos del juego no se tocan.",
       {"Your saves, options and game files are not touched.", "Seus saves, opções e arquivos do jogo não são mexidos."}},
      {"Bajarla de GitHub", {"Download it from GitHub", "Baixar do GitHub"}},
      {"Más tarde", {"Later", "Mais tarde"}},
      {"Ocultar novedades", {"Hide what's new", "Ocultar novidades"}},
      {"Novedades", {"What's new", "Novidades"}},
      {"Empezando la descarga...", {"Starting the download...", "Começando o download..."}},
      {"No está la actualización bajada.", {"The update is not downloaded.", "A atualização não está baixada."}},
      {"No se pudo instalar (¿la carpeta del juego es de solo lectura?). Quedó como estaba.",
       {"Could not install (is the game folder read-only?). Everything is as it was.",
        "Não foi possível instalar (a pasta do jogo é somente leitura?). Ficou como estava."}},
      {"La versión nueva quedó instalada: cerrá y abrí el juego.",
       {"The new version is installed: close and open the game.", "A versão nova foi instalada: feche e abra o jogo."}},

      // --- the first start's installer (mk9_installer.cpp)
      {"mk9-lauta - Instalación", {"mk9-lauta - Setup", "mk9-lauta - Instalação"}},
      {"Para jugar hace falta tu copia de Mortal Kombat Komplete Edition de Xbox 360. Elegí la imagen .iso del disco, "
       "o la carpeta donde ya la tenés extraída (la que tiene default.xex). Se revisa que sea la versión correcta y se "
       "prepara una sola vez: desde la ISO se copian ~7 GB al lado del juego.",
       {"To play you need your copy of Mortal Kombat Komplete Edition for Xbox 360. Choose the disc's .iso image, or the "
        "folder where you already have it extracted (the one with default.xex). It is checked to be the right version "
        "and prepared only once: from the ISO, ~7 GB are copied next to the game.",
        "Para jogar é preciso a sua cópia de Mortal Kombat Komplete Edition de Xbox 360. Escolha a imagem .iso do "
        "disco, ou a pasta onde você já a tem extraída (a que tem o default.xex). Verifica-se que é a versão certa e "
        "ela é preparada uma única vez: a partir da ISO são copiados ~7 GB ao lado do jogo."}},
      {"Para jugar hace falta tu copia de Mortal Kombat Komplete Edition de Xbox 360. Elegí la imagen .iso del disco, "
       "o la carpeta donde ya la tenés extraída (la que tiene default.xex). Se revisa que sea la versión correcta y se "
       "prepara una sola vez: desde la ISO se copian ~7 GB al lado del juego. También podés arrastrar la ISO o la "
       "carpeta a esta ventana.",
       {"To play you need your copy of Mortal Kombat Komplete Edition for Xbox 360. Choose the disc's .iso image, or the "
        "folder where you already have it extracted (the one with default.xex). It is checked to be the right version "
        "and prepared only once: from the ISO, ~7 GB are copied next to the game. You can also drag the ISO or the "
        "folder onto this window.",
        "Para jogar é preciso a sua cópia de Mortal Kombat Komplete Edition de Xbox 360. Escolha a imagem .iso do "
        "disco, ou a pasta onde você já a tem extraída (a que tem o default.xex). Verifica-se que é a versão certa e "
        "ela é preparada uma única vez: a partir da ISO são copiados ~7 GB ao lado do jogo. Você também pode arrastar "
        "a ISO ou a pasta para esta janela."}},
      {"Elegir la ISO...", {"Choose the ISO...", "Escolher a ISO..."}},
      {"Elegir una carpeta...", {"Choose a folder...", "Escolher uma pasta..."}},
      {"Salir", {"Quit", "Sair"}},
      {"Elegí la carpeta del juego (la que tiene default.xex)",
       {"Choose the game's folder (the one with default.xex)", "Escolha a pasta do jogo (a que tem o default.xex)"}},
      {"Imagen de disco de Xbox 360 (*.iso)", {"Xbox 360 disc image (*.iso)", "Imagem de disco de Xbox 360 (*.iso)"}},
      {"Todos los archivos", {"All files", "Todos os arquivos"}},
      {"Elegí la ISO del juego", {"Choose the game's ISO", "Escolha a ISO do jogo"}},
      {"No se pudo abrir el selector de archivos de tu escritorio: arrastrá la ISO o la carpeta a esta ventana.",
       {"Your desktop's file picker could not be opened: drag the ISO or the folder onto this window.",
        "Não foi possível abrir o seletor de arquivos da sua área de trabalho: arraste a ISO ou a pasta para esta "
        "janela."}},
      {"  ({} %, {} de {} MB)", {"  ({} %, {} of {} MB)", "  ({} %, {} de {} MB)"}},
      {"¡Listo! Arrancando el juego...", {"Done! Starting the game...", "Pronto! Iniciando o jogo..."}},
      {"Leyendo la imagen...", {"Reading the image...", "Lendo a imagem..."}},
      {"Revisando la carpeta...", {"Checking the folder...", "Verificando a pasta..."}},
      {"Cancelado.", {"Cancelled.", "Cancelado."}},
      {"No se pudo abrir el archivo.", {"The file could not be opened.", "Não foi possível abrir o arquivo."}},
      {"No es una imagen de un disco de Xbox 360.", {"It is not an Xbox 360 disc image.", "Não é uma imagem de disco de Xbox 360."}},
      {"no es la versión que usa mk9-lauta (Mortal Kombat Komplete Edition de Xbox 360, la europea: En, Fr, De, Es, "
       "It, Pt).",
       {"is not the version mk9-lauta uses (Mortal Kombat Komplete Edition for Xbox 360, the European one: En, Fr, De, "
        "Es, It, Pt).",
        "não é a versão que o mk9-lauta usa (Mortal Kombat Komplete Edition de Xbox 360, a europeia: En, Fr, De, Es, "
        "It, Pt)."}},
      {"Esta copia del juego no trae la lista de archivos.",
       {"This copy of the game does not come with its list of files.", "Esta cópia do jogo não traz a lista de arquivos."}},
      {"A la imagen le falta {}: {}", {"The image is missing {}: {}", "Falta {} na imagem: {}"}},
      {"En la imagen, {} tiene otro tamaño: {}", {"In the image, {} has a different size: {}", "Na imagem, {} tem outro tamanho: {}"}},
      {"No hay lugar: hacen falta {} GB libres en {}.",
       {"Not enough space: {} GB free are needed on {}.", "Sem espaço: são necessários {} GB livres em {}."}},
      {"No se puede escribir en {}: poné la carpeta del juego en otro lugar (por ejemplo Documentos).",
       {"Cannot write to {}: put the game folder somewhere else (for example Documents).",
        "Não é possível gravar em {}: coloque a pasta do jogo em outro lugar (por exemplo Documentos)."}},
      {"No se puede escribir en {}: poné la carpeta del juego en otro lugar (por ejemplo en tu carpeta personal).",
       {"Cannot write to {}: put the game folder somewhere else (for example in your home folder).",
        "Não é possível gravar em {}: coloque a pasta do jogo em outro lugar (por exemplo na sua pasta pessoal)."}},
      {"No se pudo escribir {}.", {"Could not write {}.", "Não foi possível gravar {}."}},
      {"No se pudo leer {} de la imagen (¿está dañada?).",
       {"Could not read {} from the image (is it damaged?).", "Não foi possível ler {} da imagem (está danificada?)."}},
      {"No se pudo escribir {} (¿se llenó el disco?).",
       {"Could not write {} (is the disk full?).", "Não foi possível gravar {} (o disco encheu?)."}},
      {"{} no coincide con el original: {}", {"{} does not match the original: {}", "{} não confere com o original: {}"}},
      {"la imagen está dañada.", {"the image is damaged.", "a imagem está danificada."}},
      {"No se pudo renombrar game_root.partial a game_root.",
       {"Could not rename game_root.partial to game_root.", "Não foi possível renomear game_root.partial para game_root."}},
      {"{}: falta", {"{}: missing", "{}: faltando"}},
      {"{}: otro tamaño", {"{}: different size", "{}: outro tamanho"}},
      {"El default.xex de esa carpeta {}", {"That folder's default.xex {}", "O default.xex dessa pasta {}"}},
      {"{} archivo(s) del juego faltan o están incompletos en esa carpeta ({}{}). Elegí la carpeta donde está "
       "default.xex, o la ISO.",
       {"{} game file(s) missing or incomplete in that folder ({}{}). Choose the folder that has default.xex, or the "
        "ISO.",
        "{} arquivo(s) do jogo faltando ou incompletos nessa pasta ({}{}). Escolha a pasta onde está o default.xex, ou "
        "a ISO."}},
      {"mk9: no se encontraron los archivos del juego y no se puede abrir la ventana del instalador ({}).\n     Abrí "
       "el juego desde tu escritorio, o poné la carpeta game_root al lado del programa.\n",
       {"mk9: the game files were not found and the installer window cannot be opened ({}).\n     Open the game from "
        "your desktop, or put the game_root folder next to the program.\n",
        "mk9: os arquivos do jogo não foram encontrados e não é possível abrir a janela do instalador ({}).\n     Abra "
        "o jogo pela sua área de trabalho, ou coloque a pasta game_root ao lado do programa.\n"}},
      {"No se encontraron los archivos del juego y no se pudo abrir la ventana del instalador ({}).\n\nPoné la carpeta "
       "game_root de tu copia del juego (Xbox 360, Komplete Edition) al lado del programa mk9, o arrancá el juego con "
       "la variable MK9_TEST_INSTALL apuntando a tu .iso o a la carpeta que tiene default.xex.",
       {"The game files were not found and the installer window could not be opened ({}).\n\nPut the game_root folder "
        "from your copy of the game (Xbox 360, Komplete Edition) next to the mk9 program, or start the game with the "
        "MK9_TEST_INSTALL variable pointing to your .iso or to the folder that has default.xex.",
        "Os arquivos do jogo não foram encontrados e não foi possível abrir a janela do instalador ({}).\n\nColoque a "
        "pasta game_root da sua cópia do jogo (Xbox 360, Komplete Edition) ao lado do programa mk9, ou inicie o jogo "
        "com a variável MK9_TEST_INSTALL apontando para a sua .iso ou para a pasta que tem o default.xex."}},

      // --- a damaged game file at the start (mk9_file_check.cpp)
      {"{}: tiene {} bytes y tendría que tener {}", {"{}: has {} bytes and should have {}", "{}: tem {} bytes e deveria ter {}"}},
      {"El juego no puede arrancar: un archivo del juego falta o está incompleto en {}:",
       {"The game cannot start: a game file is missing or incomplete in {}:",
        "O jogo não pode iniciar: um arquivo do jogo está faltando ou incompleto em {}:"}},
      {"El juego no puede arrancar: {} archivos del juego faltan o están incompletos en {}:",
       {"The game cannot start: {} game files are missing or incomplete in {}:",
        "O jogo não pode iniciar: {} arquivos do jogo estão faltando ou incompletos em {}:"}},
      {"- y {} más (están todos en la carpeta logs)",
       {"- and {} more (they are all in the logs folder)", "- e mais {} (estão todos na pasta logs)"}},
      {"Volvé a copiar la carpeta game_root de tu copia del juego (Xbox 360, Komplete Edition) al lado de mk9.exe, "
       "entera.",
       {"Copy the game_root folder from your copy of the game (Xbox 360, Komplete Edition) next to mk9.exe again, all "
        "of it.",
        "Copie de novo a pasta game_root da sua cópia do jogo (Xbox 360, Komplete Edition) ao lado do mk9.exe, "
        "inteira."}},
      {"Volvé a copiar la carpeta game_root de tu copia del juego (Xbox 360, Komplete Edition) al lado de mk9, entera.",
       {"Copy the game_root folder from your copy of the game (Xbox 360, Komplete Edition) next to mk9 again, all of "
        "it.",
        "Copie de novo a pasta game_root da sua cópia do jogo (Xbox 360, Komplete Edition) ao lado do mk9, inteira."}},

      // --- Kratos' download (mk9_kratos.cpp)
      {"Descargando a Kratos", {"Downloading Kratos", "Baixando o Kratos"}},
      {"Kratos se suma al juego (una sola vez, unos 96 MB). Cuando termine, el juego arranca con él, listo para jugar "
       "online con los que también lo tienen.",
       {"Kratos joins the game (only once, about 96 MB). When it finishes, the game starts with him, ready to play "
        "online with those who also have him.",
        "Kratos entra no jogo (uma única vez, uns 96 MB). Quando terminar, o jogo inicia com ele, pronto para jogar "
        "online com quem também o tem."}},
      {"{} de {} MB", {"{} of {} MB", "{} de {} MB"}},
      {"Si cerrás esta ventana se cierra el juego: la descarga sigue la próxima vez que lo abras.",
       {"Closing this window closes the game: the download goes on the next time you open it.",
        "Se você fechar esta janela, o jogo fecha: o download continua na próxima vez que você o abrir."}},

      // --- the close question (mk9_overlay_input.cpp)
      {"¿Cerrar el juego?", {"Close the game?", "Fechar o jogo?"}},
      {"Seguir jugando", {"Keep playing", "Continuar jogando"}},
      {"Cerrar el juego", {"Close the game", "Fechar o jogo"}},

      // --- the graphics API's notices (mk9_renderer_start.cpp)
      {"API gráfica", {"Graphics API", "API gráfica"}},
      {"El juego no pudo arrancar con {} en esta PC: volvió a {}, que sí anda. Si querés probar de nuevo, está en F2 > "
       "Pantalla e idioma > API gráfica.",
       {"The game could not start with {} on this PC: it went back to {}, which works. To try again, it is in F2 > "
        "Display and language > Graphics API.",
        "O jogo não conseguiu iniciar com {} neste PC: voltou para {}, que funciona. Se quiser tentar de novo, está em "
        "F2 > Tela e idioma > API gráfica."}},
      {"El juego no llegó a mostrar imagen en los últimos {} arranques con {}: ahora usa {}. Si querés volver, está en "
       "F2 > Pantalla e idioma > API gráfica.",
       {"The game did not show a picture in the last {} starts with {}: it now uses {}. To go back, it is in F2 > "
        "Display and language > Graphics API.",
        "O jogo não chegou a mostrar imagem nas últimas {} inicializações com {}: agora usa {}. Se quiser voltar, está "
        "em F2 > Tela e idioma > API gráfica."}},
      {"El driver de tu placa de video no tiene lo que el juego necesita de Vulkan (Vulkan 1.2 con direcciones de "
       "buffer): ahora usa {}. Si actualizás el driver, Vulkan se puede volver a elegir en F2 > Pantalla e idioma > "
       "API gráfica.",
       {"Your graphics card's driver lacks what the game needs from Vulkan (Vulkan 1.2 with buffer device addresses): "
        "it now uses {}. If you update the driver, Vulkan can be chosen again in F2 > Display and language > Graphics "
        "API.",
        "O driver da sua placa de vídeo não tem o que o jogo precisa do Vulkan (Vulkan 1.2 com endereços de buffer): "
        "agora usa {}. Se você atualizar o driver, o Vulkan pode ser escolhido de novo em F2 > Tela e idioma > API "
        "gráfica."}},
      {"La placa de video dejó de responder (se perdió el dispositivo de {}): el juego ya no puede dibujar.\n\nCerralo "
       "y volvé a abrirlo. Si vuelve a pasar, el próximo arranque prueba con {}. También ayuda actualizar el driver de "
       "la placa de video.\n\nEl detalle quedó en la carpeta logs.",
       {"The graphics card stopped responding (the {} device was lost): the game can no longer draw.\n\nClose it and "
        "open it again. If it happens again, the next start tries {}. Updating the graphics card's driver also "
        "helps.\n\nThe details are in the logs folder.",
        "A placa de vídeo parou de responder (o dispositivo {} foi perdido): o jogo não consegue mais desenhar.\n\n"
        "Feche e abra de novo. Se acontecer de novo, a próxima inicialização tenta com {}. Atualizar o driver da placa "
        "de vídeo também ajuda.\n\nOs detalhes ficaram na pasta logs."}},
      {"La placa de video dejó de responder (se perdió el dispositivo de Vulkan): el juego ya no puede dibujar.\n\n"
       "Cerralo y volvé a abrirlo. Si vuelve a pasar, actualizá el driver de la placa de video (Mesa en AMD e Intel, "
       "el de NVIDIA en NVIDIA).\n\nEl detalle quedó en la carpeta logs.",
       {"The graphics card stopped responding (the Vulkan device was lost): the game can no longer draw.\n\nClose it "
        "and open it again. If it happens again, update the graphics card's driver (Mesa on AMD and Intel, NVIDIA's on "
        "NVIDIA).\n\nThe details are in the logs folder.",
        "A placa de vídeo parou de responder (o dispositivo Vulkan foi perdido): o jogo não consegue mais desenhar.\n\n"
        "Feche e abra de novo. Se acontecer de novo, atualize o driver da placa de vídeo (Mesa na AMD e Intel, o da "
        "NVIDIA na NVIDIA).\n\nOs detalhes ficaram na pasta logs."}},

      // --- the lobby's own chat lines (lobby.cpp)
      {"{} baneó a {}", {"{} banned {}", "{} baniu {}"}},
      {"{} desbaneó a {}", {"{} unbanned {}", "{} desbaniu {}"}},
      {"{} silenció a {} por {} min", {"{} muted {} for {} min", "{} silenciou {} por {} min"}},
      {"Aviso de {}: {}", {"Notice from {}: {}", "Aviso de {}: {}"}},

      // --- King of the Hill (room.cpp, online_ui.cpp)
      {"Rey de la colina (de 3 a 4 personas)", {"King of the Hill (3 to 4 people)", "Rei da colina (de 3 a 4 pessoas)"}},
      {"Pelean dos y los demás miran la pelea en vivo. El que gana se queda; el que pierde va al final de la fila y "
       "entra el siguiente. Cada uno desde su PC.",
       {"Two fight and the others watch the fight live. The winner stays; the loser goes to the end of the line and the "
        "next one comes in. Each from their own PC.",
        "Dois lutam e os outros assistem à luta ao vivo. Quem ganha fica; quem perde vai para o fim da fila e entra o "
        "próximo. Cada um do seu PC."}},
      {"Rey de la colina: %d de 4 personas", {"King of the Hill: %d of 4 people", "Rei da colina: %d de 4 pessoas"}},
      {"pelea (jugador {})", {"fighting (player {})", "lutando (jogador {})"}},
      {"en la fila ({}°)", {"in line (#{})", "na fila ({}º)"}},
      {" · rey, {} seguidas", {" · king, {} in a row", " · rei, {} seguidas"}},
      {"Rey de la colina: el jugador 1 lleva los menús hasta VERSUS y el jugador 2 se suma con START. Los que esperan "
       "miran la pelea en vivo. Cuando termina una pelea, el que perdió va al final de la fila y el siguiente entra en "
       "su lugar: le toca el CONTINUAR? (REVANCHA, o ELEGIR OTRO para elegir su luchador).",
       {"King of the Hill: player 1 takes the menus to VERSUS and player 2 joins with START. Those waiting watch the "
        "fight live. When a fight ends, the loser goes to the end of the line and the next one takes their place: the "
        "CONTINUE? is theirs (REMATCH, or SELECT A NEW FIGHTER to pick their own).",
        "Rei da colina: o jogador 1 leva os menus até VERSUS e o jogador 2 entra com START. Quem espera assiste à luta "
        "ao vivo. Quando uma luta termina, quem perdeu vai para o fim da fila e o próximo entra no lugar dele: o "
        "CONTINUAR? é dele (REVANCHE, ou ESCOLHER OUTRO para escolher seu lutador)."}},
      {"Mirás esta pelea: esperá tu turno en la fila.",
       {"You are watching this fight: wait for your turn in line.", "Você está assistindo a esta luta: espere sua vez na fila."}},
      {"Cuando estén todos listos, empezá: pelean el host y el primero que entró, y los demás esperan en la fila "
       "mirando. El juego arranca de nuevo ya conectado.",
       {"When everyone is ready, start: the host and the first one who came in fight, and the others wait in line "
        "watching. The game starts again already connected.",
        "Quando todos estiverem prontos, comece: lutam o host e o primeiro que entrou, e os outros esperam na fila "
        "assistindo. O jogo começa de novo já conectado."}},
      {"Rey de la colina · ", {"King of the Hill · ", "Rei da colina · "}},
      {"Esa sala de Rey de la colina ya está completa (o es de otra versión)",
       {"That King of the Hill room is full (or of another version)",
        "Essa sala de Rei da colina já está completa (ou é de outra versão)"}},
      {"Sala pública de Rey de la colina: esperando jugadores (de 3 a 4)",
       {"Public King of the Hill room: waiting for players (3 to 4)",
        "Sala pública de Rei da colina: esperando jogadores (de 3 a 4)"}},
      {"Sala privada de Rey de la colina: pasales el código (de 3 a 4 personas)",
       {"Private King of the Hill room: give them the code (3 to 4 people)",
        "Sala privada de Rei da colina: passe o código para eles (de 3 a 4 pessoas)"}},
      {"Mirando · {}° en la fila", {"Watching · #{} in line", "Assistindo · {}º na fila"}},
