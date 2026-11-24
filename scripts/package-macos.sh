#!/bin/sh
# Build and package DeskFling.app + DeskFling-macOS.dmg from source.
set -eu
cd "$(dirname "$0")/.."
if [ -d .tools/bin ]; then PATH="$PWD/.tools/bin:$PATH"; export PATH; fi
if [ -d /opt/homebrew/bin ]; then PATH="/opt/homebrew/bin:$PATH"; export PATH; fi

cmake --preset macos
cmake --build --preset macos-release --target desktop_protected_mirror

package="$PWD/dist/macos-release"
app_bundle="$package/DeskFling.app"
dmg_staging="$package/.dmg-staging"
dmg_out="$PWD/dist/DeskFling-macOS.dmg"

rm -rf "$package" "$dmg_out"
mkdir -p "$app_bundle/Contents/MacOS" "$app_bundle/Contents/Resources"

cp platforms/macos/app_bundle_info.plist "$app_bundle/Contents/Info.plist"
cp platforms/macos/AppIcon.icns "$app_bundle/Contents/Resources/AppIcon.icns"
cp build/macos/platforms/macos/Release/desktop_protected_mirror "$app_bundle/Contents/MacOS/DeskFling"
chmod 755 "$app_bundle/Contents/MacOS/DeskFling"

/usr/bin/codesign --force --deep --sign - --identifier org.desktopphysicsplayground.protected-probe "$app_bundle"

cat > "$package/紧急恢复桌面.command" <<'RESTORE_EOF'
#!/bin/zsh
cd -- "${0:A:h}" || exit 1
print '正在执行独立桌面恢复检查…'
"./DeskFling.app/Contents/MacOS/DeskFling" --recover
rc=$?
"./DeskFling.app/Contents/MacOS/DeskFling" --status
print "恢复退出码：${rc}。按回车关闭。"
read -r _
exit $rc
RESTORE_EOF
chmod 755 "$package/紧急恢复桌面.command"

mkdir -p "$dmg_staging"
cp -R "$app_bundle" "$dmg_staging/"
ln -s /Applications "$dmg_staging/Applications"
cp "$package/紧急恢复桌面.command" "$dmg_staging/"
/usr/bin/hdiutil create -volname "DeskFling - 桌面游乐场" -srcfolder "$dmg_staging" -ov -format UDZO "$dmg_out"
rm -rf "$dmg_staging"

/usr/bin/shasum -a 256 "$app_bundle/Contents/MacOS/DeskFling" "$dmg_out"
