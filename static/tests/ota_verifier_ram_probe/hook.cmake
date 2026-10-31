# Compatibility marker for invocations that historically supplied this file
# via CMAKE_PROJECT_INCLUDE. The root CMakeLists performs the actual opt-in
# after production targets exist; nested Pico SDK projects must not add it.
set(OTA_VERIFIER_RAM_PROBE_HOOK_INCLUDED ON CACHE INTERNAL "T1 RAM probe hook seen")
