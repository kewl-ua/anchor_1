#!/usr/bin/env bash
# Builds the ground textures in assets/terrain from CC0 photo textures (see
# assets/CREDITS.md for where each comes from) with tools/pixelize.
#
#   tools/make_terrain.sh <folder with the downloaded sources> [pixelize.exe]
#
# Each texture's average colour is moved to its terrain's colour in
# src/theme/palette.h (so the minimap and the fallback look the same), and
# brightened by --gain 1.16: the game shades level ground down to 220/255.
set -euo pipefail

src="${1:?the folder with the downloaded source textures}"
tool="${2:-build/bin/Release/pixelize.exe}"
out="$(dirname "$0")/../assets/terrain"
mkdir -p "$out"

photo() {  # name, source file, R,G,B, extra options...
    local name="$1" file="$2" colour="$3"
    shift 3
    "$tool" photo "$src/$file" "$out/$name.png" --match "$colour" --gain 1.16 "$@"
}

photo grass    Grass004_1K-JPG_Color.jpg           84,116,62  --colors 16
photo forest   forest_leaves_02_diffuse_1k.jpg     50,76,42   --colors 16
photo trail    grass_path_2_diff_1k.jpg            118,102,72 --colors 16
photo urban    gravel_ground_01_diff_1k.jpg        122,118,108 --colors 16
photo rock     aerial_rocks_02_diff_1k.jpg         104,106,102 --colors 16 --contrast 0.7
photo concrete concrete_floor_worn_001_diff_1k.jpg 108,110,108 --colors 12 --contrast 1.0
photo dirtroad aerial_mud_1_diff_1k.jpg            138,114,80 --colors 14
photo plowed   dirt_aerial_02_diff_1k.jpg          110,92,66  --colors 14
# Sunflowers and maize: the grass, gone yellow (the rows are drawn on top).
photo crops    Grass004_1K-JPG_Color.jpg           138,140,64 --colors 14 --contrast 0.9
# Bog: wet mud gone green, water standing in the low spots.
photo swamp    brown_mud_02_diff_1k.jpg            64,86,68   --colors 14 --puddles 52,76,92,0.38 --seed 3
photo crater   burned_ground_01_diff_1k.jpg        92,82,68   --colors 14
photo riverbed dry_riverbed_rock_diff_1k.jpg       150,136,106 --colors 16
# Dug earth: trenches, foxholes, dugouts, gun pits.
photo earth    brown_mud_dry_diff_1k.jpg           98,84,62   --colors 14
# Water is generated: swells and ripples.
"$tool" water "$out/water.png" --match 56,94,136 --gain 1.16 --colors 10 --seed 7
