# nkb_natron_worker.py - the Natron side of natron-kdenlive-link.
#
# Run it INSIDE Natron's Python interpreter (headless, no window):
#     NatronRenderer -t nkb_natron_worker.py        (tarball install)
#     snap run natron -t nkb_natron_worker.py       (snap install)
# It uses only the Python 3.10 standard library that ships with Natron.
#
# WHAT IT DOES
#   Connects to the daemon as a "worker" (same wire protocol as protocol.h) and
#   serves render jobs. For every job:
#     1. writes the incoming RGBA8 frame to <exchange dir>/in_<job>.tga
#     2. loads the composition: the .ntp path sent with the job (any folder), or
#        <comps dir>/<comp_id>.ntp without one; reloaded when the file changed since
#        the last job; created as a default pass-through graph Read -> Write when the
#        file does not exist
#     3. points the node NKB_Input at the input file and NKB_Output at the output
#        file, renders the Write node for the job's frame number,
#     4. reads <exchange dir>/out_<job>.tga back and sends it to the daemon.
#   Natron's Python API has no call that hands pixels over directly, so frames
#   travel as files. Everything between NKB_Input and NKB_Output is the user's graph.
#
# WHAT THE GRAPH SEES
#   Natron's working data is premultiplied. Nodes such as Invert or Grade therefore act on
#   premultiplied RGB: for semi-transparent pixels use Unpremult -> effect -> Premult.
#
# COMPOSITION CONVENTION
#   A comp must contain a Read node named NKB_Input and a Write node named
#   NKB_Output. The worker re-applies these settings on both nodes after every
#   load (the rest of the graph is never touched):
#     NKB_Input   file is straight (un-premultiplied) alpha
#     NKB_Output  output size follows the input size; input premultiplied
#     colour      NKB_COLOR=srgb (default): the file is sRGB, the graph works in
#                 linear light (Natron's normal behaviour)
#                 NKB_COLOR=raw: no conversion, the graph sees the encoded values
#
# ENVIRONMENT (the INI file is read for addresses only)
#   NKB_HOME          data directory, default ~/NatronKdenliveLink
#   NKB_COMPS_DIR     default <NKB_HOME>/comps
#   NKB_EXCHANGE_DIR  default <NKB_HOME>/exchange  (must be readable and writable by Natron;
#                     for the snap keep it in a non-hidden folder of your real home)
#   NKB_COLOR         srgb | raw
#   NKB_MAX_JOBS      exit after this many jobs (tests); 0 = unlimited
#   NKB_KEEP_FILES    1 = do not delete exchange files (debugging)
#   NKB_LOG_LEVEL     debug | info | warn | error
#   NKB_RECONNECT     1 (default) = reconnect when the daemon restarts
#   NKB_EXIT_WITH_PID exit when this process id no longer exists (tests use it so a worker can never outlive them)
#
# Stop it with Ctrl+C, or create the file <NKB_HOME>/worker.stop.
import os
import pwd
import select
import socket
import struct
import sys
import threading
import time

# ----------------------------------------------------------------- protocol --
# Must match include/nkb/protocol.h (104 byte little-endian header).
MAGIC = 0x314B4E42
VERSION = 1
HDR = struct.Struct("<IHHQqIIHBBHHIIQQQ32s")
assert HDR.size == 104
T_HELLO, T_HELLOACK, T_JOB, T_JOBRESULT = 1, 2, 5, 6
ROLE_WORKER = 2
ST_OK, ST_ERROR = 0, 6
PF_RGBA8 = 1


def pack(msg_type, request_id=0, frame=0, w=0, h=0, pf=0, alpha=0, cs=0, status=0, flags=0,
         key_hi=0, key_lo=0, comp=b"", payload=b""):
    return HDR.pack(MAGIC, VERSION, msg_type, request_id, frame, w, h, pf, alpha, cs, status, flags,
                    0xFFFFFFFF, 0, key_hi, key_lo, len(payload), comp[:31]) + payload


# ------------------------------------------------------------------ logging --
_LEVELS = {"debug": 0, "info": 1, "warn": 2, "error": 3}
_NAMES = ["debug", "info", "warning", "error"]
_threshold = _LEVELS.get(os.environ.get("NKB_LOG_LEVEL", "debug"), 0)
_log_fd = None
_log_path = None  # set from logging.log_file in config.ini


