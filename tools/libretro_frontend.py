"""Small headless Libretro ABI adapter used by POM2 tests and benchmarks."""

import ctypes as C
from pathlib import Path
import shutil

ENVIRONMENT = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIO_BATCH = C.CFUNCTYPE(C.c_size_t, C.POINTER(C.c_int16), C.c_size_t)
INPUT_POLL = C.CFUNCTYPE(None)
INPUT_STATE = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)
KEYBOARD = C.CFUNCTYPE(None, C.c_bool, C.c_uint, C.c_uint32, C.c_uint16)
AUDIO_STATUS = C.CFUNCTYPE(None, C.c_bool, C.c_uint, C.c_bool)
MIDI_ENABLED = C.CFUNCTYPE(C.c_bool)
MIDI_READ = C.CFUNCTYPE(C.c_bool, C.POINTER(C.c_uint8))
MIDI_WRITE = C.CFUNCTYPE(C.c_bool, C.c_uint8, C.c_uint32)


VFS_GET_PATH = C.CFUNCTYPE(C.c_char_p, C.c_void_p)
VFS_OPEN = C.CFUNCTYPE(C.c_void_p, C.c_char_p, C.c_uint, C.c_uint)
VFS_CLOSE = C.CFUNCTYPE(C.c_int, C.c_void_p)
VFS_SIZE = C.CFUNCTYPE(C.c_int64, C.c_void_p)
VFS_SEEK = C.CFUNCTYPE(C.c_int64, C.c_void_p, C.c_int64, C.c_int)
VFS_READ = C.CFUNCTYPE(C.c_int64, C.c_void_p, C.c_void_p, C.c_uint64)
VFS_PATH_OP = C.CFUNCTYPE(C.c_int, C.c_char_p)
VFS_RENAME = C.CFUNCTYPE(C.c_int, C.c_char_p, C.c_char_p)


class VfsInterface(C.Structure):
    _fields_ = [("get_path", VFS_GET_PATH), ("open", VFS_OPEN), ("close", VFS_CLOSE), ("size", VFS_SIZE),
                ("tell", VFS_SIZE), ("seek", VFS_SEEK), ("read", VFS_READ), ("write", VFS_READ),
                ("flush", VFS_CLOSE), ("remove", VFS_PATH_OP), ("rename", VFS_RENAME)]


class VfsInterfaceInfo(C.Structure):
    _fields_ = [("required_interface_version", C.c_uint32), ("iface", C.POINTER(VfsInterface))]


class MidiInterface(C.Structure):
    _fields_ = [("input_enabled", MIDI_ENABLED), ("output_enabled", MIDI_ENABLED),
                ("read", MIDI_READ), ("write", MIDI_WRITE), ("flush", MIDI_ENABLED)]
PAD = {"B": 0, "Y": 1, "SELECT": 2, "START": 3, "UP": 4, "DOWN": 5,
       "LEFT": 6, "RIGHT": 7, "A": 8, "X": 9, "L": 10, "R": 11,
       "L2": 12, "R2": 13, "L3": 14, "R3": 15}
KEY = {"RETURN": 13, "SPACE": 32, "ESC": 27, "UP": 273, "DOWN": 274,
       "RIGHT": 275, "LEFT": 276}


class Variable(C.Structure):
    _fields_ = [("key", C.c_char_p), ("value", C.c_char_p)]


class Game(C.Structure):
    _fields_ = [("path", C.c_char_p), ("data", C.c_void_p), ("size", C.c_size_t),
                ("meta", C.c_char_p)]


class Info(C.Structure):
    _fields_ = [("name", C.c_char_p), ("version", C.c_char_p), ("extensions", C.c_char_p),
                ("need_fullpath", C.c_bool), ("block_extract", C.c_bool)]


class AVInfo(C.Structure):
    _fields_ = [("base_width", C.c_uint), ("base_height", C.c_uint),
                ("max_width", C.c_uint), ("max_height", C.c_uint),
                ("aspect", C.c_float), ("fps", C.c_double), ("sample_rate", C.c_double)]


class DiskControl(C.Structure):
    _fields_ = [("set_eject_state", C.CFUNCTYPE(C.c_bool, C.c_bool)),
                ("get_eject_state", C.CFUNCTYPE(C.c_bool)),
                ("get_image_index", C.CFUNCTYPE(C.c_uint)),
                ("set_image_index", C.CFUNCTYPE(C.c_bool, C.c_uint)),
                ("get_num_images", C.CFUNCTYPE(C.c_uint))]


