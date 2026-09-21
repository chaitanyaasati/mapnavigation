#!/bin/sh
# Build tiles + place index for a city and publish them to the GitHub Pages tiles repo.
#
#   tools/add_city.sh <id> "<City, Country>" <west> <south> <east> <north> <extract.osm.pbf> [lon lat zoom]
#
#   tools/add_city.sh mumbai "Mumbai, India" 72.77 18.89 73.03 19.28 ~/Documents/Arduino/maps-data/western-zone-latest.osm.pbf 72.8777 19.0760 13
#
# <id> becomes the folder / URL segment. The extract can be any Geofabrik .pbf that
# contains the bbox (https://download.geofabrik.de). Afterwards, on the device:
#   serial:  t https://chaitanyaasati.github.io/mapnavigation-tiles/<id>
#   or the WiFi setup page -> "Map tile server".
set -e
cd "$(dirname "$0")"
ID=$1; CITY=$2; W=$3; S=$4; E=$5; N=$6; PBF=$7; CLON=$8; CLAT=$9; CZ=${10}
TILES_REPO=${TILES_REPO:-$HOME/Documents/Arduino/mapnavigation-tiles}
[ -n "$PBF" ] || { sed -n 2,12p "$0"; exit 1; }
[ -x .venv/bin/python ] || { echo "run: uv venv -p 3.12 .venv && uv pip install -p .venv/bin/python osmium shapely numpy pillow"; exit 1; }

echo "== extracting $ID from $PBF"
osmium extract -b "$W,$S,$E,$N" -s smart -o "out/$ID.osm.pbf" --overwrite "$PBF"
CENTER=""; [ -n "$CZ" ] && CENTER="--center $CLON $CLAT $CZ"
echo "== building tiles"
.venv/bin/python build_tiles.py "out/$ID.osm.pbf" "out/$ID" --bbox "$W" "$S" "$E" "$N" --name "$ID" --city "$CITY" $CENTER
echo "== building place index"
.venv/bin/python build_places.py "out/$ID.osm.pbf" "out/$ID/places.bin" --bbox "$W" "$S" "$E" "$N"

if [ -d "$TILES_REPO/.git" ]; then
  echo "== publishing to $TILES_REPO"
  rm -rf "$TILES_REPO/$ID"; cp -R "out/$ID" "$TILES_REPO/$ID"
  (cd "$TILES_REPO" && git add -A && git commit -qm "$ID: tiles + places" && git push -q origin main)
  echo "Published. Pages needs a few minutes, then set the device to:"
  echo "  https://chaitanyaasati.github.io/mapnavigation-tiles/$ID"
else
  echo "tiles repo not found at $TILES_REPO (set TILES_REPO=...); tiles are in tools/out/$ID"
fi
