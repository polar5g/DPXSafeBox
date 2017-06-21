QT_BUILD_PARTS += libs
QT_QCONFIG_PATH = 

host_build {
    QT_CPU_FEATURES.x86_64 =  sse sse2
} else {
    QT_CPU_FEATURES.x86_64 =  sse sse2
}
QT_COORD_TYPE += double
CONFIG += precompile_header pcre release compile_examples sse2 sse3 ssse3 sse4_1 sse4_2 avx largefile
QMAKE_QT_VERSION_OVERRIDE = 5
sql-drivers    += odbc sqlite
styles         += windows fusion windowsxp windowsvista
