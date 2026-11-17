echo "=== hmmesa diag"
f=$SRC/dl/mesa-25.0.7.tar.xz; [ -s $f ] || curl -fsSL -o $f https://archive.mesa3d.org/mesa-25.0.7.tar.xz
echo "592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6  $f" | sha256sum -c
d=$HOME/hmmesa-peek; mkdir -p $d; tar xJf $f -C $d mesa-25.0.7/meson.build mesa-25.0.7/meson_options.txt mesa-25.0.7/src/freedreno/meson.build mesa-25.0.7/src/gallium/drivers/freedreno/meson.build mesa-25.0.7/src/gallium/meson.build
cd $d/mesa-25.0.7; grep -n "libdrm\|kgsl\|freedreno" meson.build | head -50; echo ---; cat src/freedreno/meson.build | head -80
echo "=== hmmesa diag end"
