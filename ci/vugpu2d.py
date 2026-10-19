#!/usr/bin/env python3
"""vugpu2d.py -- a vhost-user-gpu (2D only) backend for UML.

CI copy: the authoritative source is app/display/vugpu2d.py in the app repo
(lakitu12/uml-android), where it is exercised by app/display/test_vugpu2d.py.
This copy exists so the public repo's arm64 gate can boot the kernel against a
backend without reaching into a private repo; keep the two in sync until the
app-side implementation moves to C.

A UML kernel is told to attach a virtio-gpu device with

    virtio_uml.device=/tmp/vugpu.sock:16

(16 = VIRTIO_ID_GPU).  The kernel connects to that unix socket and speaks the
vhost-user dialect that arch/um/drivers/vhost_user.h defines:

  header  : u32 request, u32 flags, u32 size          (12 bytes, packed)
  features: SET_MEM_TABLE carries the guest's physmem as an *fd*; the region
            entry is {guest_addr, size, user_addr, mmap_offset} and vring
            addresses are guest user addresses inside that region.

This backend answers the virtio-gpu *control* queue's 2D command set only.
It advertises no VIRGL feature, so the guest's virtio_gpu driver takes the 2D
path (RESOURCE_CREATE_2D + ATTACH_BACKING + TRANSFER_TO_HOST_2D + FLUSH), which
is exactly what a dumb-buffer client (Xorg fbdev/modesetting, weston) needs.
No 3D, no virglrenderer, no host GL.

Why 2D: the display chain already presents pixels through a shared sink file.
A 2D backend can write the scanout resource into that same sink on FLUSH, so
virtio-gpu can replace umfb's framebuffer mirror without changing the viewer.

Not implemented (deliberately, for the first bring-up): cursor queue, fences
beyond echoing, EDID, blobs, 3D/capset commands.
"""
import argparse
import array
import mmap
import os
import select
import socket
import struct
import sys
import time

# ---- vhost-user -------------------------------------------------------------
GET_FEATURES, SET_FEATURES, SET_OWNER = 1, 2, 3
SET_MEM_TABLE = 5
SET_VRING_NUM, SET_VRING_ADDR, SET_VRING_BASE, GET_VRING_BASE = 8, 9, 10, 11
SET_VRING_KICK, SET_VRING_CALL, SET_VRING_ERR = 12, 13, 14
GET_PROTOCOL_FEATURES, SET_PROTOCOL_FEATURES = 15, 16
SET_VRING_ENABLE = 18

FLAG_REPLY, FLAG_NEED_REPLY = 1 << 2, 1 << 3
VHOST_USER_F_PROTOCOL_FEATURES = 1 << 30

# virtio feature bits we accept: VERSION_1 only (plus the transport's own).
VIRTIO_F_VERSION_1 = 1 << 32

# ---- virtio-gpu -------------------------------------------------------------
CMD_GET_DISPLAY_INFO = 0x0100
CMD_RESOURCE_CREATE_2D = 0x0101
CMD_RESOURCE_UNREF = 0x0102
CMD_SET_SCANOUT = 0x0103
CMD_RESOURCE_FLUSH = 0x0104
CMD_TRANSFER_TO_HOST_2D = 0x0105
CMD_RESOURCE_ATTACH_BACKING = 0x0106
CMD_RESOURCE_DETACH_BACKING = 0x0107

RESP_OK_NODATA = 0x1100
RESP_OK_DISPLAY_INFO = 0x1101
RESP_ERR_UNSPEC = 0x1200
RESP_ERR_INVALID_RESOURCE_ID = 0x1202

CTRL_HDR = struct.Struct('<IIQIB3x')          # type, flags, fence_id, ctx_id, ring_idx


def ctrl_hdr(type_, flags=0, fence=0):
    return CTRL_HDR.pack(type_, flags, fence, 0, 0)


class Region(object):
    def __init__(self, guest_addr, size, mm, offset, fd, mmap_offset):
        self.guest_addr = guest_addr
        self.size = size
        self.mm = mm                 # the mmap of the guest's physmem file
        self.mv = memoryview(mm)     # slices of an mmap are bytes; a memoryview
                                     # gives the writable view the rings need
        self.offset = offset         # page-offset correction of that mmap
        self.fd = fd
        self.mmap_offset = mmap_offset

    def view(self, off, length):
        return self.mv[self.offset + off:self.offset + off + length]


