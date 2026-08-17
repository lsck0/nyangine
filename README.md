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
git clone --recursive http://github.com/lsck0/nyangine.git
```

or

```bash
git clone http://github.com/lsck0/nyangine.git
cd nyangine
git submodule update --init --recursive
```

and bootstrap the build system with

```bash
clang build.c -o build -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix -Wno-initializer-overrides -Wno-gcc-compat -I./ -I./src -DNYA_NO_SDL -lm -pthread
```

then run

```bash
./build
```

to get the available commands.

## Dependencies

Dependencies are listed in ./.github/ci-packages.txt

## Signing Key

```bash
openssl req -x509 -newkey rsa:3072 -nodes -days 3650 \
    -keyout sample.key -out sample.crt \
    -subj "/CN=nyangine sample/O=nyangine/C=DE" \
    -addext "keyUsage=critical,digitalSignature" \
    -addext "extendedKeyUsage=critical,codeSigning" \
    -addext "basicConstraints=critical,CA:FALSE"

openssl pkcs12 -export -inkey sample.key -in sample.crt \
    -out .signing/sample.pfx -name "nyangine sample" \
    -passout pass:nyangine-sample-certificate
```

## Windows

In theory, building works on Windows. In practice, I don't use Windows, so who knows.

```bash
clang build.c -o build.exe -std=c2y -mavx -mavx2 -fdefer-ts -fenable-matrix -Wno-initializer-overrides -Wno-gcc-compat -I./ -I./src -DNYA_NO_SDL -lm -pthread
```
