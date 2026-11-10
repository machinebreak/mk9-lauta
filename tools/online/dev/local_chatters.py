"""Fills the LOCAL test lobby (127.0.0.1:8791) with a few chat lines and an open room from players of several
countries, then stays connected for `seconds` so the room shows in the list.

    python local_chatters.py [seconds]
"""
import asyncio
import json
import sys

import websockets

URL = 'ws://127.0.0.1:8791/lobby?game=mk9pc'
PEOPLE = [('Pepe', 'ES', 'buenas, alguien para unas peleas?'), ('Lucho', 'AR', 'yo juego, armo sala'),
          ('Joao', 'BR', 'bora jogar!'), ('Molesto', 'MX', 'spam spam spam')]


async def person(name, cc, text, host, seconds):
    async with websockets.connect(URL) as ws:
        await ws.send(json.dumps({'t': 'hello', 'v': 2, 'sub': True, 'name': name, 'country': cc}))
        await asyncio.sleep(0.5)
        await ws.send(json.dumps({'t': 'chat', 'text': text}))
        if host:
            await ws.send(json.dumps({'t': 'host', 'room': name.upper()[:4] + '01', 'title': f'Sala de {name}', 'open': True,
                                      'mode': 'fight', 'status': 'waiting'}))
        end = asyncio.get_event_loop().time() + seconds
        while asyncio.get_event_loop().time() < end:
            try:
                await asyncio.wait_for(ws.recv(), 1)
            except asyncio.TimeoutError:
                pass


async def main(seconds):
    tasks = []
    for i, (name, cc, text) in enumerate(PEOPLE):
        tasks.append(asyncio.create_task(person(name, cc, text, name in ('Lucho', 'Pepe', 'Joao'), seconds)))
        await asyncio.sleep(1.2)  # the lobby lets one line a second through
    await asyncio.gather(*tasks)


asyncio.run(main(float(sys.argv[1]) if len(sys.argv) > 1 else 60))
