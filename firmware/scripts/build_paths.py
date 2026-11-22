# PlatformIO pre-build script (see extra_scripts in platformio.ini).
#
# The Arduino core's log lines carry __FILE__, which is the full path of each source file in PlatformIO's packages
# folder, for example /Users/<name>/.platformio/packages/framework-arduinoespressif32/cores/esp32/HWCDC.cpp. That
# would put the build machine's user name into every firmware image. This maps those paths to a neutral prefix.
# Added here rather than in build_flags so a packages folder with spaces in its path still builds.
Import("env")  # noqa: F821 (SCons provides it)

for prefix, short in ((env.subst("$PROJECT_PACKAGES_DIR"), "pio-packages"),  # noqa: F821
                      (env.subst("$PROJECT_DIR"), "rekadeck")):              # noqa: F821
    env.Append(CCFLAGS=["-fmacro-prefix-map=%s=%s" % (prefix, short)])     # noqa: F821