class Frontend:
    def __init__(self, binary, system, saves, options):
        self.system = str(system).encode()
        self.saves = str(saves).encode() if saves is not None else None
        self.options = {key.encode(): val.encode() for key, val in options.items()}
        self.option_names = set()
        self.keyboard = None
        self.disk = None
        self.video = None
        self.format = 0
        self.frames = 0
        self.audio_frames = 0
        self.polls = 0
        self.midi = []            # (byte, microseconds since the previous one)
        self.audio_status = None  # the core's audio buffer status callback, if set
        # Android's saf:// paths, served from this folder by a read-only VFS
        # (set before load); None: no VFS offered.
        self.vfs_root = getattr(self, "vfs_root", None)
        self.pad = set()
        self.analog = (0, 0)
        self.core = C.CDLL(str(binary))
        self.core.retro_api_version.restype = C.c_uint
        if self.core.retro_api_version() != 1:
            raise RuntimeError("module did not report Libretro API version 1")
        self.core.retro_get_system_info.argtypes = [C.POINTER(Info)]
        self.core.retro_get_system_av_info.argtypes = [C.POINTER(AVInfo)]
        self.core.retro_load_game.argtypes = [C.POINTER(Game)]
        self.core.retro_load_game.restype = C.c_bool
        self.core.retro_set_controller_port_device.argtypes = [C.c_uint, C.c_uint]
        for name in ("init", "deinit", "unload_game", "run", "reset", "get_system_info",
                     "get_system_av_info", "set_controller_port_device"):
            getattr(self.core, "retro_" + name).restype = None
        self.callbacks = []
        for setter, kind, method in (
            ("environment", ENVIRONMENT, self.environment),
            ("video_refresh", VIDEO, self.receive_video),
            ("audio_sample", AUDIO, self.one_audio),
            ("audio_sample_batch", AUDIO_BATCH, self.batch_audio),
            ("input_poll", INPUT_POLL, self.poll),
            ("input_state", INPUT_STATE, self.input_state),
        ):
            callback = kind(method)
            setter = getattr(self.core, "retro_set_" + setter)
            setter.argtypes = [kind]
            setter.restype = None
            setter(callback)
            self.callbacks.append(callback)
        self.core.retro_init()
        self.loaded = False

    def environment(self, command, data):
        if command in (9, 31):  # GET_SYSTEM_DIRECTORY / GET_SAVE_DIRECTORY
            C.cast(data, C.POINTER(C.c_char_p))[0] = self.system if command == 9 else self.saves
        elif command == 10:  # SET_PIXEL_FORMAT
            self.format = C.cast(data, C.POINTER(C.c_int))[0]
            return self.format in (0, 1, 2)
        elif command == 12:  # SET_KEYBOARD_CALLBACK
            self.keyboard = KEYBOARD(C.cast(data, C.POINTER(C.c_void_p))[0])
        elif command in (13, 58):  # SET_DISK_CONTROL[_EXT]_INTERFACE
            # Both versions start with the same eject/index/count callbacks.
            self.disk = DiskControl.from_buffer_copy(C.string_at(data, C.sizeof(DiskControl)))
        elif command == 15:  # GET_VARIABLE
            var = Variable.from_address(data)
            if var.key not in self.options:
                return False
            var.value = self.options[var.key]
        elif command == 16:  # SET_VARIABLES (version 0 options)
            variables = C.cast(data, C.POINTER(Variable))
            index = 0
            while variables[index].key:
                self.option_names.add(variables[index].key.decode())
                index += 1
        elif command == 17:  # GET_VARIABLE_UPDATE
            C.cast(data, C.POINTER(C.c_bool))[0] = False
        elif command in (52, 57):  # GET_CORE_OPTIONS_VERSION / GET_DISK_CONTROL_INTERFACE_VERSION
            C.cast(data, C.POINTER(C.c_uint))[0] = int(command == 57)
        elif command == 45 | 0x10000:  # GET_VFS_INTERFACE
            if self.vfs_root is None:
                return False
            if not hasattr(self, "_vfs"):
                self._vfs_files = {}
                def vopen(path, mode, hints):
                    p = path.decode()
                    if not p.startswith("saf://tree/") or mode != 1:
                        return None
                    local = Path(self.vfs_root) / p[len("saf://tree/"):]
                    if not local.is_file():
                        return None
                    handle = len(self._vfs_files) + 1
                    self._vfs_files[handle] = [local.read_bytes(), 0]
                    return handle
                def vread(handle, buffer, length):
                    data, pos = self._vfs_files[handle]
                    chunk = data[pos:pos + length]
                    C.memmove(buffer, chunk, len(chunk))
                    self._vfs_files[handle][1] = pos + len(chunk)
                    return len(chunk)
                self._vfs = VfsInterface(VFS_GET_PATH(lambda h: None), VFS_OPEN(vopen),
                                         VFS_CLOSE(lambda h: 0), VFS_SIZE(lambda h: len(self._vfs_files[h][0])),
                                         VFS_SIZE(lambda h: self._vfs_files[h][1]), VFS_SEEK(lambda h, o, w: -1),
                                         VFS_READ(vread), VFS_READ(lambda h, b, n: -1), VFS_CLOSE(lambda h: -1),
                                         VFS_PATH_OP(lambda p: -1), VFS_RENAME(lambda a, b: -1))
            C.cast(data, C.POINTER(VfsInterfaceInfo))[0].iface = C.pointer(self._vfs)
        elif command == 3:  # GET_CAN_DUPE
            C.cast(data, C.POINTER(C.c_bool))[0] = True
        elif command == 62:  # SET_AUDIO_BUFFER_STATUS_CALLBACK
            self.audio_status = AUDIO_STATUS(C.cast(data, C.POINTER(C.c_void_p))[0]) if data else None
        elif command == 63:  # SET_MINIMUM_AUDIO_LATENCY
            pass
        elif command == 48 | 0x10000:  # GET_MIDI_INTERFACE
            if not hasattr(self, "_midi"):
                def record(byte, delta):
                    self.midi.append((byte, delta))
                    return True
                self._midi = MidiInterface(MIDI_ENABLED(lambda: False), MIDI_ENABLED(lambda: True),
                                           MIDI_READ(lambda byte: False), MIDI_WRITE(record),
                                           MIDI_ENABLED(lambda: True))
            C.memmove(data, C.byref(self._midi), C.sizeof(MidiInterface))
        elif command not in (6, 11, 13, 18, 35, 58, 60):  # notifications unused by this workload
            return False
        return True

    def receive_video(self, data, width, height, pitch):
        if data:
            stride = width * (4 if self.format == 1 else 2)
            if width and height and pitch >= stride:
                self.video = (C.string_at(data, pitch * (height - 1) + stride), width, height, pitch)

    def poll(self):
        self.polls += 1

    def one_audio(self, left, right):
        self.audio_frames += 1

    def batch_audio(self, data, frames):
        self.audio_frames += frames
        return frames

    def input_state(self, port, device, index, button):
        if port == 0 and device == 5 and index == 0 and button < 2:
            return self.analog[button]
        return int(port == 0 and device == 1 and button in self.pad)

    def swap_disk(self, index):
        disk = self.disk
        if disk is None or index >= disk.get_num_images():
            raise RuntimeError("frontend disk swap requires an available playlist entry")
        if not (disk.set_eject_state(True) and disk.get_eject_state()
                and disk.set_image_index(index) and disk.set_eject_state(False)
                and not disk.get_eject_state() and disk.get_image_index() == index):
            raise RuntimeError(f"frontend could not insert playlist entry {index}")

    def load(self, path):
        game = Game(str(path).encode(), None, 0, None) if path else None
        self.loaded = self.core.retro_load_game(C.byref(game) if game else None)
        if not self.loaded:
            raise RuntimeError("retro_load_game rejected the content")
        self.core.retro_set_controller_port_device(0, 1)
        info, av = Info(), AVInfo()
        self.core.retro_get_system_info(C.byref(info))
        self.core.retro_get_system_av_info(C.byref(av))
        return {"name": info.name.decode(), "version": info.version.decode(), "fps": av.fps,
                "sample_rate": av.sample_rate, "base_size": [av.base_width, av.base_height]}

    def frame(self, count=1):
        for _ in range(count):
            self.core.retro_run()
            self.frames += 1

    def key(self, name):
        if self.keyboard is None:
            raise RuntimeError("core did not provide the keyboard callback")
        key = KEY.get(name, ord(name.lower()) if len(name) == 1 else None)
        if key is None:
            raise ValueError(f"unknown key: {name}")
        character = ord(name) if len(name) == 1 else key if key < 128 else 0
        self.keyboard(True, key, character, 0)
        self.keyboard(False, key, character, 0)
        self.frame(2)

    def image(self):
        from PIL import Image

        if self.video is None:
            raise RuntimeError("no video frame received")
        data, width, height, pitch = self.video
        rawmode = {0: "BGR;15", 1: "BGRX", 2: "BGR;16"}[self.format]
        return Image.frombytes("RGB", (width, height), data.ljust(pitch * height, b"\0"),
                               "raw", rawmode, pitch, 1)

    def close(self):
        if self.loaded:
            self.core.retro_unload_game()
        self.core.retro_deinit()


def prepare_system(path, roms):
    (path / "pom2").mkdir(parents=True)
    for name in ("apple2e.rom", "apple2e_char.rom", "disk2.rom", "diskii_p6.rom"):
        source = roms / name
        if source.is_file():
            shutil.copyfile(source, path / "pom2" / name)
