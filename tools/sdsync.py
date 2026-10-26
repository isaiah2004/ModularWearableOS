"""Files to and from the watch's SD card, over USB serial or Bluetooth LE
(firmware/main/sd_xfer.c, ble_xfer.c).

usage:
  python tools/sdsync.py [SRC_DIR] [--dest DIR]       mirror SRC_DIR (default build/sd) onto the card:
                                                     only missing or changed files (size + CRC32)
  python tools/sdsync.py --ls [DIR]                   list a directory on the card
  python tools/sdsync.py --get PATH [--out DIR]       download one file (CRC-checked)
  python tools/sdsync.py --pull DIR [--out DIR]       download a directory (recursive)
  python tools/sdsync.py --rm PATH                    delete a file on the card
transport:  --port COM3 (USB; default: find the 1.43-B)   or   --ble [--ble-name NAME_OR_ADDRESS]
            (Bluetooth: default name "Bangle.js NOS"; the watch takes one connection, so disconnect
            the phone first; needs `pip install bleak`)
The ORRERY's textures live in build/sd/sky (tools/solar_rgb565.py) and go to /sdcard/sky; the
board's minute logs are in watch/logs, the IMU app's recordings in imu/. Close other serial tools
first when using USB."""
import argparse, base64, os, queue, sys, threading, time, zlib
if __package__:
    from .host_transport import (SerialLines, HostTransport, DeviceError,
                                      CommandRejected, validate_command, jsonl_trace)
else:
    from host_transport import (SerialLines, HostTransport, DeviceError,
                                CommandRejected, validate_command, jsonl_trace)

B_SERIAL = '28:84:85:B0:8B:30'   # the 1.43-B; any Espressif USB serial if it isn't attached
BLE_NAME = 'Bangle.js NOS'
XFER_CTRL = '7f5e0002-5a1d-4c3b-9e0f-6e4f53584652'
XFER_DATA = '7f5e0003-5a1d-4c3b-9e0f-6e4f53584652'


class UsbLink(SerialLines):
    block = 2048                      # the driver's rx ring is 4 KB

    def __init__(self, port=None):
        import serial
        from serial.tools import list_ports
        if not port:
            ports = list(list_ports.comports())
            port = next((p.device for p in ports if (p.serial_number or '').upper() == B_SERIAL), None)
            esp = [p.device for p in ports if p.vid == 0x303A]
            port = port or (esp[0] if len(esp) == 1 else None)
            if not port:
                sys.exit('no watch found (or several): %s' % [(p.device, p.serial_number) for p in ports])
        self.s = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=10)
        self.s.dtr = self.s.rts = False          # don't reset the chip on open
        self.s.port = port
        self.s.open()
        time.sleep(0.2)
        self.s.reset_input_buffer()
        self._rx = bytearray()
        self.name = port
        self.completion = True



class BleLink:
    block = 4096                      # the watch's stream buffer holds two

    def __init__(self, target=None):
        import asyncio
        from bleak import BleakClient, BleakScanner
        self.lines, self.buf = queue.Queue(), ''
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()

        async def connect():
            dev = None
            if target and ':' in target or (target and '-' in target):
                dev = target
            else:
                want = target or BLE_NAME
                dev = await BleakScanner.find_device_by_filter(lambda d, ad: (d.name or ad.local_name or '') == want, timeout=15)
                if not dev:
                    raise SystemExit('no "%s" advertising: is the phone connected to it? (the watch takes one link)' % want)
            c = BleakClient(dev, winrt=dict(use_cached_services=False))   # Windows caches an old service list
            await c.connect(timeout=20)
            await c.start_notify(XFER_CTRL, self._on_notify)
            return c
        self.c = self._run(connect())
        self.mtu = max(23, getattr(self.c, 'mtu_size', 23))
        self.name = 'BLE %s (MTU %d)' % (self.c.address, self.mtu)

    def _run(self, coro, timeout=60):
        import asyncio
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def _on_notify(self, _h, data):
        self.buf += bytes(data).decode('utf-8', 'replace')
        while '\n' in self.buf:
            l, self.buf = self.buf.split('\n', 1)
            self.lines.put(l.strip())

    def send(self, line): self._run(self.c.write_gatt_char(XFER_CTRL, line.encode(), response=True))
    def write(self, data):
        n = max(20, self.mtu - 3)
        async def go():
            for i in range(0, len(data), n):
                await self.c.write_gatt_char(XFER_DATA, data[i:i + n], response=False)
        self._run(go())
    def readline(self, timeout):
        try: return self.lines.get(timeout=min(timeout, 0.5))
        except queue.Empty: return ''
    def close(self):
        try: self._run(self.c.disconnect(), 10)
        except Exception: pass


