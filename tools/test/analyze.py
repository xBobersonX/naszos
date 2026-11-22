# Znajduje w nagraniu WAV fragmenty z dzwiekiem i szacuje ich czestotliwosc
# (liczac przejscia sygnalu przez srednia wartosc)
import sys, wave, struct

# QEMU nie zawsze uzupelnia rozmiary w naglowku - czytamy parametry recznie
data = open(sys.argv[1], 'rb').read()
ch, rate = struct.unpack('<HI', data[22:28])
width = struct.unpack('<H', data[34:36])[0] // 8
raw = data[44:]
raw = raw[:len(raw) - len(raw) % (width * ch)]
n = len(raw) // (width * ch)
fmt = {1: 'b', 2: 'h', 4: 'i'}[width]
samples = struct.unpack('<%d%s' % (len(raw) // width, fmt), raw)[::ch]
print(f'Nagranie: {n / rate:.2f} s, {rate} Hz, {width * 8} bit, kanaly: {ch}')

win = rate // 100                       # okna po 10 ms
segments, cur = [], None
for i in range(0, len(samples) - win, win):
    chunk = samples[i:i + win]
    mean = sum(chunk) / len(chunk)
    crossings = sum(1 for a, b in zip(chunk, chunk[1:]) if (a - mean) * (b - mean) < 0)
    loud = max(chunk) - min(chunk) > (1 << (width * 8 - 4))
    freq = crossings * 100 / 2 if loud else 0
    if cur and loud and abs(freq - cur['f']) < max(40, cur['f'] * 0.08):
        cur['end'] = i; cur['fs'].append(freq)
    else:
        if cur: segments.append(cur)
        cur = {'start': i, 'end': i, 'f': freq, 'fs': [freq]} if loud else None
if cur: segments.append(cur)

for s in segments:
    dur = (s['end'] - s['start']) / rate + 0.01
    if dur >= 0.02:
        f = sorted(s['fs'])[len(s['fs']) // 2]
        print(f"  od {s['start'] / rate:6.2f} s  czas {dur * 1000:5.0f} ms  czestotliwosc ~{f:5.0f} Hz")
