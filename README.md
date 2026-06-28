# pipe

A JUCE/C++ music game inspired by the supplied 12x12x12 pipe matrix HTML demo.

The editor uses a 2D slice to build a 3D pipe network. Taps emit water pulses through connected pipework, and valves trigger pitched synth notes when a pulse reaches them. The right panel shows an isometric view of the whole 3D structure.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug -j4
```

The app bundle is created at:

```text
build/pipe_artefacts/Debug/pipe.app
```

## Play

- Use `SELECT` to inspect cells without changing the pipework.
- Drag with `PIPE` selected to draw pipe routes on the current 2D layer.
- Switch faces and layers to build into the 3D volume.
- Place `TAP` markers as water sources.
- Place `VALVE` markers as sounding points.
- Place `DRAIN` markers to let droplets fall out of the pipework. Falling droplets drop twice as fast, disappear at the cube floor, and rejoin if they hit pipework below.
- Click a cell to inspect it. If it contains a valve, use `NOTE -` and `NOTE +` to tune that valve.
- Press `PLAY` to animate the water and hear valves.
- While playing, click an existing tap to turn it on or off without deleting it.
- While playing, click an existing drain to open or close it without deleting it.
- Use the File menu to save or load patches as JSON.
- `DEMO` reloads the starter patch; `CLEAR` empties the grid.

Keyboard shortcuts: `1` select, `2` pipe, `3` tap, `4` valve, `5` drain, `E` erase, backspace/delete removes the selected cell in select mode, space play/stop, `[` and `]` change layer.
