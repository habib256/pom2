"""Small headless Libretro ABI adapter used by POM2 tests and benchmarks."""

import ctypes as C
import shutil

ENVIRONMENT = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIO_BATCH = C.CFUNCTYPE(C.c_size_t, C.POINTER(C.c_int16), C.c_size_t)
INPUT_POLL = C.CFUNCTYPE(None)
INPUT_STATE = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)
KEYBOARD = C.CFUNCTYPE(None, C.c_bool, C.c_uint, C.c_uint32, C.c_uint16)
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


class Frontend:
    def __init__(self, binary, system, saves, options):
        self.system = str(system).encode()
        self.saves = str(saves).encode()
        self.options = {key.encode(): val.encode() for key, val in options.items()}
        self.option_names = set()
        self.keyboard = None
        self.video = None
        self.format = 0
        self.frames = 0
        self.audio_frames = 0
        self.polls = 0
        self.pad = set()
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
        return int(port == 0 and device == 1 and button in self.pad)

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
