# Force gnu17 for C sources so old libraries (e.g. QRCode 0.0.1) build
# under gcc 15, whose default C23 makes true/false keywords.
# Prepend: the later -std=gnu++2a in build_flags still wins for C++.
Import("env")
env.Prepend(CCFLAGS=["-std=gnu17"])
