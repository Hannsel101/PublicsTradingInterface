# Android release for Galaxy S24 Ultra

This project packages an arm64-v8a APK with the financial API icon created in chat.
The Android launcher includes adaptive and legacy density-specific icons.

## Local build

The verified local toolchain uses Qt 6.11.2, Android NDK 27.2.12479018,
SDK/build-tools 36, and Homebrew OpenJDK 21.

```sh
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
export PATH="$HOME/Qt6/Tools/CMake/CMake.app/Contents/bin:$JAVA_HOME/bin:$PATH"
export ANDROID_SDK_ROOT="$HOME/Library/Android/sdk"
cmake -S . -B build/android-s24-release -G 'Unix Makefiles' \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/Qt6/6.11.2/android_arm64_v8a/lib/cmake/Qt6/qt.toolchain.cmake" \
  -DQT_HOST_PATH="$HOME/Qt6/6.11.2/macos" \
  -DANDROID_SDK_ROOT="$ANDROID_SDK_ROOT" \
  -DANDROID_NDK_ROOT="$ANDROID_SDK_ROOT/ndk/27.2.12479018" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DQT_ANDROID_ABIS=arm64-v8a
cmake --build build/android-s24-release --target apk --parallel 6
python3 scripts/sign-android-release.py \
  build/android-s24-release/android-build/build/outputs/apk/release/android-build-release-unsigned.apk \
  build/android-s24-release/PublicsTradingInterface-S24Ultra-release.apk
```

## Signing key

The initial keystore has already been created. Do not regenerate it for updates.

- Directory: `~/.publics-trading-signing/` (outside the repository)
- Keystore: `publics-trading-release.p12` (PKCS#12, RSA 3072)
- Alias: `publics-trading-release`
- Password: `keystore-password.txt` (owner-only permissions; never commit or publish)
- Certificate SHA-256: `838c6eafdf55eb6774ddcf8908b7e1d615eeae67e7aee462711bacbd202e36b2`

Back up the keystore and password securely. Keep these files private; distribute
only the signed APK. Future updates must use this same signing key and an
incremented `QT_ANDROID_VERSION_CODE` in CMakeLists.txt. If an installed build
was signed with another key, Android will not accept this APK as an update.
Do not uninstall an existing installation without considering its local data.

## Verification performed

- Release APK build completed successfully.
- `apksigner verify --verbose --print-certs` passed (APK signature v3).
- `zipalign -c -P 16 4` passed.
- APK manifest: package `org.qtproject.example.appPublicsTradingInterface`,
  version 0.1/code 1, minimum SDK 28, target SDK 36, not debuggable.
- All six packaged icon images matched source resource pixels.
- ARM64 Qt OpenSSL backend plus `libssl_3.so` and `libcrypto_3.so` are packaged.
- No Android device was connected, so installation, launch, and runtime TLS
  validation on the Galaxy S24 Ultra remain to be checked on the device.
- No live trade or preflight API calls were made.

**Release builds enable actual trading when the user initiates a trade.**
