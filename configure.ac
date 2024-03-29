# initialise autoconf and set up some basic information about the program we’re packaging
AC_INIT([antd-publishers], [0.1.2a], [xsang.le@gmail.com])

# We’re going to use automake for this project
# [subdir-objects] if needed
AM_INIT_AUTOMAKE([subdir-objects])

# dependencies
# C compiler
AC_PROG_CC
# libtool for linking
AC_PROG_LIBTOOL

AC_DEFINE([_GNU_SOURCE], [1],[Use GNU source])
# AC_CANONICAL_HOST is needed to access the 'host_os' variable

has_antd=no
# check for lib antd
AC_CHECK_HEADER([antd/plugin.h],[
    has_antd=yes
    # check if the library exists
],[
    AC_MSG_ERROR([Unable to find antd, please install it first])
])
AC_CHECK_LIB([antd],[antd_send],[],[
    if test "$has_antd" = "yes"; then
        AC_MSG_ERROR([Unable to find antd shared library, please install it first])
    fi
])


cam_enable=no
# check videodev
AC_CHECK_HEADER([linux/videodev2.h],[
    # check libjpeg
    AC_CHECK_HEADER([jpeglib.h],[
        # check libjpeg
        AC_CHECK_LIB([jpeg],[jpeg_CreateCompress],[
            cam_enable=yes
        ],[])
    ],[])
],[])

AC_CHECK_LIB([jpeg],[jpeg_CreateCompress],[], [])


# debug option
AC_ARG_ENABLE([debug],
    [  --enable-debug    Turn on debugging],
    [case "${enableval}" in
        yes) AC_DEFINE([DEBUG], [1],[Enable debug]) ;;
        no) ;;
        *) AC_MSG_ERROR([bad value ${enableval} for --enable-debug]) ;;
    esac],[debug=false])

AC_CANONICAL_HOST
build_linux=no
build_windows=no
build_mac=no
# Detect the target system
case "${host_os}" in
    linux*)
        AC_DEFINE([LINUX], [1],[Linux system])
        build_linux=yes
        ;;
    darwin*)
        build_mac=yes
        AC_DEFINE([MACOS], [1],[MacOS system])
        ;;
    *)
        AC_MSG_ERROR(["OS $host_os is not supported"])
        ;;
esac
# case for window:
# cygwin*|mingw*)
#    build_windows=yes
#    ;;
# Pass the conditionals to automake
AM_CONDITIONAL([LINUX], [test "$build_linux" = "yes"])
AM_CONDITIONAL([WINDOWS], [test "$build_windows" = "yes"])
AM_CONDITIONAL([OSX], [test "$build_mac" = "yes"])
AM_CONDITIONAL([ENABLE_CAM], [test "$cam_enable" = "yes"])

# find a file called Makefile.in, substitute placeholders
# like @PACKAGE_VERSION@ with values like 0.1.0a, 
# and write the results to Makefile.
AC_CONFIG_FILES([
    Makefile 
    vterm/Makefile
    wfifo/Makefile
    syslog/Makefile
    broadcast/Makefile
])

if test x"${cam_enable}" == x"yes" ; then
    AC_CONFIG_FILES([v4l2cam/Makefile])
fi

# output the script:
AC_OUTPUT