# Dependencias nativas usadas pelo MVP.
#
# O Windows SDK fornece:
# - C++/WinRT (headers winrt/*)
# - Windows.Graphics.Capture
# - Direct3D 11 / DXGI
# - windowsapp.lib
#
# FFmpeg/FFprobe sao ferramentas externas executadas pelo aplicativo e ficam
# em third_party/ffmpeg/bin. Nao ha link estatico/dinamico com libav* no MVP.

set(YOUROTS_WINDOWS_SDK_VERSION "10.0.26100.0")
set(YOUROTS_FFMPEG_VERSION "9.0.2")
set(YOUROTS_FFMPEG_VARIANT "gyan-essentials-static")
set(YOUROTS_FFMPEG_SHA256 "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba")
