#!/bin/bash
# Publish RemotePlay to GitHub + trigger the Release build.
# Usage:  ./publish_github.sh <GITHUB_TOKEN>
#
# Pushes main + tag v0.1.0. CI (windows-build.yml) then builds the MSVC
# release, runs all 10 test suites, packages the zip and creates the
# GitHub Release with the downloadable asset automatically.
set -euo pipefail

TOKEN="${1:?usage: publish_github.sh <GITHUB_TOKEN>}"
REPO="Sadrtile123/IranRemotePlay"
ROOT=/home/z/my-project/RemotePlay
TAG="v0.1.0"

cd "$ROOT"

# One-time push URL (token never written to .git/config).
git push "https://Sadrtile123:${TOKEN}@github.com/${REPO}.git" main
git tag -f "$TAG"
git push "https://Sadrtile123:${TOKEN}@github.com/${REPO}.git" -f "$TAG"

echo ""
echo "Pushed main + $TAG. CI is building the release:"
echo "  https://github.com/${REPO}/actions"
echo ""
echo "Watch it finish, then the release appears at:"
echo "  https://github.com/${REPO}/releases/tag/$TAG"

# Optional: also attach the locally cross-compiled zip as a second asset.
if [[ -f /home/z/my-project/download/RemotePlay-0.1.0-Windows-x64.zip ]]; then
    echo ""
    read -r -p "Also upload the local MinGW build zip as an asset? [y/N] " yn
    if [[ "$yn" == "y" || "$yn" == "Y" ]]; then
        REL_ID=$(curl -s -H "Authorization: token ${TOKEN}" \
            "https://api.github.com/repos/${REPO}/releases/tags/${TAG}" | python3 -c \
            'import json,sys; d=json.load(sys.stdin); print(d.get("id",""))')
        if [[ -n "$REL_ID" ]]; then
            curl -s -X POST \
              -H "Authorization: token ${TOKEN}" -H "Content-Type: application/zip" \
              --data-binary @/home/z/my-project/download/RemotePlay-0.1.0-Windows-x64.zip \
              "https://uploads.github.com/repos/${REPO}/releases/${REL_ID}/assets?name=RemotePlay-0.1.0-Windows-x64-mingw.zip" \
              | python3 -c 'import json,sys; d=json.load(sys.stdin); print("uploaded:", d.get("browser_download_url", d))'
        else
            echo "Release not created yet - run this again after CI finishes."
        fi
    fi
fi