class Vring(object):
    def __init__(self):
        self.num = 0
        self.desc = self.avail = self.used = 0
        self.last_avail = 0
        self.kick = None                     # fd
        self.call = None                     # fd


class GPU(object):
    def __init__(self, width, height, sink_path):
        self.width, self.height = width, height
        self.sink_path = sink_path
        self.resources = {}                  # id -> dict
        self.scanout = None                  # {'res','x','y','w','h'}
        # The sink has a fixed geometry (the viewer negotiated it before boot),
        # so the backend keeps a screen-sized image and composites the scanout
        # resource into it; FLUSH publishes that image.
        self.screen = bytearray(width * height * 4)
        self.seq = 0
        self.regions = []
        self.vrings = {0: Vring(), 1: Vring()}
        self.features_done = 0
        self.stats = {'cmds': 0, 'flush': 0, 'transfer': 0, 'create': 0}

    # -- address translation ------------------------------------------------
    def ptr(self, addr, length):
        """Guest address -> writable view of the backend's mapping of it."""
        for r in self.regions:
            if r.guest_addr <= addr and addr + length <= r.guest_addr + r.size:
                return r.view(addr - r.guest_addr, length)
        return None

    def rd(self, addr, length):
        p = self.ptr(addr, length)
        return None if p is None else p[:length]

    # -- virtqueue ----------------------------------------------------------
    def process(self, idx):
        vq = self.vrings[idx]
        if not vq.num or not vq.desc:
            return 0
        avail = self.ptr(vq.avail, 4 + 2 * vq.num)
        used = self.ptr(vq.used, 4 + 8 * vq.num)
        desc = self.ptr(vq.desc, 16 * vq.num)
        if avail is None or used is None or desc is None:
            log('queue %d: cannot translate rings (desc=0x%x avail=0x%x used=0x%x num=%d)'
                % (idx, vq.desc, vq.avail, vq.used, vq.num))
            return 0
        a_idx = struct.unpack_from('<H', avail, 2)[0]
        log('queue %d: avail idx %d (last %d)' % (idx, a_idx, vq.last_avail))
        done = 0
        while vq.last_avail != a_idx:
            slot = vq.last_avail % vq.num
            head = struct.unpack_from('<H', avail, 4 + 2 * slot)[0]
            n = self.chain(idx, desc, head)
            if n is not None:
                u_slot = struct.unpack_from('<H', used, 2)[0] % vq.num
                struct.pack_into('<II', used, 4 + 8 * u_slot, head, n)
                struct.pack_into('<H', used, 2, (struct.unpack_from('<H', used, 2)[0] + 1) & 0xffff)
                done += 1
            vq.last_avail = (vq.last_avail + 1) & 0xffff
        if done and vq.call is not None:
            try:
                os.write(vq.call, struct.pack('<Q', 1))
            except OSError:
                pass
        return done

    def chain(self, idx, desc, head):
        """Walk one descriptor chain: out buffers -> cmd, in buffers -> resp."""
        out = []
        inb = []
        i = head
        guard = 0
        while True:
            guard += 1
            if guard > 64 or i >= self.vrings[idx].num:
                return None
            addr, ln, flags, nxt = struct.unpack_from('<QIHH', desc, 16 * i)
            p = self.ptr(addr, ln)
            if p is None:
                log('desc %d: addr 0x%x len %d outside regions' % (i, addr, ln))
                return None
            (inb if flags & 2 else out).append((i, p, ln))
            if not flags & 1:
                break
            i = nxt
        cmd = b''.join(bytes(x[1]) for x in out)
        if idx != 0:
            # cursor queue: drain, nothing to do in 2D bring-up
            return 0
        resp = self.gpu_cmd(cmd)
        return self.write_in(inb, resp)

    @staticmethod
    def write_in(inb, resp):
        if not resp:
            return 0
        w = 0
        for i, p, ln in inb:
            take = min(ln, len(resp) - w)
            if take <= 0:
                break
            p[:take] = resp[w:w + take]
            w += take
        return w

    # -- virtio-gpu commands ------------------------------------------------
    def gpu_cmd(self, cmd):
        if len(cmd) < CTRL_HDR.size:
            return b''
        ctype, cflags, fence, ctx, ridx = CTRL_HDR.unpack_from(cmd, 0)[:5]
        self.stats['cmds'] += 1
        if ctype == CMD_GET_DISPLAY_INFO:
            return self.cmd_display_info()
        if ctype == CMD_RESOURCE_CREATE_2D:
            return self.cmd_create_2d(cmd, fence)
        if ctype == CMD_RESOURCE_ATTACH_BACKING:
            return self.cmd_attach(cmd, fence)
        if ctype == CMD_TRANSFER_TO_HOST_2D:
            return self.cmd_transfer(cmd, fence)
        if ctype == CMD_SET_SCANOUT:
            return self.cmd_set_scanout(cmd, fence)
        if ctype == CMD_RESOURCE_FLUSH:
            return self.cmd_flush(cmd, fence)
        if ctype == CMD_RESOURCE_DETACH_BACKING:
            rid = struct.unpack_from('<I', cmd, CTRL_HDR.size)[0]
            self.resources.pop(rid, None)
            return ctrl_hdr(RESP_OK_NODATA)
        if ctype == CMD_RESOURCE_UNREF:
            rid = struct.unpack_from('<I', cmd, CTRL_HDR.size)[0]
            self.resources.pop(rid, None)
            return ctrl_hdr(RESP_OK_NODATA)
        log('unhandled cmd 0x%04x len=%d' % (ctype, len(cmd)))
        return ctrl_hdr(RESP_ERR_UNSPEC)

    def cmd_display_info(self):
        # 16 pmodes, the first one enabled and matching the configured size.
        body = bytearray(24 + 16 * 24)
        CTRL_HDR.pack_into(body, 0, RESP_OK_DISPLAY_INFO, 0, 0, 0, 0)
        struct.pack_into('<IIIIII', body, 24,
                         self.width, self.height, 0, 0, 1, 0)  # rect + enabled
        return bytes(body)

    def cmd_create_2d(self, cmd, fence):
        rid, fmt, w, h = struct.unpack_from('<IIII', cmd, CTRL_HDR.size)
        self.resources[rid] = {'fmt': fmt, 'w': w, 'h': h, 'stride': w * 4,
                               'backing': [], 'data': bytearray(w * h * 4)}
        self.stats['create'] += 1
        log('create_2d id=%d %dx%d fmt=%d' % (rid, w, h, fmt))
        return ctrl_hdr(RESP_OK_NODATA, 0, fence)

    def cmd_attach(self, cmd, fence):
        rid, n = struct.unpack_from('<II', cmd, CTRL_HDR.size)
        res = self.resources.get(rid)
        if res is None:
            return ctrl_hdr(RESP_ERR_INVALID_RESOURCE_ID, 0, fence)
        entries = []
        off = CTRL_HDR.size + 8
        for _ in range(n):
            addr, ln, _pad = struct.unpack_from('<QII', cmd, off)
            entries.append((addr, ln))
            off += 16
        res['backing'] = entries
        log('attach id=%d entries=%d' % (rid, n))
        return ctrl_hdr(RESP_OK_NODATA, 0, fence)

    def cmd_transfer(self, cmd, fence):
        # hdr(24) + rect(16) + offset(8) + resource_id(4) + pad(4)
        offset, = struct.unpack_from('<Q', cmd, CTRL_HDR.size + 16)
        rid, = struct.unpack_from('<I', cmd, CTRL_HDR.size + 24)
        x, y, w, h = struct.unpack_from('<IIII', cmd, CTRL_HDR.size)
        res = self.resources.get(rid)
        if res is None or not res['backing']:
            return ctrl_hdr(RESP_ERR_INVALID_RESOURCE_ID, 0, fence)
        # virtio-gpu semantics: the rectangle is copied from the guest's backing
        # memory, starting at `offset`, row by row, into the host resource.
        stride = res['stride']
        src_base = res['backing'][0][0] + offset
        row_bytes = w * 4
        for row in range(h):
            p = self.ptr(src_base + row * row_bytes, row_bytes)
            if p is None:
                return ctrl_hdr(RESP_ERR_UNSPEC, 0, fence)
            dst = (y + row) * stride + x * 4
            res['data'][dst:dst + row_bytes] = bytes(p[:row_bytes])
        self.stats['transfer'] += 1
        return ctrl_hdr(RESP_OK_NODATA, 0, fence)

    def cmd_set_scanout(self, cmd, fence):
        x, y, w, h = struct.unpack_from('<IIII', cmd, CTRL_HDR.size)
        sid, rid = struct.unpack_from('<II', cmd, CTRL_HDR.size + 16)
        self.scanout = {'res': rid, 'x': x, 'y': y, 'w': w, 'h': h}
        log('set_scanout %dx%d id=%d res=%d' % (w, h, sid, rid))
        return ctrl_hdr(RESP_OK_NODATA, 0, fence)

    def cmd_flush(self, cmd, fence):
        rid, = struct.unpack_from('<I', cmd, CTRL_HDR.size + 16)
        self.stats['flush'] += 1
        res = self.resources.get(rid)
        if res is not None and self.scanout and self.scanout['res'] == rid:
            sx, sy = self.scanout['x'], self.scanout['y']
            cw = min(res['w'], self.width - sx)
            ch = min(res['h'], self.height - sy)
            for row in range(max(0, ch)):
                src = row * res['stride']
                dst = ((sy + row) * self.width + sx) * 4
                self.screen[dst:dst + cw * 4] = res['data'][src:src + cw * 4]
        if self.sink_path:
            self.write_sink()
        return ctrl_hdr(RESP_OK_NODATA, 0, fence)

    # -- sink (umfb format, so existing viewers just work) -------------------
    def write_sink(self):
        stride = self.width * 4
        self.seq += 1
        hdr = struct.pack('<IIIIIII', 0x4D46554D, 1, self.width, self.height,
                          stride, self.seq, 1)
        tmp = self.sink_path + '.tmp'
        with open(tmp, 'wb') as f:
            f.write(hdr + b'\0' * (1024 - len(hdr)))
            f.write(bytes(self.screen))
        os.replace(tmp, self.sink_path)


