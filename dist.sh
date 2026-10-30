#!/bin/sh
# Builds the app without firmware and zips it for a GitHub release: build/Vladiktron-Spoke-<version>.zip
# The version comes from app/Resources/Info.plist. The .github/workflows/release.yml workflow runs this on a version tag.
set -e
cd "$(dirname "$0")"
VERSION=$(/usr/libexec/PlistBuddy -c "Print CFBundleShortVersionString" app/Resources/Info.plist)
./build.sh no-firmware                               # the file does not exist, so nothing is installed
ZIP="build/Vladiktron-Spoke-$VERSION.zip"
rm -f "$ZIP"
ditto -c -k --keepParent "build/Vladiktron Spoke.app" "$ZIP"     # keeps the signature
echo "built $ZIP"