def home_dir():
    if os.environ.get("NKB_HOME"):
        return os.environ["NKB_HOME"]
    return os.path.join(pwd.getpwuid(os.getuid()).pw_dir, "NatronKdenliveLink")


def log(level, event, **kv):
    """Same line format as the daemon: ISO UTC time, level, component, thread, event=... key=value"""
    global _log_fd
    lv = _LEVELS[level]
    if lv < _threshold:
        return
    now = time.time()
    ms = int((now % 1) * 1000)
    parts = []
    for k, v in kv.items():
        s = str(v)
        parts.append('%s="%s"' % (k, s) if (" " in s or s == "") else "%s=%s" % (k, s))
    line = "%s.%03dZ [%s] [natron-worker] [t=%d] event=%s %s" % (
        time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(now)), ms, _NAMES[lv], threading.get_native_id(), event,
        " ".join(parts))
    line = line.rstrip()
    print(line)
    sys.stdout.flush()
    try:
        if _log_fd is None:
            path = _log_path or os.path.join(home_dir(), "logs", "natron-kdenlive.log")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            _log_fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
        os.write(_log_fd, (line + "\n").encode())
    except OSError:
        pass


# ------------------------------------------------------------------- config --
def read_ini(path):
    out = {}
    section = ""
    try:
        with open(path) as f:
            for raw in f:
                s = raw.strip()
                if not s or s[0] in "#;":
                    continue
                if s[0] == "[" and s[-1] == "]":
                    section = s[1:-1].strip()
                elif "=" in s:
                    k, v = s.split("=", 1)
                    v = v.strip()
                    if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
                        v = v[1:-1]
                    out[section + "." + k.strip()] = v
    except OSError:
        pass
    return out


def parse_address(text):
    if text.startswith("unix:"):
        return ("unix", text[5:])
    if text.startswith("tcp:"):
        host, port = text[4:].rsplit(":", 1)
        return ("tcp", (host if host != "localhost" else "127.0.0.1", int(port)))
    raise ValueError("bad address " + text)


# ------------------------------------------------------------------ sockets --
class Closed(Exception):
    pass


def recv_exact(sock, n, idle_timeout=None):
    """Reads exactly n bytes. With idle_timeout, returns None if nothing arrives in time."""
    buf = bytearray()
    while len(buf) < n:
        wait = idle_timeout if (not buf and idle_timeout is not None) else 30.0
        r, _, _ = select.select([sock], [], [], wait)
        if not r:
            if not buf and idle_timeout is not None:
                return None
            raise Closed("peer stalled")
        chunk = sock.recv(min(n - len(buf), 1 << 20))
        if not chunk:
            raise Closed("connection closed")
        buf += chunk
    return bytes(buf)


def recv_msg(sock, idle_timeout=None):
    raw = recv_exact(sock, HDR.size, idle_timeout)
    if raw is None:
        return None
    f = HDR.unpack(raw)
    if f[0] != MAGIC or f[1] != VERSION:
        raise Closed("bad magic/version from daemon")
    payload = recv_exact(sock, f[16]) if f[16] else b""  # f[16] = payload_size
    return f, payload


def connect(address, token):
    kind, target = parse_address(address)
    s = socket.socket(socket.AF_UNIX if kind == "unix" else socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3.0)
    s.connect(target)
    s.settimeout(None)
    if kind == "tcp":
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.sendall(pack(T_HELLO, flags=ROLE_WORKER, payload=token.encode()))
    f, payload = recv_msg(s, 5.0) or (None, None)
    if f is None or f[2] != T_HELLOACK or f[10] != ST_OK:
        s.close()
        raise Closed("handshake rejected: %s" % (payload.decode(errors="replace") if payload else "no reply"))
    return s


# ---------------------------------------------------------------------- TGA --
def write_tga(path, w, h, rgba):
    """RGBA8 -> 32 bit uncompressed TGA, top-left origin. Slice assignment keeps this at C speed."""
    bgra = bytearray(rgba)
    bgra[0::4] = rgba[2::4]
    bgra[2::4] = rgba[0::4]
    with open(path, "wb") as f:
        f.write(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x28))
        f.write(bgra)


