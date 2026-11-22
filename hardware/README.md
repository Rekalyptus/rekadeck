# Build a Rekadeck

Everything for the case and the build. The pin-by-pin wiring, the display and battery life are also covered in
[docs/HARDWARE.md](../docs/HARDWARE.md).

| File | What it is |
|---|---|
| [rekadeck-parts-list.pdf](rekadeck-parts-list.pdf) | Every part to buy, with links |
| [rekadeck-assembly-instructions.pdf](rekadeck-assembly-instructions.pdf) | The build in 16 steps, on one large page (zoom in) |
| [3d-print/](3d-print/) | The case parts to print (STL), and how to print them |
| [cad/](cad/) | STEP files of every part and the whole assembly, to change the design |

## Parts

| # | Part | Link |
|---|---|---|
| 1 | SparkFun Thing Plus ESP32-S3 (WRL-24408) | [Amazon](https://www.amazon.com/dp/B0D1BB79SM) or [SparkFun](https://www.sparkfun.com/sparkfun-thing-plus-esp32-s3.html) |
| 2 | Waveshare 7.5" e-Paper HAT, black and white | [Amazon](https://www.amazon.com/dp/B07Z25LWTS) |
| 3 | Winfox 505573 LiPo, 3.7 V 2500 mAh | [Amazon](https://www.amazon.com/dp/B0GDQN275B) |
| 4 | Kailh Choc V1 Brown switches (4 needed, sold in 20 packs) | [Amazon](https://www.amazon.com/dp/B0D49JPM13) |
| 5 | Silicone bumpers, 10 x 3 mm (4 needed) | [Amazon](https://www.amazon.com/dp/B0FGGZ7BVR) |
| 6 | 3M 300LSE thin double-sided tape, for the battery | [Amazon](https://www.amazon.com/dp/B0CRRFHKQC) |
| 7 | Kapton tape | [Amazon](https://www.amazon.com/dp/B0049KTIAU) |
| 8 | M2 socket head screw kit: 16 x M2 x 6 and 2 x M2 x 12 | [Amazon](https://www.amazon.com/dp/B07W5J19Y5) |
| 9 | 28 AWG silicone wire, red and black, about 1.5 m | [Amazon](https://www.amazon.com/dp/B01F8PO7UK) |

Together they came to about $176 in November 2026. You'll also need:

- a JST-PH 2-pin extension, 150 mm, plug on one end and socket on the other, for the battery
- a soldering iron, flush cutters, tweezers and the 1.5 mm hex key that comes with the screw kit
- a multimeter, to check the battery's polarity before you plug it in
- hot glue, for tacking wires down (a low-temperature gun is gentler on them)

## Print the case

Five parts and four keycaps. Most print without supports. Material and support notes:
[3d-print/README.md](3d-print/README.md).

## Assemble it

Follow [rekadeck-assembly-instructions.pdf](rekadeck-assembly-instructions.pdf). It covers setting up the HAT,
wiring the keys and the board, fitting the panel and closing the case. Every screw threads straight into the plastic,
so turn each one until its head seats and stop there.

Don't skip step 12: check the battery's polarity with a multimeter before you plug it into the board. Neither the
plug's shape nor the wire colours tell you which way round it's wired, and a backwards battery can kill the board in
seconds.

Before you close the base (step 14), plug in USB-C, [install the firmware](../README.md#install-the-firmware) and
run **Hardware check** (key 1 on the Wi-Fi setup screen) to make sure the screen draws and all four keys respond.

## Change the design

The [cad/](cad/) folder has a STEP file for every printed part, plus `full_assembly_reference.step`: the whole device
with simplified models of the board, HAT, panel, adapter, battery and switches, so you can check that a change still
fits. STEP opens in FreeCAD, Fusion, Onshape, SolidWorks and most other CAD programs. Export the parts you change as
STL to print them.
