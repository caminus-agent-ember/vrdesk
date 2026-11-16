#!/usr/bin/env python3
"""A tiny Wayland client that records pointer events it receives to a file.

Used by run_e2e.sh to prove an injected click actually reaches a window surface
(sway's headless seat has no keyboard capability, so keyboard-focus is not a
valid assertion). Writes one line per event to the path given in argv[1].
"""
import mmap
import os
import sys
import tempfile

from ctypes import (CDLL, CFUNCTYPE, POINTER, Structure, byref, c_char_p, c_int,
                    c_uint32, c_void_p, cast, sizeof)

wl = CDLL("libwayland-client.so.0")

# ---- minimal protocol bindings -------------------------------------------------
INTERFACES = {}


class wl_interface(Structure):
    _fields_ = [("name", c_char_p), ("version", c_int),
                ("method_count", c_int), ("methods", c_void_p),
                ("event_count", c_int), ("events", c_void_p)]


class wl_registry_listener(Structure):
    _fields_ = [("global_", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_char_p, c_uint32)),
                ("global_remove", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32))]


class wl_pointer_listener(Structure):
    _fields_ = [("enter", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_void_p, c_int, c_int)),
                ("leave", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_void_p)),
                ("motion", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_int, c_int)),
                ("button", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_uint32, c_uint32, c_uint32)),
                ("axis", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_uint32, c_int)),
                ("frame", CFUNCTYPE(None, c_void_p, c_void_p)),
                ("axis_source", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32)),
                ("axis_stop", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_uint32)),
                ("axis_discrete", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_int)),
                ("axis_value120", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_int)),
                ("axis_relative_direction", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_uint32))]


class wl_seat_listener(Structure):
    _fields_ = [("capabilities", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32)),
                ("name", CFUNCTYPE(None, c_void_p, c_void_p, c_char_p))]


class wl_callback_listener(Structure):
    _fields_ = [("done", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32))]


class wl_buffer_listener(Structure):
    _fields_ = [("release", CFUNCTYPE(None, c_void_p, c_void_p))]


class wl_surface_listener(Structure):
    _fields_ = [("enter", CFUNCTYPE(None, c_void_p, c_void_p, c_void_p)),
                ("leave", CFUNCTYPE(None, c_void_p, c_void_p, c_void_p)),
                ("preferred_buffer_scale", CFUNCTYPE(None, c_void_p, c_void_p, c_int)),
                ("preferred_buffer_transform", CFUNCTYPE(None, c_void_p, c_void_p, c_uint32))]


def iface(name):
    if name not in INTERFACES:
        INTERFACES[name] = wl_interface(name.encode(), 1, 0, None, 0, None)
    return byref(INTERFACES[name])


for fn in ("wl_display_connect", "wl_display_get_registry", "wl_registry_add_listener",
           "wl_display_roundtrip", "wl_display_dispatch", "wl_display_flush",
           "wl_compositor_create_surface", "wl_shm_create_pool", "wl_shm_pool_create_buffer",
           "wl_surface_attach", "wl_surface_damage_buffer", "wl_surface_commit",
           "wl_seat_get_pointer", "wl_pointer_add_listener", "wl_seat_add_listener",
           "wl_surface_add_listener", "wl_buffer_add_listener", "wl_surface_damage"):
    getattr(wl, fn).restype = c_void_p

wl.wl_proxy_add_listener.restype = c_int
wl.wl_registry_bind.restype = c_void_p
wl.wl_registry_bind.argtypes = [c_void_p, c_uint32, c_void_p, c_uint32]
wl.wl_display_connect.argtypes = [c_char_p]
wl.wl_display_dispatch.argtypes = [c_void_p]
wl.wl_display_flush.argtypes = [c_void_p]
wl.wl_display_roundtrip.argtypes = [c_void_p]
wl.wl_display_get_registry.argtypes = [c_void_p]
wl.wl_registry_add_listener.argtypes = [c_void_p, c_void_p, c_void_p]
wl.wl_compositor_create_surface.argtypes = [c_void_p]
wl.wl_shm_create_pool.argtypes = [c_void_p, c_int, c_int]
wl.wl_shm_pool_create_buffer.argtypes = [c_void_p, c_int, c_int, c_int, c_int, c_uint32]
wl.wl_surface_attach.argtypes = [c_void_p, c_void_p, c_int, c_int]
wl.wl_surface_damage_buffer.argtypes = [c_void_p, c_int, c_int, c_int, c_int]
wl.wl_surface_commit.argtypes = [c_void_p]
wl.wl_seat_get_pointer.argtypes = [c_void_p]
wl.wl_pointer_add_listener.argtypes = [c_void_p, c_void_p, c_void_p]
wl.wl_seat_add_listener.argtypes = [c_void_p, c_void_p, c_void_p]
wl.wl_surface_add_listener.argtypes = [c_void_p, c_void_p, c_void_p]
wl.wl_buffer_add_listener.argtypes = [c_void_p, c_void_p, c_void_p]
wl.wl_proxy_add_listener.argtypes = [c_void_p, c_void_p, c_void_p]

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/vrwin-events.log"
W, H = 400, 300
log = open(OUT, "w", buffering=1)