def read_tga(path):
    """32 bit uncompressed TGA (any origin) -> (w, h, rgba top-down)"""
    with open(path, "rb") as f:
        b = f.read()
    idlen, ctype, itype = b[0], b[1], b[2]
    w, h = struct.unpack("<HH", b[12:16])
    bpp, desc = b[16], b[17]
    if itype != 2 or bpp != 32 or ctype != 0:
        raise ValueError("unsupported TGA written by Natron: type=%d bpp=%d" % (itype, bpp))
    off = 18 + idlen
    bgra = bytearray(b[off:off + w * h * 4])
    if len(bgra) != w * h * 4:
        raise ValueError("TGA pixel data truncated")
    rgba = bytearray(bgra)
    rgba[0::4] = bgra[2::4]
    rgba[2::4] = bgra[0::4]
    if not (desc & 0x20):  # bottom-left origin: flip to top-down
        stride = w * 4
        rows = [bytes(rgba[y * stride:(y + 1) * stride]) for y in range(h - 1, -1, -1)]
        rgba = bytearray(b"".join(rows))
    return w, h, bytes(rgba)


# -------------------------------------------------------------- composition --
class Comp:
    """Tracks which .ntp is loaded in Natron and re-applies the NKB conventions."""

    def __init__(self, comps_dir, color, exchange_dir):
        self.comps_dir = comps_dir
        self.exchange_dir = exchange_dir
        self.color = color
        self.current = None      # path of the .ntp loaded in app1
        self.mtime = None
        self.read = None
        self.write = None

    def path(self, comp_id):
        safe = "".join(c if (c.isalnum() or c in "-_.") else "_" for c in comp_id) or "default"
        return os.path.join(self.comps_dir, safe + ".ntp")

    def ensure(self, comp_id, ntp_path, w, h):
        """Returns (read_node, write_node) of the comp, loading or creating it as needed.

        ntp_path is the .ntp the Kdenlive effect names (any folder Natron can read; for the snap a non-hidden
        folder in the home). Without one (older filters, or only a comp id) it is <comps dir>/<comp id>.ntp."""
        path = ntp_path or self.path(comp_id)
        if not os.path.exists(path):
            self._create_default(comp_id, path)
        mtime = os.stat(path).st_mtime_ns
        if self.current != path or self.mtime != mtime:
            t0 = time.time()
            reason = "first_load" if self.current is None else ("comp_changed" if self.current != path else "file_changed")
            app1.loadProject(path)
            self.read = app1.getNode("NKB_Input")
            self.write = app1.getNode("NKB_Output")
            if self.read is None or self.write is None:
                self.current = None
                raise RuntimeError("comp %s has no node named NKB_Input and NKB_Output" % path)
            self._apply_conventions()
            self._warm_up(w, h)
            self.current, self.mtime = path, mtime
            log("info", "comp_loaded", comp=comp_id, path=path, reason=reason, load_ms="%.0f" % ((time.time() - t0) * 1000))
        return self.read, self.write

    def _create_default(self, comp_id, path):
        """Default pass-through graph: NKB_Input -> NKB_Output."""
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        app1.resetProject()
        r = app1.createNode("fr.inria.built-in.Read")
        r.setScriptName("NKB_Input")
        w = app1.createNode("fr.inria.built-in.Write")
        w.setScriptName("NKB_Output")
        w.connectInput(0, r)
        app1.saveProject(path)
        self.current = None
        log("info", "comp_created", comp=comp_id, path=path, kind="default pass-through (NKB_Input -> NKB_Output)")

    def _warm_up(self, w, h):
        """Throw-away render right after a load, at the size of the job's frames.

        Observed with Natron 2.5.0: the first render after loadProject() returns wrong colours (up to 192
        levels off in the tests), every later render is correct. One render first avoids it. The cause is
        not understood. It used to be a 16x16 frame, but nodes with pixel coordinates (a CornerPin whose
        "from" corners are those of a 1920x1080 frame) squeeze such a small frame to a sliver, and Natron's
        TGA writer then crashes (segfault in OpenImageIO, seen on the author's machine and reproduced). At the
        job's size the warm-up sees the same geometry as the real frames. Costs one render per load."""
        d = self.exchange_dir
        os.makedirs(d, exist_ok=True)
        src, dst = os.path.join(d, "warmup_in.tga"), os.path.join(d, "warmup_out.tga")
        write_tga(src, w, h, b"\x80\x80\x80\xff" * (w * h))  # opaque mid grey
        self.read.getParam("filename").setValue(src)
        self.write.getParam("filename").setValue(dst)
        self.apply_colorspace()
        t0 = time.time()
        app1.render(self.write, 0, 0)
        for p in (src, dst):
            try:
                os.remove(p)
            except OSError:
                pass
        log("debug", "warm_up_done", size="%dx%d" % (w, h), ms="%.0f" % ((time.time() - t0) * 1000))

    def _apply_conventions(self):
        r, w = self.read, self.write
        r.getParam("filePremult").set(2)       # file alpha is straight (un-premultiplied)
        w.getParam("inputPremult").set(1)      # Natron's working data is premultiplied
        w.getParam("formatType").setValue(0)   # output size = input size, not the project size

    def apply_colorspace(self):
        """Must run AFTER the filenames were set: Natron re-derives the colourspaces from the file type every
        time a filename changes (sRGB for 8 bit TGA) and the 'Set' flags do not keep them (tested on 2.5.0)."""
        space = "sRGB" if self.color == "srgb" else "scene_linear"
        self.read.getParam("ocioInputSpace").set(space)
        self.write.getParam("ocioOutputSpace").set(space)


