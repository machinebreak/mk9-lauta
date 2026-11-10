"""Prints every rooms list the LOCAL test lobby sends for `seconds` (python rooms_probe.py [seconds])."""
import asyncio
import json
import sys
import time

import websockets

URL = 'ws://127.0.0.1:8791/lobby?game=mk9pc'


async def main(seconds):
    async with websockets.connect(URL) as ws:
        await ws.send(json.dumps({'t': 'hello', 'v': 2, 'sub': True, 'name': 'Sonda', 'country': 'AR'}))
        await ws.send(json.dumps({'t': 'sub', 'rooms': True}))
        end = time.time() + seconds
        while time.time() < end:
            try:
                m = json.loads(await asyncio.wait_for(ws.recv(), 1))
            except asyncio.TimeoutError:
                continue
            if m.get('t') in ('rooms', 'welcome'):
                print(time.strftime('%H:%M:%S'), m.get('t'), json.dumps(m.get('rooms')), flush=True)


asyncio.run(main(float(sys.argv[1]) if len(sys.argv) > 1 else 40))