def emit(msg):
    log.write(msg + "\n")


state = {"compositor": None, "shm": None, "seat": None, "pointer": None, "surface": None}


def on_global(data, registry, name, interface, version):
    iface_name = interface.decode()
    if iface_name == "wl_compositor":
        state["compositor"] = wl.wl_registry_bind(registry, name, iface("wl_compositor"), 4)
    elif iface_name == "wl_shm":
        state["shm"] = wl.wl_registry_bind(registry, name, iface("wl_shm"), 1)
    elif iface_name == "wl_seat":
        state["seat"] = wl.wl_registry_bind(registry, name, iface("wl_seat"), 5)


def on_global_remove(data, registry, name):
    pass


def on_caps(data, seat, caps):
    emit(f"seat caps={caps}")
    if caps & 1 and not state["pointer"]:  # WL_SEAT_CAPABILITY_POINTER
        state["pointer"] = wl.wl_seat_get_pointer(seat)
        wl.wl_pointer_add_listener(state["pointer"], byref(pointer_listener), None)
        wl.wl_display_flush(display)


def on_seat_name(data, seat, name):
    pass


def on_enter(data, pointer, serial, surface, sx, sy):
    emit(f"enter {sx},{sy}")


def on_leave(data, pointer, serial, surface):
    emit("leave")


def on_motion(data, pointer, t, sx, sy):
    emit(f"motion {sx},{sy}")


def on_button(data, pointer, serial, t, button, bstate):
    emit(f"button {button} {'press' if bstate else 'release'}")


def on_axis(data, pointer, t, axis, value):
    pass


def on_frame(data, pointer):
    pass


def on_axis_source(data, pointer, src):
    pass


def on_axis_stop(data, pointer, t, axis):
    pass


def on_axis_discrete(data, pointer, axis, v):
    pass


def on_axis_value120(data, pointer, axis, v):
    pass


def on_axis_rel(data, pointer, axis, d):
    pass


def on_surface_enter(data, surface, output):
    pass


def on_surface_leave(data, surface, output):
    pass


def on_pref_scale(data, surface, scale):
    pass


def on_pref_transform(data, surface, tr):
    pass


def on_buffer_release(data, buf):
    pass


pointer_listener = wl_pointer_listener(
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_void_p, c_int, c_int)(on_enter),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_void_p)(on_leave),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_int, c_int)(on_motion),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_uint32, c_uint32, c_uint32)(on_button),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_uint32, c_int)(on_axis),
    CFUNCTYPE(None, c_void_p, c_void_p)(on_frame),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32)(on_axis_source),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_uint32)(on_axis_stop),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_int)(on_axis_discrete),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_int)(on_axis_value120),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_uint32)(on_axis_rel),
)
seat_listener = wl_seat_listener(
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32)(on_caps),
    CFUNCTYPE(None, c_void_p, c_void_p, c_char_p)(on_seat_name),
)
surface_listener = wl_surface_listener(
    CFUNCTYPE(None, c_void_p, c_void_p, c_void_p)(on_surface_enter),
    CFUNCTYPE(None, c_void_p, c_void_p, c_void_p)(on_surface_leave),
    CFUNCTYPE(None, c_void_p, c_void_p, c_int)(on_pref_scale),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32)(on_pref_transform),
)
buffer_listener = wl_buffer_listener(CFUNCTYPE(None, c_void_p, c_void_p)(on_buffer_release))
registry_listener = wl_registry_listener(
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32, c_char_p, c_uint32)(on_global),
    CFUNCTYPE(None, c_void_p, c_void_p, c_uint32)(on_global_remove),
)

display = wl.wl_display_connect(None)
if not display:
    sys.exit("cannot connect to Wayland display")
registry = wl.wl_display_get_registry(display)
wl.wl_registry_add_listener(registry, byref(registry_listener), None)
wl.wl_display_roundtrip(display)
wl.wl_display_roundtrip(display)

if not (state["compositor"] and state["shm"]):
    sys.exit("missing wl_compositor or wl_shm")

# a solid-colour buffer so the surface is mapped and can receive pointer events
stride = W * 4
size = stride * H
fd = os.memfd_create("vrwin")
os.ftruncate(fd, size)
mm = mmap.mmap(fd, size)
mm[:] = b"\x20\x30\x40\xff" * (W * H)
pool = wl.wl_shm_create_pool(state["shm"], fd, size)
buf = wl.wl_shm_pool_create_buffer(pool, 0, W, H, stride, 0)  # WL_SHM_FORMAT_XRGB8888
wl.wl_buffer_add_listener(buf, byref(buffer_listener), None)
surface = wl.wl_compositor_create_surface(state["compositor"])
state["surface"] = surface
wl.wl_surface_add_listener(surface, byref(surface_listener), None)
wl.wl_surface_attach(surface, buf, 0, 0)
wl.wl_surface_damage_buffer(surface, 0, 0, W, H)
wl.wl_surface_commit(surface)

if state["seat"]:
    wl.wl_seat_add_listener(state["seat"], byref(seat_listener), None)
wl.wl_display_flush(display)
emit("READY")

while True:
    if wl.wl_display_dispatch(display) < 0:
        emit("dispatch failed")
        break
