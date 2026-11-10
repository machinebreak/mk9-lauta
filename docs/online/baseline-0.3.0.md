# Línea de base 0.3.0 (2026-11-10)

Medido con `tools/online/dev/baseline.ps1` y resumido con `baseline_report.py` (logs en `out/f3/baseline`). PC:
Ryzen 5 8600G + Radeon 760M, 32 GB, D3D12, "Consola" (720p). Online: dos juegos en esta PC por UDP, guiones de pelea
reales (`out/f3/host_long.txt` / `guest_long.txt`), latencia emulada por lado (`mk9_net_lag_ms`) + 10 ms de variación;
`slow45` = cada juego en dos núcleos (`-HostAffinity 0x05 -GuestAffinity 0x50`). Ventana medida: la primera pelea, del
fin de la intro (+2 s) al fin de su modo (~75 s). Offline: los últimos 30 s de una pelea de la Escalera.

| Medida | offline | lag25 (RTT ~88) | lag45 (RTT ~130) | lag70 (RTT ~176) | slow45 (RTT ~135) |
|---|---|---|---|---|---|
| fps mostrados | 60,0 | 59,9 | 59,9 | 59,8 | 59,9 |
| cuadro p50 / p99 típico / p99 peor (ms) | 16,6 / 16,9 / 17,1 | 16,6 / 17,0 / 17,9 | 16,6 / 19,3 / 23,9 | 16,6 / 18,4 / 22,8 | 16,6 / 17,8 / 20,9 |
| cuadro más largo (ms) | 17,4 | 104 | 155 | 190 | 150 |
| cuadros > 20 ms en la pelea (host / guest) | 0 | 1 / 7 | 124 / 4 | 62 / 27 | 2 / 27 |
| hilo del juego: trabajo por cuadro (ms, p99) | 2,5 (4,0) | 2,5 (4,5) | 2,8 (4,2) | 2,2 (3,8) | 2,1 (3,6) |
| borde de sesión por cuadro (ms, p99) | 0 | 4-11,6 (18) | 11-12 (31) | 11,7 (16,6) | 11-13 (18) |
| render de UE3: trabajo por cuadro (ms) | 2,5 | 2,8-5,2 | 5,9-6,5 | 6,0-6,2 | 5,2-5,8 |
| rollbacks por minuto (host / guest) | – | 0 / 267 | 199 / 177 | 199 / 268 | 156 / 271 |
| profundidad media / máx (host / guest) | – | 0 / 2,3 (3) | 3,4 (5) / 1,3 (2) | 3,0 (4) / 2,4 (4) | 1,2 (2) / 3,2 (4) |
| cuadros re-corridos (% de los cuadros) | – | 0 / 17 | 19 / 6 | 17 / 18 | 5 / 24 |
| retraso de entrada automático | – | 1 | 1 | 2 | 1 |
| guardar un cuadro / restaurar (ms) | – | 0,7-0,8 / 1,4 | 0,9 / 1,3 | 0,75 / 1,8 | 0,7 / 1,5-1,6 |
| "en reposo" por borde (ms; casi todo el flush del render) | – | 10,7-14,6 | 10,1-11,7 | 11,0 | 10,5-12,5 |
| botón → pantalla, promedio (máx) ms | 18,3 (26) | 14,9-17,5 (69) | 14,7-16,4 (100) | 15,3-15,4 (116) | 14,3-16,6 (89) |
| memoria en menús / pelea (working set) | 0,96 / 1,27 GB | 0,95 / 1,35 GB | 0,94 / 1,41 GB | 0,95 / 1,37 GB | 0,95 / 1,34 GB |

## Lo que dicen

1. **Hay muchos más rollbacks de lo que suponía el plan**: 150-270 por minuto (el plan decía ~16), con 5-24 % de los
   cuadros re-corridos. Cada uno es corto (1-3 cuadros de media, 5 como máximo), así que la pelea sostiene 60 fps en
   esta PC, pero los cuadros de 20-24 ms (p99) y los picos de 100-190 ms se ven.
2. **Asimetría entre los dos lados**: con RTT 88 ms el host no hizo **ningún** rollback (sus entradas del rival llegan
   siempre a tiempo: 3.148 cuadros vivos sin foto) y el invitado hizo 305; en las otras corridas la profundidad media
   de un lado duplica o triplica la del otro. Un lado corre adelantado y paga todas las correcciones: es lo que
   arreglan la alineación de fase y el time sync continuo (fase C del plan). Prioridad alta.
3. **El borde de sesión cuesta ~11-12 ms por cuadro**, y es casi todo el flush del render de UE3 (que incluye su espera
   del vblank). El hilo del juego trabaja sólo 2-3 ms. En esta PC sobra; con dos núcleos (`slow45`) también.
4. **El render de UE3 trabaja el doble online (5-6,5 ms) que offline (2,5 ms)**: los cuadros re-corridos igual le
   mandan la escena a dibujar. Es lo que ataca la tarea B2 (cuadros re-corridos sin dibujar la escena).
5. **Los picos de 84-190 ms** salen del hilo de render ("Swap to Swap" igual de largo): a investigar en B1 (pipelines
   nuevos, cargas al final de la pelea, rollbacks profundos).
6. **Latencia local**: ~15 ms de botón a pantalla, menos que offline (18 ms): en la pelea online el juego y el render
   corren en serie y la imagen sale antes. Los máximos de 70-116 ms son los mismos picos del punto 5.
7. **Memoria**: ~1,35-1,41 GB en una pelea online (0,1 GB más que offline: la línea de tiempo).