# --------------------------------------------------------------------- jobs --
def process_job(comp, exch, fields, payload, keep):
    (_m, _v, _t, job_id, frame, w, h, pf, alpha, cs, _st, _fl, _to, ntp_len, key_hi, key_lo, _ps, comp_raw) = fields
    comp_id = comp_raw.split(b"\0", 1)[0].decode(errors="replace") or "default"
    t0 = time.time()
    # The payload is the image, followed by ntp_len bytes of the .ntp path (see Header::ntp_len).
    ntp_path = ""
    if ntp_len and ntp_len <= len(payload):
        ntp_path = bytes(payload[len(payload) - ntp_len:]).decode("utf-8", errors="replace")
        payload = payload[:len(payload) - ntp_len]
    if pf != PF_RGBA8 or len(payload) != w * h * 4:
        raise ValueError("only RGBA8 frames are supported by this worker (format=%d, bytes=%d)" % (pf, len(payload)))
    read, write = comp.ensure(comp_id, ntp_path, w, h)
    t_load = time.time()

    in_path = os.path.join(exch, "in_%d.tga" % job_id)
    out_path = os.path.join(exch, "out_%d.tga" % job_id)
    write_tga(in_path, w, h, payload)
    t_wr = time.time()
    try:
        if os.path.exists(out_path):
            os.remove(out_path)
        read.getParam("filename").setValue(in_path)
        write.getParam("filename").setValue(out_path)
        comp.apply_colorspace()
        # Rendering at the job's frame number lets animated Natron parameters follow the Kdenlive timeline;
        # the single input file is held for every frame (Read's default behaviour outside its range).
        render_frame = max(frame, 0)
        app1.render(write, render_frame, render_frame)
        t_render = time.time()
        if not os.path.exists(out_path):
            raise RuntimeError("Natron did not write the output file (see Natron messages above)")
        ow, oh, rgba = read_tga(out_path)
        if (ow, oh) != (w, h):
            raise RuntimeError("output size %dx%d differs from input %dx%d" % (ow, oh, w, h))
        t_rd = time.time()
    finally:
        if not keep:
            for p in (in_path, out_path):
                try:
                    os.remove(p)
                except OSError:
                    pass
    log("debug", "job_rendered", job=job_id, comp=comp_id, frame=frame, size="%dx%d" % (w, h),
        load_ms="%.0f" % ((t_load - t0) * 1000), write_input_ms="%.0f" % ((t_wr - t_load) * 1000),
        natron_render_ms="%.0f" % ((t_render - t_wr) * 1000), read_output_ms="%.0f" % ((t_rd - t_render) * 1000),
        total_ms="%.0f" % ((t_rd - t0) * 1000))
    return pack(T_JOBRESULT, job_id, frame, w, h, PF_RGBA8, alpha, cs, ST_OK, 0, key_hi, key_lo, comp_raw[:31], rgba)


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def serve(sock, comp, exch, max_jobs, keep, stop_file, state, parent_pid):
    while True:
        if os.path.exists(stop_file):
            log("info", "worker_stopping", reason="stop file found", path=stop_file)
            return "stop"
        if parent_pid and not pid_alive(parent_pid):
            log("info", "worker_stopping", reason="process %d is gone" % parent_pid)
            return "stop"
        msg = recv_msg(sock, 1.0)
        if msg is None:
            continue
        fields, payload = msg
        if fields[2] != T_JOB:
            log("warn", "unexpected_message", type=fields[2])
            continue
        job_id, frame = fields[3], fields[4]
        log("debug", "job_received", job=job_id, frame=frame, bytes=len(payload))
        try:
            reply = process_job(comp, exch, fields, payload, keep)
        except Exception as e:  # report the failure to the daemon, keep serving
            log("error", "job_failed", job=job_id, frame=frame, reason=repr(e))
            msg_txt = ("worker error: %r" % (e,)).encode()[:500]
            reply = pack(T_JOBRESULT, job_id, frame, fields[5], fields[6], fields[7], fields[8], fields[9], ST_ERROR, 0,
                         fields[14], fields[15], fields[17][:31], msg_txt)
        sock.sendall(reply)
        state["jobs"] += 1
        if max_jobs and state["jobs"] >= max_jobs:
            log("info", "worker_stopping", reason="NKB_MAX_JOBS reached", jobs=state["jobs"])
            return "stop"


