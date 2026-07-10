# ../src is this repo's own forked LilyGo_AMOLED library (repo root's src/, the same
# one [env] exposes to the plain-Arduino/SCons webJPEG env via lib_extra_dirs). The
# ESP-IDF/CMake builder doesn't do that kind of implicit folder discovery - unlike
# SCons, it needs every source file listed explicitly - so without these, webH264
# silently fell back to linking [env:webH264]'s lib_deps copy of the *unmodified
# upstream* xinyuan-lilygo/LilyGo-AMOLED-Series package instead of this repo's fork,
# meaning any local fix to src/ (e.g. the T4-S3 auto-detect SD-card panic fix in
# beginAMOLED_241()'s caller) silently never took effect for this example.
idf_component_register(SRCS "webH264/webH264.cpp" "webH264/h264_decode.c" "webH264/qrcodegen.c"
                             "../src/LilyGo_AMOLED.cpp" "../src/LV_Helper.cpp" "../src/initSequence.cpp"
                        INCLUDE_DIRS "webH264" "../src")
