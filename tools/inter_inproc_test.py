#!/usr/bin/env python3
"""In-process inter hop, mirroring OpenStudioRecorder's architecture:
two gst_parse_launch pipelines in ONE process; producer decodes SMPTE 302M to
raw PCM and relays it over intersink; the consumer converts and muxes to MKV.
(Run with /usr/bin/python3 - it has the gi bindings.)
"""
import gi

gi.require_version('Gst', '1.0')
from gi.repository import Gst, GLib  # noqa: E402

Gst.init(None)

D = '/home/pat/.hermes/cache/scratch'

prod_str = (
    f'filesrc location={D}/ff302m.ts ! tsdemux name=d '
    'd. ! queue ! avdec_s302m ! '
    'capsfilter caps=audio/x-raw,format=S32LE,rate=48000,channels=2,layout=interleaved ! '
    'intersink name=is producer-name=pt'
)
cons_str = (
    'intersrc name=ir producer-name=pt ! queue ! audioconvert ! '
    'capsfilter caps=audio/x-raw,format=S24LE,rate=48000,channels=2,layout=interleaved ! '
    'matroskamux ! filesink location=' + D + '/inproc.mkv'
)

producer = Gst.parse_launch(prod_str)
consumer = Gst.parse_launch(cons_str)

loop = GLib.MainLoop()
done = {'n': 0}
errors = []


def on_message(bus, msg):
    if msg.type == Gst.MessageType.ERROR:
        err, dbg = msg.parse_error()
        errors.append(f'{msg.src.name}: {err.message} | {dbg}')
    elif msg.type == Gst.MessageType.EOS:
        done['n'] += 1
        if done['n'] >= 2:
            loop.quit()
    return True


for p in (producer, consumer):
    bus = p.get_bus()
    bus.add_signal_watch()
    bus.connect('message', on_message)

producer.set_state(Gst.State.PLAYING)
consumer.set_state(Gst.State.PLAYING)

GLib.timeout_add_seconds(30, lambda: (loop.quit(), False)[1])
loop.run()

for p in (producer, consumer):
    p.set_state(Gst.State.NULL)

print('EOS count:', done['n'])
for e in errors[:6]:
    print('ERROR:', e)
import os
f = f'{D}/inproc.mkv'
print('inproc.mkv size:', os.path.getsize(f) if os.path.exists(f) else 'missing')