def main():
    home = home_dir()
    cfg = read_ini(os.path.join(home, "config.ini"))
    global _log_path
    _log_path = cfg.get("logging.log_file") or None
    address = os.environ.get("NKB_WORKER_ADDRESS") or cfg.get("daemon.worker_address", "tcp:127.0.0.1:47802")
    comps = os.environ.get("NKB_COMPS_DIR") or os.path.join(home, "comps")
    exch = os.environ.get("NKB_EXCHANGE_DIR") or os.path.join(home, "exchange")
    color = os.environ.get("NKB_COLOR", "srgb")
    max_jobs = int(os.environ.get("NKB_MAX_JOBS", "0"))
    keep = os.environ.get("NKB_KEEP_FILES") == "1"
    reconnect = os.environ.get("NKB_RECONNECT", "1") == "1"
    stop_file = os.path.join(home, "worker.stop")
    parent_pid = int(os.environ.get("NKB_EXIT_WITH_PID", "0") or 0)
    os.makedirs(exch, exist_ok=True)
    os.makedirs(comps, exist_ok=True)
    if os.path.exists(stop_file):
        os.remove(stop_file)
    if color not in ("srgb", "raw"):
        log("error", "bad_config", reason="NKB_COLOR must be srgb or raw")
        return 1
    log("info", "worker_start", natron=NatronEngine.natron.getNatronVersionString(), python=sys.version.split()[0],
        address=address, comps_dir=comps, exchange_dir=exch, color=color, home=home)

    comp = Comp(comps, color, exch)
    state = {"jobs": 0}
    while True:
        try:
            with open(os.path.join(home, "token")) as f:
                token = f.read().strip()
            sock = connect(address, token)
        except (OSError, Closed, ValueError) as e:
            log("warn", "connect_failed", address=address, reason=str(e))
            if not reconnect and state["jobs"] == 0:
                return 2
            time.sleep(2.0)
            if os.path.exists(stop_file) or (parent_pid and not pid_alive(parent_pid)):
                return 0
            continue
        log("info", "worker_connected", address=address)
        try:
            if serve(sock, comp, exch, max_jobs, keep, stop_file, state, parent_pid) == "stop":
                return 0
        except (OSError, Closed) as e:
            log("warn", "connection_lost", reason=str(e))
        finally:
            try:
                sock.close()
            except OSError:
                pass
        if not reconnect:
            return 0


if __name__ == "__main__" or "app1" in globals():
    try:
        import NatronEngine
        rc = main()
    except KeyboardInterrupt:
        log("info", "interrupted")
        rc = 0
    except Exception as exc:  # noqa
        log("error", "worker_crashed", reason=repr(exc))
        rc = 1
    sys.stdout.flush()
    # Natron's interpreter segfaults while shutting down (after our work is finished); leave immediately.
    os._exit(rc)
