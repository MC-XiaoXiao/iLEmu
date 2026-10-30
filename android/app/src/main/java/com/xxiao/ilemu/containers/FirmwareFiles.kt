package com.xxiao.ilemu.containers

import java.io.InputStream
import java.io.OutputStream

// Frontend stream adapters only. Both platforms share permission, metadata,
// archive validation and legacy cleanup rules in host_firmware.
internal object FirmwareFiles {
    init { System.loadLibrary("SDL2"); System.loadLibrary("ilemu") }
    external fun prepare(path: String)
    external fun copy(source: String, destination: String)
    external fun remove(path: String)
    external fun unpack(input: InputStream, root: String)
    external fun pack(root: String, output: OutputStream)
}
