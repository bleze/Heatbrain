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

set -u
PROJECT_DIR="${PROJECT_DIR:-$(cd "$(dirname "$0")" && pwd)}"
cd "$PROJECT_DIR" || { echo "heatbrain update: $PROJECT_DIR not found"; exit 1; }

if docker compose version >/dev/null 2>&1; then
    DC="docker compose"
elif command -v docker-compose >/dev/null 2>&1; then
    DC="docker-compose"
else
    echo "heatbrain update: docker compose not found (is Container Manager installed?)"
    exit 1
fi

before=$(docker inspect --format '{{.Image}}' heatbrain 2>/dev/null || echo none)
$DC pull heatbrain || { echo "heatbrain update: pull failed"; exit 1; }
$DC up -d heatbrain || { echo "heatbrain update: start failed"; exit 1; }
after=$(docker inspect --format '{{.Image}}' heatbrain 2>/dev/null || echo none)

if [ "$before" = "$after" ]; then
    echo "heatbrain is up to date"
else
    echo "heatbrain updated and restarted"
    docker image prune -f >/dev/null 2>&1  # remove the replaced image
fi
