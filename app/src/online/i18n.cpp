// MK9 online: the online screens in the player's language (i18n.h).

#include "online/i18n.h"

#include <string>
#include <string_view>
#include <unordered_map>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(mk9_online_language, "auto", "MK9 Online",
                      "The online screens' language: auto (the game's: English and Portuguese as they are, French, "
                      "German and Italian in English), es, en, pt")
    .allowed({"auto", "es", "en", "pt"});

namespace mk9::i18n {
namespace {

enum Lang { kEs, kEn, kPt };

Lang Current() {
  static const Lang lang = [] {
    const std::string chosen = REXCVAR_GET(mk9_online_language);
    if (chosen == "es") return kEs;
    if (chosen == "en") return kEn;
    if (chosen == "pt") return kPt;
    // the game's language (the SDK's user_language: 5 Spanish, 9 Portuguese, 1 English, 3 4 6 the others)
    const std::string game = rex::cvar::GetFlagByName("user_language");
    if (game == "5") return kEs;
    if (game == "9") return kPt;
    return kEn;
  }();
  return lang;
}

struct Text {
  const char* en;
  const char* pt;
};

const std::unordered_map<std::string_view, Text>& Table() {
  static const std::unordered_map<std::string_view, Text> table = {
      // lobby refusals
      {"Más despacio: un mensaje por segundo.", {"Slow down: one message a second.", "Mais devagar: uma mensagem por segundo."}},
      {"En el chat no se pueden pasar links.", {"Links are not allowed in the chat.", "Não é permitido enviar links no chat."}},
      {"Estás silenciado por un rato.", {"You are muted for a while.", "Você está silenciado por um tempo."}},
      {"No podés escribir en el chat.", {"You cannot write in the chat.", "Você não pode escrever no chat."}},
      {"Ese nombre está reservado.", {"That name is reserved.", "Esse nome está reservado."}},
      {"El nombre no puede quedar vacío.", {"The name cannot be empty.", "O nome não pode ficar vazio."}},
      // the connection
      {"Ping {:.0f} ms - variación {:.0f} ms - {:.0f}% perdidos - {}",
       {"Ping {:.0f} ms - jitter {:.0f} ms - {:.0f}% lost - {}", "Ping {:.0f} ms - variação {:.0f} ms - {:.0f}% perdidos - {}"}},
      {"por relay (TURN)", {"through a relay (TURN)", "por relay (TURN)"}},
      {"conexión directa", {"direct connection", "conexão direta"}},
      {"Conexión inestable (%.0f%% perdidos, variación %.0f ms)",
       {"Unstable connection (%.0f%% lost, jitter %.0f ms)", "Conexão instável (%.0f%% perdidos, variação %.0f ms)"}},
      // the window
      {"Online  (F1 para mostrar u ocultar)", {"Online  (F1 to show or hide)", "Online  (F1 para mostrar ou ocultar)"}},
      {"El online está apagado (mk9_lobby_url vacío).", {"Online is off (mk9_lobby_url empty).", "O online está desligado (mk9_lobby_url vazio)."}},
      {"Sala", {"Room", "Sala"}},
      {"Salas", {"Rooms", "Salas"}},
      {"Chat global", {"Global chat", "Chat global"}},
      {"Elegí tu nombre de usuario para jugar online. Es el que ven los demás en las salas y en el chat (de 2 a 20 letras).",
       {"Choose your username to play online. It is what the others see in the rooms and the chat (2 to 20 letters).",
        "Escolha seu nome de usuário para jogar online. É o que os outros veem nas salas e no chat (de 2 a 20 letras)."}},
      {"Guardar", {"Save", "Salvar"}},
      {"Cancelar", {"Cancel", "Cancelar"}},
      {"El nombre tiene que tener por lo menos 2 letras.", {"The name must have at least 2 letters.", "O nome precisa ter pelo menos 2 letras."}},
      {"Conectado", {"Connected", "Conectado"}},
      {"- %d en línea", {"- %d online", "- %d online"}},
      {"Conectando al lobby...", {"Connecting to the lobby...", "Conectando ao lobby..."}},
      {"Sin conexión con el lobby: reintentando", {"No connection to the lobby: retrying", "Sem conexão com o lobby: tentando de novo"}},
      {"Desconectado", {"Disconnected", "Desconectado"}},
      {"Sos:", {"You:", "Você:"}},
      {"Cambiar", {"Change", "Mudar"}},
      {"Aviso: %s", {"Notice: %s", "Aviso: %s"}},
      // the rooms
      {"Partida rápida", {"Quick match", "Partida rápida"}},
      {"Crear sala pública", {"Create public room", "Criar sala pública"}},
      {"Crear sala privada", {"Create private room", "Criar sala privada"}},
      {"Unirse con código:", {"Join with a code:", "Entrar com código:"}},
      {"Unirse", {"Join", "Entrar"}},
      {"Salas abiertas (%d)", {"Open rooms (%d)", "Salas abertas (%d)"}},
      {"Ping aprox.", {"Est. ping", "Ping aprox."}},
      {"Estado", {"Status", "Estado"}},
      {"Estimado por la distancia entre los dos países; el real se ve al entrar.",
       {"Estimated from the distance between the two countries; the real one shows once you join.",
        "Estimado pela distância entre os dois países; o real aparece ao entrar."}},
      {"Entrar", {"Join", "Entrar"}},
      {"esperando", {"waiting", "esperando"}},
      {"jugando", {"playing", "jogando"}},
      // the room
      {"Sala %s", {"Room %s", "Sala %s"}},
      {"pública", {"public", "pública"}},
      {"privada", {"private", "privada"}},
      {"Copiar código", {"Copy code", "Copiar código"}},
      {"Host: vos (", {"Host: you (", "Host: você ("}},
      {"Rival:", {"Opponent:", "Rival:"}},
      {"Vos:", {"You:", "Você:"}},
      {"Pelea con rollback", {"Fight with rollback", "Luta com rollback"}},
      {"Menús en lockstep", {"Menus in lockstep", "Menus em lockstep"}},
      {"El host (jugador 1) lleva los menús hasta VERSUS; el rival se suma con START en la pantalla de 2 jugadores. "
       "Después cada uno elige su luchador.",
       {"The host (player 1) goes through the menus to VERSUS; the opponent joins with START on the 2-player screen. "
        "Then each one picks a fighter.",
        "O host (jogador 1) navega pelos menus até VERSUS; o rival entra com START na tela de 2 jogadores. Depois cada "
        "um escolhe seu lutador."}},
      {"Terminar la sesión y volver al lobby", {"End the session and go back to the lobby", "Encerrar a sessão e voltar ao lobby"}},
      {"Cancelar listo", {"Not ready", "Cancelar pronto"}},
      {"Estoy listo", {"I'm ready", "Estou pronto"}},
      {"%s está listo", {"%s is ready", "%s está pronto"}},
      {"%s todavía no está listo", {"%s is not ready yet", "%s ainda não está pronto"}},
      {"Cuando los dos estén listos, el juego arranca de nuevo ya conectado.",
       {"When both are ready, the game starts again, already connected.",
        "Quando os dois estiverem prontos, o jogo reinicia já conectado."}},
      {"Arrancando la pelea...", {"Starting the fight...", "Iniciando a luta..."}},
      {"Salir de la sala", {"Leave the room", "Sair da sala"}},
      {"Chat de la sala", {"Room chat", "Chat da sala"}},
      {"Enviar", {"Send", "Enviar"}},
      // silenced players
      {"Silenciados (%d)", {"Muted (%d)", "Silenciados (%d)"}},
      {"Quitar", {"Remove", "Remover"}},
      {"Silenciar (no ver sus mensajes ni sus salas)", {"Mute (hide their messages and rooms)", "Silenciar (não ver suas mensagens nem suas salas)"}},
      // the session
      {"el rival", {"the opponent", "o rival"}},
      {" - retraso {}", {" - delay {}", " - atraso {}"}},
      {"Esperando a %s... %.0f s", {"Waiting for %s... %.0f s", "Esperando %s... %.0f s"}},
      {"Las dos partidas dejaron de coincidir: mandá logs\\session_*.mk9net",
       {"The two games stopped matching: send logs\\session_*.mk9net", "As duas partidas deixaram de coincidir: envie logs\\session_*.mk9net"}},
      {"Conectando con %s...", {"Connecting to %s...", "Conectando com %s..."}},
      {"Saliste de la sesión.", {"You left the session.", "Você saiu da sessão."}},
      {"{} se fue de la sesión.", {"{} left the session.", "{} saiu da sessão."}},
      {"Se cortó la conexión con {}.", {"The connection with {} was lost.", "A conexão com {} caiu."}},
      {"Las dos partidas dejaron de coincidir (se desincronizaron).",
       {"The two games stopped matching (they desynced).", "As duas partidas deixaram de coincidir (dessincronizaram)."}},
      {"No se pudo conectar con {}.", {"Could not connect to {}.", "Não foi possível conectar com {}."}},
      {"{} tiene otra versión de mk9-lauta: {} (vos: {}).\nPara jugar online tienen que tener la misma: actualicen los dos "
       "(F2 > Acerca de).",
       {"{} has another version of mk9-lauta: {} (you: {}).\nTo play online you both need the same one: update both "
        "(F2 > About).",
        "{} tem outra versão do mk9-lauta: {} (você: {}).\nPara jogar online vocês precisam da mesma: atualizem os dois "
        "(F2 > Sobre)."}},
      {"0.2.0 o anterior", {"0.2.0 or older", "0.2.0 ou anterior"}},
      {"La sesión terminó.", {"The session ended.", "A sessão terminou."}},
      {"Fin de la sesión", {"Session over", "Fim da sessão"}},
      {"Volvés al lobby en %d s.", {"Back to the lobby in %d s.", "Voltando ao lobby em %d s."}},
      {"Volver al lobby ahora", {"Back to the lobby now", "Voltar ao lobby agora"}},
      // the room's own lines and statuses (room.cpp)
      {"Sala de {}", {"{}'s room", "Sala de {}"}},
      {"Entrando a la sala {}...", {"Joining room {}...", "Entrando na sala {}..."}},
      {"El rival", {"The opponent", "O rival"}},
      {"Esperando un rival", {"Waiting for an opponent", "Esperando um rival"}},
      {"Arranca la pelea", {"The fight is starting", "A luta vai começar"}},
      {"Rival en la sala", {"Opponent in the room", "Rival na sala"}},
      {"{} entró a la sala", {"{} joined the room", "{} entrou na sala"}},
      {"En la sala de {}", {"In {}'s room", "Na sala de {}"}},
      {"Entraste a la sala de {}", {"You joined {}'s room", "Você entrou na sala de {}"}},
      {"Esa sala ya tiene dos jugadores", {"That room already has two players", "Essa sala já tem dois jogadores"}},
      {"{} se fue de la sala", {"{} left the room", "{} saiu da sala"}},
      {"{} se desconectó", {"{} disconnected", "{} se desconectou"}},
      {"Sala pública: esperando un rival", {"Public room: waiting for an opponent", "Sala pública: esperando um rival"}},
      {"Sala privada: pasale el código a tu rival", {"Private room: give the code to your opponent", "Sala privada: passe o código para o seu rival"}},
      {"El código tiene que tener de 4 a 8 letras o números", {"The code must have 4 to 8 letters or digits", "O código precisa ter de 4 a 8 letras ou números"}},
      {"Buscando rival...", {"Looking for an opponent...", "Procurando rival..."}},
      {"Saliste de la sala", {"You left the room", "Você saiu da sala"}},
      {"Conectando con el rival", {"Connecting to the opponent", "Conectando com o rival"}},
      {"Nadie esperando: tu sala quedó abierta para el próximo que busque",
       {"Nobody waiting: your room stays open for the next one who looks", "Ninguém esperando: sua sala ficou aberta para o próximo que procurar"}},
      {"La sala {} no responde", {"Room {} does not answer", "A sala {} não responde"}},
      // spectators (feed.h)
      {"Mirar", {"Watch", "Assistir"}},
      {"Mirando:", {"Watching:", "Assistindo:"}},
      {"Pidiendo la partida al host...", {"Asking the host for the match...", "Pedindo a partida ao host..."}},
      {"Dejar de mirar", {"Stop watching", "Parar de assistir"}},
      {"Alcanzando el vivo... (%.0f s)", {"Catching up with the live match... (%.0f s)", "Alcançando o ao vivo... (%.0f s)"}},
      {"%.1f s detrás del vivo", {"%.1f s behind live", "%.1f s atrás do ao vivo"}},
      {"La repetición se desvió de la partida.", {"The playback drifted from the match.", "A reprodução se desviou da partida."}},
      {"%d mirando", {"%d watching", "%d assistindo"}},
      {"No se pudo traer la partida.", {"The match could not be fetched.", "Não foi possível trazer a partida."}},
      {"Hubo un desync: el reporte quedó en la carpeta logs.",
       {"There was a desync: its report is in the logs folder.", "Houve um desync: o relatório ficou na pasta logs."}},
      {"Abrir el reporte del desync", {"Open the desync report", "Abrir o relatório do desync"}},
      {"La partida se juega con otra versión de mk9-lauta: para mirarla hay que tener la misma (F2 > Acerca de).",
       {"The match is played with another version of mk9-lauta: to watch it you need the same one (F2 > About).",
        "A partida é jogada com outra versão do mk9-lauta: para assisti-la é preciso ter a mesma (F2 > Sobre)."}},
      {"La sala ya tiene el máximo de espectadores.",
       {"The room already has as many spectators as it takes.", "A sala já tem o máximo de espectadores."}},
      {"La partida que mirabas terminó.", {"The match you were watching ended.", "A partida que você assistia terminou."}},
      {"Mirando una partida online", {"Watching an online match", "Assistindo uma partida online"}},
      // the profile (stats.h)
      {"Perfil", {"Profile", "Perfil"}},
      {"(%d V - %d D)", {"(%d W - %d L)", "(%d V - %d D)"}},
      {"Todavía no jugaste partidas online.", {"You have not played online matches yet.", "Você ainda não jogou partidas online."}},
      {"Partidas: %d - Ganadas: %d - Perdidas: %d", {"Matches: %d - Won: %d - Lost: %d", "Partidas: %d - Vitórias: %d - Derrotas: %d"}},
      {"(%.0f%% ganadas)", {"(%.0f%% won)", "(%.0f%% vitórias)"}},
      {"Tus personajes:", {"Your characters:", "Seus personagens:"}},
      {"Últimas partidas", {"Latest matches", "Últimas partidas"}},
      {"Fecha", {"Date", "Data"}},
      {"Rival", {"Opponent", "Rival"}},
      {"Resultado", {"Result", "Resultado"}},
      {"Personajes", {"Characters", "Personagens"}},
      {"Ganaste", {"Won", "Venceu"}},
      {"Perdiste", {"Lost", "Perdeu"}},
      // Discord's Rich Presence (discord.h)
      {"En los menús", {"In the menus", "Nos menus"}},
      {"En el lobby online", {"In the online lobby", "No lobby online"}},
      {"En una sala online", {"In an online room", "Em uma sala online"}},
      {"con {}", {"with {}", "com {}"}},
      {"Online: en los menús", {"Online: in the menus", "Online: nos menus"}},
      {"Peleando online", {"Fighting online", "Lutando online"}},
      {"vs {}", {"vs {}", "vs {}"}},
  };
  return table;
}

}  // namespace

const char* T(const char* es) {
  const Lang lang = Current();
  if (lang == kEs) return es;
  const auto& table = Table();
  const auto it = table.find(es);
  if (it == table.end()) return es;
  return lang == kEn ? it->second.en : it->second.pt;
}

}  // namespace mk9::i18n