def log(msg):
    sys.stderr.write('[vugpu] %s\n' % msg)
    sys.stderr.flush()


def recv_msg(s):
    """One vhost-user message plus any SCM_RIGHTS fds."""
    buf = b''
    fds = []
    while len(buf) < 12:
        chunk, anc, _fl, _ad = s.recvmsg(12 - len(buf), socket.CMSG_SPACE(8 * 4))
        if not chunk:
            raise EOFError
        for level, type_, data in anc:
            if level == socket.SOL_SOCKET and type_ == socket.SCM_RIGHTS:
                a = array.array('i')
                a.frombytes(data[:len(data) - (len(data) % 4)])
                fds.extend(a.tolist())
        buf += chunk
    req, flags, size = struct.unpack('<III', buf)
    payload = b''
    while len(payload) < size:
        chunk, anc, _fl, _ad = s.recvmsg(size - len(payload), socket.CMSG_SPACE(8 * 4))
        if not chunk:
            raise EOFError
        for level, type_, data in anc:
            if level == socket.SOL_SOCKET and type_ == socket.SCM_RIGHTS:
                a = array.array('i')
                a.frombytes(data[:len(data) - (len(data) % 4)])
                fds.extend(a.tolist())
        payload += chunk
    return req, flags, payload, fds


def send_msg(s, req, payload=b'', flags=FLAG_REPLY, fds=()):
    hdr = struct.pack('<III', req, flags, len(payload))
    if fds:
        s.sendmsg([hdr + payload], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                     array.array('i', fds).tobytes())])
    else:
        s.sendall(hdr + payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--socket', required=True, help='unix socket path (listen)')
    ap.add_argument('--width', type=int, default=1024)
    ap.add_argument('--height', type=int, default=768)
    ap.add_argument('--sink', default='', help='write scanout frames here (umfb format)')
    ap.add_argument('--once', action='store_true', help='exit after the guest disconnects')
    a = ap.parse_args()

    gpu = GPU(a.width, a.height, a.sink)
    try:
        os.unlink(a.socket)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(a.socket)
    srv.listen(1)
    log('listening on %s (%dx%d, sink=%s)' % (a.socket, a.width, a.height, a.sink or '-'))
    conn, _ = srv.accept()
    log('guest connected')
    conn.settimeout(None)

    memmaps = []
    try:
        while True:
            # Wait on the control socket *and* every kick fd: a queue only moves
            # when the guest rings its kick, which is not a vhost-user message.
            kicks = {vq.kick: idx for idx, vq in gpu.vrings.items() if vq.kick is not None}
            ready, _w, _x = select.select([conn] + list(kicks), [], [])
            for fd in ready:
                if fd is conn:
                    continue
                idx = kicks[fd]
                log('kick queue %d' % idx)
                try:
                    os.read(fd, 8)
                except OSError:
                    pass
                gpu.process(idx)
            if conn not in ready:
                continue
            req, flags, payload, fds = recv_msg(conn)
            log('req %d size %d fds %d' % (req, len(payload), len(fds)))
            if req == GET_FEATURES:
                feats = VIRTIO_F_VERSION_1 | (1 << 30)   # VERSION_1 + PROTOCOL_FEATURES
                send_msg(conn, req, struct.pack('<Q', feats), FLAG_REPLY)
            elif req == SET_FEATURES:
                gpu.features = struct.unpack('<Q', payload[:8])[0]
            elif req == GET_PROTOCOL_FEATURES:
                send_msg(conn, req, struct.pack('<Q', 1 << 3), FLAG_REPLY)  # REPLY_ACK
            elif req == SET_PROTOCOL_FEATURES:
                gpu.proto = struct.unpack('<Q', payload[:8])[0]
            elif req == SET_OWNER:
                pass
            elif req == SET_MEM_TABLE:
                num, _pad = struct.unpack_from('<II', payload, 0)
                off = 8
                for k in range(num):
                    ga, size, ua, mmo = struct.unpack_from('<QQQQ', payload, off)
                    off += 32
                    fd = fds[k]
                    mm = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE,
                                   offset=mmo & ~0xfff)
                    delta = mmo & 0xfff
                    gpu.regions.append(Region(ga, size, mm, delta, fd, mmo))
                    memmaps.append(mm)
                    log('mem region guest=0x%x size=0x%x mmap_off=0x%x' % (ga, size, mmo))
            elif req == SET_VRING_NUM:
                idx, num = struct.unpack('<II', payload[:8])
                gpu.vrings[idx & 0xff].num = num
            elif req == SET_VRING_ADDR:
                idx, _fl, desc, used, avail, lg = struct.unpack('<IIQQQQ', payload[:40])
                vq = gpu.vrings[idx]
                vq.desc, vq.used, vq.avail = desc, used, avail
            elif req == SET_VRING_BASE:
                idx, base = struct.unpack('<II', payload[:8])
                gpu.vrings[idx].last_avail = base
            elif req == SET_VRING_KICK:
                idx = struct.unpack('<I', payload[:4])[0] & 0xff
                gpu.vrings[idx].kick = fds[0] if fds else None
            elif req == SET_VRING_CALL:
                idx = struct.unpack('<I', payload[:4])[0] & 0xff
                gpu.vrings[idx].call = fds[0] if fds else None
            elif req == SET_VRING_ENABLE:
                pass
            elif req == GET_VRING_BASE:
                idx = struct.unpack('<I', payload[:4])[0] & 0xff
                send_msg(conn, req, struct.pack('<II', idx, gpu.vrings[idx].last_avail),
                         FLAG_REPLY)
            else:
                log('unhandled request %d (size %d)' % (req, len(payload)))
                if flags & FLAG_NEED_REPLY:
                    send_msg(conn, req, b'', FLAG_REPLY)

            # queues with no kick fd (in-band notifications) are polled here
            for idx, vq in gpu.vrings.items():
                if vq.kick is None and vq.desc:
                    gpu.process(idx)
    except EOFError:
        log('guest disconnected; stats=%s' % gpu.stats)
    except KeyboardInterrupt:
        pass
    finally:
        conn.close()
        srv.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