class Card:
    def __init__(self, link, *, negotiate=False, trace=None):
        self.l = link
        # Legacy SD handlers (including BLE) terminate at SDGET/SDLS/SDPUT,
        # without the UI command queue's bare DONE. Only v1 envelopes promise
        # an outer completion for these synchronous transfer commands.
        self.transport = HostTransport(link, require_done=False, trace=trace)
        if negotiate:
            self.transport.require_done = True  # hostcaps is a UI command
            self.transport.negotiate()
            self.transport.require_done = self.transport.correlated
        self.paced = None
        self.features = {}
        self.last_get = None

    def reply(self, prefixes, timeout=10):
        return self.transport.reply(prefixes, timeout)

    def send(self, command):
        self.transport.begin(command)

    def finish(self):
        self.transport.finish()

    def capabilities(self):
        if self.paced is not None:
            return self.features
        self.send('sdfeatures')
        try:
            features = self.reply('SDXFER ', 10)
        except DeviceError as exc:
            if not exc.line.startswith(('ERR unknown:', '? ')):
                raise
            self.finish()
            self.paced = False
            return self.features
        self.finish()
        self.features = dict(part.split('=', 1) for part in features.split()[1:] if '=' in part)
        self.paced = self.features.get('paced_get') == '1'
        return self.features

    def crc(self, rel):
        self.send('sdcrc ' + safe_path(rel))
        r = self.reply('SDCRC', 60).split()
        self.finish()
        return None if r[1] == 'MISSING' else (int(r[1]), int(r[2], 16))

    def ls(self, rel):
        self.send('sdls ' + safe_path(rel) if rel else 'sdls')
        out = []
        while True:
            l = self.reply('SDLS')
            if l == 'SDLS END':
                self.finish()
                return out
            size, name = l[5:].split(' ', 1)
            out.append((int(size), name))

    def rm(self, rel):
        self.send('sdrm ' + safe_path(rel))
        result = self.reply('SDRM')
        self.finish()
        return result == 'SDRM OK'

    def put(self, rel, data, *, restore_protected=False):
        rel = safe_path(rel)
        if restore_protected and self.capabilities().get('restore_put') != '1':
            raise RuntimeError('Device does not advertise checked protected restore support')
        self.send('%s %s %d' % ('sdput!' if restore_protected else 'sdput', rel, len(data)))
        r = self.reply('SDPUT')
        if r != 'SDPUT READY':
            raise IOError(r)
        t0, b = time.monotonic(), self.l.block
        for i in range(0, len(data), b):         # one block in flight: the watch acks each (SDK)
            self.l.write(data[i:i + b])
            done = min(len(data), i + b)
            r = self.reply(('SDK ', 'SDPUT'), 15)   # exact: a stray log line must not release a block
            if r != 'SDK %d' % done:
                raise IOError('%s: %s (sent %d)' % (rel, r, done))
            progress(rel, done, len(data), t0)
        r = self.reply('SDPUT', 30)
        print()
        p = r.split()
        if p[1] != 'OK' or int(p[2]) != len(data) or int(p[3], 16) != zlib.crc32(data) & 0xffffffff:
            raise IOError('%s: %s (want %d %08x)' % (rel, r, len(data), zlib.crc32(data) & 0xffffffff))
        self.finish()

    def _resync_get_usb(self):
        # Only called after a paced/probe timeout: the firmware's <=18s
        # retry window has expired before our 30s body / 60s header timeout.
        # NUL invalidates any unfinished command instead of executing a joined
        # suffix; newline ends it. Drain its DONE before sending another GET.
        self.transport.resynchronize()

    def get(self, rel):
        self.last_get = None
        for attempt in range(3):
            try:
                return self._get_once(rel)
            except (TimeoutError, CommandRejected):
                # Never replay writes or a legacy unpaced stream. BLE uses a
                # separate command/data transport and cannot use USB framing.
                if attempt == 2 or not isinstance(self.l, UsbLink) or self.paced is False:
                    raise
                self._resync_get_usb()
                # Tests/alternate adapters may implement the synchronization
                # barrier themselves; the completed old request is discarded.
                self.transport.active = None

    def _get_once(self, rel):
        # Probe without a path: old sscanf("sdget %s") also accepts "sdget2"
        # as a request for a file named "2". A distinct capability verb avoids
        # that ambiguity and never opens an unintended file on old firmware.
        if self.paced is None:
            self.capabilities()
        if self.paced:
            self.send('sdget2 ' + safe_path(rel))
            header = self.reply(('SDGET2 ', 'SDXFER ERR'), 60)
            return self._get_paced(rel, header)
        return self._get_legacy(rel)

    def _get_paced(self, rel, header):
        parts = header.split()
        if parts[1] == 'MISSING':
            self.finish()
            raise FileNotFoundError(rel)
        if len(parts) != 3 or parts[1] == 'ERR':
            raise IOError(header)
        size, crc = int(parts[1]), int(parts[2], 16)
        if size < 0:
            raise IOError(header)
        out, t0, retries, duplicates = bytearray(), time.monotonic(), 0, 0
        while True:
            line = self.reply(('SDG2 ', 'SDGET2 '), 30)
            if line.startswith('SDGET2 '):
                break
            try:
                _, offset_text, crc_text, encoded = line.split(' ', 3)
                offset, chunk_crc = int(offset_text), int(crc_text, 16)
                chunk = base64.b64decode(encoded, validate=True)
                valid = (0 <= offset <= size and 0 < len(chunk) <= 1536
                         and offset + len(chunk) <= size
                         and zlib.crc32(chunk) & 0xffffffff == chunk_crc)
            except (ValueError, TypeError):
                valid = False
            if valid and offset == len(out):
                out.extend(chunk)
                self.l.write(('SGA %d\n' % len(out)).encode())
                progress(rel, len(out), size, t0)
            elif valid and offset < len(out) and out[offset:offset + len(chunk)] == chunk:
                # An ACK can be lost. Verify the duplicate, ACK it again, never
                # append it a second time.
                self.l.write(('SGA %d\n' % len(out)).encode())
                duplicates += 1
            else:
                self.l.write(('SGR %d\n' % len(out)).encode())
                retries += 1
        if size:
            print()
        if line != 'SDGET2 END' or len(out) != size or zlib.crc32(out) & 0xffffffff != crc:
            raise IOError('%s: got %d bytes crc %08x, want %d %08x (%s)' %
                          (rel, len(out), zlib.crc32(out) & 0xffffffff, size, crc, line))
        self.last_get = dict(protocol=2, bytes=size, crc32='%08x' % crc,
                             retries_requested=retries, duplicate_frames=duplicates)
        self.finish()
        return bytes(out)

    def _get_legacy(self, rel):
        self.send('sdget ' + safe_path(rel))
        r = self.reply('SDGET', 60).split()
        if r[1] == 'MISSING':
            self.finish()
            raise FileNotFoundError(rel)
        if r[1] == 'ERR':
            raise IOError(' '.join(r))
        size, crc, out, t0 = int(r[1]), int(r[2], 16), bytearray(), time.monotonic()
        while True:
            l = self.reply(('SDG ', 'SDGET'), 30)
            if l.startswith('SDGET'):
                break
            out += base64.b64decode(l[4:], validate=True)
            progress(rel, len(out), size, t0)
        if size:
            print()
        if l != 'SDGET END' or len(out) != size or zlib.crc32(out) & 0xffffffff != crc:
            raise IOError('%s: got %d bytes crc %08x, want %d %08x (%s)' % (rel, len(out), zlib.crc32(out) & 0xffffffff, size, crc, l))
        self.finish()
        return bytes(out)


