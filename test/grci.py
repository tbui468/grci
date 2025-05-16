from ctypes import *
import os

g = None
lib = None

def init():
    global lib
    if os.name == 'posix':
        lib = CDLL("/home/thomas/hdl/libgrci.so")
    elif os.name == 'nt':
        lib = cdll.LoadLibrary("C:\\Users\\thoma\\Desktop\\hdl\\grci.dll")
    else:
        print("OS not recognized")

    #this must match order of fields in struct grci_module
    class GRCIModule(Structure):
        _fields_ = [("input_count", c_int),
                    ("output_count", c_int),
                    ("inputs", POINTER(c_bool)),
                    ("outputs", POINTER(c_bool))]

    class GRCISubmodule(Structure):
        _fields_ = [("state_count", c_int),
                    ("states", POINTER(c_bool))]
                    

    lib.grci_easy_init.argtypes = []
    lib.grci_easy_init.restype = c_void_p

    lib.grci_compile_src.argtypes = [c_void_p, c_char_p, c_size_t]
    lib.grci_compile_src.restype = None

    lib.grci_init_module.argtypes = [c_void_p, c_char_p, c_size_t]
    lib.grci_init_module.restype = POINTER(GRCIModule)

    lib.grci_submodule.argtypes = [c_void_p, c_char_p, c_size_t]
    lib.grci_submodule.restype = POINTER(GRCISubmodule)

    lib.grci_step_module.argtypes = [c_void_p]
    lib.grci_step_module.restype = c_bool

    lib.grci_destroy_module.argtypes = [c_void_p]
    lib.grci_destroy_module.restype = None

    lib.grci_cleanup.argtypes = [c_void_p]
    lib.grci_cleanup.restype = None


    global g
    g = lib.grci_easy_init()


def compile_src(src):
    size = len(src)
    c_string = src.encode('utf-8')
    lib.grci_compile_src(g, c_string, c_size_t(size))


class Submodule:
    def __init__(self, submodule):
        self.submodule = submodule
        self.states = [False] * self.submodule.contents.state_count

    def write_states(self):
        for idx, s in enumerate(self.states):
            self.submodule.contents.states[idx] = s
       
    def read_states(self): 
        for idx in range(self.submodule.contents.state_count):
            self.states[idx] = self.submodule.contents.states[idx]

class Module:
    def __init__(self, name):
        c_name = name.encode('utf-8')
        self.module = lib.grci_init_module(g, c_name, c_size_t(len(name)))
        self.input_count = self.module.contents.input_count
        self.output_count = self.module.contents.output_count
        self.inp = [False] * self.input_count
        self.out = [False] * self.output_count
        self.submodules = {}

    def step(self):
        for idx, value in enumerate(self.inp):
            self.module.contents.inputs[idx] = value

        for key in self.submodules:
            self.submodules[key].write_states()

        clock = lib.grci_step_module(self.module)

        for idx in range(self.module.contents.output_count):
            self.out[idx] = self.module.contents.outputs[idx]

        for key in self.submodules:
            self.submodules[key].read_states()

        return clock

    #returns a reference to the list of module states
    def submodule(self, name):
        if name in self.submodules:
            return self.submodules[name]
        c_name = name.encode('utf-8')
        c_size = c_size_t(len(name))
        self.submodules[name] = Submodule(lib.grci_submodule(self.module, c_name, c_size))
        return self.submodules[name]

    def __del__(self):
        #destroy module only if quit has not been called (quit will free everything)
        if not (g == None and lib == None):
            lib.grci_destroy_module(self.module)


def quit():
    global g, lib
    lib.grci_cleanup(g)
    g = None
    lib = None


