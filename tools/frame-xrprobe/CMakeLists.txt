# frame-xrprobe: one run on the Steam Frame answers the OpenXR and Vulkan questions
# the Outbreak VR port depends on (see frame-xrprobe.cpp). Added only when
# ENABLE_OPENXR is on (outbreak-vr.yml); packaged into the Frame AppDir tarball as
# usr/bin/frame-xrprobe by appimage-qt.sh.

add_executable(frame-xrprobe frame-xrprobe.cpp)

# The static OpenXR loader, the Vulkan headers only (libvulkan.so.1 is opened with
# dlopen at run time) and libdl for that dlopen.
target_link_libraries(frame-xrprobe PRIVATE openxr_loader vulkan-headers ${CMAKE_DL_LIBS})

target_compile_definitions(frame-xrprobe PRIVATE
	"FRAME_XRPROBE_VERSION=\"${PCSX2_GIT_REV} (${PCSX2_GIT_HASH})\""
)

# Linked without a libstdc++ dependency: the probe has to start on whatever the
# device has (SteamOS on the host, or an older Steam Runtime container), because a
# probe that cannot start answers nothing. It sits outside the AppImage's library
# path, so it gets none of the libraries the AppImage bundles.
target_link_options(frame-xrprobe PRIVATE -static-libstdc++)
