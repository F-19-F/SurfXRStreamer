"""Temporary receiver for explicit isolated cross-load measurements.

PAIRS, PORT, TOKEN and IPERF3 are supplied privately over SSH stdin. Never
install this helper or open streaming ports. TCP/echo accept paired source addresses
and the run token. Optional iperf listeners use their standard control protocol
on explicit Frame addresses; only their owned child processes are terminated.
"""
import json
import selectors
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path


def headset_state():
    sys.path.insert(0, str(Path.home() / '.local/share/surfxr-client/launcher'))
    from supervisor import ipc_request
    status = ipc_request('status')
    return {'state': status['state'], 'pid': status.get('pid')}


def serve():
    state = headset_state()
    if state['state'] != 'idle' or state['pid']:
        raise RuntimeError('headset_busy_no_load_started')
    sel = selectors.DefaultSelector()
    sockets, children, counts = [], [], {}
    cpu, started = time.process_time(), time.monotonic()
    max_work_ms, work_rounds, stop = 0, 0, False
    try:
        # Reserve all endpoints before starting children. A bind failure never
        # kills an existing listener. iperf owns both TCP control and UDP data.
        for name, pc, frame in PAIRS:
            for kind, port in [('udp', PORT), ('listen', PORT + 1)]:
                s = socket.socket(socket.AF_INET,
                                  socket.SOCK_DGRAM if kind == 'udp' else socket.SOCK_STREAM)
                sockets.append(s)
                if kind == 'listen':
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind((frame, port))
                s.setblocking(False)
                if kind == 'listen':
                    s.listen(4)
                sel.register(s, selectors.EVENT_READ, {'kind': kind, 'pc': pc, 'name': name})
        if IPERF3:
            for i, (_, _, frame) in enumerate(PAIRS):
                for socket_type in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
                    with socket.socket(socket.AF_INET, socket_type) as reserve:
                        reserve.bind((frame, PORT + 2 + i))
            for i, (_, _, frame) in enumerate(PAIRS):
                child = subprocess.Popen([IPERF3, '-s', '-J', '-B', frame, '-p', str(PORT + 2 + i),
                    '--idle-timeout', '25', '--server-max-duration', '30'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                children.append(child)
            time.sleep(.3)
            if any(child.poll() is not None for child in children):
                raise RuntimeError('iperf_listener_failed')
        print(json.dumps({'ready': True, 'headset': state, 'probe_qos': 'OS default',
                          'limit_seconds': 360, 'iperf3': IPERF3}), flush=True)
        while not stop and time.monotonic() - started < 360:
            events = sel.select(.05)
            work_started = time.monotonic()
            events.sort(key=lambda event: event[0].data['kind'] != 'udp')
            for key, _ in events:
                s, d = key.fileobj, key.data
                if d['kind'] == 'udp':
                    for _ in range(32):
                        try:
                            b, peer = s.recvfrom(256)
                        except BlockingIOError:
                            break
                        if peer[0] == d['pc'] and len(b) == 24 and b[:16] == TOKEN:
                            try:
                                s.sendto(b, peer)
                            except BlockingIOError:
                                pass
                elif d['kind'] == 'listen':
                    c, peer = s.accept()
                    c.setblocking(False)
                    sockets.append(c)
                    if peer[0] != d['pc']:
                        c.close()
                        continue
                    sel.register(c, selectors.EVENT_READ, dict(kind='data', pc=d['pc'],
                        name=d['name'], header=b'', count=0, valid=False))
                else:
                    try:
                        b = s.recv(262144)
                    except BlockingIOError:
                        continue
                    if not b:
                        if d['valid']:
                            s.sendall(struct.pack('!Q', d['count']))
                            counts[d['name']] = counts.get(d['name'], 0) + d['count']
                        sel.unregister(s)
                        s.close()
                        continue
                    if not d['valid']:
                        need = 20 - len(d['header'])
                        d['header'] += b[:need]
                        b = b[need:]
                        if len(d['header']) < 20:
                            continue
                        if d['header'] == TOKEN + b'STOP':
                            stop = True
                            break
                        if d['header'] == TOKEN + b'CHEK':
                            s.sendall(json.dumps(headset_state()).encode() + b'\n')
                            sel.unregister(s)
                            s.close()
                            continue
                        if d['header'] != TOKEN + b'DATA':
                            sel.unregister(s)
                            s.close()
                            continue
                        d['valid'] = True
                    d['count'] += len(b)
            max_work_ms = max(max_work_ms, (time.monotonic() - work_started) * 1000)
            work_rounds += bool(events)
    finally:
        for s in sockets:
            s.close()
        sel.close()
        for child in children:
            child.terminate()
        for child in children:
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=3)
        print(json.dumps({'received_payload_bytes': counts,
            'elapsed_seconds': time.monotonic() - started, 'cpu_seconds': time.process_time() - cpu,
            'max_ready_work_ms': max_work_ms, 'ready_work_rounds': work_rounds,
            'iperf_children_exited': [child.returncode for child in children]}), flush=True)


if __name__ == '__main__':
    serve()
