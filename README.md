<div>
    <h1 align="center">
        nyangine
    </h1>
    <h3 align="center">
        Game Engine
    </h3>
</div>

Clone the repository recursively with

```bash
git clone --recursive http://github.com/lsck0/specturm.git
```

or

```bash
git clone http://github.com/lsck0/specturm.git
cd specturm
git submodule update --init --recursive
```

and bootstrap the build system with

````bash
clang build.c -o build -std=c23 -fenable-matrix -I./src -lm -lSDL3 2> /dev/null
```

then run

```bash
./build
````

to get the available commands.