def safe_path(path):
    # Firmware paths are whitespace-free FAT paths; never permit host command
    # injection or a downloaded directory entry to escape the requested tree.
    if not path or any(c.isspace() for c in path) or '\\' in path or ':' in path:
        raise ValueError('SD paths must be nonempty forward-slash paths without whitespace')
    if any(part in ('.', '..') for part in path.split('/')):
        raise ValueError('Relative traversal is not permitted in SD paths')
    validate_command('sdget2 ' + path)
    return path


def progress(rel, done, total, t0):
    sys.stdout.write('\r  %-28s %5.1f%%  %6.0f KB/s' % (rel[-28:], 100.0 * done / max(1, total),
                     done / 1024 / max(1e-3, time.monotonic() - t0)))
    sys.stdout.flush()


def pull(card, rel, out_dir):
    n = 0
    for size, name in card.ls(rel):
        if name.startswith('System Volume'):
            continue
        if '/' in name.rstrip('/') or name.rstrip('/') in ('', '.', '..') or '\\' in name or ':' in name:
            raise ValueError('Unsafe SD directory entry: ' + repr(name))
        child = (rel.strip('/') + '/' + name.rstrip('/')).strip('/')
        if name.endswith('/'):
            n += pull(card, child, out_dir)
            continue
        dst = os.path.join(out_dir, *child.split('/'))
        if os.path.exists(dst) and os.path.getsize(dst) == size and card.crc(child) == (size, zlib.crc32(open(dst, 'rb').read()) & 0xffffffff):
            continue
        data = card.get(child)  # verified bytes first; failed GET must not truncate an existing file
        os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
        with open(dst, 'wb') as output:
            output.write(data)
        n += 1
    return n


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('src', nargs='?', default='build/sd')
    a.add_argument('--dest', default='')
    a.add_argument('--port')
    a.add_argument('--ble', action='store_true', help='use Bluetooth instead of USB')
    a.add_argument('--ble-name', default=None, help='device name or address (default "%s")' % BLE_NAME)
    a.add_argument('--ls', nargs='?', const='', default=None)
    a.add_argument('--get')
    a.add_argument('--pull')
    a.add_argument('--rm')
    a.add_argument('--out', default='.')
    a.add_argument('--restore-protected', action='store_true',
                   help='Opt in to sdput! for firmware-validated protected checkpoint paths; recording must be stopped')
    a.add_argument('--wire-log', help='Append redacted command/reply diagnostics as JSONL')
    o = a.parse_args()
    link = BleLink(o.ble_name) if o.ble else UsbLink(o.port)
    print('connected:', link.name)
    t0 = time.monotonic()
    try:
        card = Card(link, negotiate=not o.ble, trace=jsonl_trace(o.wire_log))
        if o.ls is not None:
            for size, name in card.ls(o.ls):
                print('%10d  %s' % (size, name))
        elif o.get:
            dst = os.path.join(o.out, os.path.basename(o.get))
            os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
            data = card.get(o.get)
            open(dst, 'wb').write(data)
            print('%s: %d bytes in %.1f s' % (dst, len(data), time.monotonic() - t0))
            if card.last_get:
                print('CRC verified; paced download: %d retry requests, %d duplicate frames' %
                      (card.last_get['retries_requested'], card.last_get['duplicate_frames']))
        elif o.pull is not None:
            print('%d files downloaded to %s in %.0f s' % (pull(card, o.pull, o.out), o.out, time.monotonic() - t0))
        elif o.rm:
            print('removed' if card.rm(o.rm) else 'not removed')
        else:
            files = []
            for root, _, names in os.walk(o.src):
                for n in sorted(names):
                    p = os.path.join(root, n)
                    rel = os.path.relpath(p, o.src).replace(os.sep, '/')
                    files.append((p, (o.dest.strip('/') + '/' + rel).strip('/')))
            sent = same = nbytes = 0
            for p, rel in files:
                data = open(p, 'rb').read()
                if card.crc(rel) == (len(data), zlib.crc32(data) & 0xffffffff):
                    same += 1
                    continue
                card.put(rel, data, restore_protected=o.restore_protected)
                sent += 1; nbytes += len(data)
            print('%d sent (%.1f MB in %.0f s), %d already on the card' % (sent, nbytes / 1e6, time.monotonic() - t0, same))
    finally:
        link.close()


if __name__ == '__main__':
    main()
