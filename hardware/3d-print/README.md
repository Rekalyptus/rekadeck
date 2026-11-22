# 3D printing

Print one of each of these:

- `bottom_portion_bottom_plate.stl`
- `bottom_portion_main_base_shell.stl`
- `top_portion_back_cover.stl`
- `top_portion_front_slab_shell.stl`
- `top_portion_mid_frame.stl`

Print 4 of this one:

- `keycap.stl`

## Material

I printed mine in ABS, but it can be tricky to work with. When it's heated it gives off styrene fumes that aren't
safe to breathe, so only use it if your setup can vent them (mine does). PETG is a good alternative if you live
somewhere warm or the device might sit in high heat. For most situations PLA is totally fine. Just don't use
conductive filament, since it could short the electronics.

## Supports

Most of the parts print without supports. The one exception is `bottom_portion_main_base_shell`, which has an
overhang (the little J-shaped cable hook inside) that needs supports. You only need to add supports under that hook,
not the whole part.

If your slicer adds supports to the bottom of `bottom_portion_bottom_plate`, you'll need to remove them to open up the
screw holes and fit the silicone bumpers. Be careful when you do this, because it's really easy to stab your finger.

Next: [assemble it](../README.md#assemble-it).
