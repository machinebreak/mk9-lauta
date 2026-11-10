"""The Discord app's local pipe: a handshake with mk9-lauta's application id (no activity set: nothing shows), and
what Discord answers, to check the frames' format discord.cpp writes."""
import json
import struct

for i in range(10):
    try:
        pipe = open(rf'\\.\pipe\discord-ipc-{i}', 'r+b', buffering=0)
        break
    except OSError:
        pipe = None
if not pipe:
    raise SystemExit('no Discord pipe')
body = json.dumps({'v': 1, 'client_id': '1569708793200250910'}).encode()
pipe.write(struct.pack('<II', 0, len(body)) + body)
header = pipe.read(8)
op, length = struct.unpack('<II', header)
answer = json.loads(pipe.read(length))
print('pipe', i, 'op', op, 'answer', {k: answer[k] for k in list(answer)[:3]})
