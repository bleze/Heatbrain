#!/bin/sh
# Updates heatbrain on a Synology NAS: pulls the newest image and restarts the container if it
# changed. Container Manager does not check ghcr.io images for updates, so run this instead:
#
#   Control Panel -> Task Scheduler -> Create -> Scheduled Task -> User-defined script
#     General:  Task "heatbrain update", User "root"
#     Schedule: daily, e.g. 04:00 (or leave it disabled and use "Run" when you want to update)
#     Task Settings -> Run command:
#       sh /volume1/docker/heatbrain/update.sh
#
# Copy this file to /volume1/docker/heatbrain/ (the project folder, next to the compose file).
# Your settings and history live in that folder too, so an update never touches them.
# heatbrain saves the night schedules it loaded, so updating during a Wi-Fi off period is fine.
# To see this script's output: Task Scheduler -> Settings -> "Save output results" to a folder.

set -u
# Task Scheduler runs with a minimal PATH; Synology keeps docker in /usr/local/bin.
PATH="/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin:${PATH:-}"
export PATH

fail() { echo "heatbrain update FAILED: $*"; exit 1; }

PROJECT_DIR="${PROJECT_DIR:-$(cd "$(dirname "$0")" && pwd)}"
cd "$PROJECT_DIR" || fail "folder $PROJECT_DIR not found"
echo "heatbrain update in $PROJECT_DIR ($(date '+%Y-%m-%d %H:%M'))"

[ "$(id -u)" = 0 ] || fail "runs as $(id -un), but only root may use Docker: Task Scheduler -> Edit -> General -> User: root"
command -v docker >/dev/null 2>&1 || fail "docker not found (is Container Manager installed?)"
if docker compose version >/dev/null 2>&1; then
    DC="docker compose"
elif command -v docker-compose >/dev/null 2>&1; then
    DC="docker-compose"
else
    fail "docker compose not found"
fi
COMPOSE_FILE=""
for f in compose.yaml compose.yml docker-compose.yaml docker-compose.yml; do
    [ -f "$f" ] && { COMPOSE_FILE="$f"; break; }
done
[ -n "$COMPOSE_FILE" ] ||
    fail "no compose file in $PROJECT_DIR (it has: $(ls | xargs)) - copy this script into the Container Manager project folder"
echo "using: $DC with $COMPOSE_FILE"

IMAGE=$(docker inspect --format '{{.Config.Image}}' heatbrain 2>/dev/null || echo "ghcr.io/bleze/heatbrain:latest")
running=$(docker inspect --format '{{.Image}}' heatbrain 2>/dev/null || echo none)
echo "pulling the newest image..."
$DC pull heatbrain || fail "pull failed (internet? ghcr.io reachable?)"
newest=$(docker image inspect --format '{{.Id}}' "$IMAGE" 2>/dev/null || echo unknown)

if [ "$running" = "$newest" ]; then
    echo "heatbrain is up to date"
    exit 0
fi

# Stop it properly first: heatbrain finishes its control step (a slow plug can take a while) and
# exits cleanly. Letting Docker replace it directly allows only 15 s before a forced kill, which
# DSM reports as "stopped unexpectedly".
if [ "$running" != none ]; then
    echo "stopping heatbrain (up to 60 s)..."
    $DC stop -t 60 heatbrain || fail "stop failed"
    echo "old version exited with code $(docker inspect --format '{{.State.ExitCode}}' heatbrain 2>/dev/null || echo '?') (0 = clean stop)"
fi
echo "starting the new version..."
$DC up -d heatbrain || fail "start failed"
echo "heatbrain updated and restarted"
docker image prune -f >/dev/null 2>&1  # remove the replaced image
