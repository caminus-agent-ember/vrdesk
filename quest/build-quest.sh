#!/usr/bin/env bash
# build-quest.sh - build the Quest-side vrdesk OpenXR APK without gradle.
#
#   ./build-quest.sh          # build quest/build/vrdesk.apk
#   ./build-quest.sh install  # build, then adb install to the attached Quest
#
# Needs: NDK 26.1 (/usr/lib/android-ndk), build-tools 34.0.0 + platform android-34
# (~/android-sdk), the OpenXR headers and libopenxr_loader.so under openxr/.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
NDK="${NDK:-/usr/lib/android-ndk}"
SDK="${ANDROID_SDK:-$HOME/android-sdk}"
BT="$SDK/build-tools/34.0.0"
PLATFORM="$SDK/platforms/android-34/android.jar"
GLUE="/usr/lib/android-sdk/ndk/26.1.10909125/sources/android/native_app_glue"
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang"
OUT="$HERE/build"
ABI=arm64-v8a

for f in "$BT/aapt2" "$BT/d8" "$BT/apksigner" "$BT/zipalign" "$PLATFORM" "$CC" \
         "$GLUE/android_native_app_glue.c" \
         "$PROJ/openxr/lib/$ABI/libopenxr_loader.so" "$PROJ/openxr/include/openxr/openxr.h"; do
    [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

echo "== clean"
rm -rf "$OUT"
mkdir -p "$OUT/obj" "$OUT/dex" "$OUT/apk/lib/$ABI" "$OUT/res"

echo "== native ($ABI)"
$CC -c -O2 -fPIC -Wall -Wno-unused-parameter \
    -I "$PROJ/openxr/include" -I "$GLUE" \
    -o "$OUT/obj/vrdesk_xr.o" "$HERE/jni/vrdesk_xr.c"
$CC -c -O2 -fPIC -I "$GLUE" -o "$OUT/obj/native_app_glue.o" "$GLUE/android_native_app_glue.c"
$CC -shared -o "$OUT/apk/lib/$ABI/libvrdesk_xr.so" \
    "$OUT/obj/vrdesk_xr.o" "$OUT/obj/native_app_glue.o" \
    "$PROJ/openxr/lib/$ABI/libopenxr_loader.so" \
    -landroid -llog -lEGL -lGLESv3 -lm
cp "$PROJ/openxr/lib/$ABI/libopenxr_loader.so" "$OUT/apk/lib/$ABI/"
echo "   libvrdesk_xr.so $(stat -c%s "$OUT/apk/lib/$ABI/libvrdesk_xr.so") bytes"

echo "== java"
mkdir -p "$OUT/classes"
javac -source 8 -target 8 -Xlint:-options -bootclasspath "$PLATFORM" \
      -d "$OUT/classes" $(find "$HERE/src" -name '*.java') 2>&1 | grep -v 'bootstrap class path' || true
"$BT/d8" --lib "$PLATFORM" --min-api 29 --output "$OUT/dex" \
      $(find "$OUT/classes" -name '*.class')
cp "$OUT/dex/classes.dex" "$OUT/apk/classes.dex"

echo "== resources + manifest"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM" \
      --manifest "$HERE/AndroidManifest.xml" \
      --min-sdk-version 29 --target-sdk-version 32 \
      --version-code 1 --version-name 1.0 \
      -A "$OUT/apk" 2>/dev/null || \
"$BT/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM" \
      --manifest "$HERE/AndroidManifest.xml" \
      --min-sdk-version 29 --target-sdk-version 32 \
      --version-code 1 --version-name 1.0

echo "== package"
cd "$OUT"
cp base.apk unsigned.apk
(cd apk && zip -q -r ../unsigned.apk classes.dex lib)
"$BT/zipalign" -f -p 4 unsigned.apk aligned.apk

KS="$HERE/vrdesk.keystore"
if [ ! -f "$KS" ]; then
    echo "== generating signing key"
    keytool -genkeypair -keystore "$KS" -alias vrdesk -keyalg RSA -keysize 2048 \
            -validity 10000 -storepass vrdesk123 -keypass vrdesk123 \
            -dname "CN=vrdesk, OU=dev, O=caminus, L=, S=, C=US" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:vrdesk123 --key-pass pass:vrdesk123 \
      --out "$OUT/vrdesk.apk" aligned.apk
"$BT/apksigner" verify --print-certs "$OUT/vrdesk.apk" | head -3
echo "== built $OUT/vrdesk.apk ($(stat -c%s "$OUT/vrdesk.apk") bytes)"

if [ "${1:-}" = "install" ]; then
    echo "== adb install"
    adb -s "${QUEST_SERIAL:-2G0YC5ZG7H07NS}" install -r -g "$OUT/vrdesk.apk"
fi
